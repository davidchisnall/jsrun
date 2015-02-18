/*
 * Copyright (c) 2013-2015 by Duktape authors (see AUTHORS.rst)
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

/*
 *  Command line execution tool.  Used by test cases and other manual testing.
 *
 *  For maximum portability, compile with -DDUK_CMDLINE_BAREBONES
 */

#if defined(_WIN32) || defined(_WIN64) || defined(WIN32) || \
    defined(__WIN32__) || defined(__TOS_WIN__) || defined(__WINDOWS__)
#ifndef DUK_CMDLINE_BAREBONES
/* Force barebones mode on Windows. */
#define DUK_CMDLINE_BAREBONES
#endif
#endif

#ifdef DUK_CMDLINE_BAREBONES
#define NO_READLINE
#define NO_RLIMIT
#define NO_SIGNAL
#endif

#define  GREET_CODE(variant)  \
	"print(" \
	"'((o) Duktape" variant "'" \
	", " \
	"Math.floor(Duktape.version / 10000) + '.' + " \
	"Math.floor(Duktape.version / 100) % 100 + '.' + " \
	"Duktape.version % 100" \
	");"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NO_SIGNAL
#include <signal.h>
#endif
#ifndef NO_RLIMIT
#include <sys/resource.h>
#endif
#ifndef NO_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif
#include <sys/stat.h>
#include <dlfcn.h>

#include "duktape.h"

#define  MEM_LIMIT_NORMAL   (128*1024*1024)   /* 128 MB */
#define  MEM_LIMIT_HIGH     (2047*1024*1024)  /* ~2 GB */
#define  LINEBUF_SIZE       65536

/* FIXME: additional modules should probably only be in some separate tool? */
#if 0
extern void duk_ncurses_register(duk_context *ctx);
extern void duk_socket_register(duk_context *ctx);
extern void duk_fileio_register(duk_context *ctx);
#endif

int interactive_mode = 0;

#ifndef NO_RLIMIT
static void set_resource_limits(rlim_t mem_limit_value) {
	int rc;
	struct rlimit lim;

	rc = getrlimit(RLIMIT_AS, &lim);
	if (rc != 0) {
		fprintf(stderr, "Warning: cannot read RLIMIT_AS\n");
		return;
	}

	if (lim.rlim_max < mem_limit_value) {
		fprintf(stderr, "Warning: rlim_max < mem_limit_value (%d < %d)\n", (int) lim.rlim_max, (int) mem_limit_value);
		return;
	}

	lim.rlim_cur = mem_limit_value;
	lim.rlim_max = mem_limit_value;

	rc = setrlimit(RLIMIT_AS, &lim);
	if (rc != 0) {
		fprintf(stderr, "Warning: setrlimit failed\n");
		return;
	}

#if 0
	fprintf(stderr, "Set RLIMIT_AS to %d\n", (int) mem_limit_value);
#endif
}
#endif  /* NO_RLIMIT */

#ifndef NO_SIGNAL
static void my_sighandler(int x) {
	fprintf(stderr, "Got signal %d\n", x);
	
}
static void set_sigint_handler(void) {
	(void) signal(SIGINT, my_sighandler);
}
#endif  /* NO_SIGNAL */

static int get_stack_raw(duk_context *ctx) {
	if (!duk_is_object(ctx, -1)) {
		return 1;
	}
	if (!duk_has_prop_string(ctx, -1, "stack")) {
		return 1;
	}

	/* XXX: should check here that object is an Error instance too,
	 * i.e. 'stack' is special.
	 */

	duk_get_prop_string(ctx, -1, "stack");  /* caller coerces */
	duk_remove(ctx, -2);
	return 1;
}

/* Print error to stderr and pop error. */
static void print_error(duk_context *ctx, FILE *f) {
	/* Print error objects with a stack trace specially.
	 * Note that getting the stack trace may throw an error
	 * so this also needs to be safe call wrapped.
	 */
	(void) duk_safe_call(ctx, get_stack_raw, 1 /*nargs*/, 1 /*nrets*/);
	fprintf(f, "%s\n", duk_safe_to_string(ctx, -1));
	fflush(f);
	duk_pop(ctx);
}

