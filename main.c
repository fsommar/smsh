#include <sys/types.h> /* defines the type pid_t */
#include <sys/wait.h> /* defines for instance waitpid() and WIFEXITED */
#include <errno.h> /* defines errno */
#include <unistd.h> /* defines for instance fork(), exec(), pipe() and STDIN_FILENO */
#include <stdlib.h> /* defines for instance malloc(), free(), exit(), rand() and RAND_MAX */
#include <stdio.h> /* defines for instance stderr and perror() */
#include <string.h> /* defines strcmp() and strtok() */
#include <stdint.h>
#include <stdbool.h>
#include <readline/readline.h>
#include <readline/history.h>

/* e.g. "ls -aHpl" */
typedef struct {
	char *cmd; /* "ls" */
	char **args; /* ["-aHpl"] */
} cmd;

typedef struct {
	uint32_t length;
	cmd *cmds;
	bool bg;
} commands;

commands *parse_commands(char *);
int exec_cmd(cmd *);

/*
 * 1. Read input.
 * 2. Split into arguments and create commands struct.
 * 3. Execute each command and pipe if more than one in the background if bg or foreground otherwise.
 *
 * Make sure child processes are killed when parent is by registering signal handlers.
 */

int main(void) {

	for (;;) {
		/* input is allocated in readline.
		 * it's the callee's obligation to free it */
		char *input = readline("smsh ¥ ");
		if (!input) break;

		/* 2. Split into arguments. */
		commands *commands = parse_commands(input);

		if (!commands || commands->length == 0) {
			continue;
		}

		/* TODO: Fork. Execute commands */
		if (commands->length == 1) {
			if (!exec_cmd(commands->cmds)) {
				/* Execute failed */
			}
		} else {
			/* Commands were piped, handle it accordingly */
		}

		if (!commands->bg) {
			/* Handle foreground waiting */
		}

		printf("%s\n", input);

		add_history(input);

		free(input);
	}

	return 0;
}

commands *parse_commands(char *input) {
	return NULL;
}


int exec_cmd(cmd *command) {
	return 0;
}
