# dilcfg — DSD TECH diLogger config tool (Unix)
# Single-file C, libc + POSIX termios only. Linux, FreeBSD, OpenBSD.

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
EXTRA_CFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE

all: dilcfg

dilcfg: dilcfg.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o dilcfg dilcfg.c

clean:
	rm -f dilcfg
