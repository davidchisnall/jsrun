Duktape FFI Experimentation
===========================

This repository contains a simple program that wraps the DukTape embedded
JavaScript interpreter and implements the required platform-specific
functionality for module loading.  It also includes a program that generates C
code for FFI.

Given a C file, the ffigen program writes (to standard output) a C file that
can be compiled into a library and will 

To compile the example test, run these commands:

	$ make
	$ cd examples
	$ ../ffigen test.c > generated.c 
	$ indent generated.c
	$ clang -g wrapper.c -fPIC -shared -o test.so

You can then run the `tst.js` example with jsrun and it will load the shared
library and be able to find the relevant functions.
