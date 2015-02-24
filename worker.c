/*
 * Copyright (c) 2015 David Chisnall
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * $FreeBSD$
 */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include "jsrun.h"

#ifdef TRACE_WORKERS
#define LOG(...) fprintf(stderr, VA_ARGS)
#else
#define LOG(...) do {} while(0)
#endif

/**
 * Macro that attempts to acquire a lock and will automatically release it once
 * the current scope ends. 
 */
#define LOCK_FOR_SCOPE(lock) \
	pthread_mutex_lock(&lock);\
	__attribute__((cleanup(release_lock)))\
	__attribute__((unused)) pthread_mutex_t *lock_pointer = &lock


/**
 * Structure for a message sent via a `port`.
 */
struct message
{
	/**
	 * Next message in the list.
	 */
	struct message *next;
	/**
	 * The serialised object in the list.  This memory is owned by the message
	 * struct and must be `free()`d when the message is deleted.
	 */
	char *contents;
	/**
	 * The worker that sent this message.  This allows the correct
	 * `onMessage()` method to be called.
	 */
	void *receiver;
};

/**
 * A simple message queue.  This is not terribly efficient, but given that
 * every message involves a malloc and free call and interaction with an
 * interpreter, speed is not the overriding goal here.
 *
 * All accesses to the port must be done while the `lock` mutex is held.
 */
struct port
{
	/**
	 * The number of JavaScript objects that reference this port.  Each port
	 * has a single worker that can receive messages and (potentially) multiple
	 * instances of the object that can send messages.  When the sending end is
	 * garbage collected, the object finaliser will decrement this reference
	 * count and then signal the condition variable.
	 *
	 * Ports can be deleted in one of two ways:
	 *
	 * 1. The refcount drops to 0 while the port is still connected to a
	 *    worker.  The worker is then responsible for deleting the port after
	 *    processing any pending messages.
	 * 2. The refcount drops to zero after the port has been disconnected (e.g.
	 *    by abnormal termination of the worker).  Any attempts to push
	 *    messages into the queue should fail and relinquish their ownership of
	 *    the port (decrementing the refcount).  The object that causes the
	 *    refcount to drop to 0 should free it.
	 */
	int refcount;
	/**
	 * Indicates that this port is waiting for messages from a single producer.
	 * This is used to indicate that the thread may be part of a garbage cycle.
	 */
	_Atomic(bool) waiting;
	/**
	 * Flag indicating that the worker on the receiving end has exited.  The
	 * sender that drops the reference count to zero is responsible for
	 * cleaning up the structure.
	 */
	_Atomic(bool) disconnected;
	/**
	 * Flag indicating that worker should terminate.  No more messages should
	 * be processed.
	 */
	_Atomic(bool) terminated;
	/**
	 * The lock used to protect the message queue.
	 */
	pthread_mutex_t   lock;
	/**
	 * The condition variable that this thread will wait on when there are no
	 * pending messages in the queue.
	 */
	pthread_cond_t cond;
	/**
	 * The next message to process.
	 */
	struct message *message_head;
	/**
	 * The insertion point for message in the queue.
	 */
	struct message *message_tail;
};

/**
 * Struct representing a worker.  
 */
struct worker
{
	/**
	 * The thread that is running this worker.
	 */
	pthread_t thread;
	/**
	 * The name of the file that this will load.
	 */
	char *file;
	/**
	 * The Duktape context used for JavaScript execution in this thread.
	 */
	duk_context *ctx;
	/**
	 * Lock used to protect the worker.  The only concurrent access to this
	 * structure happens when the Worker object is finalised.  At this point,
	 * the receive_port's 
	 */
	pthread_mutex_t lock;
	/**
	 * The Worker object that corresponds to this thread.
	 */
	void *object;
	/**
	 * The port that is bound to this worker, which it will use for receiving
	 * messages.  This will be inserted into the context's heap stash so that
	 * it can be retrieved when creating a new worker.
	 */
	struct port *receive_port;
	/**
	 * The port that is used to deliver messages to the parent.
	 */
	struct port *parent_port;
};

/**
 * Release a mutex.  This function should never be called directly.  It exists
 * so that the `LOCK_FOR_SCOPE()` macro can use the cleanup attribute to
 * release the lock when it goes out of scope.
 */
static inline void
release_lock(pthread_mutex_t **mtx)
{
	pthread_mutex_unlock(*mtx);
}

