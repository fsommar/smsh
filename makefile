main: main.c
	gcc -o main -pedantic -Wall -ansi -O4 main.c -lreadline

run: main
	./main
