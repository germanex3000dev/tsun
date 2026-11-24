CFLAGS ?= -std=c11 -O2 -Wall -Wextra

all: tsun

tsun: tsun.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f tsun

.PHONY: all clean
