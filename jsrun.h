#include "duktape.h"

/**
 * Initialise the objects required for module loading to work.
 */
void init_modules(duk_context *ctx);
/**
 * Initialise the objects required for workers.
 */
void init_workers(duk_context *ctx);
/**
 * Creates a dictionary called env containing the environment for this process.
 */
void init_env(duk_context *ctx);
/**
 * Initialize TypedArray support.
 */
void init_typed_array(duk_context *ctx);
/**
 * Keep the context running for as long as it has a receive port with pending
 * messages.
 */
void run_message_loop(duk_context *ctx);
/**
 * Load the specified file into the context and execute it.  Returns 0 on
 * success, non-zero on failure.
 */
int handle_file(duk_context *ctx, const char *filename);
/**
 * Print an error (including backtrace) to the specified file and pop it from
 * the stack.
 */
void print_error(duk_context *ctx, FILE *f);

/**
 * Initialise all of the default objects provided by this environment.
 */
static inline void init_default_objects(duk_context *ctx)
{
	init_env(ctx);
	init_modules(ctx);
	init_workers(ctx);
	init_typed_array(ctx);
}
