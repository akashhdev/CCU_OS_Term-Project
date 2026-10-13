CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDLIBS  += -lpthread -lm

all: pdm_bench

pdm_bench: pdm_bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f pdm_bench

.PHONY: all clean
