MAKEFLAGS += -r # No builtin rules
.DELETE_ON_ERROR: # Delete failed targets
.SECONDARY: # Do not delete intermediate files

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
