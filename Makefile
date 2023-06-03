include suckless.mk

CC ?= cc
CFLAGS = -Wall -Wextra -pedantic -std=c99
LDFLAGS =
LDLIBS =

all: ramon .ramon_setcap

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

%: %.o
	$(CC) $(LDFLAGS) $< $(LDLIBS) -o $@

.ramon_setcap: ramon
	sudo setcap cap_dac_override+eip ramon
	@touch $@

clean:
	rm -f ramon
	rm -f *.o

re:
	$(MAKE) clean
	$(MAKE)
