CFLAGS=-fwrapv -O2 -fno-strict-aliasing -Wstrict-prototypes -g -Wall -fPIC 

#PREFIX?=/usr

#CFLAGS+=-pthread -DTHREADING_MADNESS

SONAME=libucoev.so

all: ${SONAME}

${SONAME}: ucoev.c ucoev.h
	gcc ${CFLAGS} -c ucoev.c 
	gcc -shared -Wl,-soname,${SONAME} -Wl,-R${PREFIX}/lib -o ${SONAME} ucoev.o -lev -lc

clean:
	rm -f ${SONAME} *.o

pfxinst: clean all
	install -D ${SONAME} ${PREFIX}/lib/${SONAME}
	install -D -m 644 ucoev.h ${PREFIX}/include/ucoev.h

install: clean all
	install -D ${SONAME} ${DESTDIR}/usr/lib/${SONAME}
	install -D ucoev.h ${DESTDIR}/usr/include/ucoev.h

