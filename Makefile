CC ?= cc
CFLAGS ?= -Wall -Wextra
LDLIBS ?= -pthread

.PHONY: all clean

all: server

server: server.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	$(RM) server
