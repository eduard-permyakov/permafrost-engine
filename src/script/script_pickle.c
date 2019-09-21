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
#include "../lib/public/vec.h"
#include "../asset_load.h"

#include <frameobject.h>
#include <assert.h>


#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))
#define TOP(_stk)   (vec_AT(_stk, vec_size(_stk)-1))
#define CHK_TRUE(_pred, _label) if(!(_pred)) goto _label

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
#define PF_PROTO        'a' /* identify pickle protocol version                     */
#define PF_TRUE         'b' /* push True                                            */
#define PF_FALSE        'c' /* push False                                           */
#define PF_NEWOBJ       'd' /* build object by applying cls.__new__ to argtuple     */
#define PF_NAMEDREF     'e' /* create named attribute from topmost stack items      */
#define PF_NAMEDWEAKREF 'f' /* create named weakref attribute from topmost stack items */

#define PF_BUILTIN      'A' /* Push new reference to built-in that is identified by its' fully-qualified name */
/* TBD ... */

struct memo_entry{
    int idx;
    PyObject *obj;
};

VEC_TYPE(pobj, PyObject*)
VEC_IMPL(static inline, pobj, PyObject*)

VEC_TYPE(int, int)
VEC_IMPL(static inline, int, int)

KHASH_MAP_INIT_INT64(memo, struct memo_entry)

