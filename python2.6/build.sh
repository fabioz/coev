#!/bin/sh

PREFIX=${PREFIX:=~/prefix}

export CPPFLAGS="-I${PREFIX}/include -DUCOEV_THREADS"
export LDFLAGS="-L/${PREFIX}/lib -Wl,-rpath=${PREFIX}/lib"
export LIBS=-lucoev

make clean 
./configure --prefix=${PREFIX} && make install

