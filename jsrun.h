#include "duktape.h"

void init_modules(duk_context *ctx);

static inline void init_default_objects(duk_context *ctx)
{
	init_modules(ctx);
}
