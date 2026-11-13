#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <sys/signalfd.h>
#include <poll.h>
#define LENGTH(X) (sizeof(X) / sizeof (X[0]))
#define CMDLENGTH        50
#define STATUSLENGTH     (LENGTH(blocks) * CMDLENGTH + 1)

typedef struct {
	char* icon;
	char* command;
	unsigned int interval;
	unsigned int signal;
} Block;
void sighandler();
void buttonhandler(int ssi_int);
void replace(char *str, char old, char new);
void remove_all(char *str, char to_remove);
void getcmds(int time);
void getsigcmds(int signal);
void setupsignals();
int getstatus(char *str, char *last);
void setroot();
void statusloop();
void termhandler(int signum);


#include "config.h"

static char statusbar[LENGTH(blocks)][CMDLENGTH] = {0};
static char statusstr[2][STATUSLENGTH];
static volatile sig_atomic_t statusContinue = 1;
static int signalFD;
static int timerInterval = -1;
static void (*writestatus) () = setroot;

void replace(char *str, char old, char new)
{
	for(char * c = str; *c; c++)
		if(*c == old)
			*c = new;
}

// the previous function looked nice but unfortunately it didnt work if to_remove was in any position other than the last character
// theres probably still a better way of doing this
void remove_all(char *str, char to_remove) {
	char *read = str;
	char *write = str;
	while (*read) {
		if (*read != to_remove) {
			*write++ = *read;
		}
		++read;
	}
	*write = '\0';
}

int gcd(int a, int b)
{
	int temp;
	while (b > 0){
		temp = a % b;

		a = b;
		b = temp;
	}
	return a;
}


//opens process *cmd and stores output in *output
void getcmd(const Block *block, char *output)
{
	size_t offset = 0;
	output[0] = '\0';
	if (block->signal) {
		output[0] = (char)block->signal;
		output[1] = '\0';
		offset = 1;
	}

	FILE *cmdf = popen(block->command, "r");
	if (!cmdf)
		return;

	char tmpstr[CMDLENGTH] = "";
	char *s;
	int e;
	do {
		errno = 0;
		s = fgets(tmpstr, sizeof(tmpstr), cmdf);
		e = errno;
	} while (!s && e == EINTR);
	pclose(cmdf);

	remove_all(tmpstr, '\n');
	snprintf(output + offset, CMDLENGTH - offset, "%s%s",
		block->icon ? block->icon : "", tmpstr);

	if (block != &blocks[LENGTH(blocks) - 1]) {
		size_t used = strlen(output);
		if (used > offset && used < CMDLENGTH - 1)
			snprintf(output + used, CMDLENGTH - used, "%s", delim ? delim : "");
	}
}

void getcmds(int time)
{
	const Block* current;
	for(int i = 0; i < LENGTH(blocks); i++)
	{
		current = blocks + i;
		if ((current->interval != 0 && time % current->interval == 0) || time == -1){
			getcmd(current,statusbar[i]);
        }
	}
}

void getsigcmds(int signal)
{
	const Block *current;
	for (int i = 0; i < LENGTH(blocks); i++)
	{
		current = blocks + i;
		if (current->signal == signal){
			getcmd(current,statusbar[i]);
        }
	}
}

void setupsignals()
{
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGALRM); // Timer events
	sigaddset(&signals, SIGUSR1); // Button events
	// All signals assigned to blocks
	for (size_t i = 0; i < LENGTH(blocks); i++)
		if (blocks[i].signal > 0)
			sigaddset(&signals, SIGRTMIN + blocks[i].signal);
	// Create signal file descriptor for pooling
	signalFD = signalfd(-1, &signals, 0);
	// Block all real-time signals
	for (int i = SIGRTMIN; i <= SIGRTMAX; i++) sigaddset(&signals, i);
	sigprocmask(SIG_BLOCK, &signals, NULL);
	// Do not transform children into zombies
	struct sigaction sigchld_action = {
  		.sa_handler = SIG_DFL,
  		.sa_flags = SA_NOCLDWAIT
	};
	sigaction(SIGCHLD, &sigchld_action, NULL);

}

