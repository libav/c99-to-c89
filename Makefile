all: converter

OBJS = convert.o

CC=/opt/local/bin/clang-mp-3.2
LD=$(CC)
CFLAGS=-I/opt/local/libexec/llvm-3.2/include -g
LDFLAGS=-L/opt/local/libexec/llvm-3.2/lib -g
LIBS=-lclang

clean:
	rm -f converter $(OBJS)
	rm -f unit.c.c unit2.c.c

test1: converter
	$(CC) -E unit.c -o unit.prev.c
	./converter unit.prev.c unit.post.c
	diff -u unit.{prev,post}.c

test2: converter
	$(CC) -E unit2.c -o unit2.prev.c
	./converter unit2.prev.c unit2.post.c
	diff -u unit2.{prev,post}.c

test3: converter
	$(CC) $(CFLAGS) -E -o convert.prev.c convert.c
	./converter convert.prev.c convert.post.c
	diff -u convert.{prev,post}.c

converter: $(OBJS)
	$(LD) -o $@ $< $(LDFLAGS) $(LIBS)

convert.o: convert.c
	$(CC) $(CFLAGS) -o $@ -c $<