int wrapped_compile_execute(duk_context *ctx) {
	int comp_flags;

	comp_flags = 0;
	duk_compile(ctx, comp_flags);

#if 0
	/* FIXME: something similar with public API */
	if (interactive_mode) {
		duk_hcompiledfunction *f = (duk_hcompiledfunction *) duk_get_hobject(ctx, -1);

		if (f && DUK_HOBJECT_IS_COMPILEDFUNCTION((duk_hobject *) f)) {
			fprintf(stdout, "[bytecode length %d opcodes, registers %d, constants %d, inner functions %d]\n",
				(int) DUK_HCOMPILEDFUNCTION_GET_CODE_COUNT(f),
				(int) f->nregs,
				(int) DUK_HCOMPILEDFUNCTION_GET_CONSTS_COUNT(f),
				(int) DUK_HCOMPILEDFUNCTION_GET_FUNCS_COUNT(f));
			fflush(stdout);
		} else {
			fprintf(stdout, "[invalid compile result]\n");
			fflush(stdout);
		}
	}
#endif

	duk_push_global_object(ctx);  /* 'this' binding */
	duk_call_method(ctx, 0);

	if (interactive_mode) {
		/*
		 *  In interactive mode, write to stdout so output won't interleave as easily.
		 *
		 *  NOTE: the ToString() coercion may fail in some cases; for instance,
		 *  if you evaluate:
		 *
		 *    ( {valueOf: function() {return {}}, toString: function() {return {}}});
		 *
		 *  The error is:
		 *
		 *    TypeError: failed to coerce with [[DefaultValue]]
		 *            duk_api.c:1420
		 *
		 *  These errors are caught and printed out as errors although
		 *  the errors are not generated by user code as such.  Changing
		 *  duk_to_string() to duk_safe_to_string() would avoid these
		 *  errors.
		 */

		fprintf(stdout, "= %s\n", duk_to_string(ctx, -1));
		fflush(stdout);
	} else {
		/* In non-interactive mode, success results are not written at all.
		 * It is important that the result value is not string coerced,
		 * as the string coercion may cause an error in some cases.
		 */
	}

	duk_pop(ctx);
	return 0;
}

int handle_fh(duk_context *ctx, FILE *f, const char *filename) {
	char *buf = NULL;
	int len;
	int got;
	int rc;
	int retval = -1;

	if (fseek(f, 0, SEEK_END) < 0) {
		goto error;
	}
	len = (int) ftell(f);
	if (fseek(f, 0, SEEK_SET) < 0) {
		goto error;
	}
	buf = (char *) malloc(len);
	if (!buf) {
		goto error;
	}

	got = fread((void *) buf, (size_t) 1, (size_t) len, f);

	duk_push_lstring(ctx, buf, got);
	duk_push_string(ctx, filename);

	free(buf);
	buf = NULL;

	interactive_mode = 0;  /* global */

	rc = duk_safe_call(ctx, wrapped_compile_execute, 2 /*nargs*/, 1 /*nret*/);
	if (rc != DUK_EXEC_SUCCESS) {
		print_error(ctx, stderr);
		goto error;
	} else {
		duk_pop(ctx);
		retval = 0;
	}
	/* fall thru */

 cleanup:
	if (buf) {
		free(buf);
	}
	return retval;

 error:
	fprintf(stderr, "error in executing file %s\n", filename);
	fflush(stderr);
	goto cleanup;
}

int handle_file(duk_context *ctx, const char *filename) {
	FILE *f = NULL;
	int retval;

	f = fopen(filename, "rb");
	if (!f) {
		fprintf(stderr, "failed to open source file: %s\n", filename);
		fflush(stderr);
		goto error;
	}

	retval = handle_fh(ctx, f, filename);

	fclose(f);
	return retval;

 error:
	return -1;
}

