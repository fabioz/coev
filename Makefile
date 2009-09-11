CFLAGS=-fwrapv -O2 -fno-strict-aliasing -Wstrict-prototypes -g -Wall -fPIC
PREFIX?=~/prefix

#CFLAGS+=-pthread -DTHREADING_MADNESS

SONAME=libucoev.so

all: ${SONAME}

${SONAME}: ucoev.c ucoev.h
	gcc ${CFLAGS} -c ucoev.c 
	gcc -shared -Wl,-soname,${SONAME} -o ${SONAME} ucoev.o -lev -lc

clean:
	rm -f libcoev.so coev.o

install: clean all
	install -D ${SONAME} ${PREFIX}/lib/${SONAME}
	install -D ucoev.h ${PREFIX}/include/ucoev.h
