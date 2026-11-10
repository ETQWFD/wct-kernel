CC ?= gcc
CFLAGS ?= -O2 -Wall
all: wctetool
wctetool: wcte.c wcte.h wctetool.c
	$(CC) $(CFLAGS) -o $@ wcte.c wctetool.c
clean:
	rm -f wctetool *.wcte
