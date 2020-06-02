
/* Return the compiler identification, if possible. */

#include "Python.h"

#ifndef COMPILER

#ifdef __GNUC__
/* To not break compatibility with things that determine
   CPU arch by calling get_build_version in msvccompiler.py
   (such as NumPy) add "32 bit" or "64 bit (AMD64)" on Windows
   and also use a space as a separator rather than a newline. */
#if defined(_WIN32)
#define COMP_SEP " "
#if defined(__x86_64__)
#define ARCH_SUFFIX " 64 bit (AMD64)"
#else
#define ARCH_SUFFIX " 32 bit"
#endif
#else
#define COMP_SEP "\n"
#define ARCH_SUFFIX ""
#endif
#define COMPILER COMP_SEP "[GCC " __VERSION__ ARCH_SUFFIX "]"
#endif

#endif /* !COMPILER */

#ifndef COMPILER

#ifdef __cplusplus
#define COMPILER "[C++]"
#else
#define COMPILER "[C]"
#endif

#endif /* !COMPILER */

const char *
Py_GetCompiler(void)
{
    return COMPILER;
}