/**
 * Construct a new port.
 */
static struct port *
create_port(void)
{
	struct port *p = calloc(sizeof(struct port),1);
	p->lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	p->cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	return p;
}

/**
 * Free a message.  The message must be removed from any ports before passing
 * to this function.
 */
static void
free_message(struct message *m)
{
	assert(m->next == NULL);
	free(m->contents);
	free(m);
}

/**
 * Free a port, including any outstanding messages.
 */
static void
free_port(struct port *p)
{
	if (p == NULL)
	{
		return;
	}
	assert(p->refcount == 0);
	struct message *m = p->message_head;
	while (m != NULL)
	{
		struct message *next = m->next;
		m->next = NULL;
		free_message(m);
		m = next;
	}
	pthread_mutex_destroy(&p->lock);
	pthread_cond_destroy(&p->cond);
	free(p);
}

/**
 * Release a reference to the sending port.  Returns true the remote end has
 * already been destroyed.
 */
static bool 
release_sending_port(struct port *p)
{
	LOG("Signalling sending port...\n");
	p->refcount--;
	pthread_cond_signal(&p->cond);
	LOG("Released port %p, refcount is now %d\n", p, p->refcount);
	return true;
}

/**
 * Post a message into a port.
 */
static bool
send_message(struct port *p, struct message *m)
{
	assert(p);
	m->next = NULL;
	LOCK_FOR_SCOPE(p->lock);
	if (p->terminated || p->disconnected)
	{
		LOG("Not sending message, receiver is down\n");
		free_message(m);
		return false;
	}
	p->waiting = false;
	LOG("Setting waiting to false for %p\n", p);
	if (p->message_tail != NULL)
	{
		p->message_tail->next = m;
	}
	else
	{
		// We only need to wake up the receiving thread on a transition from
		// empty to non-empty, as it will only sleep on the condvar when the
		// thread is empty (and it owns the mutex).
		p->message_head = m;
		pthread_cond_signal(&p->cond);
	}
	p->message_tail = m;
	return true;
}

/**
 * Try to garbage collect workers.  Returns true if one or more workers has
 * been deleted.
 */
static bool
try_to_collect_workers(struct port *p, duk_context *ctx)
{
	LOG("Trying to GC threads for context %p\n", ctx);
	// Hold the receive lock for the entire GC process.  This should prevent a
	// potential race where a child sends a message to us and then waits, but
	// we miss it.  Instead, the child will block attempting to send the
	// message and not start waiting until we finish this GC cycle.
	if (p->message_head != NULL)
	{
		return false;
	}
	duk_push_heap_stash(ctx);
	duk_get_prop_string(ctx, -1, "workers");
	// If we don't have a workers list, we have no children to wait for.
	if (!duk_is_object(ctx, -1))
	{
		duk_pop(ctx); // undefined (workers)
		duk_pop(ctx); // heap stash
		return false;
	}
	duk_get_prop_string(ctx, -1, "length");
	duk_int_t length = duk_get_int(ctx, -1);
	duk_pop(ctx); // length
	bool all_waiting = true;
#ifndef NDEBUG
	duk_int_t top = duk_get_top(ctx);
#endif
	LOG("Checking %d children\n", length);
	// Iterate over the array of workers.
	for (duk_int_t i=0 ; i<length ; i++)
	{
		assert(top == duk_get_top(ctx));
		duk_push_int(ctx, i);
		duk_get_prop(ctx, -2);
		if (duk_is_object(ctx, -1))
		{
			duk_get_prop_string(ctx, -1, "\xFF" "worker_struct");
			struct worker *w = duk_get_pointer(ctx, -1);
			duk_pop(ctx); // worker_struct
			if (w == NULL)
			{
				duk_pop(ctx); // Worker
				continue;
			}
			LOG("Inspecting worker %p (%d)\n", w->object, w->receive_port->waiting);
			// We don't need to lock the receive port, because only the parent
			// (i.e. us) is allowed to move the worker from a waiting to a
			// non-waiting state.  Thia also avoids the thread deadlocking with
			// itself if the finalise method is called.
			// If the worker is waiting, replace the GC'd pointer with a
			// non-GC'd one.
			if (w->receive_port->waiting || w->receive_port->disconnected)
			{
				void *ptr = duk_get_heapptr(ctx, -1);
				LOG("Trying to collect worker %p (waiting: %d)\n", ptr, w->receive_port->waiting);
				duk_pop(ctx); // Worker as object
				duk_push_int(ctx, i);
				duk_push_pointer(ctx, ptr); // Worker as non-GC'd pointer
				duk_put_prop(ctx, -3);
			}
			else
			{
				duk_pop(ctx); // Worker
				LOG("Worker %p (port %p) is not waiting\n", w->object, w->receive_port);
				all_waiting = false;
			}
		}
		else
		{
			duk_pop(ctx); // Worker
		}
	}
	// Run the GC a couple of times to make sure that we clean up any
	// workers that are no longer referenced.
	duk_gc(ctx, 0);
	duk_gc(ctx, 0);
	LOG("Re-adding roots for live workers in context %p\n", ctx);
	duk_int_t insert_ptr = 0;
	for (duk_int_t i=0 ; i<length ; i++)
	{
		duk_push_int(ctx, i);
		duk_get_prop(ctx, -2);
		// If the index is an object, not something undefined, then mark the
		// pointer as an object pointer again.
		if (duk_is_pointer(ctx, -1))
		{
			void *ptr = duk_get_pointer(ctx, -1);
			LOG("Failed to collect worker %p\n", ptr);
			duk_pop(ctx); // Worker
			duk_push_int(ctx, insert_ptr++);
			duk_push_heapptr(ctx, ptr);
			duk_put_prop(ctx, -3);
		}
		else
		{
			if (duk_is_object(ctx, -1))
			{
				LOG("Didn't try to collect worker %p\n", duk_get_heapptr(ctx, -1));
				insert_ptr++;
			}
			duk_pop(ctx); // Worker
		}
	}
	// Resize the array
	duk_push_int(ctx, insert_ptr+1);
	duk_put_prop_string(ctx, -2, "length");
	duk_pop(ctx); // array
	duk_pop(ctx); // global stash 
	LOG("Collected threads for context %p, all waiting? %d (%d left)\n", ctx, all_waiting, insert_ptr+1);
	return all_waiting;
}


