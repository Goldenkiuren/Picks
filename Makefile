#flags de compilação
CC=gcc
CFLAGS=-pthread

all: servidor cliente

servidor: servidor.c
	$(CC) $(CFLAGS) servidor.c -o servidor

cliente: cliente.c
	$(CC) $(CFLAGS) cliente.c -o cliente

clean:
	rm -f servidor cliente