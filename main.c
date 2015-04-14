#include <sys/types.h> /* defines the type pid_t */
#include <sys/wait.h> /* defines for instance waitpid() and WIFEXITED */
#include <errno.h> /* defines errno */
#include <unistd.h> /* defines for instance fork(), exec(), pipe() and STDIN_FILENO */
#include <stdlib.h> /* defines for instance malloc(), free(), exit(), rand() and RAND_MAX */
#include <stdio.h> /* defines for instance stderr and perror() */
#include <string.h> /* defines strcmp() and strtok() */
#include <stdbool.h>
#include <readline/readline.h>

extern char *strtok_r(char *, const char *, char **);
#define SMSH ("smsh")

/* e.g. "ls -aHpl" */
typedef struct {
	uint32_t num_args; /* 2 */
	char *bin; /* "ls" */
	char **args; /* ["ls", "-aHpl", NULL] */
} Command;

typedef struct {
	uint32_t length;
	Command **cmds;
	bool bg;
} CommandList;

CommandList *parse_commands(char *);
int exec_cmd(Command *);
int exec_commands(const CommandList *);

/*
 * 1. Read input.
 * 2. Split into arguments and create CommandList struct.
 * 3. Execute each command and pipe if more than one in the background if bg or foreground otherwise.
 *
 * Make sure child processes are killed when parent is by registering signal handlers.
 */

int main(void) {

	for (;;) {
		/* Declarations at the top */
		int status;
		char prompt[90];
		char *input;
		char history[1024];
		const char *path = getcwd(NULL, 80);
		CommandList *commands;

		strcpy(prompt, path);
		strcat(prompt, " ¥ ");

		/* input is allocated in readline.
		 * it's the callee's (our) obligation to free it */
		input = readline(prompt);
		if (!input) break;
		/* parse_commands modifies input - copy and save for adding to history */
		strcpy(history, input);

		/* 2. Parse arguments into commands. */
		commands = parse_commands(input);

		if (!commands) { continue; }
		if (commands->length == 0) {
			free(commands->cmds);
			free(commands);
			continue;
		}

		/* TODO: Fork. Execute commands */
		if (commands->length == 1) {
			if (exec_cmd(commands->cmds[0]) < 0) {
				/* Execute failed */
				perror(SMSH);
			}
		} else {
			/* Commands were piped, handle it accordingly */
			exec_commands(commands);
		}

		if (!commands->bg) {
			/* Handle foreground waiting */
			wait(&status);
			/* TODO: Handle freeing for background as well */
			free(commands->cmds);
			free(commands);
		}

		add_history(history);

		free(input);
	}

	return EXIT_SUCCESS;
}

CommandList *parse_commands(char *input) {
	/* Free in main method after processing all the commands */
	CommandList *commands = malloc(sizeof(*commands));

	/* Split the inputs into commands by using the pipeline as a deliminator */
	const char *pipe_delim = "|";
	char *save_pipe_ptr;
	char *cmd_str = strtok_r(input, pipe_delim, &save_pipe_ptr);

	size_t cmds_buf_len = 2;
	commands->bg = false;
	commands->length = 0;
	commands->cmds = calloc(cmds_buf_len, sizeof(*commands->cmds));

	while (NULL != cmd_str) {
		/* Free in main method after processing the command */
		Command *command = malloc(sizeof(*command));

		/* Split the command into tokens by using space as a deliminator */
		const char *delim = " ";
		char *save_space_ptr;
		char *arg_str = strtok_r(cmd_str, delim, &save_space_ptr);

		size_t args_buf_len = 3;
		command->bin = arg_str;
		command->num_args = 0;
		command->args = calloc(args_buf_len, sizeof(*command->args));

		/* Adds all the tokens to the command arguments, including the command itself */
		while (NULL != arg_str) {
			if (commands->bg) {
				/* If '&' already was seen then it's not the last symbol */
				fprintf(stderr, "smsh: inaccurate use of background character '&' (%s)", arg_str);
				return NULL;
			}

			if (0 == strcmp(arg_str, "&")) {
				commands->bg = true;
			} else {
				/* grow args buffer if necessary */
				if (command->num_args + 1 >= args_buf_len) {
					args_buf_len += 2;
					command->args = realloc(command->args, args_buf_len * sizeof(*command->args));
					if (!command->args) {
						perror("realloc fail");
						exit(EXIT_FAILURE);
					}
				}

				command->args[command->num_args++] = arg_str;
				/* Terminate the list with a NULL pointer as expected by execv */
				command->args[command->num_args] = NULL;
			}
			arg_str = strtok_r(NULL, delim, &save_space_ptr);
		}

		/* grow commands buffer if necessary */
		if (commands->length + 1 >= cmds_buf_len) {
			cmds_buf_len += 2;
			commands->cmds = realloc(commands->cmds, cmds_buf_len * sizeof(*commands->cmds));
			if (!commands->cmds) {
				perror("realloc fail");
				exit(EXIT_FAILURE);
			}
		}

		commands->cmds[commands->length++] = command;
		cmd_str = strtok_r(NULL, pipe_delim, &save_pipe_ptr);
	}

	return commands;
}

int exit_cmd(char **);
int cd_cmd(char **);
int checkEnv_cmd(char **);

const char *builtins[] = {
	"exit",
	"cd",
	"checkEnv"
};

int (*builtins_funcs[]) (char **) = {
	&exit_cmd,
	&cd_cmd,
	&checkEnv_cmd
};

#define NUM_BUILTINS ((int) (sizeof(builtins) / sizeof(*builtins)))

int exec_cmd(Command *command) {
	int i;
	/* Check for command in builtins first.
	 * If it does not exist there then assume it's an existing command. */
	for (i = 0; i < NUM_BUILTINS; i++) {
		if (0 == strcmp(command->bin, builtins[i])) {
			return (*builtins_funcs[i])(command->args);
		}
	}

	/* TODO: Error handling; check PID */
	if (!fork()) {
		return execvp(command->bin, command->args);
	} else {
		free(command->args);
		free(command);
		/* continue execution as parent */
		return EXIT_SUCCESS;
	}
}

int exec_commands(const CommandList *commands) {
	/* Pipes etc etc */
	if (!fork()) {
		/* in child */
		exit(EXIT_SUCCESS);
	} else {
		printf("TODO: Pipe multiple commands.\n");
		return EXIT_SUCCESS;
	}
}

/* Built in commands */
int exit_cmd(char **args) {
	/* TODO: Cleanup child processes? */
	exit(EXIT_SUCCESS);
}

int cd_cmd(char **args) {
	char *dir = "~";
	if (args[1]) {
		dir = args[1];
	}
	if (0 != chdir(dir)) {
		perror("cd");
	}
	return EXIT_SUCCESS;
}

int checkEnv_cmd(char **args) {
	exit(EXIT_SUCCESS);
}