#ifdef NO_READLINE
int handle_interactive(duk_context *ctx) {
	const char *prompt = "duk> ";
	char *buffer = NULL;
	int retval = 0;
	int rc;
	int got_eof = 0;

	duk_eval_string(ctx, GREET_CODE(" [no readline]"));
	duk_pop(ctx);

	buffer = (char *) malloc(LINEBUF_SIZE);
	if (!buffer) {
		fprintf(stderr, "failed to allocated a line buffer\n");
		fflush(stderr);
		retval = -1;
		goto done;
	}

	while (!got_eof) {
		size_t idx = 0;

		fwrite(prompt, 1, strlen(prompt), stdout);
		fflush(stdout);

		for (;;) {
			int c = fgetc(stdin);
			if (c == EOF) {
				got_eof = 1;
				break;
			} else if (c == '\n') {
				break;
			} else if (idx >= LINEBUF_SIZE) {
				fprintf(stderr, "line too long\n");
				fflush(stderr);
				retval = -1;
				goto done;
			} else {
				buffer[idx++] = (char) c;
			}
		}

		duk_push_lstring(ctx, buffer, idx);
		duk_push_string(ctx, "input");

		interactive_mode = 1;  /* global */

		rc = duk_safe_call(ctx, wrapped_compile_execute, 2 /*nargs*/, 1 /*nret*/);
		if (rc != DUK_EXEC_SUCCESS) {
			/* in interactive mode, write to stdout */
			print_error(ctx, stdout);
			retval = -1;  /* an error 'taints' the execution */
		} else {
			duk_pop(ctx);
		}
	}

 done:
	if (buffer) {
		free(buffer);
		buffer = NULL;
	}

	return retval;
}
#else  /* NO_READLINE */
int handle_interactive(duk_context *ctx) {
	const char *prompt = "duk> ";
	char *buffer = NULL;
	int retval = 0;
	int rc;

	duk_eval_string(ctx, GREET_CODE(""));
	duk_pop(ctx);

	/*
	 *  Note: using readline leads to valgrind-reported leaks inside
	 *  readline itself.  Execute code from an input file (and not
	 *  through stdin) for clean valgrind runs.
	 */

	rl_initialize();

	for (;;) {
		if (buffer) {
			free(buffer);
			buffer = NULL;
		}

		buffer = readline(prompt);
		if (!buffer) {
			break;
		}

		if (buffer && buffer[0] != (char) 0) {
			add_history(buffer);
		}

		duk_push_lstring(ctx, buffer, strlen(buffer));
		duk_push_string(ctx, "input");

		if (buffer) {
			free(buffer);
			buffer = NULL;
		}

		interactive_mode = 1;  /* global */

		rc = duk_safe_call(ctx, wrapped_compile_execute, 2 /*nargs*/, 1 /*nret*/);
		if (rc != DUK_EXEC_SUCCESS) {
			/* in interactive mode, write to stdout */
			print_error(ctx, stdout);
			retval = -1;  /* an error 'taints' the execution */
		} else {
			duk_pop(ctx);
		}
	}

	if (buffer) {
		free(buffer);
		buffer = NULL;
	}

	return retval;
}
#endif  /* NO_READLINE */

typedef duk_ret_t(*initfn)(duk_context*);

static duk_ret_t read_file(duk_context *ctx) {
	duk_idx_t top = duk_get_top(ctx);
	if (top != 1)
	{
		return DUK_RET_TYPE_ERROR;
	}
	const char *fileName = duk_get_string(ctx, -1);
	struct stat sb;
	if (stat(fileName, &sb))
	{
		return 0;
	}
	FILE *f = fopen(fileName, "r");
	if (!f)
	{
		return 0;
	}
	void *buffer = malloc(sb.st_size);
	size_t len = fread(buffer, 1, sb.st_size, f);
	duk_push_lstring(ctx, buffer, len);
	free(buffer);
	return 1;
}

static duk_ret_t load_native_module(duk_context *ctx) {
	duk_idx_t top = duk_get_top(ctx);
	if (top != 1)
	{
		return DUK_RET_TYPE_ERROR;
	}
	const char *file = duk_get_string(ctx, -1);
	void *lib = dlopen(file, RTLD_LOCAL);
	if (!lib)
	{
		return 0;
	}
	initfn init = (initfn)dlsym(lib, "dukopen_module");
	if (init)
	{
		return init(ctx);
	}
	return 0;
}

