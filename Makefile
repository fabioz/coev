CFLAGS=-fwrapv -O2 -fno-strict-aliasing -Wstrict-prototypes -g -Wall -fPIC
PREFIX?=~/prefix

#CFLAGS+=-pthread -DTHREADING_MADNESS

all: libcoev.so

libcoev.so: coev.c coev.h
	gcc ${CFLAGS} -c coev.c 
	gcc -shared -Wl,-soname,libcoev.so -o libcoev.so coev.o -lev -lc

clean:
	rm -f libcoev.so coev.o

install: clean all
	install -D libcoev.so ${PREFIX}/lib
	install -D coev.h ${PREFIX}/include
