#include <stdlib.h>
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>

int main(void) {

	for (;;) {
		/* input is allocated in readline.
		 * it's the callee's obligation to free it */
		char * input = readline("smsh ¥ ");
		if (!input) break;

		printf("%s\n", input);

		add_history(input);

		free(input);
	}

	return 1;
}
