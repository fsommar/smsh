#include "main.h"

static sigjmp_buf prompt_mark;
static pid_t pid = -1;
static bool fg_process = false;

/*
 * 1. Read input.
 * 2. Split into arguments and create CommandList struct.
 * 3. Execute each command and pipe if more than one,
 * in the background if '&' was found or foreground otherwise.
 *
 * Make sure child processes are killed when parent is by registering signal handlers.
 */
int main(void) {
	/* Register signal handler */
	struct sigaction sa;
	sa.sa_handler = &signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP;

#if SIGDET
	/* This is for cleanup of background processes */
	TRY_OR_EXIT(sigaction(SIGCHLD, &sa, NULL), "sigaction");
#endif
	/* Intercept SIGINT for parent and pass it to child */
	TRY_OR_EXIT(sigaction(SIGINT, &sa, NULL), "sigaction");
	TRY_OR_EXIT(sigaction(SIGTERM, &sa, NULL), "sigaction");

	/* Set prompt mark here for jumping to from the signal handler */
	while (0 != sigsetjmp(prompt_mark, 1));

	/* Loop forever (until EOF), reading user input */
	for (;;) {
		/* Assume the length of the prompt
		 * will never exceed 1024 characters. */
		char prompt[1024], input[1024], *tmp;
		struct timeval before, after;
		CommandList commands;
		pid_t zombie;

		/* Clear the buffers on the stack. */
		memset(prompt, 0, sizeof(prompt));
		memset(input, 0, sizeof(input));

		commands.bg = false;
		commands.length = 0;

		/* Check for completed child processes */
		while (0 < (zombie = waitpid(0, NULL, WNOHANG))) {
			printf("%d done\n", (int) zombie);
		}
		fflush(stdout);

		if (NULL == getcwd(prompt, 1024)) {
			/* Ignore the error and continue;
			 * if the path is greater than 1024 characters
			 * then it probably doesn't fare well from being
			 * used as a prompt anyway. */
		}
		substitute_home(prompt);
		strcat(prompt, " ¥ ");

		/* tmp is allocated in readline and it's the callee's (our)
		 * obligation to free it. */
		tmp = readline(prompt);
		/* On e.g. Ctrl-D the input is null and the shell is exited */
		if (!tmp) break;

		/* ENTERING CRITICAL AREA */
		TRY_OR_EXIT(sighold(SIGINT), "sighold");

		/* parse_commands modifies input - copy and save for adding to history */
		strcpy(input, tmp);
		free(tmp);

		if (*input) {
			/* Add command line history for the user's convenience */
			add_history(input);
		}

		/* 2. Parse arguments into commands. */
		parse_commands(&commands, input);

		if (0 == commands.length) {
			free(commands.cmds);
			/* For some reason an empty command was received. */
			continue;
		}

		gettimeofday(&before, NULL);
		exec(&commands);
		/* EXITING CRITICAL AREA */
		TRY_OR_EXIT(sigrelse(SIGINT), "sigrelse");

		if (fg_process) {
			int status;
			uint64_t time_taken;

			TRY_OR_EXIT(sighold(SIGCHLD), "sighold");

			/* Wait for foreground process */
			while (-1 != waitpid(pid, &status, 0));
			if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
				/* An error occurred during the execution.
				 * Do not print the time it took to run the command. */
				continue;
			}
			gettimeofday(&after, NULL);

			TRY_OR_EXIT(sigrelse(SIGCHLD), "sigrelse");

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

void exec(CommandList *commands) {
	fg_process = !commands->bg;

	if (1 == commands->length) {
		if (EXIT_SUCCESS != exec_cmd(commands->cmds[0])) {
			/* Execute of command failed */
			fg_process = false;
		}
	} else {
		size_t i;
		int ret;
		/* Commands were piped, handle it accordingly.
		 *
		 * To prevent the signal handler from registering
		 * when commands in the pipeline finishes, the SIGCHLD
		 * signal is masked during execution.
		 *
		 * The process is forked to allow for commands like
		 * `sleep 10 | ls | sort` to both be run in the foreground
		 * and suspend prompt until finished, and be run in the
		 * background and immediately return the prompt to the
		 * user.
		 */
		TRY_OR_EXIT(sighold(SIGCHLD), "sighold");
		switch (pid = fork()) {
			case -1:
				/* Skip the execution of a command and
				 * try again at the next prompt. */
				perror("fork");
				fg_process = false;
				break;
			case 0:
				ret = exec_commands(commands, 0, STDIN_FILENO);
				free(commands->cmds);
				while (-1 != waitpid(-getpgid(pid), NULL, 0)) {
					exit(EXIT_FAILURE);
				}
				exit(ret);
			default:
				pid = -getpgid(pid);
				for (i = 0; i < commands->length; i++) {
					free(commands->cmds[i]->args);
					free(commands->cmds[i]);
				}
		}
		TRY_OR_EXIT(sigrelse(SIGCHLD), "sigrelse");
	}

	free(commands->cmds);
}

void parse_commands(CommandList *commands, char *input) {
	/* Split the inputs into commands by using the pipeline as a deliminator */
	const char *pipe_delim = "|", *delim = " ";
	char *save_pipe_ptr, *save_space_ptr;
	char *cmd_str = strtok_r(input, pipe_delim, &save_pipe_ptr);

	size_t cmds_buf_len = 2;
	commands->cmds = calloc(cmds_buf_len, sizeof(*commands->cmds));

	while (NULL != cmd_str) {
		/* Split the command into tokens by using space as a deliminator */
		char *arg_str = strtok_r(cmd_str, delim, &save_space_ptr);
		size_t args_buf_len = 3;

		/* The callee should free this after processing the command */
		Command *command = malloc(sizeof(*command));
		command->num_args = 0;
		command->args = calloc(args_buf_len, sizeof(*command->args));

		/* Adds all the tokens to the command arguments, including the command itself */
		while (NULL != arg_str) {
			/* If a previous command indicated bg it indicates a parse error.
			 * Only the last command can have & as an indicator. */
			if (commands->bg) {
				size_t i;
				for (i = 0; i < commands->length; i++) {
					free(commands->cmds[i]->args);
					free(commands->cmds[i]);
				}
				free(command->args);
				free(command);

				commands->length = 0;
				/* If '&' already was seen then it's not the last symbol */
				fprintf(stderr, SMSH ": unexpected token '&'\n");
				return;
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
}

int exec_cmd(Command *command) {
	/* Check for command in builtins first.
	 * If it does not exist there then assume it's an existing command. */
	int i;
	for (i = 0; i < NUM_BUILTINS; i++) {
		if (0 == strcmp(command->args[0], builtins[i])) {
			int ret = (*builtins_funcs[i])(command->args);
			free(command->args);
			free(command);
			return ret;
		}
	}

	/* Fork the process and execute the command on the child process */
	TRY_OR_EXIT(pid = fork(), "fork");

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
	free(command->args);
	free(command);
	exit(EXIT_FAILURE);
}

int exec_commands(CommandList *commands, const size_t cmd_index, const int fd_in) {
	Pipe pipefd;
	int ret, status;
	pid_t this_pid;

	if (cmd_index == commands->length - 1) {
		/* The last command in the pipeline is special cased as it
		 * shouldn't call itself recursively and additionally not
		 * redirect STDOUT. */
		size_t i;
		TRY(pid = fork(), "fork");
		if (0 == pid) {
			/* Redirect the previous command's pipe to this
			 * command's STDIN. */
			TRY(dup2(fd_in, STDIN_FILENO), "dup2");
			TRY(close(fd_in), "previous FD");
			/* Hard code support for the `pager` command in pipes */
			if (0 == strcmp(commands->cmds[cmd_index]->args[0], "pager")) {
				const char *pager = getenv("PAGER");
				/* If the PAGER environment variable contains something,
				 * that command is tried first. */
				if (pager) {
					if (-1 == execlp(pager, pager, (char *) NULL)) {
						perror(SMSH);
					}
				}
				/* If that fails, less is tried */
				if (-1 == execlp("less", "less", (char *) NULL)) {
					perror(SMSH);
				}
				/* Finally, more is tried */
				if (-1 == execlp("more", "more", (char *) NULL)) {
					perror(SMSH);
				}
				/* If everything fails then exit the child with EXIT_FAILURE */
				free(commands->cmds);
				exit(EXIT_FAILURE);
			}
			return run_cmd(commands->cmds[cmd_index]);
		}

		/* Close the file descriptor from the
		 * previous command's pipe. */
		TRY(close(fd_in), "previous FD");

		/* Free the commands as they are no longer needed */
		for (i = 0; i < commands->length; i++) {
			free(commands->cmds[i]->args);
			free(commands->cmds[i]);
		}
		return EXIT_SUCCESS;
	}

	/* The following code is for the general recursion,
	 * i.e. first to second to last commands. */

	TRY(pipe(pipefd), "pipe");
	TRY(pid = fork(), "fork");

	if (0 == pid) {
		/* fd_in is STDIN for the very first command */
		if (fd_in != STDIN_FILENO) {
			/* Redirect input pipes */
			TRY(dup2(fd_in, STDIN_FILENO), "dup2");
			TRY(close(fd_in), "previous FD");
		}

		/* Redirect the output pipes */
		TRY(dup2(pipefd[PIPE_WRITE_SIDE], STDOUT_FILENO), "dup2");
		TRY(close(pipefd[PIPE_WRITE_SIDE]), "pipe write");
		TRY(close(pipefd[PIPE_READ_SIDE]), "pipe read");

		return run_cmd(commands->cmds[cmd_index]);
	}

	/* Close the pipes and recurse */
	TRY(close(pipefd[PIPE_WRITE_SIDE]), "pipe write");
	if (fd_in != STDIN_FILENO) {
		TRY(close(fd_in), "previous FD");
	}

	this_pid = pid;
	ret = exec_commands(commands, cmd_index + 1, pipefd[PIPE_READ_SIDE]);
	if (-1 != waitpid(this_pid, &status, 0) &&
			(!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS)) {
		/* The process was exited with an error (or not at all). */
		return EXIT_FAILURE;
	}
	return ret;
}

/* The built-in exit command */
int exit_cmd(char **args) {
	(void) args; /* Workaround for unused variable */
#if SIGDET
	/* "If the action for the SIGCHLD signal is set to SIG_IGN,
	 * child processes of the calling processes shall
	 * not be transformed into zombie processes when they terminate."
	 */
	if (SIG_ERR == signal(SIGCHLD, SIG_IGN)) {
		perror("signal");
		exit(EXIT_FAILURE);
	}
#endif

	/* Ignore SIGTERM in parent and send it to all child processes */
	if (SIG_ERR == signal(SIGTERM, SIG_IGN)) {
		perror("signal");
		exit(EXIT_FAILURE);
	}
	if (-1 == kill(0, SIGTERM)) {
		perror("kill");
		exit(EXIT_FAILURE);
	}

#if !SIGDET
	/* Poll and wait for child processes to finish */
	while (-1 != waitpid(0, NULL, 0));
#endif

	exit(EXIT_SUCCESS);
}

static int cd(const char *dir) {
	if (0 != chdir(dir)) {
		perror("cd");
	}
	/* This is a workaround to prevent
	 * the running time for the cd command
	 * to print on completion. */
	return EXIT_FAILURE;
}

/* The built-in cd command */
int cd_cmd(char **args) {
	/* For simplicity's sake, the directory is expected
	 * to never be larger than 1024 characters.
	 *
	 * Additionally, $HOME is assumed to always be set
	 * for the same reason. If it's undefined there's
	 * nothing to do anyway. */
	char dir[1024], *tmp;
	memset(dir, 0, sizeof(dir));

	if (!args[1]) {
		/* 0 arguments given. */
		return cd(getenv("HOME"));
	}

	if (args[2]) {
		/* 2 (or more) arguments given. */
		fprintf(stderr, "cd: only one argument is supported.\n");
		return EXIT_FAILURE;
	}

	/* One argument was given; the directory.
	 * First, perform substitution on ~, should it exist. */
	tmp = args[1];
	if (tmp[0] == '~') {
		strcpy(dir, getenv("HOME"));
		/* Copy everything after ~ to the dir. */
		strcat(dir, &tmp[1]);
	} else {
		strcat(dir, tmp);
	}

	return cd(dir);
}

/* Used for creating commands in checkEnv to be passed into
 * exec. */
#define CREATE_COMMAND(cmd) \
cmd = malloc(sizeof(*cmd)); \
cmd->num_args = 1; \
cmd->args = calloc(2, sizeof(*cmd->args)); \
cmd->args[0] = (char *) cmd ## _ ## s; \
commands.cmds[commands.length++] = cmd;

static const char *printenv_s = "printenv";
static const char *sort_s = "sort";
static const char *pager_s = "pager";
static const char *grep_s = "grep";

/* The built-in checkEnv command */
int checkEnv_cmd(char **args) {
	CommandList commands;
	Command *printenv, *grep, *sort, *pager;

	commands.bg = false;
	commands.length = 0;
	commands.cmds = calloc(args[1] ? 4 : 3, sizeof(*commands.cmds));

	CREATE_COMMAND(printenv);

	/* If an argument is passed to checkEnv, pipe printenv into
	 * grep with the supplied arguments. */
	if (args[1]) {
		size_t i;
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
		commands.cmds[commands.length++] = grep;
	}

	CREATE_COMMAND(sort);
	CREATE_COMMAND(pager);

	exec(&commands);
	return EXIT_SUCCESS;
}

/* Helper function when creating the prompt */
void substitute_home(char *dst) {
	char *tmp = getenv("HOME");
	size_t i, len = strlen(tmp);

	/* Check if first part of dst equals $HOME. */
	for (i = 0; i < len; i++) {
		if (!tmp[i] || !dst[i] || tmp[i] != dst[i]) {
			return;
		}
	}

	/* Remove traces of $HOME path
	 * and replace with ~. */
	dst[0] = '~';
	/* Move everything after $HOME to after ~,
	 * and clear its memory. */
	for (i = 1; i < len || dst[len + i - 1]; i++) {
		dst[i] = dst[len + i - 1];
		dst[len + i - 1] = 0;
	}
}

/* The function handling the two signals that
 * are caught by the program: SIGINT and SIGCHLD. */
void signal_handler(int sig) {
	switch (sig) {
		case SIGTERM:
			/* If the pid is greater than 0 then it wasn't a
			 * termination of a process group and therefore of a single
			 * command. Because it is we can exit safely and not have to
			 * worry about the parent process getting killed. */
			if (pid > 0) {
				exit(EXIT_SUCCESS);
			}
			break;
		case SIGINT:
			/* Only kill if child and fg process */
			if (fg_process && -1 != pid) {
				if (-1 == kill(pid, SIGTERM)) {
					/* Child couldn't be killed, but perror can't be used here
					 * because it is not safe for use in a signal handler.
					 * The error is purposefully ignored. */
					return;
				}
				if (-1 == waitpid(pid, NULL, 0)) {
					/* This probably means that the child for some reason
					 * already has been waited for.
					 * There's nothing that can be done and therefore
					 * the error is ignored. */
				}
			}
			break;
		case SIGCHLD:
			/* This will only run when SIGDET=1. */
			/* Previously, the terminated background processes were
			 * printed here. However, because printf is not safe for use in a signal
			 * handler, it was updated to jump to the prompt instead. */
			if (fg_process) {
				return;
			}
			break;
		default: return;
	}
	/* Print a newline before the new prompt so that it doesn't
	 * show on the same line as the previous prompt that was jumped from */
	if (-1 == write(STDOUT_FILENO, "\n", 1)) {
		/* The newline couldn't be printed.
		 * Like above, perror can't be used and because the newline
		 * isn't vital this error is purposefully ignored. */
	}
	/* Jump back to prompt */
	siglongjmp(prompt_mark, 1);
}