const char modSearch[] =
"Duktape.modSearch = function (id, require, exports, module) {\n"
"    var name;\n"
"    var src;\n"
"    var found = false;\n"
"\n"
"    // FIXME: Should look at various default search paths.\n"
"\n"
"    // Try to load a native library\n"
"    name = id + '.so';\n"
"    var lib = Duktape.loadNativeModule(name);\n"
"    if (!lib)\n"
"    {\n"
"       name = './' + id + '.so';\n"
"       lib = Duktape.loadNativeModule(name);\n"
"    }\n"
"    if (lib)\n"
"    {\n"
"        for(var prop in lib) {\n"
"            exports[prop] = lib[prop];\n"
"        }\n"
"        found = true;\n"
"    }\n"
"\n"
"    // Try to load a JavaScript library\n"
"    name = id + '.js';\n"
"    src = Duktape.readFile(name);\n"
"    if (typeof src === 'string')\n"
"    {\n"
"        found = true;\n"
"    }\n"
"\n"
"    if (!found)\n"
"    {\n"
"        throw new Error('module not found: ' + id);\n"
"    }\n"
"    return src;\n"
"}";


int main(int argc, char *argv[]) {
	duk_context *ctx = NULL;
	int retval = 0;
	int have_files = 0;
	int interactive = 0;
	int memlimit_high = 1;
	int i;

	/*
	 *  Signal handling setup
	 */

#ifndef NO_SIGNAL
	set_sigint_handler();

	/* This is useful at the global level; libraries should avoid SIGPIPE though */
	/*signal(SIGPIPE, SIG_IGN);*/
#endif

	/*
	 *  Parse options
	 */

	for (i = 1; i < argc; i++) {
		char *arg = argv[i];
		if (!arg) {
			goto usage;
		}
		if (strcmp(arg, "-r") == 0) {
			memlimit_high = 0;
		} else if (strcmp(arg, "-i") == 0) {
			interactive = 1;
		} else if (strlen(arg) >= 1 && arg[0] == '-') {
			goto usage;
		} else {
			have_files = 1;
		}
	}
	if (!have_files) {
		interactive = 1;
	}

	/*
	 *  Memory limit
	 */

#ifndef NO_RLIMIT
	set_resource_limits(memlimit_high ? MEM_LIMIT_HIGH : MEM_LIMIT_NORMAL);
#else
	(void) memlimit_high;  /* suppress warning */
#endif

	/*
	 *  Create context and execute any argument file(s)
	 */

	ctx = duk_create_heap_default();
	duk_push_global_object(ctx);
	duk_get_prop_string(ctx, -1, "Duktape");
	duk_push_c_function(ctx, load_native_module, 1);
	duk_put_prop_string(ctx, -2, "loadNativeModule");
	duk_push_c_function(ctx, read_file, 1);
	duk_put_prop_string(ctx, -2, "readFile");
	duk_pop(ctx);
	duk_pop(ctx);
	duk_eval_string(ctx, modSearch);
#if 0
	duk_ncurses_register(ctx);
	duk_socket_register(ctx);
	duk_fileio_register(ctx);
#endif

	for (i = 1; i < argc; i++) {
		char *arg = argv[i];
		if (!arg) {
			continue;
		} else if (strlen(arg) >= 1 && arg[0] == '-') {
			continue;
		}

		if (handle_file(ctx, arg) != 0) {
			retval = 1;
			goto cleanup;
		}
	}

	/*
	 *  Enter interactive mode if options indicate it
	 */

	if (interactive) {
		if (handle_interactive(ctx) != 0) {
			retval = 1;
			goto cleanup;
		}
	}

	/*
	 *  Cleanup and exit
	 */

 cleanup:
	if (interactive) {
		fprintf(stderr, "Cleaning up...\n");
		fflush(stderr);
	}

	if (ctx) {
		duk_destroy_heap(ctx);
	}

	return retval;

	/*
	 *  Usage
	 */

 usage:
	fprintf(stderr, "Usage: duk [-i] [-r] [<filenames>]\n"
	                "\n"
	                "   -i      enter interactive mode after executing argument file(s)\n"
	                "   -r      use lower memory limit (used by test runner)"
#ifdef NO_RLIMIT
	                " (disabled)\n"
#else
	                "\n"
#endif
	                "\n"
	                "If <filename> is omitted, interactive mode is started automatically.\n");
	fflush(stderr);
	exit(1);
}