int getstatus(char *str, char *last)
{
	snprintf(last, STATUSLENGTH, "%s", str);
	str[0] = '\0';
	for (size_t i = 0; i < LENGTH(blocks); i++) {
		size_t used = strlen(str);
		if (used >= STATUSLENGTH - 1)
			break;
		snprintf(str + used, STATUSLENGTH - used, "%s", statusbar[i]);
	}
	return strcmp(str, last); // 0 if they are the same
}

void setroot()
{
	if (!getstatus(statusstr[0], statusstr[1])) // Only set root if text has changed.
		return;
	Display *display = XOpenDisplay(NULL);
	if (!display)
		return;
	int display_screen = DefaultScreen(display);
	Window display_root = RootWindow(display, display_screen);
	XStoreName(display, display_root, statusstr[0]);
	XCloseDisplay(display);
}

void pstdout()
{
	if (!getstatus(statusstr[0], statusstr[1]))//Only write out if text has changed.
		return;
	printf("%s\n",statusstr[0]);
	fflush(stdout);
}


void statusloop()
{
	setupsignals();
    // first figure out the default wait interval by finding the
    // greatest common denominator of the intervals
    for(int i = 0; i < LENGTH(blocks); i++){
        if(blocks[i].interval){
            timerInterval = gcd(blocks[i].interval, timerInterval);
        }
    }
    getcmds(-1);     // Fist time run all commands
    raise(SIGALRM);  // Schedule first timer event
    int ret;
    struct pollfd pfd[] = {{.fd = signalFD, .events = POLLIN}};
    while (statusContinue) {
        // Wait for new signal
        ret = poll(pfd, sizeof(pfd) / sizeof(pfd[0]), -1);
        if (ret < 0 || !(pfd[0].revents & POLLIN)) break;
        sighandler(); // Handle signal
    }
}

void sighandler()
{
	static int time = 0;
	struct signalfd_siginfo si;
	int ret = read(signalFD, &si, sizeof(si));
	if (ret < 0) return;
	int signal = si.ssi_signo;
	switch (signal) {
		case SIGALRM:
			// Execute blocks and schedule the next timer event
			getcmds(time);
			alarm(timerInterval);
			time += timerInterval;
			break;
		case SIGUSR1:
			// Handle buttons
			buttonhandler(si.ssi_int);
			return;
		default:
			// Execute the block that has the given signal
			getsigcmds(signal - SIGRTMIN);
			break;
	}
	writestatus();
}

void buttonhandler(int ssi_int)
{
	char button[2] = {(char)('0' + (ssi_int & 0xff)), '\0'};
	pid_t process_id = getpid();
	int sig = ssi_int >> 8;
	const Block *current = NULL;
	for (size_t i = 0; i < LENGTH(blocks); i++) {
		if (blocks[i].signal == (unsigned int)sig) {
			current = &blocks[i];
			break;
		}
	}
	if (!current)
		return;

	if (fork() == 0) {
		char shcmd[1024];
		int written = snprintf(shcmd, sizeof(shcmd), "%s && kill -%u %d",
			current->command, current->signal + SIGRTMIN, process_id);
		if (written < 0 || (size_t)written >= sizeof(shcmd))
			_exit(EXIT_FAILURE);
		char *command[] = {"/bin/sh", "-c", shcmd, NULL};
		setenv("BLOCK_BUTTON", button, 1);
		setsid();
		execvp(command[0], command);
		_exit(EXIT_FAILURE);
	}
}


void termhandler(int signum)
{
	statusContinue = 0;
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp("-d", argv[i])) {
			if (i + 1 >= argc) {
				fprintf(stderr, "dwmblocks: -d requires a delimiter\n");
				return EXIT_FAILURE;
			}
			delim = argv[++i];
		} else if (!strcmp("-p", argv[i])) {
			writestatus = pstdout;
		}
	}
	signal(SIGTERM, termhandler);
	signal(SIGINT, termhandler);
	statusloop();
	if (signalFD >= 0)
		close(signalFD);
	return EXIT_SUCCESS;
}
