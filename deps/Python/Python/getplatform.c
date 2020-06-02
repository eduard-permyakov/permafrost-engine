
#include "Python.h"

#ifdef __MINGW32__
#  undef PLATFORM
/* see PC/pyconfig.h */
#  define PLATFORM "win32"
#endif

#ifndef PLATFORM
#define PLATFORM "unknown"
#endif

const char *
Py_GetPlatform(void)
{
	return PLATFORM;
}
