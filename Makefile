all: converter

OBJS = convert.o

CC=/opt/local/bin/clang-mp-3.2
LD=$(CC)
CFLAGS=-I/opt/local/libexec/llvm-3.2/include
LDFLAGS=-L/opt/local/libexec/llvm-3.2/lib
LIBS=-lclang

clean:
	rm -f converter $(OBJS)
	rm -f unit.c.c unit2.c.c

test1: converter
	$(CC) -E unit.c -o unit.c.c
	./converter unit.c.c

test2: converter
	$(CC) -E unit2.c -o unit2.c.c
	./converter unit2.c.c

test3: converter
	$(CC) $(CFLAGS) -E -o convert.c.c convert.c
	./converter convert.c.c

converter: $(OBJS)
	$(LD) -o $@ $< $(LDFLAGS) $(LIBS)

convert.o: convert.c
	$(CC) $(CFLAGS) -o $@ -c $<
