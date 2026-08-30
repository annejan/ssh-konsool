# Tests that run on a development machine, not on the badge.
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
          -fno-omit-frame-pointer -DHAVE_STRLCPY=0

.PHONY: all run clean
all: test_hosts test_font

test_hosts: test_hosts.c shims.c shims.h ../../main/hosts.c ../../main/hosts.h
	$(CC) $(CFLAGS) -I. -o $@ test_hosts.c shims.c

test_font: test_font.c pax_types.h ../../main/terminal_font.c ../../main/terminal_font.h
	$(CC) $(CFLAGS) -I. -o $@ test_font.c ../../main/terminal_font.c

run: test_hosts test_font
	./test_hosts
	./test_font

clean:
	rm -f test_hosts test_font
