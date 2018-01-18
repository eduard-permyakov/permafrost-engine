/* Must be included first */
#include <Python.h>

#include "public/script.h"

bool S_Init(char *progname, const char *base_path)
{
    Py_SetProgramName(progname);
    Py_Initialize();

    return true;
}

void S_Shutdown(void)
{
    Py_Finalize();
}

bool S_RunFile(FILE *stream)
{
    return true;
}

