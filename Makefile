include suckless.mk

CC ?= cc
CFLAGS = -Wall -Wextra -pedantic -std=c99
LDFLAGS =
LDLIBS = -lrt

VERSION=$(shell git describe --dirty --tags HEAD || git rev-parse --short HEAD || echo v_unknown)
CFLAGS += -DRAMON_VERSION="\"$(VERSION)\""

all: ramon .ramon_setcap

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

%: %.o
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

ramon: ramon.o opts.o

.ramon_setcap: ramon
	sudo setcap cap_dac_override+eip ramon
	@touch $@

.PHONY: install
install:
	sudo install -t /usr/local/bin ramon ramon-render.py ramon-compare.py
	sudo setcap cap_dac_override+eip /usr/local/bin/ramon

clean:
	rm -f ramon
	rm -f *.o

re:
	$(MAKE) clean
	$(MAKE)
