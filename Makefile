CC     ?= cc
CFLAGS ?= -Wall -Wextra -O2

.PHONY: all clean

all: ephemeral_ports

ephemeral_ports: ephemeral_ports.c
	$(CC) $(CFLAGS) -o ephemeral_ports ephemeral_ports.c

clean:
	rm -f ephemeral_ports