struct pickle_ctx{
    khash_t(memo) *memo;
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
static int byte_array_pickle  (struct pickle_ctx *, PyObject *, SDL_RWops *); 
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
static int get_set_descr_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int wrapper_descr_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int member_descr_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int dict_proxy_pickle  (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int cfunction_pickle   (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int code_pickle        (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int traceback_pickle   (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int frame_pickle       (struct pickle_ctx *, PyObject *, SDL_RWops *);
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

/* Unpickling functions */
static int op_int           (struct unpickle_ctx *, SDL_RWops *);
static int op_stop          (struct unpickle_ctx *, SDL_RWops *);
static int op_string        (struct unpickle_ctx *, SDL_RWops *);
static int op_put           (struct unpickle_ctx *, SDL_RWops *);
static int op_get           (struct unpickle_ctx *, SDL_RWops *);
static int op_mark          (struct unpickle_ctx *, SDL_RWops *);
static int op_pop_mark      (struct unpickle_ctx *, SDL_RWops *);
static int op_tuple         (struct unpickle_ctx *, SDL_RWops *);
static int op_empty_tuple   (struct unpickle_ctx *, SDL_RWops *);
static int op_empty_list    (struct unpickle_ctx *, SDL_RWops *);
static int op_appends       (struct unpickle_ctx *, SDL_RWops *);
static int op_empty_dict    (struct unpickle_ctx *, SDL_RWops *);
static int op_setitems      (struct unpickle_ctx *, SDL_RWops *);

static int op_ext_builtin   (struct unpickle_ctx *, SDL_RWops *);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(str)  *s_id_qualname_map;

// TODO: categorize into 'primal' objects which can be instantiated directly in a script
//       and 'derived' objects, which must be derived from an existing object (primal or builtin)

static const struct pickle_entry s_type_dispatch_table[] = {
    /* The Python 2.7 public built-in types. These types may be instantiated directly 
     * in any script. 
     */
    {.type = &PyType_Type,                  .picklefunc = type_pickle                   }, /* type() */
    {.type = &PyBool_Type,                  .picklefunc = bool_pickle                   }, /* bool() */
    {.type = &PyString_Type,                .picklefunc = string_pickle                 }, /* str() */
    {.type = &PyByteArray_Type,             .picklefunc = byte_array_pickle             }, /* bytearray() */
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

    /* Built-in singletons. These may not be instantiated directly  */
    /* The PyNotImplemented_Type and PyNone_Type are not exported. */
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
};

static unpickle_func_t s_op_dispatch_table[256] = {
    [INT] = op_int,
    [STOP] = op_stop,
    [STRING] = op_string,
    [GET] = op_get,
    [PUT] = op_put,
    [MARK] = op_mark,
    [POP_MARK] = op_pop_mark,
    [TUPLE] = op_tuple,
    [EMPTY_TUPLE] = op_empty_tuple,
    [EMPTY_LIST] = op_empty_list,
    [APPENDS] = op_appends,
    [EMPTY_DICT] = op_empty_dict,
    [SETITEMS] = op_setitems,
};

static unpickle_func_t s_ext_op_dispatch_table[256] = {
    [PF_BUILTIN] = op_ext_builtin,
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

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
    Py_INCREF(mod);
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
            PyObject_Print(parent, stdout, 0);
            printf("\n");
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

static int type_pickle        (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int bool_pickle        (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}

static int string_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
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

static int byte_array_pickle  (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {} 

static int list_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
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

static int super_pickle       (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {} 
static int base_obj_pickle    (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {} 
static int range_pickle       (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {} 

static int dict_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
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

    PyObject *iter = PyObject_CallMethod(obj, "iteritems", "()");
    assert(iter);

    for(PyObject *curr = PyIter_Next(iter); curr; curr = PyIter_Next(iter)) {
    
        assert(PyTuple_Check(curr) && PyTuple_Size(curr) == 2);

        PyObject *key = PyTuple_GET_ITEM(curr, 0);
        if(!pickle_obj(ctx, key, rw)) {
            Py_DECREF(curr);
            Py_DECREF(iter);
            assert(PyErr_Occurred());
            return -1;
        }

        PyObject *val = PyTuple_GET_ITEM(curr, 1);
        if(!pickle_obj(ctx, val, rw)) {
            Py_DECREF(curr);
            Py_DECREF(iter);
            assert(PyErr_Occurred());
            return -1;
        }

        Py_DECREF(curr);
    }
    Py_DECREF(iter);

    CHK_TRUE(rw->write(rw, &setitems, 1, 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int set_pickle         (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
#ifdef Py_USING_UNICODE
static int unicode_pickle     (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {} 
#endif
static int slice_pickle       (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int static_method_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
#ifndef WITHOUT_COMPLEX
static int complex_pickle     (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
#endif
static int float_pickle       (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int buffer_pickle      (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {} 
static int long_pickle        (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}

static int int_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
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

static int frozen_set_pickle  (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {} 
static int property_pickle    (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int memory_view_pickle (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}


/* From cPickle:
 * Tuples are the only builtin immutable type that can be recursive
 * (a tuple can be reached from itself), and that requires some subtle
 * magic so that it works in all cases.  IOW, this is a long routine.
 */
static int tuple_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
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

static int enum_pickle        (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int reversed_pickle    (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int method_pickle      (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int function_pickle    (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int class_pickle       (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int gen_pickle         (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int instance_pickle    (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int file_pickle        (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int cell_pickle        (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int get_set_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int wrapper_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int member_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int dict_proxy_pickle  (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}

static int cfunction_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    uintptr_t id = (uintptr_t)obj;
    khiter_t k = kh_get(str, s_id_qualname_map, id);
    if(k == kh_end(s_id_qualname_map)) {
    
        SET_RUNTIME_EXC("Could not find built-in function qualified name in index: %s", 
            ((PyCFunctionObject*)obj)->m_ml->ml_name);
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

static int code_pickle        (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int traceback_pickle   (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int frame_pickle       (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int not_implemented_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int none_pickle        (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int ellipsis_pickle    (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int weakref_ref_pickle (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int weakref_callable_proxy_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int weakref_proxy_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int match_pickle       (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int pattern_pickle     (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int scanner_pickle     (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int zip_importer_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int st_entry_pickle    (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int class_method_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int class_method_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int dict_items_pickle  (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int dict_keys_pickle   (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int dict_values_pickle (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int method_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int call_iter_pickle   (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int seq_iter_pickle    (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int byte_array_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int dict_iter_item_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int dict_iter_key_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int dict_iter_value_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int field_name_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int formatter_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int list_iter_pickle   (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int list_rev_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int set_iter_pickle    (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}
static int tuple_iter_pickle  (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) {}

static int op_int(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
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

static int op_stop(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    ctx->stop = true;
    return 0;
}

static int op_string(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    PyObject *str = 0;
    Py_ssize_t len;

    char buff[MAX_LINE_LEN];
    READ_LINE(rw, buff, fail);
    len = strlen(buff);
    char *p = buff;

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
        SET_RUNTIME_EXC("Pickle string not wrapped in quotes:%s", buff);
        goto fail; /* Strings returned by __repr__ should be wrapped in quotes */
    }

    str = PyString_DecodeEscape(p, len, NULL, 0, NULL);
    if(!str) {
        assert(PyErr_Occurred()); 
        goto fail;
    }

    vec_pobj_push(&ctx->stack, str);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    return -1;
}

static int op_put(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
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
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    return -1;
}

static int op_get(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
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
    return vec_int_push(&ctx->mark_stack, vec_size(&ctx->stack)) ? 0 : -1;
}

static int op_pop_mark(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
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
        Py_INCREF(elem);
        PyTuple_SET_ITEM(tuple, tup_len - i - 1, elem);
    }

    vec_pobj_push(&ctx->stack, tuple);
    return 0;
}

static int op_empty_tuple(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
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

static int op_ext_builtin(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
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

static pickle_func_t picklefunc_for_type(PyObject *obj)
{
    for(int i = 0; i < ARR_SIZE(s_type_dispatch_table); i++) {
    
        if(obj->ob_type == s_type_dispatch_table[i].type)
            return s_type_dispatch_table[i].picklefunc;
    }
    return NULL;
}

static bool pickle_ctx_init(struct pickle_ctx *ctx)
{
    if(NULL == (ctx->memo = kh_init(memo))) {
        SET_EXC(PyExc_MemoryError, "Memo table allocation");
        goto fail_memo;
    }

    return true;

fail_memo:
    return false;
}

static void pickle_ctx_destroy(struct pickle_ctx *ctx)
{
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

static bool pickle_obj(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *stream)
{
    pickle_func_t pf;

    if(memo_contains(ctx, obj)) {
        CHK_TRUE(emit_get(ctx, obj, stream), fail);
        return true;
    }

    if(NULL == (pf = picklefunc_for_type(obj))) {
        SET_RUNTIME_EXC("Cannot pickle object of type:%s", obj->ob_type->tp_name);
        return false;
    }

    if(0 != pf(ctx, obj, stream)) {
        assert(PyErr_Occurred());
        return false; 
    }

    /* Some objects (eg. lists) may already be memoized */
    if(!memo_contains(ctx, obj)) {
        memoize(ctx, obj);
        CHK_TRUE(emit_put(ctx, obj, stream), fail);
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

bool S_Pickle_Init(void)
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

    if(!S_Traverse_IndexQualnames(s_id_qualname_map))
        goto fail_traverse;

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
        CHK_TRUE(upf, err);
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
    assert(PyErr_Occurred());
    unpickle_ctx_destroy(&ctx);
    return NULL;
}

bool S_UnpickleObjgraphByName(const char *module, const char *name, SDL_RWops *stream)
{

}

