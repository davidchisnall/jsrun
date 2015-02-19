
OBJECTS=duktape.o jsrun.o modules.o

all: ffigen jsrun

LLVM_CONFIG ?= llvm-config
CFLAGS+=-O0 -g

ffigen: ffigen.cc
	c++ -o ffigen ffigen.cc -I `${LLVM_CONFIG} --includedir` -L `${LLVM_CONFIG} --libdir` -lclang -std=c++11 -fblocks

jsrun: $(OBJECTS)
	cc -o jsrun -rdynamic $(OBJECTS) -ledit -lm

clean:
	rm -f jsrun ffigen $(OBJECTS)
