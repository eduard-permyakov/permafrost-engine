/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

/* Some of the code is derived from the cPickle module implementation */

#include "script_pickle.h"
#include "traverse.h"
#include "private_types.h"
#include "../lib/public/vec.h"
#include "../asset_load.h"

#include <frameobject.h>
#include <assert.h>


#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))
#define TOP(_stk)   (vec_AT(_stk, vec_size(_stk)-1))
#define CHK_TRUE(_pred, _label) if(!(_pred)) goto _label
#define MIN(a, b)   ((a) < (b) ? (a) : (b))

#define SET_EXC(_type, ...)                                                     \
    do {                                                                        \
        char errbuff[1024];                                                     \
        off_t written = snprintf(errbuff, sizeof(errbuff),                      \
            "%s:%d: ", __FILE__, __LINE__);                                     \
        written += snprintf(errbuff + written, sizeof(errbuff) - written,       \
            __VA_ARGS__);                                                       \
        errbuff[sizeof(errbuff)-1] = '\0';                                      \
        PyErr_SetString(_type, errbuff);                                        \
    }while(0)

#define SET_RUNTIME_EXC(...) SET_EXC(PyExc_RuntimeError, __VA_ARGS__);

#define DEFAULT_ERR(_type, ...)                                                 \
    do {                                                                        \
        if(PyErr_Occurred())                                                    \
            break;                                                              \
        SET_EXC(_type, __VA_ARGS__);                                            \
    }while(0)

#define TRACE_OP(op, ctx)                                                       \
    do{                                                                         \
        PyObject *mod = PyDict_GetItemString(PySys_GetObject("modules"), "pf"); \
        PyObject *flag = PyObject_GetAttrString(mod, "trace_pickling");         \
        if(flag && !PyObject_IsTrue(flag)) {                                    \
            Py_DECREF(flag);                                                    \
            break;                                                              \
        }                                                                       \
        printf("[U] %-14s: [stack size: %4zu] [mark stack size: %4zu] (%s:%d)\n", \
            #op, vec_size(&ctx->stack), vec_size(&ctx->mark_stack),             \
            __FILE__, __LINE__);                                                \
        Py_DECREF(flag);                                                        \
    }while(0)

#define TRACE_PICKLE(obj)                                                       \
    do{                                                                         \
        PyObject *mod = PyDict_GetItemString(PySys_GetObject("modules"), "pf"); \
        PyObject *flag = PyObject_GetAttrString(mod, "trace_pickling");         \
        if(flag && !PyObject_IsTrue(flag)) {                                    \
            Py_DECREF(flag);                                                    \
            break;                                                              \
        }                                                                       \
        PyObject *repr = PyObject_Repr(obj);                                    \
        printf("[P] %-24s: (%-36s:%4d) [0x%08lx] %s\n", obj->ob_type->tp_name,  \
            __FILE__, __LINE__, (uintptr_t)obj, PyString_AS_STRING(repr));      \
        Py_DECREF(flag);                                                        \
        Py_DECREF(repr);                                                        \
    }while(0)

/* The original protocol 0 ASCII opcodes */

#define MARK            '(' /* push special markobject on stack                     */
#define STOP            '.' /* every pickle ends with STOP                          */
#define POP             '0' /* discard topmost stack item                           */
#define POP_MARK        '1' /* discard stack top through topmost markobject         */
#define DUP             '2' /* duplicate top stack item                             */
#define FLOAT           'F' /* push float object; decimal string argument           */
#define INT             'I' /* push integer or bool; decimal string argument        */
#define BININT          'J' /* push four-byte signed int                            */
#define BININT1         'K' /* push 1-byte unsigned int                             */
#define LONG            'L' /* push long; decimal string argument                   */
#define BININT2         'M' /* push 2-byte unsigned int                             */
#define NONE            'N' /* push None                                            */
#define PERSID          'P' /* push persistent object; id is taken from string arg  */
#define BINPERSID       'Q' /*  "       "         "  ;  "  "   "     "  stack       */
#define REDUCE          'R' /* apply callable to argtuple, both on stack            */
#define STRING          'S' /* push string; NL-terminated string argument           */
#define BINSTRING       'T' /* push string; counted binary string argument          */
#define SHORT_BINSTRING 'U' /*  "     "   ;    "      "       "      " < 256 bytes  */
#define UNICODE         'V' /* push Unicode string; raw-unicode-escaped'd argument  */
#define BINUNICODE      'X' /*   "     "       "  ; counted UTF-8 string argument   */
#define APPEND          'a' /* append stack top to list below it                    */
#define BUILD           'b' /* call __setstate__ or __dict__.update()               */
#define GLOBAL          'c' /* push self.find_class(modname, name); 2 string args   */
#define DICT            'd' /* build a dict from stack items                        */
#define EMPTY_DICT      '}' /* push empty dict                                      */
#define APPENDS         'e' /* extend list on stack by topmost stack slice          */
#define GET             'g' /* push item from memo on stack; index is string arg    */
#define BINGET          'h' /*   "    "    "    "   "   "  ;   "    " 1-byte arg    */
#define INST            'i' /* build & push class instance                          */
#define LONG_BINGET     'j' /* push item from memo on stack; index is 4-byte arg    */
#define LIST            'l' /* build list from topmost stack items                  */
#define EMPTY_LIST      ']' /* push empty list                                      */
#define OBJ             'o' /* build & push class instance                          */
#define PUT             'p' /* store stack top in memo; index is string arg         */
#define BINPUT          'q' /*   "     "    "   "   " ;   "    " 1-byte arg         */
#define LONG_BINPUT     'r' /*   "     "    "   "   " ;   "    " 4-byte arg         */
#define SETITEM         's' /* add key+value pair to dict                           */
#define TUPLE           't' /* build tuple from topmost stack items                 */
#define EMPTY_TUPLE     ')' /* push empty tuple                                     */
#define SETITEMS        'u' /* modify dict by adding topmost key+value pairs        */
#define BINFLOAT        'G' /* push float; arg is 8-byte float encoding             */

/* Permafrost Engine extensions to protocol 0 */

#define PF_EXTEND       'x' /* Interpret the next opcode as a Permafrost Engine extension opcode */
/* The extension opcodes: */
#define PF_PROTO        'a' /* identify pickle protocol version */
#define PF_TRUE         'b' /* push True */
#define PF_FALSE        'c' /* push False */
#define PF_GETATTR      'd' /* Get new reference to attribute(TOS) of object(TOS1) and push it on the stack */
#define PF_POPMARK      'e' /* Discard the top mark on the mark stack */
#define PF_SETATTRS     'f' /* Set attributes of object on TOS1 with dict at TOS */
#define PF_NOTIMPL      'g' /* Push NotImplemented */
#define PF_ELLIPSIS     'h' /* Push Ellipsis */

#define PF_BUILTIN      'A' /* Push new reference to built-in that is identified by its' fully-qualified name */
#define PF_TYPE         'B' /* Push new class created from the top 4 stack items (name, bases, dict, metaclass) */
#define PF_CODE         'C' /* Push code object from the 14 TOS items (the args to PyCode_New) */
#define PF_FUNCTION     'D' /* Push function object from TOS items */
#define PF_EMPTY_CELL   'E' /* Push empty cell */
#define PF_CELL         'E' /* Push cell with TOS contents */
#define PF_BYTEARRAY    'F' /* Push byte array from encoded string on TOS */
#define PF_SUPER        'G' /* Push super from 2 TOS items */
#define PF_EMPTYFUNC    'H' /* Push dummy function object */
#define PF_BASEOBJ      'I' /* Push an 'object' instance */
#define PF_SYSLONGINFO  'J' /* Push a 'sys.long_info' instance */
#define PF_NULLIMPORTER 'K' /* Push an imp.NullImporter instance */
#define PF_SYSFLOATINFO 'L' /* Push a 'sys.float_info' instance */
#define PF_SET          'M' /* Push a set from TOS tuple */
#define PF_FROZENSET    'N' /* Push a frozenset from TOS tuple */
#define PF_CLASS        'O' /* Push an old class created from the top 3 stack items (name, bases, methods) */
#define PF_INST         'P' /* Push 'instance' from 'classobj' and 'dict' on TOS */
#define PF_GETSETDESC   'Q' /* Push 'getset_descriptor' instance from top 3 stack items */
#define PF_MODULE       'R' /* PUsh module with name on TOS */
#define PF_NEWINST      'S' /* Create new-style instance with type on TOS and a builtin instance of the outer-most base on TOS1 */

#define EXC_START_MAGIC ((void*)0x1234)
#define EXC_END_MAGIC   ((void*)0x4321)

struct memo_entry{
    int idx;
    PyObject *obj;
};

VEC_TYPE(pobj, PyObject*)
VEC_IMPL(static inline, pobj, PyObject*)

VEC_TYPE(int, int)
VEC_IMPL(static inline, int, int)

VEC_TYPE(char, char)
VEC_IMPL(static inline, char, char)

KHASH_MAP_INIT_INT64(memo, struct memo_entry)

struct pickle_ctx{
    khash_t(memo) *memo;
    /* Any objects newly created during serialization must 
     * get pushed onto this buffer, to be decref'd during context
     * destruction. We wish to pickle them using the normal flow,
     * using memoization but if the references are not retained, 
     * the memory backing the object may be given to another object,
     * causing our memo entry to change unexpectedly. So we retain
     * all newly-created objects until pickling is done. */
    vec_pobj_t     to_free;
};

struct unpickle_ctx{
    vec_pobj_t     stack;
    vec_pobj_t     memo;
    vec_int_t      mark_stack;
    bool           stop;
};

typedef int (*pickle_func_t)(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *stream);
typedef int (*unpickle_func_t)(struct unpickle_ctx *ctx, SDL_RWops *stream);

struct pickle_entry{
    PyTypeObject  *type; 
    pickle_func_t  picklefunc;
};

extern PyTypeObject PyListIter_Type;
extern PyTypeObject PyListRevIter_Type;
extern PyTypeObject PyTupleIter_Type;
extern PyTypeObject PySTEntry_Type;


static bool pickle_obj(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *stream);
static void memoize(struct pickle_ctx *ctx, PyObject *obj);
static bool memo_contains(const struct pickle_ctx *ctx, PyObject *obj);
static int memo_idx(const struct pickle_ctx *ctx, PyObject *obj);
static bool emit_get(const struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw);
static bool emit_put(const struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw);

