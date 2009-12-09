#!/bin/sh

if [ -z "${PREFIX}" ]
then
    echo "please setenv PREFIX"
    exit 0
fi

mkdir -p $PREFIX || exit 1

( cd libucoev && make pfxinst ) || exit 1
( cd python2.6 && ./build.sh ) || exit 1
[ -f ez_setup.py ] || wget http://peak.telecommunity.com/dist/ez_setup.py || exit 1
$PREFIX/bin/python ez_setup.py || exit 1
( cd python-coev && rm -rf build && ./build.sh ) || exit 1
( cd python-coewsgi && ${PREFIX}/bin/python setup.py install ) || exit 1
( cd python-psycoev && rm -rf build && ./build.sh ) || exit 1
( cd python-evmemcached && ${PREFIX}/bin/python setup.py install ) || exit 1

# this depends on Pylons which depends on easyinstall which depends on python
# which we just installed so ... the commands below will pull Paste, Pylons
# and the rest of stuff needed for a Pylons project
#
#( cd examples/pylons && ${PREFIX}/bin/python setup.py install ) || exit 1

echo
echo All rebuilt and reinstalled.
echo