static bool
get_message(struct port *p,
            struct port *parent,
            struct message **m,
            duk_context *ctx)
{
	LOCK_FOR_SCOPE(p->lock);
	if (p->terminated)
	{
		LOG("Waiting for message on %p, terminate: %d\n", p, p->terminated);
		return false;
	}
	// Sleep while there are no pending messages, but there are threads that
	// may send messages.
	if (p->message_head == NULL && p->refcount > 0)
	{
		// If the reference count is 1, then we have no children.  If the
		// reference count is more than 1, but the children are also all
		// waiting, then we can propagate the waiting state up the tree.
		if (parent != NULL)
		{
			// Release the lock and reacquire in the order (top-down) that the
			// GC needs.
			pthread_mutex_unlock(&p->lock);
			LOCK_FOR_SCOPE(parent->lock);
			pthread_mutex_lock(&p->lock);
			bool waiting = try_to_collect_workers(p, ctx);
			waiting |= p->refcount == 1;
			// Re-do the checks with both locks held and signal the parent that
			// we're waiting if we really are.
			if (p->message_head == NULL && waiting)
			{
				LOG("Setting waiting to true for %p and signalling %p\n", p, parent);
				p->waiting = true;
				pthread_cond_signal(&parent->cond);
			}
			assert(parent);
		}
		else
		{
			// If we're the top-level thread, then try to collect children and
			// if we can then give up now
			if (try_to_collect_workers(p, ctx))
			{
				return false;
			}
		}
		// If we still have messages waiting, get them.
		if (p->message_head == NULL && p->refcount > 0)
		{
			LOG("Sleeping on port %p (%d senders)\n", p, p->refcount);
			pthread_cond_wait(&p->cond, &p->lock);
		}
		LOG("Waking up port %p, message: %p\n", p, p->message_head);
		assert((p->waiting == false) || (p->message_head == NULL));
	}
	if (p->message_head == NULL)
	{
		LOG("Exiting with no messages\n");
		return false;
	}
	*m = p->message_head;
	p->message_head = (*m)->next;
	(*m)->next = NULL;
	if (p->message_head == NULL)
	{
		p->message_tail = NULL;
	}
	LOG("received on port %p, message: %p for %p\n", p, *m, (*m)->receiver);
	return true;
}


/**
 * Clean up a worker.
 */