/* Pickling functions */
static int type_pickle        (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int bool_pickle        (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int string_pickle      (struct pickle_ctx *, PyObject *, SDL_RWops *); 
static int bytearray_pickle  (struct pickle_ctx *, PyObject *, SDL_RWops *); 
static int list_pickle        (struct pickle_ctx *, PyObject *, SDL_RWops *); 
static int super_pickle       (struct pickle_ctx *, PyObject *, SDL_RWops *); 
static int base_obj_pickle    (struct pickle_ctx *, PyObject *, SDL_RWops *); 
static int range_pickle       (struct pickle_ctx *, PyObject *, SDL_RWops *); 
static int dict_pickle        (struct pickle_ctx *, PyObject *, SDL_RWops *); 
static int set_pickle         (struct pickle_ctx *, PyObject *, SDL_RWops *);
#ifdef Py_USING_UNICODE
static int unicode_pickle     (struct pickle_ctx *, PyObject *, SDL_RWops *); 
#endif
static int slice_pickle       (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int static_method_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
#ifndef WITHOUT_COMPLEX
static int complex_pickle     (struct pickle_ctx *, PyObject *, SDL_RWops *);
#endif
static int float_pickle       (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int buffer_pickle      (struct pickle_ctx *, PyObject *, SDL_RWops *); 
static int long_pickle        (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int int_pickle         (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int frozen_set_pickle  (struct pickle_ctx *, PyObject *, SDL_RWops *); 
static int property_pickle    (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int memory_view_pickle (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int tuple_pickle       (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int enum_pickle        (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int reversed_pickle    (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int method_pickle      (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int function_pickle    (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int class_pickle       (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int gen_pickle         (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int instance_pickle    (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int file_pickle        (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int cell_pickle        (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int module_pickle      (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int get_set_descr_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int wrapper_descr_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int member_descr_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int dict_proxy_pickle  (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int long_info_pickle   (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int float_info_pickle  (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int sys_flags_pickle   (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int sys_version_pickle (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int cfunction_pickle   (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int code_pickle        (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int traceback_pickle   (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int frame_pickle       (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int null_importer_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int not_implemented_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int none_pickle        (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int ellipsis_pickle    (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int weakref_ref_pickle (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int weakref_callable_proxy_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int weakref_proxy_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int match_pickle       (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int pattern_pickle     (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int scanner_pickle     (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int zip_importer_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int st_entry_pickle    (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int class_method_descr_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int class_method_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int dict_items_pickle  (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int dict_keys_pickle   (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int dict_values_pickle (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int method_descr_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int call_iter_pickle   (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int seq_iter_pickle    (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int byte_array_iter_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int dict_iter_item_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int dict_iter_key_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int dict_iter_value_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int field_name_iter_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int formatter_iter_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int list_iter_pickle   (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int list_rev_iter_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int set_iter_pickle    (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int tuple_iter_pickle  (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int newclass_instance_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int placeholder_inst_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);

/* Unpickling functions */
static int op_int           (struct unpickle_ctx *, SDL_RWops *);
static int op_long          (struct unpickle_ctx *, SDL_RWops *);
static int op_stop          (struct unpickle_ctx *, SDL_RWops *);
static int op_string        (struct unpickle_ctx *, SDL_RWops *);
static int op_put           (struct unpickle_ctx *, SDL_RWops *);
static int op_get           (struct unpickle_ctx *, SDL_RWops *);
static int op_mark          (struct unpickle_ctx *, SDL_RWops *);
static int op_pop           (struct unpickle_ctx *, SDL_RWops *);
static int op_pop_mark      (struct unpickle_ctx *, SDL_RWops *);
static int op_tuple         (struct unpickle_ctx *, SDL_RWops *);
static int op_empty_tuple   (struct unpickle_ctx *, SDL_RWops *);
static int op_empty_list    (struct unpickle_ctx *, SDL_RWops *);
static int op_appends       (struct unpickle_ctx *, SDL_RWops *);
static int op_empty_dict    (struct unpickle_ctx *, SDL_RWops *);
static int op_setitems      (struct unpickle_ctx *, SDL_RWops *);
static int op_none          (struct unpickle_ctx *, SDL_RWops *);
static int op_unicode       (struct unpickle_ctx *, SDL_RWops *);

static int op_ext_builtin   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_type      (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_getattr   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_code      (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_function  (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_empty_cell(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_cell      (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_true      (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_false     (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_bytearray (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_super     (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_popmark   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_emptyfunc (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_baseobj   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_setattrs  (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_notimpl   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_ellipsis  (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_syslonginfo(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_sysfloatinfo(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_nullimporter(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_set       (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_frozenset (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_class     (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_inst      (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_getsetdesc(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_module    (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_newinst   (struct unpickle_ctx *, SDL_RWops *);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(str) *s_id_qualname_map;

static struct pickle_entry s_type_dispatch_table[] = {
    /* The Python 2.7 public built-in types. These types may be instantiated directly 
     * in any script. 
     */
    {.type = &PyType_Type,                  .picklefunc = type_pickle                   }, /* type() */
    {.type = &PyBool_Type,                  .picklefunc = bool_pickle                   }, /* bool() */
    {.type = &PyString_Type,                .picklefunc = string_pickle                 }, /* str() */
    {.type = &PyByteArray_Type,             .picklefunc = bytearray_pickle              }, /* bytearray() */
    {.type = &PyList_Type,                  .picklefunc = list_pickle                   }, /* list() */
    {.type = &PySuper_Type,                 .picklefunc = super_pickle                  }, /* super() */
    {.type = &PyBaseObject_Type,            .picklefunc = base_obj_pickle               }, /* object() */
    {.type = &PyRange_Type,                 .picklefunc = range_pickle                  }, /* xrange() */
    {.type = &PyDict_Type,                  .picklefunc = dict_pickle                   }, /* dict() */
    {.type = &PySet_Type,                   .picklefunc = set_pickle                    }, /* set() */
#ifdef Py_USING_UNICODE
    {.type = &PyUnicode_Type,               .picklefunc = unicode_pickle                }, /* unicode() */
#endif
    {.type = &PySlice_Type,                 .picklefunc = slice_pickle                  }, /* slice() */
    {.type = &PyStaticMethod_Type,          .picklefunc = static_method_pickle          }, /* staticmethod() */
#ifndef WITHOUT_COMPLEX
    {.type = &PyComplex_Type,               .picklefunc = complex_pickle                }, /* complex() */
#endif
    {.type = &PyFloat_Type,                 .picklefunc = float_pickle                  }, /* float() */
    {.type = &PyBuffer_Type,                .picklefunc = buffer_pickle                 }, /* buffer() */
    {.type = &PyLong_Type,                  .picklefunc = long_pickle                   }, /* long() */
    {.type = &PyInt_Type,                   .picklefunc = int_pickle                    }, /* int() */
    {.type = &PyFrozenSet_Type,             .picklefunc = frozen_set_pickle             }, /* frozenset() */
    {.type = &PyProperty_Type,              .picklefunc = property_pickle               }, /* property() */
    {.type = &PyMemoryView_Type,            .picklefunc = memory_view_pickle            }, /* memoryview() */
    {.type = &PyTuple_Type,                 .picklefunc = tuple_pickle                  }, /* tuple() */
    {.type = &PyEnum_Type,                  .picklefunc = enum_pickle                   }, /* enumerate() */
    {.type = &PyReversed_Type,              .picklefunc = reversed_pickle               }, /* reversed() */
    {.type = &PyMethod_Type,                .picklefunc = method_pickle                 }, /* indirectly: instance methods */ 
    {.type = &PyFunction_Type,              .picklefunc = function_pickle               }, /* indirectly: function */
    {.type = &PyClass_Type,                 .picklefunc = class_pickle                  }, /* indirectly: Old-style class */
    {.type = &PyGen_Type,                   .picklefunc = gen_pickle                    }, /* indirectly: return value from generator function */
    {.type = &PyInstance_Type,              .picklefunc = instance_pickle               }, /* instance() */
    {.type = &PyFile_Type,                  .picklefunc = file_pickle                   }, /* open() */
    {.type = &PyClassMethod_Type,           .picklefunc = class_method_pickle           }, /* classmethod() */
    {.type = &PyCell_Type,                  .picklefunc = cell_pickle                   },

    {.type = &PyModule_Type,                .picklefunc = module_pickle,                },

    /* These are from accessing the attributes of built-in types; created via PyDescr_ API*/
    {.type = &PyGetSetDescr_Type,           .picklefunc = get_set_descr_pickle          },
    {.type = &PyWrapperDescr_Type,          .picklefunc = wrapper_descr_pickle          },
    {.type = &PyMemberDescr_Type,           .picklefunc = member_descr_pickle           },
    {.type = NULL /* &PyClassMethodDescr_Type */, .picklefunc = class_method_descr_pickle},
    {.type = NULL /* &PyMethodDescr_Type */,      .picklefunc = method_descr_pickle      },

    /* This is a reference to C code. As such, we pickle by reference. */
    {.type = &PyCFunction_Type,             .picklefunc = cfunction_pickle              },
    /* Derived from function objects */
    {.type = &PyCode_Type,                  .picklefunc = code_pickle                   },
    /* These can be retained from sys.exc_info(): XXX: see creation paths */
    {.type = &PyTraceBack_Type,             .picklefunc = traceback_pickle              },
    {.type = &PyFrame_Type,                 .picklefunc = frame_pickle                  },
    {.type = &PyNullImporter_Type,          .picklefunc = null_importer_pickle          },

    /* Built-in singletons. These may not be instantiated directly  */
    /* The PyNotImplemented_Type and PyNone_Type are not exported. */
    {.type = NULL,                          .picklefunc = not_implemented_pickle        },
    {.type = NULL,                          .picklefunc = none_pickle                   },
    {.type = &PyEllipsis_Type,              .picklefunc = ellipsis_pickle               },

    /* The following are a result of calling the PyWeakref API with an existing object.
     * A weakly-refenreced object must be unpickled before weak references to it are restored. 
     */
    {.type = &_PyWeakref_RefType,           .picklefunc = weakref_ref_pickle            },
    {.type = &_PyWeakref_CallableProxyType, .picklefunc = weakref_callable_proxy_pickle },
    {.type = &_PyWeakref_ProxyType,         .picklefunc = weakref_proxy_pickle          },

    /* The following builtin types are are defined in Modules but are compiled
     * as part of the Python shared library. They may not be instantiated. */
    {.type = &PySTEntry_Type,               .picklefunc = st_entry_pickle               },
    {.type = NULL /* &Match_Type */,        .picklefunc = match_pickle                  },
    {.type = NULL /* &Pattern_Type */,      .picklefunc = pattern_pickle                },
    {.type = NULL /* &Scanner_Type */,      .picklefunc = scanner_pickle                },
    {.type = NULL /* &ZipImporter_Type */,  .picklefunc = zip_importer_pickle           },

    /* These are additional builtin types that are used internally in CPython
     * and modules compiled into the library. Python code may gain references to 
     * these 'opaque' objects but they may not be instantiated directly from scripts. 
     */

    /* This is derived from an existing dictionary object using the PyDictProxy API */
    {.type = &PyDictProxy_Type,             .picklefunc = dict_proxy_pickle             },

    /* Built-in struct sequences (i.e. 'named tuples') */
    {.type = NULL, /* Long_InfoType */      .picklefunc = long_info_pickle              },
    {.type = NULL, /* FloatInfoType */      .picklefunc = float_info_pickle             },

    /* The following are non-instantiatable named tuple singletons in 'sys' */
    {.type = NULL, /* FlagsType */          .picklefunc = sys_flags_pickle              },
    {.type = NULL, /* VersionInfoType */    .picklefunc = sys_version_pickle            },

    /* Derived with dict built-in methods */
    {.type = &PyDictItems_Type,             .picklefunc = dict_items_pickle             },
    {.type = &PyDictKeys_Type,              .picklefunc = dict_keys_pickle              },
    {.type = &PyDictValues_Type,            .picklefunc = dict_values_pickle            },

    /* Iterator types. Derived by calling 'iter' on an object. */
    {.type = &PyCallIter_Type,              .picklefunc = call_iter_pickle              },
    {.type = &PySeqIter_Type,               .picklefunc = seq_iter_pickle               },
    {.type = &PyByteArrayIter_Type,         .picklefunc = byte_array_iter_pickle        },
    {.type = &PyDictIterItem_Type,          .picklefunc = dict_iter_item_pickle         },
    {.type = &PyDictIterKey_Type,           .picklefunc = dict_iter_key_pickle          },
    {.type = &PyDictIterValue_Type,         .picklefunc = dict_iter_value_pickle        },
    {.type = &PyListIter_Type,              .picklefunc = list_iter_pickle              },
    {.type = &PyTupleIter_Type,             .picklefunc = tuple_iter_pickle             },
    {.type = &PyListRevIter_Type,           .picklefunc = list_rev_iter_pickle          },
    {.type = NULL /* &PySetIter_Type */,       .picklefunc = set_iter_pickle            },
    {.type = NULL /* &PyFieldNameIter_Type */, .picklefunc = field_name_iter_pickle     },
    {.type = NULL /* &PyFormatterIter_Type */, .picklefunc = formatter_iter_pickle      },

    /* A PyCObject cannot be instantiated directly, but may be exported by C
     * extension modules. As it is a wrapper around a raw memory address exported 
     * by some module, we cannot reliablly save and restore it 
     */
    {.type = &PyCObject_Type,               .picklefunc = NULL                          },
    /* Newer version of CObject */
    {.type = &PyCapsule_Type,               .picklefunc = NULL                          },

    /* The following built-in types can never be instantiated. 
     */
    {.type = &PyBaseString_Type,            .picklefunc = NULL                          },

    /* The built-in exception types. All of them can be instantiated directly.  */
    { EXC_START_MAGIC },
    {.type = NULL /* PyExc_BaseException */,              .picklefunc = NULL            },
    {.type = NULL /* PyExc_Exception */,                  .picklefunc = NULL            },
    {.type = NULL /* PyExc_StandardError */,              .picklefunc = NULL            },
    {.type = NULL /* PyExc_TypeError */,                  .picklefunc = NULL            },
    {.type = NULL /* PyExc_StopIteration */,              .picklefunc = NULL            },
    {.type = NULL /* PyExc_GeneratorExit */,              .picklefunc = NULL            },
    {.type = NULL /* PyExc_SystemExit */,                 .picklefunc = NULL            },
    {.type = NULL /* PyExc_KeyboardInterrupt */,          .picklefunc = NULL            },
    {.type = NULL /* PyExc_ImportError */,                .picklefunc = NULL            },
    {.type = NULL /* PyExc_EnvironmentError */,           .picklefunc = NULL            },
    {.type = NULL /* PyExc_IOError */,                    .picklefunc = NULL            },
    {.type = NULL /* PyExc_OSError */,                    .picklefunc = NULL            },
#ifdef MS_WINDOWS
    {.type = NULL /* PyExc_WindowsError */,               .picklefunc = NULL            },
#endif
#ifdef __VMS
    {.type = NULL /* PyExc_VMSError */,                   .picklefunc = NULL            },
#endif
    {.type = NULL /* PyExc_EOFError */,                   .picklefunc = NULL            },
    {.type = NULL /* PyExc_RuntimeError */,               .picklefunc = NULL            },
    {.type = NULL /* PyExc_NotImplementedError */,        .picklefunc = NULL            },
    {.type = NULL /* PyExc_NameError */,                  .picklefunc = NULL            },
    {.type = NULL /* PyExc_UnboundLocalError */,          .picklefunc = NULL            },
    {.type = NULL /* PyExc_AttributeError */,             .picklefunc = NULL            },
    {.type = NULL /* PyExc_SyntaxError */,                .picklefunc = NULL            },
    {.type = NULL /* PyExc_IndentationError */,           .picklefunc = NULL            },
    {.type = NULL /* PyExc_TabError */,                   .picklefunc = NULL            },
    {.type = NULL /* PyExc_LookupError */,                .picklefunc = NULL            },
    {.type = NULL /* PyExc_IndexError */,                 .picklefunc = NULL            },
    {.type = NULL /* PyExc_KeyError */,                   .picklefunc = NULL            },
    {.type = NULL /* PyExc_ValueError */,                 .picklefunc = NULL            },
    {.type = NULL /* PyExc_UnicodeError */,               .picklefunc = NULL            },
#ifdef Py_USING_UNICODE
    {.type = NULL /* PyExc_UnicodeEncodeError */,         .picklefunc = NULL            },
    {.type = NULL /* PyExc_UnicodeDecodeError */,         .picklefunc = NULL            },
    {.type = NULL /* PyExc_UnicodeTranslateError */,      .picklefunc = NULL            },
#endif
    {.type = NULL /* PyExc_AssertionError */,             .picklefunc = NULL            },
    {.type = NULL /* PyExc_ArithmeticError */,            .picklefunc = NULL            },
    {.type = NULL /* PyExc_FloatingPointError */,         .picklefunc = NULL            },
    {.type = NULL /* PyExc_OverflowError */,              .picklefunc = NULL            },
    {.type = NULL /* PyExc_ZeroDivisionError */,          .picklefunc = NULL            },
    {.type = NULL /* PyExc_SystemError */,                .picklefunc = NULL            },
    {.type = NULL /* PyExc_ReferenceError */,             .picklefunc = NULL            },
    {.type = NULL /* PyExc_MemoryError */,                .picklefunc = NULL            },
    {.type = NULL /* PyExc_BufferError */,                .picklefunc = NULL            },
    {.type = NULL /* PyExc_Warning */,                    .picklefunc = NULL            },
    {.type = NULL /* PyExc_UserWarning */,                .picklefunc = NULL            },
    {.type = NULL /* PyExc_DeprecationWarning */,         .picklefunc = NULL            },
    {.type = NULL /* PyExc_PendingDeprecationWarning */,  .picklefunc = NULL            },
    {.type = NULL /* PyExc_SyntaxWarning */,              .picklefunc = NULL            },
    {.type = NULL /* PyExc_RuntimeWarning */,             .picklefunc = NULL            },
    {.type = NULL /* PyExc_FutureWarning */,              .picklefunc = NULL            },
    {.type = NULL /* PyExc_ImportWarning */,              .picklefunc = NULL            },
    {.type = NULL /* PyExc_UnicodeWarning */,             .picklefunc = NULL            },
    {.type = NULL /* PyExc_BytesWarning */,               .picklefunc = NULL            },
    { EXC_END_MAGIC },
};

/* An 'empty' user-defined type that acts as a placeholder for engine-defiend types for now */
static PyObject *s_placeholder_type = NULL;

/* The permafrost engine built-in types: defer handling of these for now */
static struct pickle_entry s_pf_dispatch_table[] = {
    {.type = NULL, /* PyEntity_type */      .picklefunc = placeholder_inst_pickle      },
    {.type = NULL, /* PyAnimEntity_type */  .picklefunc = placeholder_inst_pickle      },
    {.type = NULL, /* PyCombatableEntity_type */ .picklefunc = placeholder_inst_pickle },
    {.type = NULL, /* PyTile_type */        .picklefunc = placeholder_inst_pickle      },
    {.type = NULL, /* PyWindow_type */      .picklefunc = placeholder_inst_pickle      },
    {.type = NULL, /* PyUIButtonStyle_type */ .picklefunc = placeholder_inst_pickle    },
};

static unpickle_func_t s_op_dispatch_table[256] = {
    [INT] = op_int,
    [LONG] = op_long,
    [STOP] = op_stop,
    [STRING] = op_string,
    [GET] = op_get,
    [PUT] = op_put,
    [MARK] = op_mark,
    [POP] = op_pop,
    [POP_MARK] = op_pop_mark,
    [TUPLE] = op_tuple,
    [EMPTY_TUPLE] = op_empty_tuple,
    [EMPTY_LIST] = op_empty_list,
    [APPENDS] = op_appends,
    [EMPTY_DICT] = op_empty_dict,
    [SETITEMS] = op_setitems,
    [NONE] = op_none,
    [UNICODE] = op_unicode,
};

static unpickle_func_t s_ext_op_dispatch_table[256] = {
    [PF_BUILTIN] = op_ext_builtin,
    [PF_TYPE] = op_ext_type,
    [PF_GETATTR] = op_ext_getattr,
    [PF_CODE] = op_ext_code,
    [PF_FUNCTION] = op_ext_function,
    [PF_EMPTY_CELL] = op_ext_empty_cell,
    [PF_CELL] = op_ext_cell,
    [PF_TRUE] = op_ext_true,
    [PF_FALSE] = op_ext_false,
    [PF_BYTEARRAY] = op_ext_bytearray,
    [PF_SUPER] = op_ext_super,
    [PF_POPMARK] = op_ext_popmark,
    [PF_EMPTYFUNC] = op_ext_emptyfunc,
    [PF_BASEOBJ] = op_ext_baseobj,
    [PF_SETATTRS] = op_ext_setattrs,
    [PF_NOTIMPL] = op_ext_notimpl,
    [PF_ELLIPSIS] = op_ext_ellipsis,
    [PF_SYSLONGINFO] = op_ext_syslonginfo,
    [PF_NULLIMPORTER] = op_ext_nullimporter,
    [PF_SYSFLOATINFO] = op_ext_sysfloatinfo,
    [PF_SET] = op_ext_set,
    [PF_FROZENSET] = op_ext_frozenset,
    [PF_CLASS] = op_ext_class,
    [PF_INST] = op_ext_inst,
    [PF_GETSETDESC] = op_ext_getsetdesc,
    [PF_MODULE] = op_ext_module,
    [PF_NEWINST] = op_ext_newinst,
};

/* Standard modules not imported on initialization which also contain C builtins */
static const char *s_extra_indexed_mods[] = {
    "imp",
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool type_is_builtin(PyObject *type)
{
    assert(PyType_Check(type));

    for(int i = 0; i < ARR_SIZE(s_type_dispatch_table); i++) {

        if(s_type_dispatch_table[i].type == (PyTypeObject*)type)
            return true;
    }
    for(int i = 0; i < ARR_SIZE(s_pf_dispatch_table); i++) {

        if(s_pf_dispatch_table[i].type == (PyTypeObject*)type)
            return true;
    }
    return false;
}

static int dispatch_idx_for_picklefunc(pickle_func_t pf)
{
    for(int i = 0; i < ARR_SIZE(s_type_dispatch_table); i++) {
        if(s_type_dispatch_table[i].picklefunc == pf)
            return i;
    }
    return -1;
}

/* Some of the built-in types are declared 'static' but can still be referenced
 * via scripting. An example is the 'method_descriptor' type. We can still get 
 * a pointer to the type via the API and use that for matching purposes.
 */
static void load_private_type_refs(void)
{
    int idx;
    PyObject *tmp = NULL;

    /* PyMethodDescr_Type */
    idx = dispatch_idx_for_picklefunc(method_descr_pickle);
    tmp = PyDescr_NewMethod(&PyType_Type, PyType_Type.tp_methods);
    assert(tmp);
    assert(!strcmp(tmp->ob_type->tp_name, "method_descriptor"));
    s_type_dispatch_table[idx].type = tmp->ob_type;
    Py_DECREF(tmp);

    /* PyClassMethodDescr_Type */
    idx = dispatch_idx_for_picklefunc(class_method_descr_pickle);
    tmp = PyDescr_NewClassMethod(&PyType_Type, PyType_Type.tp_methods);
    assert(tmp);
    assert(!strcmp(tmp->ob_type->tp_name, "classmethod_descriptor"));
    s_type_dispatch_table[idx].type = tmp->ob_type;
    Py_DECREF(tmp);

    /* PyNone_Type */
    idx = dispatch_idx_for_picklefunc(none_pickle);
    tmp = Py_None;
    assert(!strcmp(tmp->ob_type->tp_name, "NoneType"));
    s_type_dispatch_table[idx].type = tmp->ob_type;

    /* PyNotImplemented_Type */
    idx = dispatch_idx_for_picklefunc(not_implemented_pickle);
    tmp = Py_NotImplemented;
    assert(!strcmp(tmp->ob_type->tp_name, "NotImplementedType"));
    s_type_dispatch_table[idx].type = tmp->ob_type;

    /* Long_InfoType */
    idx = dispatch_idx_for_picklefunc(long_info_pickle);
    tmp = PyLong_GetInfo();
    assert(!strcmp(tmp->ob_type->tp_name, "sys.long_info"));
    s_type_dispatch_table[idx].type = tmp->ob_type;
    Py_DECREF(tmp);

    /* FloatInfoType */
    idx = dispatch_idx_for_picklefunc(float_info_pickle);
    tmp = PyFloat_GetInfo();
    assert(!strcmp(tmp->ob_type->tp_name, "sys.float_info"));
    s_type_dispatch_table[idx].type = tmp->ob_type;
    Py_DECREF(tmp);

    /* FlagsType */
    idx = dispatch_idx_for_picklefunc(sys_flags_pickle);
    tmp = PySys_GetObject("flags"); /* borrowed */
    assert(!strcmp(tmp->ob_type->tp_name, "sys.flags"));
    s_type_dispatch_table[idx].type = tmp->ob_type;

    /* VersionInfoType */
    idx = dispatch_idx_for_picklefunc(sys_version_pickle);
    tmp = PySys_GetObject("version_info"); /* borrowed */
    assert(!strcmp(tmp->ob_type->tp_name, "sys.version_info"));
    s_type_dispatch_table[idx].type = tmp->ob_type;

    assert(!PyErr_Occurred());
}

static void load_exception_types(void)
{
    int base_idx = -1;
    for(int i = 0; i < ARR_SIZE(s_type_dispatch_table); i++) {
    
        if(s_type_dispatch_table[i].type == EXC_START_MAGIC) {
            base_idx = i + 1;
            break;
        }
    }
    assert(base_idx >= 0);

    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_BaseException;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_Exception;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_StandardError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_TypeError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_StopIteration;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_GeneratorExit;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_SystemExit;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_KeyboardInterrupt;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_ImportError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_EnvironmentError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_IOError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_OSError;
#ifdef MS_WINDOWS
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_WindowsError;
#endif
#ifdef __VMS
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_VMSError;
#endif
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_EOFError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_RuntimeError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_NotImplementedError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_NameError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_UnboundLocalError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_AttributeError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_SyntaxError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_IndentationError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_TabError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_LookupError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_IndexError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_KeyError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_ValueError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_UnicodeError;
#ifdef Py_USING_UNICODE
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_UnicodeEncodeError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_UnicodeDecodeError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_UnicodeTranslateError;
#endif
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_AssertionError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_ArithmeticError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_FloatingPointError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_OverflowError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_ZeroDivisionError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_SystemError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_ReferenceError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_MemoryError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_BufferError;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_Warning;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_UserWarning;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_DeprecationWarning;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_PendingDeprecationWarning;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_SyntaxWarning;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_RuntimeWarning;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_FutureWarning;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_ImportWarning;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_UnicodeWarning;
    s_type_dispatch_table[base_idx++].type = (PyTypeObject*) PyExc_BytesWarning;

    assert(s_type_dispatch_table[base_idx].type == EXC_END_MAGIC);
}

static void load_engine_builtin_types(void)
{
    PyObject *pfmod = PyDict_GetItemString(PySys_GetObject("modules"), "pf");
    assert(pfmod); /* borrowed ref */
    s_pf_dispatch_table[0].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "Entity");
    s_pf_dispatch_table[1].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "AnimEntity");
    s_pf_dispatch_table[2].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "CombatableEntity");
    s_pf_dispatch_table[3].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "Tile");
    s_pf_dispatch_table[4].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "Window");
    s_pf_dispatch_table[5].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "UIButtonStyle");

    for(int i = 0; i < ARR_SIZE(s_pf_dispatch_table); i++)
        assert(s_pf_dispatch_table[i].type);
    assert(!PyErr_Occurred());
}

static void pre_build_index(void)
{
    /* Import all the modules that have C builtins that are not 
     * in sys.modules at initialization time so that their builtins, 
     * too, can be indexed.  
     */
    for(int i = 0; i < ARR_SIZE(s_extra_indexed_mods); i++) {

        PyObject *mod = PyImport_ImportModule(s_extra_indexed_mods[i]);
        assert(mod && mod->ob_refcnt == 2);
        Py_DECREF(mod); /* remains cached in sys.modules */
    }
}

static void post_build_index(void)
{
    /* Knowing that we have not cached any references to the
     * modules since importing them, it is safe to delete
     * them from sys.modules (and thus have the module object
     * garbage-collected) 
     */
    PyObject *sysmods = PySys_GetObject("modules");

    for(int i = 0; i < ARR_SIZE(s_extra_indexed_mods); i++) {
    
        assert(PyDict_GetItemString(sysmods, s_extra_indexed_mods[i])->ob_refcnt == 1);
        PyDict_DelItemString(sysmods, s_extra_indexed_mods[i]);
    }
}

//TODO: dynamically loads mods from s_extra_indexed_mods
static PyObject *qualname_new_ref(const char *qualname)
{
    char copy[strlen(qualname)];
    strcpy(copy, qualname);

    const char *modname = copy;
    char *curr = strstr(copy, ".");
    if(curr)
        *curr++ = '\0';

    PyObject *modules_dict = PySys_GetObject("modules"); /* borrowed */
    assert(modules_dict);
    PyObject *mod = PyDict_GetItemString(modules_dict, modname);
    Py_XINCREF(mod);
    if(!mod) {
        SET_RUNTIME_EXC("Could not find module %s for qualified name %s", modname, qualname);
        return NULL;
    }

    PyObject *parent = mod;
    while(curr) {
        char *end = strstr(curr, ".");
        if(end)
            *end++ = '\0';
    
        if(!PyObject_HasAttrString(parent, curr)) {
            Py_DECREF(parent);
            SET_RUNTIME_EXC("Could not look up attribute %s in qualified name %s", curr, qualname);
            return NULL;
        }

        PyObject *attr = PyObject_GetAttrString(parent, curr);
        Py_DECREF(parent);
        parent = attr;
        curr = end;
    }

    return parent;
}

/* Non-derived attributes are those that don't return a new 
 * object on attribute lookup. 
 * This function returns a new reference.
 */
static PyObject *nonderived_writable_attrs(PyObject *obj)
{
    PyObject *attrs = PyObject_Dir(obj);
    assert(attrs);
    PyObject *ret = PyDict_New();
    assert(ret);

    for(int i = 0; i < PyList_Size(attrs); i++) {

        PyObject *name = PyList_GET_ITEM(attrs, i); /* borrowed */
        assert(PyString_Check(name));
        if(!PyObject_HasAttr(obj, name))
            continue;

        PyObject *attr = PyObject_GetAttr(obj, name);
        assert(attr);

        /* This is a 'derived' attribute */
        if(attr->ob_refcnt == 1) {
            Py_DECREF(attr);
            continue;
        }

        /* Try to write the attribute to itself. This will throw TypeError
         * or AttributeError if the attribute is not writable. */
        if(0 != PyObject_SetAttr(obj, name, attr)) {
            assert(PyErr_Occurred());        
            assert(PyErr_ExceptionMatches(PyExc_TypeError)
                || PyErr_ExceptionMatches(PyExc_AttributeError));
            PyErr_Clear();
            Py_DECREF(attr);
            continue;
        }

        PyDict_SetItem(ret, name, attr);
        Py_DECREF(attr);
    }

    Py_DECREF(attrs);
    return ret;
}

static int builtin_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    uintptr_t id = (uintptr_t)obj;
    khiter_t k = kh_get(str, s_id_qualname_map, id);
    if(k == kh_end(s_id_qualname_map)) {
    
        PyObject *repr = PyObject_Repr(obj);
        SET_RUNTIME_EXC("Could not find built-in qualified name in index: %s", 
            PyString_AS_STRING(repr));
        Py_DECREF(repr);
        return -1;
    }

    const char xtend = PF_EXTEND;
    const char builtin = PF_BUILTIN;
    const char *qname = kh_value(s_id_qualname_map, k);

    CHK_TRUE(rw->write(rw, &xtend, 1, 1), fail);
    CHK_TRUE(rw->write(rw, &builtin, 1, 1), fail);
    CHK_TRUE(rw->write(rw, qname, strlen(qname), 1), fail);
    CHK_TRUE(rw->write(rw, "\n", 1, 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static PyObject *method_func(PyObject *obj, const char *name)
{
    PyObject *attr = PyObject_GetAttrString(obj, name);
    assert(attr);

    if(PyMethod_Check(attr)) {

        PyMethodObject *meth = (PyMethodObject*)attr;
        Py_INCREF(meth->im_func);
        Py_DECREF(attr);
        return meth->im_func;

    }else{
        return attr;
    }
}

static int type_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    if(type_is_builtin(obj))
        return builtin_pickle(ctx, obj, rw); 

    TRACE_PICKLE(obj);
    assert(PyType_Check(obj));
    PyTypeObject *type = (PyTypeObject*)obj;

    /* push name */
    PyObject *name = PyString_FromString(type->tp_name);
    vec_pobj_push(&ctx->to_free, name);
    CHK_TRUE(pickle_obj(ctx, name, rw), fail);

    /* push tuple of base classes */
    PyObject *bases = type->tp_bases;
    assert(bases);
    assert(PyTuple_Check(bases));
    CHK_TRUE(pickle_obj(ctx, bases, rw), fail);

    /* Push dict */
    PyObject *dict = PyDict_New();
    vec_pobj_push(&ctx->to_free, dict);

    if(PyObject_HasAttrString(obj, "__slots__")) {
        PyObject *slots = PyObject_GetAttrString(obj, "__slots__");
        PyDict_SetItemString(dict, "__slots__", slots);
        Py_DECREF(slots);
    }

    if(PyObject_HasAttrString(obj, "__init__")) {
        PyObject *init = method_func(obj, "__init__");
        PyDict_SetItemString(dict, "__init__", init);
        Py_DECREF(init);
    }

    if(PyObject_HasAttrString(obj, "__new__")) {
        PyObject *newm = method_func(obj, "__new__");
        PyDict_SetItemString(dict, "__new__", newm);
        Py_DECREF(newm);
    }

    CHK_TRUE(pickle_obj(ctx, dict, rw), fail);

    /* Push metaclass: The '__metaclass__' attribute if it exists, else 'type' */
    if(PyObject_HasAttrString(obj, "__metaclass__")) {

        PyObject *meta = PyObject_GetAttrString(obj, "__metaclass__");
        assert(PyType_Check(meta));
        bool ret = pickle_obj(ctx, meta, rw);
        Py_DECREF(meta);
        CHK_TRUE(ret, fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, (PyObject*)&PyType_Type, rw), fail);
    }

    const char ops[] = {PF_EXTEND, PF_TYPE};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int bool_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(obj == Py_True || obj == Py_False);
    const char true_ops[] = {PF_EXTEND, PF_TRUE};
    const char false_ops[] = {PF_EXTEND, PF_FALSE};

    if(obj == Py_True) {
        CHK_TRUE(rw->write(rw, true_ops, ARR_SIZE(true_ops), 1), fail);
    }else {
        CHK_TRUE(rw->write(rw, false_ops, ARR_SIZE(false_ops), 1), fail);
    }
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int string_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    char op = STRING;
    char *repr_str;
    PyObject *repr = NULL;

    if (NULL == (repr = PyObject_Repr((PyObject*)obj))) {
        assert(PyErr_Occurred());
        return -1;
    }
    repr_str = PyString_AS_STRING((PyStringObject *)repr);

    CHK_TRUE(rw->write(rw, &op, 1, 1), fail);
    CHK_TRUE(rw->write(rw, repr_str, strlen(repr_str), 1), fail);
    CHK_TRUE(rw->write(rw, "\n", 1, 1), fail);

    Py_XDECREF(repr);
    return 0;

fail:
    Py_XDECREF(repr);
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int bytearray_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    char *buff = PyByteArray_AS_STRING(obj);
    size_t len = PyByteArray_GET_SIZE(obj);
    PyObject *str = PyString_Encode(buff, len, "UTF-8", "strict");
    if(!str)
        goto fail;
    vec_pobj_push(&ctx->to_free, str);
    if(!pickle_obj(ctx, str, rw))
        goto fail;

    const char ops[] = {PF_EXTEND, PF_BYTEARRAY};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int list_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    const char empty_list = EMPTY_LIST;
    const char appends = APPENDS;
    const char mark = MARK;

    assert(PyList_Check(obj));
    CHK_TRUE(rw->write(rw, &empty_list, 1, 1), fail);

    if(PyList_Size(obj) == 0)
        return 0;

    /* Memoize the empty list before pickling the elements. The elements may 
     * reference the list itself. */
    memoize(ctx, obj);
    CHK_TRUE(emit_put(ctx, obj, rw), fail);

    if(rw->write(rw, &mark, 1, 1) < 0)
        goto fail;

    for(int i = 0; i < PyList_Size(obj); i++) {
    
        PyObject *elem = PyList_GET_ITEM(obj, i);
        assert(elem);
        if(!pickle_obj(ctx, elem, rw)) {
            assert(PyErr_Occurred());
            return -1;
        }
    }

    CHK_TRUE(rw->write(rw, &appends, 1, 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int super_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    superobject *su = (superobject*)obj;
    CHK_TRUE(pickle_obj(ctx, (PyObject*)su->type, rw), fail);
    CHK_TRUE(pickle_obj(ctx, su->obj, rw), fail);

    const char ops[] = {PF_EXTEND, PF_SUPER};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int base_obj_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    const char ops[] = {PF_EXTEND, PF_BASEOBJ};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int range_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
} 

static int dict_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    const char empty_dict = EMPTY_DICT;
    const char setitems = SETITEMS;
    const char mark = MARK;

    assert(PyDict_Check(obj));
    CHK_TRUE(rw->write(rw, &empty_dict, 1, 1), fail);

    if(PyDict_Size(obj) == 0)
        return 0;

    /* Memoize the empty dict before pickling the elements. The elements may 
     * reference the list itself. */
    memoize(ctx, obj);
    CHK_TRUE(emit_put(ctx, obj, rw), fail);
    CHK_TRUE(rw->write(rw, &mark, 1, 1), fail);

    PyObject *key, *value;
    Py_ssize_t pos = 0;

    while(PyDict_Next(obj, &pos, &key, &value)) {
    
        if(!pickle_obj(ctx, key, rw)) {
            assert(PyErr_Occurred());
            return -1;
        }

        if(!pickle_obj(ctx, value, rw)) {
            assert(PyErr_Occurred());
            return -1;
        }
    }

    CHK_TRUE(rw->write(rw, &setitems, 1, 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int set_elems_pickle(struct pickle_ctx *ctx, PyObject *anyset, SDL_RWops *rw)
{
    size_t nitems = PySet_Size(anyset);
    PyObject *ret = PyTuple_New(nitems);
    CHK_TRUE(ret, fail);
    vec_pobj_push(&ctx->to_free, ret);

    PyObject *key;
    Py_ssize_t pos = 0;
    int i = 0;

    while(_PySet_Next(anyset, &pos, &key)) {

        Py_INCREF(key);
        PyTuple_SET_ITEM(ret, i++, key);
    }
    CHK_TRUE(pickle_obj(ctx, ret, rw), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int set_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    assert(PySet_Check(obj));
    CHK_TRUE(0 == set_elems_pickle(ctx, obj, rw), fail);

    const char ops[] = {PF_EXTEND, PF_SET};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

#ifdef Py_USING_UNICODE

/* from cPickle:
   A copy of PyUnicode_EncodeRawUnicodeEscape() that also translates
   backslash and newline characters to \uXXXX escapes. */
static PyObject *
modified_EncodeRawUnicodeEscape(const Py_UNICODE *s, Py_ssize_t size)
{
    PyObject *repr;
    char *p;
    char *q;

    static const char *hexdigit = "0123456789abcdef";
#ifdef Py_UNICODE_WIDE
    const Py_ssize_t expandsize = 10;
#else
    const Py_ssize_t expandsize = 6;
#endif

    if (size > PY_SSIZE_T_MAX / expandsize)
        return PyErr_NoMemory();

    repr = PyString_FromStringAndSize(NULL, expandsize * size);
    if (repr == NULL)
        return NULL;
    if (size == 0)
        return repr;

    p = q = PyString_AS_STRING(repr);
    while (size-- > 0) {
        Py_UNICODE ch = *s++;
#ifdef Py_UNICODE_WIDE
        /* Map 32-bit characters to '\Uxxxxxxxx' */
        if (ch >= 0x10000) {
            *p++ = '\\';
            *p++ = 'U';
            *p++ = hexdigit[(ch >> 28) & 0xf];
            *p++ = hexdigit[(ch >> 24) & 0xf];
            *p++ = hexdigit[(ch >> 20) & 0xf];
            *p++ = hexdigit[(ch >> 16) & 0xf];
            *p++ = hexdigit[(ch >> 12) & 0xf];
            *p++ = hexdigit[(ch >> 8) & 0xf];
            *p++ = hexdigit[(ch >> 4) & 0xf];
            *p++ = hexdigit[ch & 15];
        }
        else
#else
        /* Map UTF-16 surrogate pairs to '\U00xxxxxx' */
        if (ch >= 0xD800 && ch < 0xDC00) {
            Py_UNICODE ch2;
            Py_UCS4 ucs;

            ch2 = *s++;
            size--;
            if (ch2 >= 0xDC00 && ch2 <= 0xDFFF) {
                ucs = (((ch & 0x03FF) << 10) | (ch2 & 0x03FF)) + 0x00010000;
                *p++ = '\\';
                *p++ = 'U';
                *p++ = hexdigit[(ucs >> 28) & 0xf];
                *p++ = hexdigit[(ucs >> 24) & 0xf];
                *p++ = hexdigit[(ucs >> 20) & 0xf];
                *p++ = hexdigit[(ucs >> 16) & 0xf];
                *p++ = hexdigit[(ucs >> 12) & 0xf];
                *p++ = hexdigit[(ucs >> 8) & 0xf];
                *p++ = hexdigit[(ucs >> 4) & 0xf];
                *p++ = hexdigit[ucs & 0xf];
                continue;
            }
            /* Fall through: isolated surrogates are copied as-is */
            s--;
            size++;
        }
#endif
        /* Map 16-bit characters to '\uxxxx' */
        if (ch >= 256 || ch == '\\' || ch == '\n') {
            *p++ = '\\';
            *p++ = 'u';
            *p++ = hexdigit[(ch >> 12) & 0xf];
            *p++ = hexdigit[(ch >> 8) & 0xf];
            *p++ = hexdigit[(ch >> 4) & 0xf];
            *p++ = hexdigit[ch & 15];
        }
        /* Copy everything else as-is */
        else
            *p++ = (char) ch;
    }
    *p = '\0';
    _PyString_Resize(&repr, p - q);
    return repr;
}

static int unicode_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    const char unicode = UNICODE;
    CHK_TRUE(rw->write(rw, &unicode, 1, 1), fail);

    PyObject *repr = modified_EncodeRawUnicodeEscape(PyUnicode_AS_UNICODE(obj),
        PyUnicode_GET_SIZE(obj));
    CHK_TRUE(repr, fail);
    CHK_TRUE(rw->write(rw, PyString_AS_STRING(repr), PyString_GET_SIZE(repr), 1), fail);
    CHK_TRUE(rw->write(rw, "\n", 1, 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}
#endif

static int slice_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int static_method_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return placeholder_inst_pickle(ctx, obj, rw);
}

#ifndef WITHOUT_COMPLEX
static int complex_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}
#endif

static int float_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int buffer_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
} 

static int long_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    char str[32];
    long l = PyInt_AS_LONG((PyIntObject *)obj);
    Py_ssize_t len = 0;

    str[0] = LONG;
    PyOS_snprintf(str + 1, sizeof(str) - 1, "%ld\n", l);
    CHK_TRUE(rw->write(rw, str, 1, strlen(str)), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int int_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    char str[32];
    long l = PyInt_AS_LONG((PyIntObject *)obj);
    Py_ssize_t len = 0;

    str[0] = INT;
    PyOS_snprintf(str + 1, sizeof(str) - 1, "%ld\n", l);
    CHK_TRUE(rw->write(rw, str, 1, strlen(str)), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int frozen_set_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    assert(PyFrozenSet_Check(obj));
    CHK_TRUE(0 == set_elems_pickle(ctx, obj, rw), fail);

    const char ops[] = {PF_EXTEND, PF_FROZENSET};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
} 

static int property_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int memory_view_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

/* From cPickle:
 * Tuples are the only builtin immutable type that can be recursive
 * (a tuple can be reached from itself), and that requires some subtle
 * magic so that it works in all cases.  IOW, this is a long routine.
 */
static int tuple_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    Py_ssize_t len, i;

    assert(PyTuple_Check(obj));
    len = PyTuple_Size((PyObject*)obj);
    Py_INCREF(obj);

    if(len == 0) {

        char str[] = {EMPTY_TUPLE};
        CHK_TRUE(rw->write(rw, str, 1, ARR_SIZE(str)), fail);
        return 0;
    }

    /* id(tuple) isn't in the memo now.  If it shows up there after
     * saving the tuple elements, the tuple must be recursive, in
     * which case we'll pop everything we put on the stack, and fetch
     * its value from the memo.
     */

    const char mark = MARK;
    const char pmark = POP_MARK;
    const char tuple = TUPLE;
    CHK_TRUE(rw->write(rw, &mark, 1, 1), fail);

    for(int i = 0; i < len; i++) {

        PyObject *elem = PyTuple_GET_ITEM(obj, i);
        assert(elem);
        if(!pickle_obj(ctx, elem, rw)) {
            assert(PyErr_Occurred());
            return -1;
        }
    }

    if(memo_contains(ctx, obj)) {
    
        /* pop the stack stuff we pushed */
        CHK_TRUE(rw->write(rw, &pmark, 1, 1), fail);
        /* fetch from memo */
        CHK_TRUE(emit_get(ctx, obj, rw), fail);
        return 0;
    }

    /* Not recursive. */
    CHK_TRUE(rw->write(rw, &tuple, 1, 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int enum_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int reversed_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int method_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) 
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int function_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    assert(PyFunction_Check(obj));
    PyFunctionObject *func = (PyFunctionObject*)obj;

    const char emptyfunc[] = {PF_EXTEND, PF_EMPTYFUNC};
    CHK_TRUE(rw->write(rw, emptyfunc, ARR_SIZE(emptyfunc), 1), fail);

    /* Memoize the function object before recursing into its' attributes 
     * as the function may be self-referencing. This is why we need to 
     * first create a 'dummy' function object and set its' 'code' and 
     * 'dict' attributes after. */
    memoize(ctx, obj);
    CHK_TRUE(emit_put(ctx, obj, rw), fail);

    CHK_TRUE(pickle_obj(ctx, func->func_code, rw), fail);
    CHK_TRUE(pickle_obj(ctx, func->func_globals, rw), fail);

    if(func->func_closure) {
        CHK_TRUE(pickle_obj(ctx, func->func_closure, rw), fail);
    }else{
        Py_INCREF(Py_None);
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    if(func->func_defaults) {
        CHK_TRUE(pickle_obj(ctx, func->func_defaults, rw), fail);
    }else{
        Py_INCREF(Py_None);
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    const char ops[] = {PF_EXTEND, PF_FUNCTION};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int class_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyClass_Check(obj));
    PyClassObject *cls = (PyClassObject*)obj;

    /* push name */
    if(!pickle_obj(ctx, cls->cl_name, rw))
        goto fail;

    /* push tuple of base classes */
    PyObject *bases = cls->cl_bases;
    assert(bases);
    assert(PyTuple_Check(bases));

    if(!pickle_obj(ctx, bases, rw))
        goto fail;

    if(!pickle_obj(ctx, cls->cl_dict, rw))
        goto fail;

    const char ops[] = {PF_EXTEND, PF_CLASS};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int gen_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int instance_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    assert(PyInstance_Check(obj));
    PyInstanceObject *inst = (PyInstanceObject*)obj;
    CHK_TRUE(pickle_obj(ctx, (PyObject*)inst->in_class, rw), fail);
    CHK_TRUE(pickle_obj(ctx, inst->in_dict, rw), fail);

    const char ops[] = {PF_EXTEND, PF_INST};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int file_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return placeholder_inst_pickle(ctx, obj, rw);
}

static int cell_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    assert(PyCell_Check(obj));
    PyCellObject *cell = (PyCellObject*)obj;
    const char ec_ops[] = {PF_EXTEND, PF_EMPTY_CELL};
    const char ops[] = {PF_EXTEND, PF_CELL};

    if(cell->ob_ref == NULL) {
        CHK_TRUE(rw->write(rw, ec_ops, ARR_SIZE(ec_ops), 1), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, cell->ob_ref, rw), fail);
        CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ec_ops), 1), fail);
    }
    return 0;
    
fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int module_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    assert(PyModule_Check(obj));
    const char *name = PyModule_GetName(obj);
    assert(name);

    PyObject *str = PyString_FromString(name);
    vec_pobj_push(&ctx->to_free, str);

    CHK_TRUE(pickle_obj(ctx, str, rw), fail);

    const char ops[] = {PF_EXTEND, PF_MODULE};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int get_set_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    assert(obj->ob_type == &PyGetSetDescr_Type);
    PyGetSetDescrObject *desc = (PyGetSetDescrObject*)obj;

    if(type_is_builtin((PyObject*)desc->d_type))
        return builtin_pickle(ctx, obj, rw);

    /* The getset_descriptor is not indexed because it was dynamically created */
    TRACE_PICKLE(obj);

    CHK_TRUE(pickle_obj(ctx, (PyObject*)desc->d_type, rw), fail);
    CHK_TRUE(pickle_obj(ctx, desc->d_name, rw), fail);

    const char getattr[] = {PF_EXTEND, PF_GETSETDESC};
    CHK_TRUE(rw->write(rw, getattr, ARR_SIZE(getattr), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int wrapper_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    return builtin_pickle(ctx, obj, rw);
}

static int member_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    return builtin_pickle(ctx, obj, rw);
}

static int dict_proxy_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int long_info_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    const char ops[] = {PF_EXTEND, PF_SYSLONGINFO}; 
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int float_info_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    const char ops[] = {PF_EXTEND, PF_SYSFLOATINFO};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int sys_flags_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    return builtin_pickle(ctx, obj, rw);
}

static int sys_version_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    return builtin_pickle(ctx, obj, rw);
}

static int cfunction_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    return builtin_pickle(ctx, obj, rw);
}

static int code_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyCode_Check(obj));
    PyCodeObject *co = (PyCodeObject*)obj;

    PyObject *co_argcount = PyInt_FromLong(co->co_argcount);
    vec_pobj_push(&ctx->to_free, co_argcount);
    CHK_TRUE(pickle_obj(ctx, co_argcount, rw), fail);

    PyObject *co_nlocals = PyInt_FromLong(co->co_nlocals);
    vec_pobj_push(&ctx->to_free, co_nlocals);
    CHK_TRUE(pickle_obj(ctx, co_nlocals, rw), fail);

    PyObject *co_stacksize = PyInt_FromLong(co->co_stacksize);
    vec_pobj_push(&ctx->to_free, co_stacksize);
    CHK_TRUE(pickle_obj(ctx, co_stacksize, rw), fail);

    PyObject *co_flags = PyInt_FromLong(co->co_flags);
    vec_pobj_push(&ctx->to_free, co_flags);
    CHK_TRUE(pickle_obj(ctx, co_flags, rw), fail);

    CHK_TRUE(pickle_obj(ctx, co->co_code, rw), fail);
    CHK_TRUE(pickle_obj(ctx, co->co_consts, rw), fail);
    CHK_TRUE(pickle_obj(ctx, co->co_names, rw), fail);
    CHK_TRUE(pickle_obj(ctx, co->co_varnames, rw), fail);
    CHK_TRUE(pickle_obj(ctx, co->co_freevars, rw), fail);
    CHK_TRUE(pickle_obj(ctx, co->co_cellvars, rw), fail);
    CHK_TRUE(pickle_obj(ctx, co->co_filename, rw), fail);
    CHK_TRUE(pickle_obj(ctx, co->co_name, rw), fail);

    PyObject *co_firstlineno = PyInt_FromLong(co->co_firstlineno);
    vec_pobj_push(&ctx->to_free, co_firstlineno);
    CHK_TRUE(pickle_obj(ctx, co_firstlineno, rw), fail);
    CHK_TRUE(pickle_obj(ctx, co->co_lnotab, rw), fail);

    const char ops[] = {PF_EXTEND, PF_CODE};
    CHK_TRUE(rw->write(rw, ops, 2, 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int traceback_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int frame_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int null_importer_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    const char ops[] = {PF_EXTEND, PF_NULLIMPORTER};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;
fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int not_implemented_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(obj == Py_NotImplemented);
    const char ops[] = {PF_EXTEND, PF_NOTIMPL};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;
fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int none_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) 
{
    TRACE_PICKLE(obj);
    assert(obj == Py_None);
    const char none = NONE;
    CHK_TRUE(rw->write(rw, &none, 1, 1), fail);
    return 0;
fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int ellipsis_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) 
{
    TRACE_PICKLE(obj);
    assert(obj == Py_Ellipsis);
    const char ops[] = {PF_EXTEND, PF_ELLIPSIS};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;
fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int weakref_ref_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return placeholder_inst_pickle(ctx, obj, rw);
}

static int weakref_callable_proxy_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) 
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int weakref_proxy_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int match_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int pattern_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int scanner_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) 
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int zip_importer_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int st_entry_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int class_method_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int class_method_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return placeholder_inst_pickle(ctx, obj, rw);
}

static int dict_items_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int dict_keys_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int dict_values_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int method_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    return builtin_pickle(ctx, obj, rw);
}

static int call_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int seq_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int byte_array_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int dict_iter_item_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int dict_iter_key_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) 
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int dict_iter_value_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int field_name_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int formatter_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int list_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int list_rev_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int set_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) 
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int tuple_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = PyObject_Repr(obj);
    printf("%s: %s\n", __func__, PyString_AS_STRING(repr));
    Py_DECREF(repr);
    return 0;
}

static int newclass_instance_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    const char ops[] = {PF_EXTEND, PF_NEWINST};

    PyTypeObject *type = obj->ob_type;
    assert(type->tp_flags & Py_TPFLAGS_HEAPTYPE);
    assert(type->tp_mro);
    assert(PyTuple_Check(type->tp_mro));
    assert(PyTuple_GET_SIZE(type->tp_mro) >= 1);

    bool found = false;
    for(int i = 0; i < PyTuple_GET_SIZE(type->tp_mro); i++) {

        PyObject *basetype = PyTuple_GET_ITEM(type->tp_mro, i);
        if(!type_is_builtin(basetype))
            continue;
        
        /* This is the first builtin type in the MRO. By just
         * being one of the bases, we know it is binary compatible
         * with the object. Forcefully "upcast" the instance to this 
         * type (by patching the instance's ob_type field) and send 
         * it to 'pickle_obj'. By pickling this instance as if it 
         * were an instance of the 'outer-most' builtin base, we are 
         * guaranteed that the C fields of the type are saved (and we 
         * get to use existing code to achieve this). The extra stuff 
         * added in the scripting-defined bases can be restored with 
         * PyObject_SetAttr calls.
         */
        PyTypeObject *saved = obj->ob_type;
        obj->ob_type = (PyTypeObject*)basetype;
        CHK_TRUE(pickle_obj(ctx, obj, rw), fail);
        obj->ob_type = saved;

        found = true;
        break;
    }

    assert(found);
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int placeholder_inst_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    const char ops[] = {PF_EXTEND, PF_NEWINST};

    CHK_TRUE(0 == base_obj_pickle(ctx, obj, rw), fail);
    CHK_TRUE(pickle_obj(ctx, s_placeholder_type, rw), fail);
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int op_int(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(INT, ctx);

    char buff[MAX_LINE_LEN];
    READ_LINE(rw, buff, fail);

    errno = 0;
    char *endptr;
    long l = strtol(buff, &endptr, 0);
    if (errno || (*endptr != '\n') || (endptr[1] != '\0')) {
        SET_RUNTIME_EXC("Bad int in pickle stream [offset: %ld]", rw->seek(rw, RW_SEEK_CUR, 0));
        goto fail;
    }

    vec_pobj_push(&ctx->stack, PyInt_FromLong(l));
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    return -1;
}

static int op_long(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(LONG, ctx);

    char buff[MAX_LINE_LEN];
    READ_LINE(rw, buff, fail);

    errno = 0;
    char *endptr;
    long l = strtol(buff, &endptr, 0);
    if (errno || (*endptr != '\n') || (endptr[1] != '\0')) {
        SET_RUNTIME_EXC("Bad long in pickle stream [offset: %ld]", rw->seek(rw, RW_SEEK_CUR, 0));
        goto fail;
    }

    vec_pobj_push(&ctx->stack, PyLong_FromLong(l));
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    return -1;
}

static int op_stop(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(STOP, ctx);
    ctx->stop = true;
    return 0;
}

static int op_string(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(STRING, ctx);

    PyObject *strobj = 0;
    Py_ssize_t len;

    vec_char_t str;
    vec_char_init(&str);
    char c;
    do { 
        CHK_TRUE(SDL_RWread(rw, &c, 1, 1), fail);
        CHK_TRUE(vec_char_push(&str, c), fail);
    }while(c != '\n');
    vec_AT(&str, vec_size(&str)-1) = '\0';

    char *p = str.array;
    len = vec_size(&str)-1;

    /* Strip trailing whitespace */
    while (len > 0 && p[len-1] <= ' ')
        len--;

    /* Strip outermost quotes */
    if (len > 1 && p[0] == '"' && p[len-1] == '"') {
        p[len-1] = '\0';
        p += 1;
        len -= 2;
    } else if (len > 1 && p[0] == '\'' && p[len-1] == '\'') {
        p[len-1] = '\0';
        p += 1;
        len -= 2;
    }else {
        SET_RUNTIME_EXC("Pickle string not wrapped in quotes:%s", str.array);
        goto fail; /* Strings returned by __repr__ should be wrapped in quotes */
    }

    strobj = PyString_DecodeEscape(p, len, NULL, 0, NULL);
    if(!strobj) {
        assert(PyErr_Occurred()); 
        goto fail;
    }

    vec_char_destroy(&str);
    vec_pobj_push(&ctx->stack, strobj);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    vec_char_destroy(&str);
    return -1;
}

static int op_put(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PUT, ctx);

    char buff[MAX_LINE_LEN];
    READ_LINE(rw, buff, fail);

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        return -1;
    }

    char *end;
    int idx = strtol(buff, &end, 10);
    if(!idx && end != buff + strlen(buff)-1) /* - newline */ {
        SET_RUNTIME_EXC("Bad index in pickle stream: [offset: %ld]", rw->seek(rw, RW_SEEK_CUR, 0));
        return -1;
    }

    vec_pobj_resize(&ctx->memo, idx + 1);
    ctx->memo.size = idx + 1;    
    vec_AT(&ctx->memo, idx) = TOP(&ctx->stack);
    Py_INCREF(vec_AT(&ctx->memo, idx)); /* The memo references everything in it */
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    return -1;
}

static int op_get(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(GET, ctx);

    char buff[MAX_LINE_LEN];
    READ_LINE(rw, buff, fail);

    char *end;
    int idx = strtol(buff, &end, 10);
    if(!idx && end != buff + strlen(buff) - 1) /* - newline */ {
        SET_RUNTIME_EXC("Bad index in pickle stream: [offset: %ld]", rw->seek(rw, RW_SEEK_CUR, 0));
        return -1;
    }

    if(vec_size(&ctx->memo) <= idx) {
        SET_RUNTIME_EXC("No memo entry for index: %d", idx);
        return -1;
    }

    vec_pobj_push(&ctx->stack, vec_AT(&ctx->memo, idx));
    Py_INCREF(TOP(&ctx->stack));

    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    return -1;
}

static int op_mark(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(MARK, ctx);
    return vec_int_push(&ctx->mark_stack, vec_size(&ctx->stack)) ? 0 : -1;
}

static int op_pop(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(POP, ctx);
    if(vec_size(&ctx->stack) == 0) {
        SET_RUNTIME_EXC("Stack underflow");
        return -1;
    }

    Py_DECREF(TOP(&ctx->stack));
    vec_pobj_pop(&ctx->stack);
    return 0;
}

static int op_pop_mark(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(POP_MARK, ctx);
    if(vec_size(&ctx->mark_stack) == 0) {
        SET_RUNTIME_EXC("Mark stack underflow");
        return -1;
    }

    int mark = vec_int_pop(&ctx->mark_stack);
    if(vec_size(&ctx->stack) < mark) {
        SET_RUNTIME_EXC("Popped mark beyond stack limits: %d", mark);
        return -1;
    }

    while(vec_size(&ctx->stack) > mark) {
    
        PyObject *obj = vec_pobj_pop(&ctx->stack);
        Py_DECREF(obj);
    }
    return 0;
}

static int op_tuple(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(TUPLE, ctx);
    if(vec_size(&ctx->mark_stack) == 0) {
        SET_RUNTIME_EXC("Mark stack underflow");
        return -1;
    }

    int mark = vec_int_pop(&ctx->mark_stack);
    if(vec_size(&ctx->stack) < mark) {
        SET_RUNTIME_EXC("Popped mark beyond stack limits: %d", mark);
        return -1;
    }

    size_t tup_len = vec_size(&ctx->stack) - mark;
    PyObject *tuple = PyTuple_New(tup_len);
    assert(tuple);

    for(int i = 0; i < tup_len; i++) {
        PyObject *elem = vec_pobj_pop(&ctx->stack);
        PyTuple_SET_ITEM(tuple, tup_len - i - 1, elem);
    }

    vec_pobj_push(&ctx->stack, tuple);
    return 0;
}

static int op_empty_tuple(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(EMPTY_TUPLE, ctx);
    PyObject *tuple = PyTuple_New(0);
    if(!tuple) {
        assert(PyErr_Occurred());
        return -1;
    }
    vec_pobj_push(&ctx->stack, tuple);
    return 0;
}

static int op_empty_list(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(EMPTY_LIST, ctx);
    PyObject *list = PyList_New(0);
    if(!list) {
        assert(PyErr_Occurred());
        return -1;
    }
    vec_pobj_push(&ctx->stack, list);
    return 0;
}

static int op_appends(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(APPENDS, ctx);
    if(vec_size(&ctx->mark_stack) == 0) {
        SET_RUNTIME_EXC("Mark stack underflow");
        return -1;
    }

    int mark = vec_int_pop(&ctx->mark_stack);
    if(vec_size(&ctx->stack) < mark-1) {
        SET_RUNTIME_EXC("Popped mark beyond stack limits: %d", mark);
        return -1;
    }

    size_t extra_len = vec_size(&ctx->stack) - mark;
    PyObject *list = vec_AT(&ctx->stack, mark-1);
    if(!PyList_Check(list)) {
        SET_RUNTIME_EXC("No list found at mark");
        return -1;
    }

    PyObject *append = PyList_New(extra_len);
    if(!append) {
        assert(PyErr_Occurred());
        return -1;
    }

    for(int i = 0; i < extra_len; i++) {
        PyObject *elem = vec_pobj_pop(&ctx->stack);
        Py_INCREF(elem);
        PyList_SetItem(append, extra_len - i - 1, elem);
    }

    size_t og_len = PyList_Size(list);
    PyList_SetSlice(list, og_len, og_len, append);
    Py_DECREF(append);
    return 0;
}

static int op_empty_dict(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(EMPTY_DICT, ctx);
    PyObject *dict = PyDict_New();
    if(!dict) {
        assert(PyErr_Occurred());
        return -1;
    }
    vec_pobj_push(&ctx->stack, dict);
    return 0;
}

static int op_setitems(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(SETITEMS, ctx);
    if(vec_size(&ctx->mark_stack) == 0) {
        SET_RUNTIME_EXC("Mark stack underflow");
        return -1;
    }

    int mark = vec_int_pop(&ctx->mark_stack);
    if(vec_size(&ctx->stack) < mark-1) {
        SET_RUNTIME_EXC("Popped mark beyond stack limits: %d", mark);
        return -1;
    }

    size_t nitems = vec_size(&ctx->stack) - mark;
    if(nitems % 2) {
        SET_RUNTIME_EXC("Non-even number of key-value pair objects");
        return -1;
    }
    nitems /= 2;

    PyObject *dict = vec_AT(&ctx->stack, mark-1);
    if(!PyDict_Check(dict)) {
        SET_RUNTIME_EXC("Dict not found at mark: %d", mark);
        return -1;
    }

    for(int i = 0; i < nitems; i++) {

        PyObject *val = vec_pobj_pop(&ctx->stack);
        PyObject *key = vec_pobj_pop(&ctx->stack);
        PyDict_SetItem(dict, key, val);
        Py_DECREF(key);
        Py_DECREF(val);
    }
    return 0;
}

static int op_none(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(NONE, ctx);
    Py_INCREF(Py_None);
    vec_pobj_push(&ctx->stack, Py_None);
    return 0;
}

static int op_unicode(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(UNICODE, ctx);

    vec_char_t str;
    vec_char_init(&str);
    char c;
    do { 
        CHK_TRUE(SDL_RWread(rw, &c, 1, 1), fail);
        CHK_TRUE(vec_char_push(&str, c), fail);
    }while(c != '\n');
    vec_AT(&str, vec_size(&str)-1) = '\0';

    PyObject *unicode = PyUnicode_DecodeRawUnicodeEscape(str.array, vec_size(&str)-1, NULL);
    CHK_TRUE(unicode, fail);

    vec_pobj_push(&ctx->stack, unicode);
    vec_char_destroy(&str);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    vec_char_destroy(&str);
    return -1;
}

static int op_ext_builtin(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_BUILTIN, ctx);

    char buff[MAX_LINE_LEN];
    READ_LINE(rw, buff, fail);
    buff[strlen(buff)-1] = '\0'; /* Strip newline */

    PyObject *ret;
    if(NULL == (ret = qualname_new_ref(buff))) {
        assert(PyErr_Occurred()); 
        return -1;
    }

    vec_pobj_push(&ctx->stack, ret);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    return -1;
}

static int op_ext_type(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_TYPE, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 4) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    PyObject *meta = vec_pobj_pop(&ctx->stack);
    PyObject *dict = vec_pobj_pop(&ctx->stack);
    PyObject *bases = vec_pobj_pop(&ctx->stack);
    PyObject *name = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(meta)) {
        SET_RUNTIME_EXC("PF_TYPE: 'type' (metatype) not found at TOS");
        goto fail_typecheck;
    }

    if(!PyDict_Check(dict)) {
        SET_RUNTIME_EXC("PF_TYPE: Dict not found at TOS1");
        goto fail_typecheck;
    }

    if(!PyTuple_Check(bases)) {
        SET_RUNTIME_EXC("PF_TYPE: (bases) tuple not found at TOS2");
        goto fail_typecheck;
    }

    if(!PyString_Check(name)) {
        SET_RUNTIME_EXC("PF_TYPE: Name not found at TOS3");
        goto fail_typecheck;
    }

    PyObject *args = Py_BuildValue("(OOO)", name, bases, dict);
    PyObject *retval = NULL;

    if((retval = PyType_Type.tp_new((PyTypeObject*)meta, args, NULL)) == NULL)
        goto fail_build;

    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_build:
    Py_DECREF(args);
fail_typecheck:
    Py_DECREF(name);
    Py_DECREF(bases);
    Py_DECREF(dict);
fail_underflow:
    assert((ret && PyErr_Occurred()) || (!ret && !PyErr_Occurred()));
    return ret;
}

static int op_ext_getattr(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_GETATTR, ctx);
    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow"); 
        return -1;
    }
    PyObject *name = vec_pobj_pop(&ctx->stack);
    PyObject *obj = vec_pobj_pop(&ctx->stack);

    int ret = -1;
    if(!PyString_Check(name)) {
        SET_RUNTIME_EXC("PF_GETATTR: Expecting string (name) at TOS");
        goto fail_typecheck;
    }

    PyObject *attr = PyObject_GetAttr(obj, name);
    CHK_TRUE(attr, fail_get);

    vec_pobj_push(&ctx->stack, attr);
    ret = 0;

fail_get:
fail_typecheck:
    Py_DECREF(name);
    Py_DECREF(obj);
    return ret;
}

static int op_ext_code(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_CODE, ctx);
    if(vec_size(&ctx->stack) < 14) {
        SET_RUNTIME_EXC("Stack underflow"); 
        return -1;
    }

    int rval = -1;
    PyObject *lnotab = vec_pobj_pop(&ctx->stack);
    PyObject *firstlineno = vec_pobj_pop(&ctx->stack);
    PyObject *name = vec_pobj_pop(&ctx->stack);
    PyObject *filename = vec_pobj_pop(&ctx->stack);
    PyObject *cellvars = vec_pobj_pop(&ctx->stack);
    PyObject *freevars = vec_pobj_pop(&ctx->stack);
    PyObject *varnames = vec_pobj_pop(&ctx->stack);
    PyObject *names = vec_pobj_pop(&ctx->stack);
    PyObject *consts = vec_pobj_pop(&ctx->stack);
    PyObject *code = vec_pobj_pop(&ctx->stack);
    PyObject *flags = vec_pobj_pop(&ctx->stack);
    PyObject *stacksize = vec_pobj_pop(&ctx->stack);
    PyObject *nlocals = vec_pobj_pop(&ctx->stack);
    PyObject *argcount = vec_pobj_pop(&ctx->stack);

    if(!PyInt_Check(argcount)
    || !PyInt_Check(nlocals)
    || !PyInt_Check(stacksize)
    || !PyInt_Check(flags)
    || !PyInt_Check(firstlineno)) {
        SET_RUNTIME_EXC("PF_CODE: argcount, nlocals, stacksize, flags, firstlinenoe must be an integers"); 
        goto fail;
    }

    PyObject *ret = (PyObject*)PyCode_New(
        PyInt_AS_LONG(argcount),
        PyInt_AS_LONG(nlocals),
        PyInt_AS_LONG(stacksize),
        PyInt_AS_LONG(flags),
        code,
        consts,
        names,
        varnames,
        freevars,
        cellvars,
        filename,
        name,
        PyInt_AS_LONG(firstlineno),
        lnotab
    );

    if(!ret) {
        SET_RUNTIME_EXC("PF_CODE: argcount, nlocals, stacksize, flags, firstlinenoe must be an integers"); 
        goto fail;
    }

    vec_pobj_push(&ctx->stack, ret);
    rval = 0;

fail:
    Py_DECREF(lnotab);
    Py_DECREF(firstlineno);
    Py_DECREF(name);
    Py_DECREF(filename);
    Py_DECREF(cellvars);
    Py_DECREF(freevars);
    Py_DECREF(varnames);
    Py_DECREF(names);
    Py_DECREF(consts);
    Py_DECREF(code);
    Py_DECREF(flags);
    Py_DECREF(stacksize);
    Py_DECREF(nlocals);
    Py_DECREF(argcount);
    return rval;
}

static int op_ext_function(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_FUNCTION, ctx);
    if(vec_size(&ctx->stack) < 5) {
        SET_RUNTIME_EXC("Stack underflow"); 
        return -1;
    }

    PyObject *defaults = vec_pobj_pop(&ctx->stack);
    PyObject *closure = vec_pobj_pop(&ctx->stack);
    PyObject *globals = vec_pobj_pop(&ctx->stack);
    PyObject *code = vec_pobj_pop(&ctx->stack);
    PyFunctionObject *op = (PyFunctionObject*)vec_pobj_pop(&ctx->stack);

    /* Set the code and globals of the empty function object. This is 
     * the exact same flow as if the code and globals were passed to
     * 'PyFunction_New'. We just wanted to memoize the function object
     * before pickling its' members to handle self-referencing. 
     *
     * The following is ripped from PyFunction_New in funcobject.c:
     */

    PyObject *__name__ = NULL;
    PyObject *doc;
    PyObject *consts;
    PyObject *module;
    op->func_weakreflist = NULL;
    op->func_code = code; /* Steal ref */
    op->func_globals = globals; /* Steal ref */
    op->func_name = ((PyCodeObject *)code)->co_name;
    Py_INCREF(op->func_name);
    op->func_defaults = NULL; /* No default arguments */
    op->func_closure = NULL;
    consts = ((PyCodeObject *)code)->co_consts;
    if (PyTuple_Size(consts) >= 1) {
        doc = PyTuple_GetItem(consts, 0);
        if (!PyString_Check(doc) && !PyUnicode_Check(doc))
            doc = Py_None;
    }
    else
        doc = Py_None;
    Py_INCREF(doc);
    op->func_doc = doc;
    op->func_dict = NULL;
    op->func_module = NULL;

    /* __module__: If module name is in globals, use it.
       Otherwise, use None.
    */
    if (!__name__) {
        __name__ = PyString_InternFromString("__name__");
        if (!__name__) {
            assert(PyErr_Occurred());
            goto fail;
        }
    }
    module = PyDict_GetItem(globals, __name__);
    if (module) {
        Py_INCREF(module);
        op->func_module = module;
    }
    /* End PyFunction_New */

    if(closure != Py_None && !PyTuple_Check(closure)){
        SET_RUNTIME_EXC("Closure must be a tuple or None");
        goto fail;
    }else if(closure != Py_None 
    && (0 != PyFunction_SetClosure((PyObject*)op, closure))){
        goto fail; 
    }

    if(defaults != Py_None && !PyTuple_Check(defaults)){
        SET_RUNTIME_EXC("Defaults must be a tuple or None");
        goto fail;
    }else if(defaults != Py_None 
    && (0 != PyFunction_SetDefaults((PyObject*)op, defaults))){
        goto fail; 
    }

    Py_DECREF(closure);
    Py_DECREF(defaults);
    vec_pobj_push(&ctx->stack, (PyObject*)op);
    return 0;

fail:
    Py_DECREF(closure);
    Py_DECREF(defaults);
    Py_DECREF(op);
    return -1;
}

static int op_ext_empty_cell(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_EMPTY_CELL, ctx);
    PyObject *cell = PyCell_New(NULL);
    assert(cell); 
    vec_pobj_push(&ctx->stack, cell);
    return 0;
}

static int op_ext_cell(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_CELL, ctx);

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow"); 
        return -1;
    }

    PyObject *cell = PyCell_New(vec_pobj_pop(&ctx->stack));
    assert(cell);
    vec_pobj_push(&ctx->stack, cell);
    return 0;
}

static int op_ext_true(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_TRUE, ctx);
    Py_INCREF(Py_True);
    vec_pobj_push(&ctx->stack, Py_True);
    return 0;
}

static int op_ext_false(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_FALSE, ctx);
    Py_INCREF(Py_False);
    vec_pobj_push(&ctx->stack, Py_False);
    return 0;
}

static int op_ext_bytearray(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_BYTEARRAY, ctx);

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow"); 
        return -1;
    }

    PyObject *encoded = vec_pobj_pop(&ctx->stack);
    if(!PyString_Check(encoded)) {
        SET_RUNTIME_EXC("PF_BYTEARRAY: Expecting string at TOS"); 
        return -1;
    }

    PyObject *bytes = PyString_AsDecodedString(encoded, "UTF-8", "strict");
    if(!bytes) {
        Py_DECREF(encoded);
        return -1;
    }
    Py_DECREF(encoded);

    PyObject *ret = PyByteArray_FromStringAndSize(PyString_AS_STRING(bytes), PyString_GET_SIZE(bytes));
    assert(ret);
    Py_DECREF(bytes);
    vec_pobj_push(&ctx->stack, ret);
    return 0;
}

static int op_ext_super(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_SUPER, ctx);

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow"); 
        return -1;
    }

    PyObject *obj = vec_pobj_pop(&ctx->stack);
    PyObject *type = vec_pobj_pop(&ctx->stack);

    Py_INCREF(obj);
    Py_INCREF(type);
    PyObject *args = PyTuple_New(2);
    PyTuple_SetItem(args, 0, type);
    PyTuple_SetItem(args, 1, obj);

    PyObject *ret = PyObject_Call((PyObject*)&PySuper_Type, args, NULL);
    Py_DECREF(args);
    if(!ret)
        return -1;

    vec_pobj_push(&ctx->stack, ret);
    return 0;
}

static int op_ext_popmark(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_POPMARK, ctx);

    if(vec_size(&ctx->mark_stack) < 1) {
        SET_RUNTIME_EXC("Mark stack underflow"); 
        return -1;
    }

    vec_int_pop(&ctx->mark_stack);
    return 0;
}

static int op_ext_emptyfunc(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_EMPTYFUNC, ctx);

    PyFunctionObject *ret = PyObject_GC_New(PyFunctionObject, &PyFunction_Type);
    assert(ret);

    size_t extra = sizeof(PyFunctionObject) - sizeof(PyObject);
    memset(((PyObject*)ret)+1, 0, extra);

    _PyObject_GC_TRACK(ret);
    vec_pobj_push(&ctx->stack, (PyObject*)ret);
    return 0;
}

static int op_ext_baseobj(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_BASEOBJ, ctx);

    PyObject *args = PyTuple_New(0);
    PyObject *ret = PyObject_Call((PyObject*)&PyBaseObject_Type, args, NULL);
    Py_DECREF(args);
    assert(ret);

    vec_pobj_push(&ctx->stack, ret);
    return 0;
}

static int op_ext_setattrs(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_SETATTRS, ctx);

    int ret = -1;
    if(vec_size(&ctx->mark_stack) < 1) {
        SET_RUNTIME_EXC("Mark stack underflow"); 
        return -1;
    }

    const int mark = vec_int_pop(&ctx->mark_stack);
    size_t nitems = vec_size(&ctx->stack) - mark;

    if(nitems % 2) {
        SET_RUNTIME_EXC("Non-even number of key-value pair objects");
        return -1;
    }
    nitems /= 2;

    if(vec_size(&ctx->stack) < nitems*2 + 1) {
        SET_RUNTIME_EXC("Stack underflow"); 
        return -1;
    }

    PyObject *const obj = vec_AT(&ctx->stack, mark - 1);
    for(int i = 0; i < nitems; i++) {
    
        PyObject *val = vec_pobj_pop(&ctx->stack);
        PyObject *key = vec_pobj_pop(&ctx->stack);

        ret = (0 == PyObject_SetAttr(obj, key, val)) ? 0 : -1;

        Py_DECREF(key);
        Py_DECREF(val);
        if(ret)
            return -1;
    }
    PyObject *top = vec_pobj_pop(&ctx->stack);
    assert(obj == top);

    //TODO: also delete any NDW attributes not in the dict

    vec_pobj_push(&ctx->stack, obj);
    return 0;
}

static int op_ext_notimpl(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_NOTIMPL, ctx);
    Py_INCREF(Py_NotImplemented);
    vec_pobj_push(&ctx->stack, Py_None);
    return 0;
}

static int op_ext_ellipsis(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_ELLIPSIS, ctx);
    Py_INCREF(Py_Ellipsis);
    vec_pobj_push(&ctx->stack, Py_Ellipsis);
    return 0;
}

static int op_ext_syslonginfo(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_SYSLONGINFO, ctx);
    PyObject *ret = PyLong_GetInfo();
    assert(ret);
    vec_pobj_push(&ctx->stack, ret);
    return 0;
}

static int op_ext_sysfloatinfo(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_SYSFLOATINFO, ctx);
    PyObject *ret = PyFloat_GetInfo();
    assert(ret);
    vec_pobj_push(&ctx->stack, ret);
    return 0;
}

static int op_ext_set(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_SET, ctx);
    if(vec_size(&ctx->stack) == 0) {
        SET_RUNTIME_EXC("Stack underflow");
        return -1;
    }
    PyObject *items = vec_pobj_pop(&ctx->stack);
    if(!PyTuple_Check(items)) {
        SET_RUNTIME_EXC("PF_SET: Expecting a tuple of set items on TOS");
        return -1;
    }
    PyObject *ret = PySet_New(items);
    if(!ret) {
        assert(PyErr_Occurred());
        return -1;
    }

    Py_DECREF(items);
    vec_pobj_push(&ctx->stack, ret);
    return 0;
}

static int op_ext_frozenset(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_SET, ctx);
    if(vec_size(&ctx->stack) == 0) {
        SET_RUNTIME_EXC("Stack underflow");
        return -1;
    }
    PyObject *items = vec_pobj_pop(&ctx->stack);
    if(!PyTuple_Check(items)) {
        SET_RUNTIME_EXC("PF_FROZENSET: Expecting a tuple of set items on TOS");
        return -1;
    }
    PyObject *ret = PyFrozenSet_New(items);
    if(!ret) {
        assert(PyErr_Occurred());
        return -1;
    }

    Py_DECREF(items);
    vec_pobj_push(&ctx->stack, ret);
    return 0;
}

static int op_ext_class(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_CLASS, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 3) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    PyObject *dict = vec_pobj_pop(&ctx->stack);
    PyObject *bases = vec_pobj_pop(&ctx->stack);
    PyObject *name = vec_pobj_pop(&ctx->stack);

    if(!PyDict_Check(dict)) {
        SET_RUNTIME_EXC("PF_CLASS: Dict not found at TOS");
        goto fail_typecheck;
    }

    if(!PyTuple_Check(bases)) {
        SET_RUNTIME_EXC("PF_CLASS: (bases) tuple not found at TOS1");
        goto fail_typecheck;
    }

    if(!PyString_Check(name)) {
        SET_RUNTIME_EXC("PF_CLASS: Name not found at TOS2");
        goto fail_typecheck;
    }

    PyObject *cls;
    if(NULL == (cls = PyClass_New(bases, dict, name)))
        goto fail_build;

    vec_pobj_push(&ctx->stack, cls);
    ret = 0;

fail_build:
fail_typecheck:
    Py_DECREF(name);
    Py_DECREF(bases);
    Py_DECREF(dict);
fail_underflow:
    assert((ret && PyErr_Occurred()) || (!ret && !PyErr_Occurred()));
    return ret;
}

static int op_ext_inst(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_INST, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *dict = vec_pobj_pop(&ctx->stack);
    PyObject *klass = vec_pobj_pop(&ctx->stack);

    if(!PyDict_Check(dict)) {
        SET_RUNTIME_EXC("PF_INST: dict not found at TOS");
        goto fail_typecheck;
    }

    if(!PyClass_Check(klass)) {
        SET_RUNTIME_EXC("PF_INST: classobj not found at TOS1");
        goto fail_typecheck;
    }

    PyObject *inst = PyInstance_NewRaw(klass, dict);
    CHK_TRUE(inst, fail_inst);

    vec_pobj_push(&ctx->stack, inst);
    ret = 0;

fail_inst:
fail_typecheck:
    Py_DECREF(klass);
    Py_DECREF(dict);
fail_underflow:
    assert((ret && PyErr_Occurred()) || (!ret && !PyErr_Occurred()));
    return ret;
}

static int op_ext_getsetdesc(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_GETSETDESC, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *name = vec_pobj_pop(&ctx->stack);
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyString_Check(name)) {
        SET_RUNTIME_EXC("PF_GETSETDESC: Expecting string at TOS");
        goto fail_typecheck;
    }

    if(!PyType_Check(type)) {
        SET_RUNTIME_EXC("PF_GETSETDESC: Expecting type at TOS1");
        goto fail_typecheck;
    }

    PyTypeObject *tp_type = (PyTypeObject*)type;
    PyGetSetDef *found = NULL;
    for(PyGetSetDef *curr = tp_type->tp_getset; curr && curr->name; curr++) {
    
        if(0 == strcmp(curr->name, PyString_AS_STRING(name))) {
            found = curr; 
            break;
        }
    }

    if(!found) {
        SET_RUNTIME_EXC("Could not find getset_descriptor (%s) of type (%s)",
            PyString_AS_STRING(name), tp_type->tp_name);
        goto fail_desc;
    }

    PyObject *desc = PyDescr_NewGetSet(tp_type, found);
    CHK_TRUE(desc, fail_desc);
    vec_pobj_push(&ctx->stack, desc);
    ret = 0;

fail_desc:
fail_typecheck:
    Py_DECREF(name);
    Py_DECREF(type);
fail_underflow:
    assert((ret && PyErr_Occurred()) || (!ret && !PyErr_Occurred()));
    return ret;
}

static int op_ext_module(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_MODULE, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    PyObject *name = vec_pobj_pop(&ctx->stack);
    if(!PyString_Check(name)) {
        SET_RUNTIME_EXC("PF_MODULE: Expecting string (name) on TOS"); 
        goto fail_typecheck;
    }

    PyObject *mod = PyModule_New(PyString_AS_STRING(name));
    CHK_TRUE(mod, fail_mod);

    vec_pobj_push(&ctx->stack, mod);
    ret = 0;

fail_mod:
fail_typecheck:
    Py_DECREF(name);
fail_underflow:
    assert((ret && PyErr_Occurred()) || (!ret && !PyErr_Occurred()));
    return ret;
}

/*
 * Return a new instance of a dummy class that is a direct sublass of the object's builtin type.
 */
static PyObject *heaptype_wrapped(PyObject *obj)
{
    assert(type_is_builtin((PyObject*)obj->ob_type));

    PyObject *args = Py_BuildValue("(s(O){})", "__placeholder__", obj->ob_type);
    assert(args);

    PyObject *newtype = PyObject_Call((PyObject*)&PyType_Type, args, NULL);
    Py_DECREF(args);
    assert(newtype);
    assert(((PyTypeObject*)newtype)->tp_flags & Py_TPFLAGS_HEAPTYPE);

    /* Instance args */
    args = NULL;
    if(obj->ob_type == &PyBaseObject_Type) {
        args = PyTuple_New(0);
    }
    else if(obj->ob_type == &PyTuple_Type) {
        args = Py_BuildValue("(O)", obj);
    }
    else if(obj->ob_type == &PyType_Type) {
        PyTypeObject *tp = (PyTypeObject*)obj;
        args = Py_BuildValue("(sOO)", tp->tp_name, tp->tp_bases, tp->tp_dict);
    }
    else {
        //TODO: support all types
        printf("%s\n", obj->ob_type->tp_name);
        assert(0); 
    }
    assert(args);

    PyObject *inst = PyObject_Call(newtype, args, NULL);
    assert(inst);

    Py_DECREF(args);
    Py_DECREF(newtype);
    return inst;
}

static int op_ext_newinst(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_NEWINST, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    PyObject *type = vec_pobj_pop(&ctx->stack);
    PyObject *baseinst = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)) {
        SET_RUNTIME_EXC("PF_NEWINST: Expecting type on TOS"); 
        goto fail_typecheck;
    }

    PyTypeObject *tp_type = (PyTypeObject*)type;
    PyObject *mro = tp_type->tp_mro;
    assert(PyTuple_Check(mro));

    /* quick sanity check */
    bool type_found = false;
    for(int i = 0; i < PyTuple_GET_SIZE(mro); i++) {
        if(PyObject_IsInstance(baseinst, PyTuple_GET_ITEM(mro, i))) {
            type_found = true;
            break;
        }
    }

    if(!type_found) {
        SET_RUNTIME_EXC("PF_NEWINST: Object on TOS1 must be "
        "an instance of one of the bases of the type on TOS.");
        goto fail_typecheck;
    }

    /* An instance of a built-in type cannot be converted to an instance of 
     * a heaptype as it may not have had enough memory allocated. We must 
     * 'wrap' it. 
     */
    PyObject *new_obj = NULL;
    if(baseinst->ob_type->tp_flags & Py_TPFLAGS_HEAPTYPE) {
        new_obj = baseinst;
        Py_INCREF(new_obj);
    }else{
        new_obj = heaptype_wrapped(baseinst);
    }
    assert(new_obj);
    assert(new_obj->ob_type->tp_flags & Py_TPFLAGS_HEAPTYPE);
    Py_DECREF(baseinst);

    /* This is assigning to __class__, but with no error checking */
    Py_DECREF(Py_TYPE(new_obj));
    Py_TYPE(new_obj) = tp_type;
    Py_INCREF(Py_TYPE(new_obj));

    vec_pobj_push(&ctx->stack, new_obj);
    ret = 0;

fail_inst:
fail_typecheck:
    Py_DECREF(type);
fail_underflow:
    assert((ret && PyErr_Occurred()) || (!ret && !PyErr_Occurred()));
    return ret;
}

static int op_ext_nullimporter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_NULLIMPORTER, ctx);

    PyObject *args = PyTuple_Pack(1, PyString_FromString("...")); /* Pass any invalid path; it's not saved */
    PyObject *ret = PyObject_Call((PyObject*)&PyNullImporter_Type, args, NULL);
    Py_DECREF(args);
    assert(ret);

    vec_pobj_push(&ctx->stack, ret);
    return 0;
}

static pickle_func_t picklefunc_for_type(PyObject *obj)
{
    for(int i = 0; i < ARR_SIZE(s_type_dispatch_table); i++) {
    
        if(obj->ob_type == s_type_dispatch_table[i].type)
            return s_type_dispatch_table[i].picklefunc;
    }

    /* It's not one of the Python builtins - it may be an engine builtin */
    for(int i = 0; i < ARR_SIZE(s_pf_dispatch_table); i++) {
    
        if(obj->ob_type == s_pf_dispatch_table[i].type)
            return s_pf_dispatch_table[i].picklefunc;
    }

    return NULL;
}

static bool pickle_ctx_init(struct pickle_ctx *ctx)
{
    if(NULL == (ctx->memo = kh_init(memo))) {
        SET_EXC(PyExc_MemoryError, "Memo table allocation");
        goto fail_memo;
    }

    vec_pobj_init(&ctx->to_free);
    return true;

fail_memo:
    return false;
}

static void pickle_ctx_destroy(struct pickle_ctx *ctx)
{
    for(int i = 0; i < vec_size(&ctx->to_free); i++) {
        Py_DECREF(vec_AT(&ctx->to_free, i));    
    }

    vec_pobj_destroy(&ctx->to_free);
    kh_destroy(memo, ctx->memo);
}

static bool unpickle_ctx_init(struct unpickle_ctx *ctx)
{
    vec_pobj_init(&ctx->stack);
    vec_pobj_init(&ctx->memo);
    vec_int_init(&ctx->mark_stack);
    ctx->stop = false;
    return true;
}

static void unpickle_ctx_destroy(struct unpickle_ctx *ctx)
{
    for(int i = 0; i < vec_size(&ctx->memo); i++) {
        Py_DECREF(vec_AT(&ctx->memo, i));    
    }
    
    vec_int_destroy(&ctx->mark_stack);
    vec_pobj_destroy(&ctx->memo);
    vec_pobj_destroy(&ctx->stack);
}

static bool memo_contains(const struct pickle_ctx *ctx, PyObject *obj)
{
    uintptr_t id = (uintptr_t)obj;
    khiter_t k = kh_get(memo, ctx->memo, id);
    if(k != kh_end(ctx->memo))
        return true;
    return false;
}

static int memo_idx(const struct pickle_ctx *ctx, PyObject *obj)
{
    uintptr_t id = (uintptr_t)obj;
    khiter_t k = kh_get(memo, ctx->memo, id);
    assert(kh_exist(ctx->memo, k));
    return kh_value(ctx->memo, k).idx;
}

static void memoize(struct pickle_ctx *ctx, PyObject *obj)
{
    int ret;
    int idx = kh_size(ctx->memo);

    khiter_t k = kh_put(memo, ctx->memo, (uintptr_t)obj, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(ctx->memo, k) = (struct memo_entry){idx, obj};
}

static bool emit_get(const struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    char str[32];
    snprintf(str, ARR_SIZE(str), "%c%d\n", GET, memo_idx(ctx, obj));
    str[ARR_SIZE(str)-1] = '\0';
    return rw->write(rw, str, 1, strlen(str));
}

static bool emit_put(const struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    char str[32];
    snprintf(str, ARR_SIZE(str), "%c%d\n", PUT, memo_idx(ctx, obj));
    str[ARR_SIZE(str)-1] = '\0';
    return rw->write(rw, str, 1, strlen(str));
}

static bool pickle_attrs(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    /* The parent object must already be memoized to handle self-referencing */
    assert(memo_contains(ctx, obj));
    const char mark = MARK;
    CHK_TRUE(rw->write(rw, &mark, 1, 1), fail);

    PyObject *ndw_attrs = nonderived_writable_attrs(obj);
    vec_pobj_push(&ctx->to_free, ndw_attrs);

    PyObject *key, *value;
    Py_ssize_t pos = 0;
    bool has_cls = false;

    while(PyDict_Next(ndw_attrs, &pos, &key, &value)) {
        if(0 == strcmp("__class__", PyString_AS_STRING(key))) {
            has_cls = true;
            continue; /* Save the __class__ for last */
        }
        CHK_TRUE(pickle_obj(ctx, key, rw), fail);
        CHK_TRUE(pickle_obj(ctx, value, rw), fail);
    }

    /* Push the __class__ attribute last onto the stack, 
     * meaning it will be the very first one to get set
     * during unpickling. For some types this is a special
     * attribute that other attributes (ex. getset descriptors)
     * rely on to be set. 
     */
    if(has_cls) {
        key = PyString_FromString("__class__");
        value = PyDict_GetItem(ndw_attrs, key);
        assert(value);

        int res = pickle_obj(ctx, key, rw);
        Py_DECREF(key);
        CHK_TRUE(res, fail);
        CHK_TRUE(pickle_obj(ctx, value, rw), fail);
    }

    const char setattrs[] = {PF_EXTEND, PF_SETATTRS};
    CHK_TRUE(rw->write(rw, setattrs, ARR_SIZE(setattrs), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static bool pickle_obj(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *stream)
{
    pickle_func_t pf;

    if(memo_contains(ctx, obj)) {
        CHK_TRUE(emit_get(ctx, obj, stream), fail);
        return true;
    }

    pf = picklefunc_for_type(obj);
    /* It's not one of the known builtins */
    if(NULL == pf) {

        if(obj->ob_type->tp_flags & Py_TPFLAGS_HEAPTYPE) {
            pf = newclass_instance_pickle;
        }else{
            SET_RUNTIME_EXC("Cannot pickle object of type:%s", obj->ob_type->tp_name);
            return false;
        }
    }
    assert(pf);

    if(0 != pf(ctx, obj, stream)) {
        assert(PyErr_Occurred());
        return false; 
    }

    /* Some objects (eg. lists) may already be memoized */
    if(!memo_contains(ctx, obj)) {
        memoize(ctx, obj);
        CHK_TRUE(emit_put(ctx, obj, stream), fail);
    }

    if(pickle_attrs(ctx, obj, stream)) {
        assert(PyErr_Occurred());
        return false; 
    }

    assert(!PyErr_Occurred());
    return true;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return false;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool S_Pickle_Init(PyObject *module)
{
    /* Set up the id: qualname map at initialization time, _after_ registering
     * all the engine builtins (as we wish to be able to look up builtins in
     * this map), but _before_ any code is run. The reason for the latter rule 
     * is so that we can assume that anything in this map will be present in
     * the interpreter session after initialization, so that the same builtin
     * may be found in another session by its' qualified path.
     */
    s_id_qualname_map = kh_init(str);
    if(!s_id_qualname_map)
        goto fail_id_qualname;

    Py_INCREF(Py_False);
    PyModule_AddObject(module, "trace_pickling", Py_False);

    pre_build_index();
    if(!S_Traverse_IndexQualnames(s_id_qualname_map))
        goto fail_traverse;
    post_build_index();

    load_private_type_refs();
    load_exception_types();
    load_engine_builtin_types();

    /* Temporary user-defined class to use as a stub until we support all types */
    PyObject *args = Py_BuildValue("(s(O){})", "__placeholder__", (PyObject*)&PyBaseObject_Type);
    s_placeholder_type = PyObject_Call((PyObject*)&PyType_Type, args, NULL);
    Py_DECREF(args);
    assert(s_placeholder_type);

    return true;

fail_traverse:
    kh_destroy(str, s_id_qualname_map);
fail_id_qualname:
    return false;
}

void S_Pickle_Shutdown(void)
{
    uint64_t key;
    const char *curr;
    kh_foreach(s_id_qualname_map, key, curr, {
        free((char*)curr);
    });

    kh_destroy(str, s_id_qualname_map);
    Py_DECREF(s_placeholder_type);
}

bool S_PickleObjgraph(PyObject *obj, SDL_RWops *stream)
{
    struct pickle_ctx ctx;
    int ret = pickle_ctx_init(&ctx);
    if(!ret) 
        goto err;

    if(!pickle_obj(&ctx, obj, stream))
        goto err;

    char term[] = {STOP, '\0'};
    CHK_TRUE(stream->write(stream, term, 1, ARR_SIZE(term)), err_write);

    pickle_ctx_destroy(&ctx);
    return true;

err_write:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
err:
    assert(PyErr_Occurred());
    pickle_ctx_destroy(&ctx);
    return false;
}

bool S_PickleObjgraphByName(const char *module, const char *name, SDL_RWops *stream)
{
    PyObject *modules_dict = PySys_GetObject("modules");
    assert(modules_dict);

    if(!PyMapping_HasKeyString(modules_dict, (char*)module)) {
        SET_EXC(PyExc_NameError, "Module not found");
        goto fail_mod_key;
    }

    PyObject *mod = PyMapping_GetItemString(modules_dict, (char*)module);
    assert(mod);

    if(!PyObject_HasAttrString(mod, name)) {
        SET_EXC(PyExc_NameError, "Attribute not found");
        goto fail_attr_key;
    }

    PyObject *obj = PyObject_GetAttrString(mod, name);
    int ret = S_PickleObjgraph(obj, stream);
    Py_DECREF(obj);
    Py_DECREF(mod);
    return ret;

fail_attr_key:
    Py_DECREF(mod);
fail_mod_key:
    return false;
}

PyObject *S_UnpickleObjgraph(SDL_RWops *stream)
{
    struct unpickle_ctx ctx;
    unpickle_ctx_init(&ctx);

    while(!ctx.stop) {
    
        char op;
        bool xtend = false;

        CHK_TRUE(stream->read(stream, &op, 1, 1), err);

        if(op == PF_EXTEND) {

            CHK_TRUE(stream->read(stream, &op, 1, 1), err);
            xtend =true;
        }

        unpickle_func_t upf = xtend ? s_ext_op_dispatch_table[op]
                                    : s_op_dispatch_table[op];
        if(!upf) {
            SET_RUNTIME_EXC("Bad %sopcode %c", (xtend ? "extended " : ""), op);
            goto err;
        }
        CHK_TRUE(upf(&ctx, stream) == 0, err);
    }

    if(vec_size(&ctx.stack) != 1) {
        SET_RUNTIME_EXC("Unexpected stack size after 'STOP'");
        goto err;
    }

    PyObject *ret = vec_pobj_pop(&ctx.stack);
    unpickle_ctx_destroy(&ctx);

    assert(!PyErr_Occurred());
    return ret;

err:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    unpickle_ctx_destroy(&ctx);
    return NULL;
}

bool S_UnpickleObjgraphByName(const char *module, const char *name, SDL_RWops *stream)
{

}

