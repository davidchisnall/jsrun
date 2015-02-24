#include "duktape.h"

void init_modules(duk_context *ctx);
/**
 * Creates a dictionary called env containing the environment for this process.
 */
void init_env(duk_context *ctx);

static inline void init_default_objects(duk_context *ctx)
{
	init_env(ctx);
	init_modules(ctx);
}
