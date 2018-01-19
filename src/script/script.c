/* Must be included first */
#include <Python.h>

#include "public/script.h"
#include "../entity.h"

#include <stdio.h>

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyMethodDef pf_module_methods[] = {
    {NULL}  /* Sentinel */
};

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

PyMODINIT_FUNC initpf(void)
{
    PyObject *module;
    module = Py_InitModule("pf", pf_module_methods);
    if(!module)
        return;

    Entity_PyRegister(module);
}

bool S_Init(char *progname, const char *base_path)
{
    Py_SetProgramName(progname);
    Py_Initialize();

    initpf();

    return true;
}

void S_Shutdown(void)
{
    Py_Finalize();
}

bool S_RunFile(const char *path)
{
    FILE *script = fopen(path, "r");
    if(!script)
        return false;

    PyRun_SimpleFile(script, path); 
    fclose(script);
    return true;
}

