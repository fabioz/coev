# Synopsis #

coev framework is a stab on providing reasonably fast asynchronous IO to both Python code itself, and any C extension modules used.

# Introduction #

The coev library turned out to be quite similar to the GNU Pth.

It can be seen as a userland non-preemptively scheduled threading library.

However, this project's goal is to support fast and massively parallel IO in Python, while avoiding inversion of control flow issues of pure-python frameworks (like Twisted), and allow C extensions/modules access to the scheduling (unlike in twisted and greenlet/stackless), thus getting rid of arguably ugly and slow python wrappers around them (target being specifically DB-API modules).

Coev also provides means to pass values and inject exceptions on explicit switch between threads thus effectively making them coroutines, although not first-class ones (yet).

Python threads are replaced by the coroutines, retaining existing Python thread API. Locks and TLS are reimplemented to not require memory allocation after reaching a ceiling specific to a particular app/workload combination.

As of 06 November 2009 this is alpha-quality code, mostly feature-complete, but not optimized or cleaned up, and probably with lots of bugs.

As of 19 March 2010 this is beta-quality code, with feature set enough for my current needs, and bug count low enough to be put into production (which it was).

# Details #

The project right now consists of:
  * **libucoev** - pure-C library providing coroutine support, io-event-driven scheduler, and supporting code to satisfy CPython's threading model needs - locks and "thread"-local storage. Buffered recv/send support on top of core functionality also sits here.
  * **python2.6** - patched CPython 2.6. Modifications are minimal, consisting of a new thread-model header and support for return value of a coroutine in `threadmodule.c` (module `_thread`).
  * **python-coev** - python bindings for the rest of libucoev functionality.
  * **psycoev** - psycopg2 rewritten to use the libucoev. Support for LOBs is not avaliable (yet).
  * **python-evmemcached** - memcached client ported to the resulting framework.
  * **python-coewsgi** - http/wsgi gateway from the Paste project, ported to the framework. Performance is not as good as Twisted/Tornado yet, but approaches it.
  * **examples/pylons** - an empty Pylons project, with all relevant bits attached: coewsgi as server, sqlalchemy with psycoev as DB-API module, debug settings exposed in the ini file.
  * **examples/django** - a Django helloworld app and coewsgi-specific launcher.
  * **cqsl** -  C version of parse\_qs/parse\_qsl function from cgi/urlparse modules. Code lifted from mod\_python delivers noticeable perfomance benefits on long query strings.

# Building #

Prerequisites:

  * set/swap/make/getcontext functions in your libc
  * [libev](http://libev.schmorp.de/) ( debian/ubuntu: libev-dev )
  * [libpq](http://www.postgresql.org/docs/8.4/static/libpq.html) ( debian/ubuntu: libpq-dev )


After you get all of the above installed, choose appropriate prefix and do:
```
PREFIX=/some/prefix ./build.sh
```

in the root directory of the project.

# Packaging #

Everything was debianized for deployment on the ubuntu-server.
So a dpkg-buildpackage followed by dpkg-i in the right order (libucoev, python2.6, python-coev, then the rest) will set you up.