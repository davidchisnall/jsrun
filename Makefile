
OBJECTS=duktape.o jsrun.o modules.o worker.o env.o typedarray.o

all: ffigen jsrun

LLVM_CONFIG ?= llvm-config
CFLAGS+=-O0 -g
#CFLAGS+=-O3 -DNDEBUG
#CXXFLAGS+=-O0 -g
CFLAGS+=-Werror -DDUK_OPT_UNDERSCORE_SETJMP=1

ffigen: ffigen.cc
	${CXX} ${CXXFLAGS} -o ffigen ffigen.cc -I `${LLVM_CONFIG} --includedir` -L `${LLVM_CONFIG} --libdir` -lclang -std=c++11

jsrun: $(OBJECTS)
	${CC} -o jsrun -rdynamic $(OBJECTS) -ledit -lm

clean:
	rm -f jsrun ffigen $(OBJECTS)
