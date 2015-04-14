main: main.c
	gcc -o main -pedantic -Wall -Wextra -std=c89 -O4 -g main.c -lreadline

run: main
	@./main

clean:
	-rm main