static void
cleanup_worker(struct worker *w)
{
	LOG("Cleaning up worker %p\n", w);
	duk_destroy_heap(w->ctx);
	free(w->file);
	// Wait for the refcount to drop to 0 and then delete it.
	{
		w->receive_port->disconnected = true;
		LOCK_FOR_SCOPE(w->receive_port->lock);
		while (!(w->receive_port->refcount == 0))
		{
			LOG("Waiting for the last reference to our receive port (%p) to disappear\n", w->receive_port);
			pthread_cond_wait(&w->receive_port->cond, &w->receive_port->lock);
		}
	}
	// Release our reference to the parent port.
	LOCK_FOR_SCOPE(w->parent_port->lock);
	LOG("Parent port refcount: %d\n", w->parent_port->refcount);
	release_sending_port(w->parent_port);
	LOG("Destroying worker struct %p (object: %p)\n", w, w->object);
	free_port(w->receive_port);
	free(w);
}

static void
decode_string(duk_context *ctx, const char *str)
{
	duk_push_string(ctx, str);
	// FIXME: Handle failure
	duk_json_decode(ctx, -1);
}

struct port *
get_thread_port(duk_context *ctx)
{
	struct port *p;
	duk_push_heap_stash(ctx);
	// Port used for injecting messages into the run loop
	duk_get_prop_string(ctx, -1, "default_port");
	if (duk_is_pointer(ctx, -1))
	{
		p = duk_get_pointer(ctx, -1);
		duk_pop(ctx);
		LOCK_FOR_SCOPE(p->lock);
	}
	else
	{
		duk_pop(ctx);
		p = create_port();
		duk_push_pointer(ctx, p);
		duk_put_prop_string(ctx, -2, "default_port");
	}
	duk_pop(ctx);
	return p;
}

static bool
prepare_onmessage(duk_context *ctx)
{
	// Get the onmessage function and stick it on the top of the stack
	if (duk_get_prop_string(ctx, -1, "onMessage") != 1)
	{
		return false;
	}
	// If the onmessage variable is not a function then return it.
	if (!duk_is_function(ctx, -1))
	{
		duk_pop(ctx);
		return false;
	}
	return true;
}

void
run_message_loop(duk_context *ctx)
{
	duk_push_heap_stash(ctx);
	duk_get_prop_string(ctx, -1, "worker_struct");
	struct worker *w = duk_get_pointer(ctx, -1);
	duk_pop(ctx);
	struct port *receive_port = get_thread_port(ctx);
	struct port *parent_port = w ? w->parent_port : NULL;
#ifndef NDEBUG
	duk_int_t top = duk_get_top(ctx);
#endif
	struct message *m;
	bool possibly_dead = false;
	do
	{
		if (get_message(receive_port, parent_port, &m, ctx))
		{
			if (receive_port->terminated)
			{
				LOG("Not processing message, worker terminated\n");
				break;
			}
			assert(top == duk_get_top(ctx));
			// If the receiver is null, then this is aimed at the global
			// receive port.
			if (m->receiver == NULL)
			{
				duk_push_global_object(ctx);
				if (prepare_onmessage(ctx))
				{
					decode_string(ctx, m->contents);
					duk_call(ctx, 1);
					// We don't care about the return value.
					duk_pop(ctx);
				}
				duk_pop(ctx); // global object
			}
			else
			{
				// Push the worker
				duk_push_heapptr(ctx, m->receiver);
				LOG("Received message '%s' for worker %p\n", m->contents, m->receiver);
				if (prepare_onmessage(ctx))
				{
					// Push the `this` object.
					duk_dup(ctx, -2);
					// Push the argument
					decode_string(ctx, m->contents);
					// Call the method and ignore the return
					duk_call_method(ctx, 1);
					duk_pop(ctx);
				}
				duk_pop(ctx); // Worker object
			}
			assert(top == duk_get_top(ctx));
			free_message(m);
		}
		// If we've been told to exit, or there are no more event sources, then
		// exit without trying to GC children.
		if (receive_port->terminated)
		{
			break;
		}
		assert(top == duk_get_top(ctx));
		LOCK_FOR_SCOPE(receive_port->lock);
		possibly_dead = try_to_collect_workers(receive_port, ctx);
		assert(top == duk_get_top(ctx));
		// If all of our children are blocked and we have no parent, then exit.
		if (possibly_dead && (w == NULL))
		{
			return;
		}
	} while (receive_port->refcount > 0);
	LOG("Run loop exiting for %p\n", ctx);
}

