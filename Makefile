include suckless.mk

CC ?= cc
CFLAGS = -Wall -Wextra -pedantic
LDFLAGS =
LDLIBS =

all: ramon

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

%: %.o
	$(CC) $(LDFLAGS) $< $(LDLIBS) -o $@

clean:
	rm -f ramon
	rm -f *.o

re:
	$(MAKE) clean
	$(MAKE)
