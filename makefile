SIGDET="-D SIGDET"

main: main.c
	gcc -o main $(SIGDET) -pedantic -Wall -Wextra -std=c89 -O4 -g main.c -lreadline -ltermcap

run: main
	@./main

clean:
	-rm main