static int
post_message_global(duk_context *ctx)
{
	// Expect exactly one argument
	const char *json = duk_json_encode(ctx, 0);
	if (json == NULL)
	{
		return DUK_RET_TYPE_ERROR;
	}
	duk_push_heap_stash(ctx);
	duk_get_prop_string(ctx, -1, "worker_struct");
	struct worker *w = duk_get_pointer(ctx, -1);
	duk_pop(ctx);
	struct message *m = malloc(sizeof(struct message));
	m->contents = strdup(json);
	m->receiver = w->object;
	// FIXME: handle termination
	send_message(w->parent_port, m);
	return 0;
}

static duk_ret_t
compile_execute(duk_context *ctx)
{
	duk_compile(ctx, 0);
	duk_push_global_object(ctx);  /* 'this' binding */
	duk_call_method(ctx, 0);
	duk_pop(ctx); // result
	return 1;
}

static duk_ret_t
get_closing(duk_context *ctx)
{
	LOG("Closing called\n");
	duk_push_heap_stash(ctx);
	duk_get_prop_string(ctx, -1, "worker_struct");
	struct worker *w = duk_get_pointer(ctx, -1);
	duk_push_boolean(ctx, w->receive_port->terminated);
	return 1;
}


/**
 * Function passed to `pthread_create` to create a new context and run a worker.
 */
static void *
run_worker(struct worker *w)
{
	// Construct a new JavaScript context for the 
	duk_context *ctx = duk_create_heap_default();
	init_default_objects(ctx);
	// Store the worker object in the heap so that it can be accessed from
	// postMessage() calls.
	duk_push_heap_stash(ctx);
	duk_push_pointer(ctx, w);
	duk_put_prop_string(ctx, -2, "worker_struct");
	duk_push_pointer(ctx, w->receive_port);
	duk_put_prop_string(ctx, -2, "default_port");
	duk_pop(ctx);
	// Set the global postMessage() function to call back to the parent thread.
	duk_push_global_object(ctx);
	duk_push_c_function(ctx, post_message_global, 1);
	duk_put_prop_string(ctx, -2, "postMessage");
	duk_push_string(ctx, "closing");
	duk_push_c_function(ctx, get_closing, 0);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER);
	// Load and run the file
	if (handle_file(ctx, w->file) == 0)
	{
		run_message_loop(ctx);
	}
	LOG("Worker %p exiting!\n", w->object);
	cleanup_worker(w);
	return NULL;
}

/**
 * The `postMessage()` method on a Worker object.  Sends a message to a child
 * thread that will be handled by the global scope.
 */
static int
post_message_method(duk_context *ctx)
{
	// Expect exactly one argument
	const char *json = duk_json_encode(ctx, 0);
	if (json == NULL)
	{
		return DUK_RET_TYPE_ERROR;
	}
	duk_push_this(ctx);
	duk_get_prop_string(ctx, -1, "\xFF" "worker_struct");
	struct worker *w = duk_get_pointer(ctx, -1);
	struct port *p = w->receive_port;
	struct message *m = malloc(sizeof(struct message));
	m->contents = strdup(json);
	m->receiver = NULL;
	// FIXME: handle termination
	send_message(p, m);
	return 0;
}

/**
 * Method to terminate a worker.  Leaves the worker in an undefined state, but
 * will not actually garbage collect it until all of the message queues have
 * exited.
 */
static duk_ret_t
terminate_method(duk_context *ctx)
{
	duk_push_this(ctx);
	duk_get_prop_string(ctx, -1, "\xFF" "worker_struct");
	struct worker *w = duk_get_pointer(ctx, -1);
	// If we've already called terminate, don't do anything else.
	if (w->receive_port->terminated)
	{
		return 0;
	}
	LOCK_FOR_SCOPE(w->receive_port->lock);
	w->receive_port->terminated = true;
	pthread_cond_signal(&w->receive_port->cond);
	LOG("Set terminate flag\n");
	return 0;
}

/**
 * Constructor function for Worker objects.
 */
