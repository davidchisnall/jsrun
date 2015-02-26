#!/bin/sh

# Try to split the input file into a filename and suffix and determine its type
# (C/C++ - currently no Objective-C support, though it would be reasonably easy
# to add)
PREFIX=`basename -s .c $1`
SUFFIX=.c
COMPILER=cc
if [ $PREFIX == $1 ] ; then
	PREFIX=`basename -s .cc $1`
	SUFFIX=.cc
	COMPILER=c++
fi
if [ $PREFIX == $1 ] ; then
	PREFIX=`basename -s .cpp $1`
	SUFFIX=.cpp
	COMPILER=c++
fi
if [ $PREFIX == $1 ] ; then
	echo Unable to determine suffix for $1
	exit 1
fi
OUTSOURCE=${PREFIX}_generated${SUFFIX}
OUTLIB=${PREFIX}.so

../ffigen $@ > $OUTSOURCE
shift
# This step isn't actually required, but it's useful if someone wants to look
# at the generated code.
indent $OUTSOURCE

if [ `uname` == 'Darwin' ] ; then
	RDYNAMIC="-undefined dynamic_lookup"
else
	RDYNAMIC=-rdynamic
fi
$COMPILER -g $@ $OUTSOURCE -fPIC $RDYNAMIC -I.. -shared -Wno-incompatible-pointer-types -o $OUTLIB
