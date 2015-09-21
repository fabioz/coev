# Short list #

  * AMD64 ABI broken
  * 2x memcpy(), realloc() and free() at each switch
  * Python internals are messed up
  * No switching from within C extensions

# Details #

## AMD64 ABI broken ##

As you may know, a switch() implies saving and loading processor register values,
so that execution continues in the other coroutine (context) as if nothing happened. What registers' values should be saved over such a switch is defined by the platform ABI. I'm looking at the x86\_64 platform, also known as AMD64. Its formal definition can be found [here](http://www.x86-64.org/documentation.html).

Let's compare glibc's swapcontext versus greenlet 0.3 one.

### greenlet-0.3: ###

  * save `rdx, rbx, r12-r15`
  * realloc() some memory, copy a stack fragment there
  * restore other context's values in reverse order
  * copy stack fragment from memory, free() it

### eglibc-2.10.1: ###

  * save `rdx, rbx, r12-r15, r9, r8, rdi, rsi, rcx, rbp`
  * save floating-point environment
  * save current signal mask
  * restore other context's stack pointer and other values in reverse order


As can be seen from the above, the glibc's swapcontext() preserves 6 more integer registers, the floating point registers and the signal mask. While the signal mask is not very relevant, not preserving all register values that compilers expect to be preserved according to the ABI leads to hard-to-catch bugs and incorrect results.

## 2x memcpy(), realloc() and free() at each switch ##

From the comparison above, you can see that every switch to/from greenlet calls memcpy() twice, realloc() and free() once. The amount of data copied depends on application, I have seen it anywhere from 20 to 100+k. In an application handling tens to hundreds of requests per second, this adds up to a significant amount of copying, allocating and freeing memory.

The swapcontext() on the other hand, copies zero data.


## Python internals are messed up ##

PyThreadState is an internal Python structure, tracking, among other stuff, the exception currently raised.

greenlet does not keep a PyThreadState per a greenlet. This means that if an exception is raised in one greenlet, and a switch occurs to another greenlet that also raises an exception, the first exception data will be lost.

This basically means that a switch() in except: clause has undefined side-effects.

PyThreadState also has other interesting fields: recursion\_depth, frame, tracing and profiling-related ones.

  * `recursion_depth` - instead of indicating depth of stack within one execution context, with greenlets this shows something like `greenlet_count*average_greenlet_stack_depth`. When it exceeds Python's recursion limit, you are greeted by RecursionError.

  * `frame` - instead of representing Python stack of an execution context, this is an unholy mess of frames created within different greenlets. Consider this: when you spawn a greenlet and it runs the callable you supplied, this creates a frame. When you switch somewhere else, execution continues with the frame that was just created. This results in meaningless tracebacks. Moreover, when some greenlet exits its callable, a frame that has no relation to it gets deallocated, and its local symbol table goes with it. I'm really not sure how greenlets even appear to work.

## No switching from within C extensions ##

Well, you can't switch from within C code to another greenlet. It does not export C API, and even if it did, the problems listed above will most likely make it unusable anyway.

### Corollary ###

  * event-loop must have a Python wrapper.
  * event dispatch must be handled in Python code
  * coroutine scheduling must be handled in Python code

Each of those points hurts performance due to introducing a huge number of Python-to-C calls with all associated data marshalling.


# Conclusion #

Greenlets have a huge number of potential side-effects.
One can not rely on correct execution of code when greenlets are used.
Their performance is also severely limited.