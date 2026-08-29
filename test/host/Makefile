# Tests that run on a development machine, not on the badge.
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
          -fno-omit-frame-pointer -DHAVE_STRLCPY=0

.PHONY: all run clean
all: test_hosts

test_hosts: test_hosts.c shims.c shims.h ../../main/hosts.c ../../main/hosts.h
	$(CC) $(CFLAGS) -I. -o $@ test_hosts.c shims.c

run: test_hosts
	./test_hosts

clean:
	rm -f test_hosts
