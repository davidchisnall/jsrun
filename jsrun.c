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


#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "jsrun.h"

bool interactive_mode;

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

	duk_push_global_object(ctx);  /* 'this' binding */
	duk_call_method(ctx, 0);

	if (interactive_mode) {
		fprintf(stdout, "= %s\n", duk_safe_to_string(ctx, -1));
		fflush(stdout);
	}

	duk_pop(ctx);
	return 0;
}

static int
handle_fh(duk_context *ctx, FILE *f, const char *filename) {
	char *buf = NULL;
	char *program;
	size_t len;
	size_t got;
	int rc;
	int retval = -1;

	if (fseek(f, 0, SEEK_END) < 0)
	{
		goto error;
	}
	len = ftell(f);
	if (fseek(f, 0, SEEK_SET) < 0)
	{
		goto error;
	}
	buf = malloc(len);
	if (!buf)
	{
		goto error;
	}

	got = fread(buf, 1, len, f);
	program = buf;

	// If this file starts with a #! then skip over the first line.
	if (program[0] == '#' && program[1] == '!')
	{
		while ((got > 0) && *program != '\n')
		{
			got--;
			program++;
		}
	}

	duk_push_lstring(ctx, program, got);
	duk_push_string(ctx, filename);

	free(buf);
	buf = NULL;

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
	return retval;

 error:
	fprintf(stderr, "error in executing file %s\n", filename);
	fflush(stderr);
	goto cleanup;
}

int
handle_file(duk_context *ctx, const char *filename)
{
	FILE *f;
	int retval;

	f = fopen(filename, "rb");
	if (f == NULL)
	{
		fprintf(stderr, "failed to open source file: %s\n", filename);
		fflush(stderr);
		return -1;
	}

	retval = handle_fh(ctx, f, filename);

	fclose(f);
	return retval;
}

int handle_interactive(duk_context *ctx) {
	const char *prompt = "javascript> ";
	char *buffer;
	int retval = 0;
	int rc;

	/*
	 *  Note: using readline leads to valgrind-reported leaks inside
	 *  readline itself.  Execute code from an input file (and not
	 *  through stdin) for clean valgrind runs.
	 */

	rl_initialize();

	for (;;)
	{
		buffer = readline(prompt);
		if (buffer != NULL)
		{
			break;
		}

		if (buffer[0] != '\0')
		{
			add_history(buffer);
		}

		duk_push_string(ctx, buffer);
		// The file name for the executing context
		// TODO: We could make stack traces more readable by adding a counter
		// in each input and setting that name here.
		duk_push_string(ctx, "input");

		free(buffer);
		buffer = NULL;

		interactive_mode = true;  /* global */

		rc = duk_safe_call(ctx, wrapped_compile_execute, 2 /*nargs*/, 1 /*nret*/);
		if (rc != DUK_EXEC_SUCCESS)
		{
			print_error(ctx, stdout);
			retval = -1;  /* an error 'taints' the execution */
		}
		else
		{
			duk_pop(ctx);
		}
	}

	return retval;
}

void
usage(void)
{
	fprintf(stderr, "Usage: duk [-i] [-l {bytes} ] [<filenames>]\n"
	                "\n"
	                "   -i         enter interactive mode after executing argument file(s)\n"
	                "\n"
	                "If <filename> is omitted, interactive mode is started automatically.\n");
	fflush(stderr);
	exit(1);
}

int main(int argc, char *argv[])
{
	duk_context *ctx;
	int retval = 0;
	bool have_file = false;
	bool interactive = false;
	bool memlimit_high = true;
	int ch;

	/*
	 *  Parse options
	 */
	if (argc < 2)
	{
		usage();
	}

	while ((ch = getopt(argc, argv, "ir")) != -1)
	{
		switch (ch)
		{
			case 'r':
				memlimit_high = false;
				break;
			case 'i':
				interactive = true;
				break;
			case '?':
			default:
				usage();
		}
	}
	argc -= optind;
	argv += optind;
	have_file = argc > 0;

	// Create the context
	ctx = duk_create_heap_default();
	init_default_objects(ctx);

	// Create an array containing all arguments after the 
	duk_push_global_object(ctx);
	duk_push_array(ctx);
	for (int i = 1; i < argc; i++)
	{
		duk_push_int(ctx, i-1);
		duk_push_string(ctx, argv[i]);
		duk_put_prop(ctx, -3);
	}
	duk_put_prop_string(ctx, -2, "program_arguments");
	duk_pop(ctx);


	duk_push_global_object(ctx);
	duk_push_array(ctx);


	if (have_file)
	{
		char *arg = argv[0];

		if (strlen(arg) == 1 && arg[0] == '-')
		{
			interactive = true;
		}
		else if (handle_file(ctx, arg) == 0)
		{
			interactive = false;
			run_message_loop(ctx);
		}
		else
		{
			retval = 1;
			interactive = false;
		}
	}


	/*
	 *  Enter interactive mode if options indicate it
	 */

	if (interactive)
	{
		if (handle_interactive(ctx) != 0)
		{
			retval = 1;
		}
		fprintf(stderr, "Cleaning up...\n");
		fflush(stderr);
	}

	duk_destroy_heap(ctx);

	return retval;
}
