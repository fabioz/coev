# What's the difference #

`Greenlet` module which is the basis for the `eventlet`, `gevent` modules and the whole of Stackless implements stack-switching with memmove and some black magic saving/restoring the register values.

## This is bad because: ##

  * It is a reimplementation of `swapcontext()` and friends. With due respect, I seriously doubt it is maintained better than the libc.
  * It can not be used by C extensions.
  * It uses `memmove()`. This is slower than `swapcontext()` which basically just swaps register values.
  * The scheduler must be implemented in Python.

For a more thorough analysis of the greenlet, see GreenletProblems page.

## The coev on the other hand .... ##

Is based on a pure-C library, thus coroutine-mode execution can be patched right inside the C extensions, thus getting rid of multiple passes through C/Python boundary. It is also not a complex task - the postgres DB-API module, `psycopg2`, only required a fork because of problems with code quality of the original.

It also exposes coroutines as Python threads.

## Thus ##

  * As long as the scheduler is running, one can use modules that use locks and/or threads ( `threading, logging, sqlalchemy` ... ) without modification.
  * The GIL is reduced to a counter increment/decrement, as are all locks.
  * One can't spawn a thread :).
  * Python context (Py\_ThreadState) is switched automagically by corresponding macroes.

## The socketfile ##

libucoev includes the implementation of a `socketfile` object. I decided to not reimplement or `*`gasp`*` monkey-patch the perfomance-critical socket module - bulk of it can and should be used in unmodified form. The `socketfile`  is a read buffer on top of a file descriptor, behaving like a blocking-mode socket - switching back to scheduler in case there is not enough data. It provides the usual read/readline/write methods