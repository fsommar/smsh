#include "main.h"

/*
 * 1. Read input.
 * 2. Split into arguments and create CommandList struct.
 * 3. Execute each command and pipe if more than one,
 * in the background if '&' was found or foreground otherwise.
 *
 * Make sure child processes are killed when parent is by registering signal handlers.
 */
static sigjmp_buf prompt_mark;
static volatile sig_atomic_t bg_killed = false, fg_killed = false;
static pid_t pid = 0;
static bool fg_process = false;

void signal_handler(int sig) {
	switch (sig) {
		case SIGINT:
			/* Only kill if child and fg process */
			if (fg_process && -1 != pid) {
				fg_killed = true;
				if (-1 == kill(pid, SIGKILL)) {
					/* Child couldn't be killed, but perror can't be used here
					 * because it is not safe for use in a signal handler.
					 * The error is purposefully ignored. */
					fg_killed = false;
				}
			}
			break;
		case SIGCHLD:
			/* This will only run when SIGDET=1. */
			/* Previously, the terminated background processes were
			 * printed here. However, because printf is not safe for use in a signal
			 * handler, it was updated to use a flag instead. */
			if (fg_process || fg_killed) {
				fg_killed = false;
				return;
			}
			bg_killed = 1;
			break;
		default: return;
	}
	if (-1 == write(STDOUT_FILENO, "\n", 1)) {
		/* The newline couldn't be printed.
		 * Like above, perror can't be used and because the newline
		 * isn't vital this error is purposefully ignored. */
	}
	/* Jump back to prompt */
	siglongjmp(prompt_mark, 1);
}

int main(void) {
	/* Register signal handler */
	struct sigaction sa;
	sa.sa_handler = &signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP;

#if SIGDET
	/* This is for cleanup of background processes */
	TRY(sigaction(SIGCHLD, &sa, NULL), "sigaction");
#endif
	/* Intercept SIGINT for parent and pass it to child */
	TRY(sigaction(SIGINT, &sa, NULL), "sigaction");

	/* Set prompt mark here for jumping to from the signal handler */
	while (0 != sigsetjmp(prompt_mark, 1));

	/* Loop forever, reading user input */
	for (;;) {
		char prompt[90];
		char *input, *path;
		char history[1024];
		CommandList *commands;
		struct timeval before, after;
		/* Check bg processes if pid is 0,
		 * i.e. pid is not a foreground process */
		bool check_bg = !fg_killed;

		sighold(SIGINT);
		/* Assume the length of CWD is never greater than 80 characters */
		path = getcwd(NULL, 80);

#if SIGDET
		check_bg = check_bg && bg_killed;
		bg_killed = 0;
#endif
		if (check_bg) {
			/* Check for completed child processes */
			pid_t zombie;
			while (0 < (zombie = waitpid(0, NULL, WNOHANG))) {
				printf("%d done\n", (int) zombie);
			}
			fflush(stdout);
		}

		strcpy(prompt, path);
		free(path);
		strcat(prompt, " ¥ ");
		sigrelse(SIGINT);

		/* input is allocated in readline.
		 * it's the callee's (our) obligation to free it */
		input = readline(prompt);
		/* On e.g. Ctrl-D the input is null and the shell is exited */
		if (!input) break;

		/* ENTERING CRITICAL AREA */
		sighold(SIGINT);

		/* parse_commands modifies input - copy and save for adding to history */
		strcpy(history, input);
		free(input);

		if (history[0]) {
			/* Add command line history for the user's convenience */
			add_history(history);
		}

		/* 2. Parse arguments into commands. */
		commands = parse_commands(history);

		if (!commands) {
			sigrelse(SIGINT);
			continue;
		}
		if (0 == commands->length) {
			/* For some reason an empty command was received. */
			goto next;
		}

		fg_process = !commands->bg;

		gettimeofday(&before, NULL);
		if (1 == commands->length) {
			if (EXIT_SUCCESS != exec_cmd(commands->cmds[0])) {
				/* Execute of command failed */
				fg_process = false;
			}
		} else {
			/* Commands were piped, handle it accordingly */
			exec_commands(commands, 0, STDIN_FILENO);
		}

next:
		free(commands->cmds);
		free(commands);
		sigrelse(SIGINT);

		if (fg_process) {
			uint64_t time_taken;

			sighold(SIGCHLD);

			/* Wait for foreground process */
			while (-1 != waitpid(pid, NULL, 0));
			gettimeofday(&after, NULL);

			sigrelse(SIGCHLD);

			time_taken = (uint64_t) (1000 * (after.tv_sec - before.tv_sec) +
					(after.tv_usec - before.tv_usec) / 1000);
			printf("%" PRIu64 " ms\n", time_taken);
			fflush(stdout);
			fg_process = false;
		}
	}

	/* Call exit command on exit to clean up child processes */
	return exit_cmd(NULL);
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
		command->num_args = 0;
		command->args = calloc(args_buf_len, sizeof(*command->args));

		/* Adds all the tokens to the command arguments, including the command itself */
		while (NULL != arg_str) {
			if (commands->bg) {
				uint32_t i;
				for (i = 0; i < commands->length; i++) {
					free(commands->cmds[i]->args);
					free(commands->cmds[i]);
				}
				free(commands->cmds);
				free(commands);
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
						perror("realloc");
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
				perror("realloc");
				exit(EXIT_FAILURE);
			}
		}

		commands->cmds[commands->length++] = command;
		cmd_str = strtok_r(NULL, pipe_delim, &save_pipe_ptr);
	}

	return commands;
}

