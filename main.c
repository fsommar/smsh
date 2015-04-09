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

extern char *strtok_r(char *, const char *, char **);
#define SMSH ("smsh")

/* e.g. "ls -aHpl" */
typedef struct {
	uint32_t num_args;
	char *cmd; /* "ls" */
	char **args; /* ["ls", "-aHpl"] */
} cmd;

typedef struct {
	uint32_t length;
	cmd **cmds;
	bool bg;
} commands;

commands *parse_commands(char *);
int exec_cmd(cmd *);
int exec_commands(commands);

/*
 * 1. Read input.
 * 2. Split into arguments and create commands struct.
 * 3. Execute each command and pipe if more than one in the background if bg or foreground otherwise.
 *
 * Make sure child processes are killed when parent is by registering signal handlers.
 */

int main(void) {

	for (;;) {
		/* Declarations at the top */
		char prompt[90];
		char *input;
		const char *path = getcwd(NULL, 80);
		commands *commands;

		strcpy(prompt, path);
		strcat(prompt, " ¥ ");

		/* input is allocated in readline.
		 * it's the callee's (our) obligation to free it */
		input = readline(prompt);
		if (!input) break;

		/* 2. Parse arguments into commands. */
		commands = parse_commands(input);

		if (!commands || commands->length == 0) {
			continue;
		}

		/* TODO: Fork. Execute commands */
		if (commands->length == 1) {
			if (!exec_cmd(*commands->cmds)) {
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
	int num_cmds = 0, num_args = 0, bg_counter = 0;
	int cmds_buf_len = 2, args_buf_len = 2;

	const char *pipe_delim = "|";
	char *cmd_str;
	char *save_pipe_ptr;
	
	const char *delim = " ";
	char *token;
	char *save_space_ptr;

	char **args;
	cmd **cmds;
	commands *commands_struct;
	cmd *command;


	/* Free in main method after processing all the commands */
	commands_struct = malloc(sizeof(*commands_struct));

	cmds = calloc(cmds_buf_len, sizeof(*cmds));

	/* Split the inputs into commands by using the pipeline as a deliminator */
	cmd_str = strtok_r(input, pipe_delim, &save_pipe_ptr);

	while (NULL != cmd_str) {
		/* Free in main method after processing the command */
		command = malloc(sizeof(*command));
		args_buf_len = 2;
		args = calloc(args_buf_len, sizeof(*args));

		/* Split the command into tokens by using space as a deliminator */
		token = strtok_r(cmd_str, delim, &save_space_ptr);
		command->cmd = token;

		num_args = 0;
		/* Adds all the tokens to the command arguments, including the command itself */
		while (NULL != token) {
			/* Counts the number of background characters to be able to indicate parse warnings */
			if (0 == strcmp(token, "&")) { bg_counter++; }

			/* grow buffer if necessary */
			if (num_args >= args_buf_len - 1) {
				/* realloc buffer */
				args_buf_len += 2;
				args = realloc(args, args_buf_len);
				if (!args) {
					/* Couldn't allocate enough memory */
					exit(EXIT_FAILURE);
				}
			}

			args[num_args] = token;
			token = strtok_r(NULL, delim, &save_space_ptr);
			num_args++;
		}

		/* grow buffer if necessary */
		if (num_cmds >= cmds_buf_len - 1) {
			/* realloc buffer */
			cmds_buf_len += 2;
			cmds = realloc(cmds, cmds_buf_len);
			if (!cmds) {
				/* Couldn't allocate enough memory */
				exit(EXIT_FAILURE);
			}
		}

		command->args = args;
		command->num_args = num_args;
		cmds[num_cmds] = command;
		num_cmds++;
		cmd_str = strtok_r(NULL, pipe_delim, &save_pipe_ptr);
	}

	commands_struct->cmds = cmds;
	commands_struct->length = num_cmds;

	/* Check if the processes should be run in the background or if there are parse errors */
	if (0 == bg_counter) {
		commands_struct->bg = false;
		return commands_struct;
	} else if (1 == bg_counter) {
		cmd last_command = *commands_struct->cmds[commands_struct->length - 1];
		if (0 == strcmp(last_command.args[last_command.num_args - 1], "&")) {
			commands_struct->bg = true;
			return commands_struct;
		}
	}
	
	fprintf(stderr, "smsh: inaccurate use of background character '&'\n");

	return NULL;
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

#define NUM_BUILTINS (sizeof(builtins) / sizeof(*builtins))

int exec_cmd(cmd *command) {
	int i;
	/* Check for command in builtins first.
	 * If it does not exist there then assume it's an existing command. */
	for (i = 0; i < NUM_BUILTINS; i++) {
		if (0 == strcmp(command->cmd, builtins[i])) {
			return (*builtins_funcs[i])(command->args);
		}
	}

	return execvp(command->cmd, command->args);
}

int exec_commands(commands commands) {
	/* Pipes etc etc */
	exit(EXIT_SUCCESS);
}

/* Built in commands */
int exit_cmd(char **args) {
	/* TODO: Cleanup child processes? */
	exit(EXIT_SUCCESS);
}

int cd_cmd(char **args) {
	/* TODO: Make sure len(args) >= 2 */
	char *dir = "~";
	if (args[1]) {
		dir = args[1];
	}
	if (0 != chdir(dir)) {
		perror(SMSH);
	}
	exit(EXIT_SUCCESS);
}

int checkEnv_cmd(char **args) {
	exit(EXIT_SUCCESS);
}
