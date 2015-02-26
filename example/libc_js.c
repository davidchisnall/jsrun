#include <sys/stat.h>
#include <stdio.h>
#include <dlfcn.h>
#include <dirent.h>

struct point {
	float x,y;
};

enum example {
	one=1, two, three
};

enum {
	alpha, bravo, charlie
};
