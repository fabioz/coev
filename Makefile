all: libcoev.so
CFLAGS=-fwrapv -O2 -fno-strict-aliasing -Wstrict-prototypes -g -Wall -fPIC

#CFLAGS+=-pthread -DTHREADING_MADNESS


libcoev.so: coev.c coev.h
	gcc ${CFLAGS} -c coev.c 
	gcc -shared -Wl,-soname,libcoev.so -o libcoev.so coev.o -lev -lc

clean:
	rm -f libcoev.so coev.o

install: clean all
	mkdir -p /home/lxnt/prefix/lib /home/lxnt/prefix/include
	cp libcoev.so /home/lxnt/prefix/lib
	cp coev.h /home/lxnt/prefix/include