static int
spawn_worker(duk_context *ctx)
{
	// If this isn't called as a constructor, then return undefined.
	if (!duk_is_constructor_call(ctx))
	{
		return 0;
	}
	// Expect exactly one argument
	if (duk_get_top(ctx) != 1)
	{
		return DUK_RET_TYPE_ERROR;
	}
	// If the argument is not a string, raise an error
	const char *fn = duk_get_string(ctx, -1);
	if (fn == NULL)
	{
		return DUK_RET_TYPE_ERROR;
	}
	struct worker *w = malloc(sizeof(struct worker));
	w->file = strdup(fn);
	w->ctx = NULL;
	w->receive_port = create_port();
	w->receive_port->refcount = 1;
	w->parent_port = get_thread_port(ctx);
	duk_push_this(ctx);
	w->object = duk_get_heapptr(ctx, -1);
	LOG("Created worker %p in context %p\n", w->object, ctx);
	if (pthread_create(&w->thread, NULL, (void *(*)(void *))run_worker, w))
	{
		cleanup_worker(w);
		return 0;
	}
	duk_push_pointer(ctx, w);
	duk_put_prop_string(ctx, -2, "\xFF" "worker_struct");
	duk_push_heap_stash(ctx);
	duk_get_prop_string(ctx, -1, "workers");
	if (!duk_is_array(ctx, -1))
	{
		duk_pop(ctx);
		duk_push_array(ctx);
		//duk_dup_top(ctx);
		//duk_put_prop_string(ctx, -3, "workers");
		duk_put_prop_string(ctx, -2, "workers");
		duk_get_prop_string(ctx, -1, "workers");
	}
	duk_push_heapptr(ctx, w->object);
	duk_get_prop_string(ctx, -2, "length");
	assert(duk_is_number(ctx, -1));
	duk_dup_top(ctx);
	// worker["\xffindex"] = workers.length
	duk_put_prop_string(ctx, -3, "\xFF" "index");
	// workers[workers.length] = worker;
	assert(duk_is_array(ctx, -3));
	assert(duk_is_object(ctx, -2));
	assert(duk_is_number(ctx, -1));
	duk_swap_top(ctx, -2);
	duk_put_prop(ctx, -3);
	duk_pop(ctx); //array
	duk_pop(ctx); // heap stash
	LOCK_FOR_SCOPE(w->parent_port->lock);
	w->parent_port->refcount++;
	return 0;
}

static int
finalise_worker(duk_context *ctx)
{
	LOG("Destroying worker %p in context %p\n", duk_get_heapptr(ctx, -1), ctx);
	duk_get_prop_string(ctx, -1, "\xFF" "worker_struct");
	struct worker *w = duk_get_pointer(ctx, -1);
	if (w == NULL)
	{
		LOG("Not destroying worker, no worker_struct property!\n");
		return 0;
	}
	duk_pop(ctx); // worker_struct
	duk_get_prop_string(ctx, -1, "\xFF" "index");
	duk_push_heap_stash(ctx);
	duk_get_prop_string(ctx, -1, "workers");
	duk_dup(ctx, -3);
	assert(duk_is_number(ctx, -1));
	assert(duk_is_array(ctx, -2));
	// Delete the reference to this in the workers array.
	duk_del_prop(ctx, -2);
	duk_pop(ctx); // workers array
	duk_pop(ctx); // heap stash
	duk_pop(ctx); // index
	// Disclaim our reference to the receiving port.  This will cause the
	// worker thread to clean up the port.
	LOG("Destroying's receive port ref %p\n", w->receive_port);
	assert(w->receive_port->waiting || w->receive_port->disconnected);
	LOCK_FOR_SCOPE(w->receive_port->lock);
	release_sending_port(w->receive_port);
	// Remove the hidden properties, so if we are spuriously finalised twice we
	// won't break things.
	duk_del_prop_string(ctx, -1, "\xFF" "worker_struct");
	duk_get_prop_string(ctx, -1, "\xFF" "index");
	return 0;
}

void
init_workers(duk_context *ctx)
{
	duk_push_global_object(ctx);
	duk_push_c_function(ctx, spawn_worker, 1);
	// Construct the prototype object for workers
	duk_push_object(ctx);
	duk_push_c_function(ctx, post_message_method, 1);
	duk_put_prop_string(ctx, -2, "postMessage");
	duk_push_c_function(ctx, terminate_method, 1);
	duk_put_prop_string(ctx, -2, "terminate");
	duk_push_c_function(ctx, finalise_worker, 1);
	duk_set_finalizer(ctx, -2);
	// Set the prototype property for the constructor
	duk_put_prop_string(ctx, -2, "prototype");
	// Name the Worker function in the global scope
	duk_put_prop_string(ctx, -2, "Worker");
	duk_pop(ctx);
}
