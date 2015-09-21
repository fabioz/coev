# Introduction #

This all started when I realized that it is not possible to do a greenlet.switch() from C extension. I struggled some time trying to make it work, but it seemed that greenlets' switching code was not preserving enough register values or breaking the ABI in some even more sinister way.

So I turned to the time-proven libc support for switching context: get/make/swap/setcontext().

# The components #

## The libucoev library ##

This is the core library. It wraps the contexts (`ucontext_t`) so that a) they can be exposed as threads to the Python, and b) implements the I/O event-driven scheduler, so that a context may wait for an event on a given file descriptor, or for a timeout.

The context hierarchy concept was borrowed from the greenlet module.

The library also contains auxillary code like locks and context-local storage to further satisfy the Python needs.

Main difference to the greenlet mode of operation is that stacks for the contexts are mmap()-ed and not constructed on the fly from pieces all over the heap.

Contrary to the gut feeling, this does not lead to excessive memory consumption. The stacks are mmap()-ed just like the thread stacks, and physical pages are mapped on first access only, so in practice only small amount of actual memory is used for each stack. However, each context uses 2Mb of address space, so running production apps on AMD64 is essential, as typical x86 machine will run out of address space (not memory!) at something like 1-2 thousand contexts.


## Python patch ##

It consists of a thread\_ucoev.h file binding the libucoev as a threading backend,
and a trivial patch to Modules/threadmodule.c than implements passing return value or exception that killed the "thread" to its parent.

Patches are simple enough that I don't expect any problems porting them to 2.7, 3.1 or the Unladen Swallow. In fact, I expect the patch to apply cleanly or with minimal fuzz.

## The python-coev bindings ##

This extension is a thin wrapper over the libucoev, exposing basics like switch() and wait(), scheduler control, socketfile object and misc stuff like statistics.


# The scheduler #

The scheduler is a simple FIFO queue of 'runnable', that is _ready to be switched into_ contexts. On each iteration, the scheduler moves aside the current queue and goes through it, switching to each context in turn. Having done that, one iteration of libev's event loop is done, to schedule contexts that have fresh events.

I/O-driven part of it consists of libev event loop. A context that requests a wait for an event is marked as waiting and corresponding watcher (in libev terms) is activated, and the control is passed to the scheduler context. When the event arrives or timeout expires, the event handler is called by the libev, whose job is to put the context in question into the scheduler queue.

# How does it look from the Python side? #

Answer: just like threads.

`thread.start_new_thread(...)` creates a context, and schedules it for execution at the next scheduler iteration. After that you need to start or switch to an existing scheduler context and it will run.

To wait for an event one needs an fd in nonblocking mode:

`coev.wait(fd, coev.READ | coev.WRITE, timeout)`

This will wait for an event or for timeout to elapse and will return None or raise some exception. Under the hood it will activate an libev watcher and switch to scheduler so other contexts can be ran.



