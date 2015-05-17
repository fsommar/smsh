SIGDET="-D SIGDET"

main: main.c main.h
	gcc -o main $(SIGDET) -pedantic -Wall -Wextra -Weverything -std=c89 -O4 -g main.c -lreadline -ltermcap

run: main
	@./main

clean:
	-rm main
