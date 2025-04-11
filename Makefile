all: echo_server

main.o: main.c
	gcc -c -Wall -Wextra -Werror main.c -o main.o

echo_server: main.o
	gcc main.o -o echo_server

clean:
	rm -f main.o
	mr -f echo_server