int exec_cmd(Command *command) {
	/* Check for command in builtins first.
	 * If it does not exist there then assume it's an existing command. */
	int i;
	for (i = 0; i < NUM_BUILTINS; i++) {
		if (0 == strcmp(command->args[0], builtins[i])) {
			int ret_val = (*builtins_funcs[i])(command->args);
			free(command->args);
			free(command);
			return ret_val;
		}
	}

	/* Fork the process and execute the command on the child process */
	TRY(pid = fork(), "fork");

	if (0 == pid) { /* Start execution as child */
		return run_cmd(command);
	}

	/* Continue execution as parent */
	free(command->args);
	free(command);
	return EXIT_SUCCESS;
}

int run_cmd(Command *command) {
	execvp(command->args[0], command->args);
	/* If we end up here an error has occurred */
	perror(SMSH);
	exit(EXIT_FAILURE);
}

int exec_commands(CommandList *commands, const uint32_t cmd_index, const int fd_in) {
	Pipe pipefd;

	if (cmd_index == commands->length - 1) {
		uint32_t i;
		TRY(pid = fork(), "fork");
		if (0 == pid) {
			TRY(dup2(fd_in, STDIN_FILENO), "dup2");
			TRY(close(fd_in), "previous FD");
			/* Hard code support for the `pager` command in pipes */
			if (0 == strcmp(commands->cmds[cmd_index]->args[0], "pager")) {
				const char *pager = getenv("PAGER");
				if (pager) {
					execlp(pager, pager, (char *) NULL);
					perror(SMSH);
				}
				execlp("less", "less", (char *) NULL);
				perror(SMSH);
				execlp("more", "more", (char *) NULL);
				perror(SMSH);
				exit(EXIT_FAILURE);
			}
			return run_cmd(commands->cmds[cmd_index]);
		}

		TRY(close(fd_in), "previous FD")

		for (i = 0; i < commands->length; i++) {
			free(commands->cmds[i]->args);
			free(commands->cmds[i]);
		}
		return EXIT_SUCCESS;
	}

	TRY(pipe(pipefd), "pipe");
	TRY(pid = fork(), "fork");

	if (0 == pid) {
		if (fd_in != STDIN_FILENO) {
			TRY(dup2(fd_in, STDIN_FILENO), "dup2");
			TRY(close(fd_in), "previous FD");
		}

		TRY(dup2(pipefd[PIPE_WRITE_SIDE], STDOUT_FILENO), "dup2");
		TRY(close(pipefd[PIPE_WRITE_SIDE]), "pipe write");
		TRY(close(pipefd[PIPE_READ_SIDE]), "pipe read");

		return run_cmd(commands->cmds[cmd_index]);
	}

	TRY(close(pipefd[PIPE_WRITE_SIDE]), "pipe write");
	if (fd_in != STDIN_FILENO) {
		TRY(close(fd_in), "previous FD");
	}

	return exec_commands(commands, cmd_index + 1, pipefd[PIPE_READ_SIDE]);
}

/* The built-in exit command */
int exit_cmd(char **args) {
	(void) args; /* Workaround for unused variable */
#if SIGDET
	/* "If the action for the SIGCHLD signal is set to SIG_IGN,
	 * child processes of the calling processes shall
	 * not be transformed into zombie processes when they terminate."
	 */
	signal(SIGCHLD, SIG_IGN);
#endif

	/* Ignore SIGTERM in parent and send it to all child processes */
	signal(SIGTERM, SIG_IGN);
	TRY(kill(0, SIGTERM), "kill");

#if !SIGDET
	/* Poll and wait for child processes to finish */
	while (-1 != waitpid(-1, NULL, 0));
#endif

	exit(EXIT_SUCCESS);
}

/* The built-in cd command */
int cd_cmd(char **args) {
	/* Assume cd goes to $HOME by default.
	 * If an argument is passed then cd to that location. */
	const char *dir = getenv("HOME");
	if (args[1]) {
		dir = args[1];
	}
	if (0 != chdir(dir)) {
		perror("cd");
	}
	return EXIT_SUCCESS;
}

/* Used for creating commands in checkEnv to be passed into
 * exec_commands. */
#define CREATE_COMMAND(cmd) \
cmd = malloc(sizeof(*cmd)); \
cmd->num_args = 1; \
cmd->args = calloc(2, sizeof(*cmd->args)); \
cmd->args[0] = (char *) cmd ## _ ## s; \
command_list->cmds[command_list->length++] = cmd;

static const char *printenv_s = "printenv";
static const char *sort_s = "sort";
static const char *pager_s = "pager";
static const char *grep_s = "grep";

/* The built-in checkEnv command */
int checkEnv_cmd(char **args) {
	int ret_val;
	CommandList *command_list;
	Command *printenv, *grep, *sort, *pager;

	command_list = malloc(sizeof(*command_list));
	command_list->bg = false;
	command_list->length = 0;

	command_list->cmds = calloc(args[1] ? 4 : 3, sizeof(*command_list->cmds));

	CREATE_COMMAND(printenv);

	/* If an argument is passed to checkEnv, pipe printenv into
	 * grep with the supplied arguments. */
	if (args[1]) {
		uint32_t i;
		grep = malloc(sizeof(*grep));
		grep->num_args = 0;
		while (args[grep->num_args]) {
			grep->num_args++;
		}
		grep->args = calloc(grep->num_args + 1, sizeof(*grep->args));
		grep->args[0] = (char *) grep_s;
		for (i = 1; i < grep->num_args; i++) {
			grep->args[i] = args[i];
		}
		command_list->cmds[command_list->length++] = grep;
	}

	CREATE_COMMAND(sort);
	CREATE_COMMAND(pager);

	ret_val = exec_commands(command_list, 0, STDIN_FILENO);
	free(command_list->cmds);
	free(command_list);
	return ret_val;
}
