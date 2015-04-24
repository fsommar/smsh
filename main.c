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

extern char *strtok_r(char *, const char *, char **);
#define SMSH ("smsh")
#define SIGDET 1

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
int exec_commands(const CommandList *, const uint32_t);
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
static struct timeval fg_time;
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
			while (0 < (zombie = waitpid(-1, NULL, WNOHANG))) {
				fprintf(stderr, "%d done\n", (int) zombie);
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
		const char *path = getcwd(NULL, 80);
		CommandList *commands;

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

		add_history(history);

		if (commands->length == 1) {
			if (EXIT_SUCCESS != exec_cmd(commands->cmds[0])) {
				/* Execute failed */
				continue;
			}
		} else {
			/* Commands were piped, handle it accordingly */
			exec_commands(commands, 0);
		}

		if (!commands->bg) {
			struct timeval cur;
			uint64_t time_taken;
			int status;

#if SIGDET
			void (*sighandler)(int) = signal(SIGCHLD, SIG_IGN);
#endif

			/* Wait for foreground process */
			wait(&status);

#if SIGDET
			signal(SIGCHLD, sighandler);
#endif

			gettimeofday(&cur, NULL);

			time_taken = 1000 * (cur.tv_sec - fg_time.tv_sec) + (cur.tv_usec - fg_time.tv_usec) / 1000;
			printf("%llu ms\n", time_taken);

			/* TODO: Handle freeing for background as well */
			free(commands->cmds);
			free(commands);
		}
		/* Clear pid (only used by foreground processes) */
		pid = 0;

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
typedef int Pipe[2];

int exec_cmd(Command *command) {
	int i;

	/* Check for command in builtins first.
	 * If it does not exist there then assume it's an existing command. */
	for (i = 0; i < NUM_BUILTINS; i++) {
		if (0 == strcmp(command->bin, builtins[i])) {
			gettimeofday(&fg_time, NULL);
			return (*builtins_funcs[i])(command->args);
		}
	}

	/* Fork the process and execute the command on the child process */
	pid = fork();
	if (0 == pid) { /* Start execution as child */
		execvp(command->bin, command->args);
		perror(SMSH);
		return EXIT_FAILURE;
	} else if (-1 == pid) { /* Error handling */
		perror("fork");
		return EXIT_FAILURE;
	} else { /* Continue execution as parent */
		gettimeofday(&fg_time, NULL);
		free(command->args);
		free(command);
		return EXIT_SUCCESS;
	}
}

int exec_commands(const CommandList *commands, const uint32_t cmd_index) {
	pid_t pid;
	Pipe pipefd;
	int status, ret_val;

	/* Create a new pipe for every command except first and last */
	if (cmd_index != 0 || !(cmd_index >= commands->length)) {
		if (-1 == pipe(pipefd)) {
			perror("pipe");
			return EXIT_FAILURE;
		}
	}

	/* Fork every process except for the last */
	if (!(cmd_index >= commands->length)) {
		pid = fork();
		if (-1 == pid) {
			perror("fork");
			return EXIT_FAILURE;
		}
	} else { /* Last process should write to STDOUT */
		return execvp(commands->cmds[cmd_index - 1]->bin, commands->cmds[cmd_index - 1]->args);
	}

	if (0 == pid) { /* Start execution as child */

		if (0 != cmd_index) { /* All but the first child reads from pipe */
			ret_val = dup2(pipefd[PIPE_READ_SIDE], STDIN_FILENO);

			if (-1 == ret_val) {
				perror("dup2");
				return EXIT_FAILURE;
			}
			/* Pipe closing failures */
			if (-1 == close(pipefd[PIPE_READ_SIDE])) {
				perror("close pipe read side");
				return EXIT_FAILURE;
			}
			if (-1 == close(pipefd[PIPE_WRITE_SIDE])) {
				perror("close pipe write side");
				return EXIT_FAILURE;
			}
		}

		ret_val = exec_commands(commands, cmd_index + 1);
		wait(&status);
		return ret_val;
	}
	/* Continue execution as parent */

	if (0 != cmd_index) { /* All but the first parent writes to pipe */
		ret_val = dup2(pipefd[PIPE_WRITE_SIDE], STDOUT_FILENO);

		if (-1 == ret_val) {
			perror("dup2");
			return EXIT_FAILURE;
		}
		/* Pipe closing failures */
		if (-1 == close(pipefd[PIPE_READ_SIDE])) {
			perror("close pipe read side");
			return EXIT_FAILURE;
		}
		if (-1 == close(pipefd[PIPE_WRITE_SIDE])) {
			perror("close pipe write side");
			return EXIT_FAILURE;
		}
	}

	if (0 != cmd_index) { /* All but the first parent executes a process */
		return execvp(commands->cmds[cmd_index - 1]->bin, commands->cmds[cmd_index - 1]->args);
	} else {
		wait(&status);
		return EXIT_SUCCESS;
	}
}

/* Built in commands */
int exit_cmd(char **args) {
#if SIGDET
	/* "If the action for the SIGCHLD signal is set to SIG_IGN,
	 * child processes of the calling processes shall not be transformed into zombie processes when they terminate."
	 */
	signal(SIGCHLD, SIG_IGN);
#endif

	/* Ignore SIGTERM in parent and send it to all child processes */
	signal(SIGTERM, SIG_IGN);
	kill(0, SIGTERM);

#if !SIGDET
	/* Poll and wait for child processes to finish */
	while (-1 != wait(NULL));
#endif

	exit(EXIT_SUCCESS);
}

int cd_cmd(char **args) {
	char *dir = getenv("HOME");
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
