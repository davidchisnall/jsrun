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
#include "jsrun.h"

extern char **environ;

void
init_env(duk_context *ctx)
{
	// TODO: It might be cleaner to make this a property on some Process object
	// that can also have some other metadata (e.g. command line args)
	duk_push_global_object(ctx);
	duk_push_object(ctx);
	for (char **env=environ ; *env!=NULL ; env++)
	{
		char *kv = *env;
		char *val = strchr(kv, '=');
		duk_push_lstring(ctx, kv, val-kv);
		// Skip the =
		duk_push_string(ctx, val+1);
		duk_put_prop(ctx, -3);
	}
	duk_put_prop_string(ctx, -2, "environ");
	duk_pop(ctx);
}
