#define _POSIX_SOURCE (200809L)
#define _XOPEN_SOURCE (500)
#include <inttypes.h>
#include <sys/types.h> /* defines the type pid_t */
#include <sys/wait.h> /* defines for instance waitpid() and WIFEXITED */
#include <signal.h>
#include <sys/time.h>
#include <errno.h> /* defines errno */
#include <unistd.h> /* defines for instance fork(), exec(), pipe() and STDIN_FILENO */
#include <stdlib.h> /* defines for instance malloc(), free(), exit(), rand() and RAND_MAX */
#include <stdio.h> /* defines for instance stderr and perror() */
#include <string.h> /* defines strcmp() and strtok() */
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <readline/readline.h>
#include <readline/history.h>

#ifndef strtok_r
extern char *strtok_r(char *, const char *, char **);
#endif

#define SMSH ("smsh")
#define NUM_BUILTINS ((int) (sizeof(builtins) / sizeof(*builtins)))
#define PIPE_READ_SIDE (0)
#define PIPE_WRITE_SIDE (1)
/* Checks a syscall's return value and returns on error */
#define TRY(syscall, str) \
if (-1 == (syscall)) { \
	perror(str); \
	return EXIT_FAILURE; \
}
/* Make it obvious by type what's used as a pipe */
typedef int Pipe[2];

/* e.g. "ls -aHpl" */
typedef struct {
	uint32_t num_args; /* 2 */
	char **args; /* ["ls", "-aHpl", NULL] */
} Command;

/* Used for Command(s) and if it should run in fg or bg */
typedef struct {
	uint32_t length;
	Command **cmds;
	bool bg;
} CommandList;

void signal_handler(int);
CommandList *parse_commands(char *);
int exec_cmd(Command *);
int exec_commands(CommandList *, const uint32_t, const int);
int run_cmd(Command *);
int exit_cmd(char **);
int cd_cmd(char **);
int checkEnv_cmd(char **);

/* Names of the supported built-in functions */
static const char *builtins[] = {
	"exit",
	"cd",
	"checkEnv"
};

/* Pointers to the built-in functions that the shell supports */
static int (*builtins_funcs[]) (char **) = {
	&exit_cmd,
	&cd_cmd,
	&checkEnv_cmd
};
