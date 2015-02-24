Workers
=======

We provide an implementation of HTML5 Web Workers, with some limitations.  

Messages
--------

In the current implementation only objects that can be serialised as
JSON can be posted between workers.  This means no buffers, no code, no
pointers, and so on.

Garbage Collection
------------------

The Web Worker design intrinsically introduces a cycle: the Worker object in
the creating thread refers to the other thread, which implicitly refers to the
Worker.

It is only safe to delete a Worker if all of the following conditions are met:

- The thread is waiting for events (i.e. its receiving message queue is empty)
- There are no pending messages from the worker to another thread (these can
  cause resurrection)
- The Worker object is no longer reachable (or subsequent `postMessage()` calls
  would need the worker to exist.

The initial implementation does not have shared workers, so all workers are
arranged in a tree.  We therefore gain the extra invariant that leaf nodes must
be collected first (as any leaf can `postMessage()` to its parent, resurrecting
it).  We can determine if a worker is a leaf, because its receive port has a
reference count of 1 (only the parent, no children, can send messages to it).

Having made that determination, we must rendezvous the child with its parent at
a point in their run loops when they are not executing JavaScript code.  We can
then check that the child's run loop is empty and that the parent's does not
contain messages from the child.  Once this determination is made, we can remove
the global reference to the worker object in the parent and force garbage
collection.  If worker object is finalized, then we delete it.  If any messages
are sent to or from it before the finalizer is called, then we must resurrect it
and try again.

This is implemented by the following algorithm:

- Storing each worker in an array in the heap stash on creation.  The index for
  the worker is stored in a field in the worker for easy lookup.
- When a worker is paused with no messages in its queue and a reference count of
  one, it sets the `waiting` flag in its receive port.  It will also signal its
  parent port, to trigger the next step.
- When a worker is paused with no messages and a reference count of more than
  one, it iterates over the array of all workers to see if any are in the
  waiting state.
- If a worker is in the waiting state, then there are no in-flight messages so
  the only way that additional messages can be generated is by the
  set a`postMessage()` method being invoked on the worker.  Removing it from the
  global array and invoking the garbage collector will delete it if it is no
  longer referenced.
- If the worker is finalised, then we delete its parent port and signal its
  receive port to trigger the thread to exit.
- If the worker is still referenced, then we add it back to the global array.
  As no code is running, the child worker can only be referenced if this thread
  receives a message.  We set the `waiting` flag in our receive port and signal
  the parent.  If we have no parent, then we exit immediately.

Termination
-----------

The Web Worker spec is very vague about what termination means.  We do not allow
workers to be abruptly terminated in the middle of JavaScript execution, instead
we set a flag in their port indicating that they should terminate after
finishing processing the next message.

The `closing` property on the global object is implemented with a getter that
reads the `terminate` flag from the current worker.  This allows long-running
JavaScript to gracefully terminate.

Implementation
--------------

Each worker runs in a separate thread.  Each thread (including the main thread)
has two ports associated with it, stored a structure referenced from the global
stash.  

Ports are unidirectional message queues, protected by a mutex and condition
variable (they could be replaced with a lockless version easily, but there's
little point as messages are copied in `malloc()`'d memory and received by an
interpreter, so this is likely to be premature optimisation).  

Plans
-----

Shared workers require the ability to access the port of the worker and pass it
to another worker.  This will require some generalisations of the message
passing support, though the underlying transport is already designed to support
senders from multiple threads.  It will also complicate garbage collection
significantly, as it will introduce the possibility of cycles.  It remains to be
seen whether the effort in upgrading the worker GC to a full tracing
implementation is really worth it.

It would also be nice to provide a `SandboxedWorker` that ran in a separate
process and called `cap_enter()` before loading any code (but after opening the
relevant files).  The constructor for this would most likely need to take an
array of files to open and have a slightly modified version of the modules code
that would only permit loading modules that had been passed in.
