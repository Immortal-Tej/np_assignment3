CC = g++ -Wextra -O2 -std=c++17
CC_FLAGS = -w -g




all: test client server


main_curses.o: main_curses.c
	$(CC) -Wall -I. -c main_curses.c


test: main_curses.o
	$(CC) -I./ -Wall main_curses.o -lncurses  -o test 


client: client.o
	$(CC) -Wall -o cchat client.o

server: server.o
	$(CC) -Wall -o cserverd server.o


clean:
	rm *.o *.a test cserverd cchat
