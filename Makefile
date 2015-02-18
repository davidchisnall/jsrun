
all: ffigen jsrun

LLVM_CONFIG ?= llvm-config
CFLAGS+=-O0 -g

ffigen: ffigen.cc
	c++ -o ffigen ffigen.cc -I `${LLVM_CONFIG} --includedir` -L `${LLVM_CONFIG} --libdir` -lclang -std=c++11 -fblocks

jsrun: duktape.o duk_cmdline.o
	cc -o jsrun -rdynamic duktape.o duk_cmdline.o -ledit -lm

clean:
	rm -f jsrun ffigen duktape.o duk_cmdline.o
