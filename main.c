#define _POSIX_SOURCE (200809L)
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
int exec_commands(CommandList *, const uint32_t, const int);
int run_cmd(Command *);
int exit_cmd(char **);
int cd_cmd(char **);
int checkEnv_cmd(char **);

/*
 * 1. Read input.
 * 2. Split into arguments and create CommandList struct.
 * 3. Execute each command and pipe if more than one in the background if bg or foreground otherwise.
 *
 * Make sure child processes are killed when parent is by registering signal handlers.
 */
sigjmp_buf prompt_mark;
pid_t pid;

void signal_handler(int sig) {
	pid_t zombie;
	switch (sig) {
		case SIGINT:
			/* Only kill if child */
			if (0 != pid && -1 == kill(pid, SIGKILL)) {
				perror("kill");
			}
			printf("\n");
			break;
		case SIGCHLD:
			while (0 < (zombie = waitpid(0, NULL, WNOHANG))) {
				printf("%d done\n", (int) zombie);
			}
			break;
		default: return;
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
	if (-1 == sigaction(SIGCHLD, &sa, NULL)) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}
#endif
	if (-1 == sigaction(SIGINT, &sa, NULL)) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}

	/* Set prompt mark here for jumping to from the signal handler */
	while (0 != sigsetjmp(prompt_mark, 1));

	for (;;) {
		char prompt[90];
		char *input;
		char history[1024];
		char *path = getcwd(NULL, 80);
		CommandList *commands;
		struct timeval before, after;

#if !SIGDET
		/* Check for completed child processes */
		pid_t zombie;
		while (0 < (zombie = waitpid(0, NULL, WNOHANG))) {
			printf("%d done\n", (int) zombie);
		}
		fflush(stdout);
#endif

		strcpy(prompt, path);
		strcat(prompt, " ¥ ");

		/* input is allocated in readline.
		 * it's the callee's (our) obligation to free it */
		input = readline(prompt);
		free(path);
		if (!input) break;
		/* parse_commands modifies input - copy and save for adding to history */
		strcpy(history, input);

		/* 2. Parse arguments into commands. */
		commands = parse_commands(input);

		if (!commands) {
			free(input);
			continue;
		}
		if (0 == commands->length) {
			goto next;
		}

		add_history(history);

		gettimeofday(&before, NULL);
		if (1 == commands->length) {
			if (EXIT_SUCCESS != exec_cmd(commands->cmds[0])) {
				/* Execute failed */
				goto next;
			}
		} else {
			/* Commands were piped, handle it accordingly */
			exec_commands(commands, 0, STDIN_FILENO);
		}

		if (!commands->bg) {
			uint64_t time_taken;

#if SIGDET
			void (*sighandler)(int) = signal(SIGCHLD, SIG_IGN);
#endif

			/* Wait for foreground process(es) */
			while (-1 != waitpid(-1, NULL, 0) || ECHILD != errno);
			gettimeofday(&after, NULL);

			time_taken = 1000 * (after.tv_sec - before.tv_sec) +
				(after.tv_usec - before.tv_usec) / 1000;
			printf("%" PRIu64 " ms\n", time_taken);

#if SIGDET
			signal(SIGCHLD, sighandler);
#endif
		}

next:
		/* Clear pid (only used by foreground processes) */
		pid = 0;
		free(commands->cmds);
		free(commands);
		free(input);
	}

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
#define PIPE_READ_SIDE (0)
#define PIPE_WRITE_SIDE (1)
#define TRY(syscall, str) \
if (-1 == (syscall)) { \
	perror(str); \
	return EXIT_FAILURE; \
}
typedef int Pipe[2];

int exec_cmd(Command *command) {
	/* Check for command in builtins first.
	 * If it does not exist there then assume it's an existing command. */
	int i;
	for (i = 0; i < NUM_BUILTINS; i++) {
		if (0 == strcmp(command->bin, builtins[i])) {
			int ret_val = (*builtins_funcs[i])(command->args);
			free(command->args);
			free(command);
			return ret_val;
		}
	}

	/* Fork the process and execute the command on the child process */
	pid = fork();
	if (-1 == pid) { /* Error handling */
		perror("fork");
		return EXIT_FAILURE;
	}

	if (0 == pid) { /* Start execution as child */
		return run_cmd(command);
	}

	/* Continue execution as parent */
	free(command->args);
	free(command);
	return EXIT_SUCCESS;
}

int run_cmd(Command *command) {
	execvp(command->bin, command->args);
	/* If we end up here an error has occurred */
	perror(SMSH);
	free(command->args);
	free(command);
	return EXIT_FAILURE;
}


int exec_commands(CommandList *commands, const uint32_t cmd_index, const int fd_in) {
	Pipe pipefd;

	if (cmd_index == commands->length - 1) {
		uint32_t i;
		TRY(pid = fork(), "fork");
		if (0 == pid) {
			TRY(dup2(fd_in, STDIN_FILENO), "dup2");
			TRY(close(fd_in), "previous FD");
			/* Hack for making pager work */
			if (0 == strcmp(commands->cmds[cmd_index]->bin, "pager")) {
				char *pager = getenv("PAGER");
				if (NULL != pager) {
					execlp(pager, pager, (char *) NULL);
					perror(SMSH);
				}
				execlp("less", "less", (char *) NULL);
				perror(SMSH);
				execlp("more", "more", (char *) NULL);
				perror(SMSH);
				free(pager);
				return EXIT_FAILURE;
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

/* Built in commands */
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
	kill(0, SIGTERM);

#if !SIGDET
	/* Poll and wait for child processes to finish */
	while (-1 != waitpid(-1, NULL, 0));
#endif

	exit(EXIT_SUCCESS);
}

int cd_cmd(char **args) {
	const char *dir = getenv("HOME");
	if (args[1]) {
		dir = args[1];
	}
	if (0 != chdir(dir)) {
		perror("cd");
	}
	return EXIT_SUCCESS;
}

#define CREATE_COMMAND(cmd) \
cmd = malloc(sizeof(*cmd)); \
cmd->num_args = 1; \
cmd->bin = cmd ## _ ## s; \
cmd->args = malloc(sizeof(*cmd->args)); \
cmd->args[0] = cmd ## _ ## s; \
command_list->cmds[command_list->length++] = cmd;

int checkEnv_cmd(char **args) {
	int ret_val;
	CommandList *command_list;
	Command *printenv, *grep, *sort, *pager;

	char *printenv_s = "printenv";
	char *sort_s = "sort";
	char *pager_s = "pager";

	command_list = malloc(sizeof(*command_list));
	command_list->bg = false;
	command_list->length = 0;

	command_list->cmds = calloc(args[1] ? 4 : 3, sizeof(*command_list->cmds));

	CREATE_COMMAND(printenv);

	if (args[1]) {
		char *grep_s = "grep";
		args[0] = grep_s;
		grep = malloc(sizeof(*grep));
		while (args[grep->num_args - 1]) {
			grep->num_args++;
		}
		grep->bin = grep_s;
		grep->args = args;
		command_list->cmds[command_list->length++] = grep;
	}

	CREATE_COMMAND(sort);
	CREATE_COMMAND(pager);

	ret_val = exec_commands(command_list, 0, STDIN_FILENO);
	free(command_list->cmds);
	free(command_list);
	return ret_val;
}
