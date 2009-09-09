#!/usr/bin/env python

import ez_setup
ez_setup.use_setuptools()

from setuptools import setup, Extension

VERSION = '0.3'
DESCRIPTION = 'libcoev bindings - I/O-scheduled coroutines'
LONG_DESCRIPTION = """
    This is one half of py.magic.greenlet module fork/split.
    (<http://codespeak.net/py/>)

    The "greenlet" package is a spin-off of Stackless, a version of CPython that
    supports micro-threads called "tasklets".  Tasklets run pseudo-concurrently
    (typically in a single or a few OS-level threads) and are synchronized with
    data exchanges on "channels".

    A "greenlet", on the other hand, is a still more primitive notion of
    micro-thread with no implicit scheduling; coroutines, in other words.

    Greenlets are provided as a C extension module for the regular unmodified
    interpreter.

    libcoev and the present module are the results of separating coroutine 
    implementation in C from the python interface, and then slapping an 
    libev-based scheduler on top. 
"""

CLASSIFIERS = filter(None, map(str.strip,
"""                 
Intended Audience :: Developers
License :: OSI Approved :: MIT License
Natural Language :: English
Programming Language :: Python
Operating System :: OS Independent
Topic :: Software Development :: Libraries :: Python Modules
""".splitlines()))

REPOSITORY="http://coev.lxnt.info/"

daext = Extension(
    name='coev', 
    sources=['modcoev.c'], 
    undef_macros=['NDEBUG'],
    libraries=['coev'])

setup(
    name="coev",
    version=VERSION,
    description=DESCRIPTION,
    long_description=LONG_DESCRIPTION,
    classifiers=CLASSIFIERS,
    maintainer="Alexander Sabourenkov",
    maintainer_email="screwdriver@lxnt.info",
    url=REPOSITORY,
    license="MIT License",
    platforms=['any'],
    test_suite='nose.collector',
    download_url=REPOSITORY,
    ext_modules=[daext],
)
