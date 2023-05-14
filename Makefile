all: ramon

ramon: ramon.o

clean:
	rm -f ramon
	rm -f *.o

re: clean all
