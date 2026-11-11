CC     = gcc
CFLAGS = -Wall -Wextra -g

all: server client

server: server.c common.h
	$(CC) $(CFLAGS) -o server server.c

client: client.c common.h
	$(CC) $(CFLAGS) -o client client.c

test: all
	./test.sh

clean:
	rm -f server client os_server.log /tmp/os_server_fifo /tmp/os_*_fifo
