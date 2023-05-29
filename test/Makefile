include ../suckless.mk

CC ?= cc
CFLAGS = -Wall -Wextra -pedantic
LDFLAGS =
LDLIBS =

TARGETS=$(patsubst %.c,%.run,$(wildcard *.c))
all: $(TARGETS)

%.run: %.exe
	../ramon ./$<

%.exe: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

clean:
	rm -f *.exe

re:
	$(MAKE) clean
	$(MAKE)
