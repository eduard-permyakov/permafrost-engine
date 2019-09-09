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

#include "script_pickle.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"
#include "../asset_load.h"

#include <frameobject.h>

#include <assert.h>


#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

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
#define PF_PROTO        'p' /* identify pickle protocol                             */
#define PF_TRUE         't' /* push True                                            */
#define PF_FALSE        'f' /* push False                                           */
#define PF_NEWOBJ       'n' /* build object by applying cls.__new__ to argtuple     */
#define PF_NAMEDREF     'r' /* create named attribute from topmost stack items      */
#define PF_NAMEDWEAKREF 'w' /* create named weakref attribute from topmost stack items */

#define PF_MODULE       'A' /* Create module object from topmost stack items        */
/* TBD ... */

VEC_TYPE(pobj, PyObject*)
VEC_IMPL(static inline, pobj, PyObject*)

struct pickle_ctx{
    //stack
    //memo
    //mapping of builtins
};

struct unpickle_ctx{
    //stack
    vec_pobj_t stack;
    //memo
    //mapping of builtins
    //marks
    bool stop;
};

typedef int (*pickle_func_t)(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *stream);
typedef int (*unpickle_func_t)(struct unpickle_ctx *ctx, SDL_RWops *stream);

struct pickle_entry{
    PyTypeObject  *type; 
    pickle_func_t  picklefunc;
};

extern PyTypeObject PyListIter_Type;
extern PyTypeObject PyListRevIter_Type;
extern PyTypeObject PyTupleIter_Type;
extern PyTypeObject PySTEntry_Type;

/* Pickling functions */
static int type_pickle        (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int bool_pickle        (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int string_pickle      (struct pickle_ctx *, const PyObject *, SDL_RWops *); 
static int byte_array_pickle  (struct pickle_ctx *, const PyObject *, SDL_RWops *); 
static int list_pickle        (struct pickle_ctx *, const PyObject *, SDL_RWops *); 
static int super_pickle       (struct pickle_ctx *, const PyObject *, SDL_RWops *); 
static int base_obj_pickle    (struct pickle_ctx *, const PyObject *, SDL_RWops *); 
static int range_pickle       (struct pickle_ctx *, const PyObject *, SDL_RWops *); 
static int dict_pickle        (struct pickle_ctx *, const PyObject *, SDL_RWops *); 
static int set_pickle         (struct pickle_ctx *, const PyObject *, SDL_RWops *);
#ifdef Py_USING_UNICODE
static int unicode_pickle     (struct pickle_ctx *, const PyObject *, SDL_RWops *); 
#endif
static int slice_pickle       (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int static_method_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
#ifndef WITHOUT_COMPLEX
static int complex_pickle     (struct pickle_ctx *, const PyObject *, SDL_RWops *);
#endif
static int float_pickle       (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int buffer_pickle      (struct pickle_ctx *, const PyObject *, SDL_RWops *); 
static int long_pickle        (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int int_pickle         (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int frozen_set_pickle  (struct pickle_ctx *, const PyObject *, SDL_RWops *); 
static int property_pickle    (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int memory_view_pickle (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int tuple_pickle       (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int enum_pickle        (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int reversed_pickle    (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int method_pickle      (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int function_pickle    (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int class_pickle       (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int gen_pickle         (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int instance_pickle    (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int file_pickle        (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int cell_pickle        (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int get_set_descr_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int wrapper_descr_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int member_descr_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int dict_proxy_pickle  (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int cfunction_pickle   (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int code_pickle        (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int traceback_pickle   (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int frame_pickle       (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int not_implemented_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int none_pickle        (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int ellipsis_pickle    (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int weakref_ref_pickle (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int weakref_callable_proxy_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int weakref_proxy_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int match_pickle       (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int pattern_pickle     (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int scanner_pickle     (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int zip_importer_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int st_entry_pickle    (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int class_method_descr_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int class_method_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int dict_items_pickle  (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int dict_keys_pickle   (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int dict_values_pickle (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int method_descr_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int call_iter_pickle   (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int seq_iter_pickle    (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int byte_array_iter_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int dict_iter_item_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int dict_iter_key_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int dict_iter_value_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int field_name_iter_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int formatter_iter_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int list_iter_pickle   (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int list_rev_iter_pickle(struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int set_iter_pickle    (struct pickle_ctx *, const PyObject *, SDL_RWops *);
static int tuple_iter_pickle  (struct pickle_ctx *, const PyObject *, SDL_RWops *);

/* Unpickling functions */
static int load_int           (struct unpickle_ctx *, SDL_RWops *);
static int load_stop          (struct unpickle_ctx *, SDL_RWops *);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

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
    [INT]   = load_int,
    [STOP]  = load_stop,
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int type_pickle        (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int bool_pickle        (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int string_pickle      (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {} 
static int byte_array_pickle  (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {} 
static int list_pickle        (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {} 
static int super_pickle       (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {} 
static int base_obj_pickle    (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {} 
static int range_pickle       (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {} 
static int dict_pickle        (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {} 
static int set_pickle         (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
#ifdef Py_USING_UNICODE
static int unicode_pickle     (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {} 
#endif
static int slice_pickle       (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int static_method_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
#ifndef WITHOUT_COMPLEX
static int complex_pickle     (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
#endif
static int float_pickle       (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int buffer_pickle      (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {} 
static int long_pickle        (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}

static int int_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw)
{
    char c_str[32];
    long l = PyInt_AS_LONG((PyIntObject *)obj);
    Py_ssize_t len = 0;

    c_str[0] = INT;
    PyOS_snprintf(c_str + 1, sizeof(c_str) - 1, "%ld\n", l);
    if(rw->write(rw, c_str, 1, strlen(c_str)) < 0)
        return -1;
    return 0;
}

static int frozen_set_pickle  (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {} 
static int property_pickle    (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int memory_view_pickle (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int tuple_pickle       (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int enum_pickle        (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int reversed_pickle    (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int method_pickle      (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int function_pickle    (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int class_pickle       (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int gen_pickle         (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int instance_pickle    (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int file_pickle        (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int cell_pickle        (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int get_set_descr_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int wrapper_descr_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int member_descr_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int dict_proxy_pickle  (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int cfunction_pickle   (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int code_pickle        (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int traceback_pickle   (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int frame_pickle       (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int not_implemented_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int none_pickle        (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int ellipsis_pickle    (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int weakref_ref_pickle (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int weakref_callable_proxy_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int weakref_proxy_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int match_pickle       (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int pattern_pickle     (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int scanner_pickle     (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int zip_importer_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int st_entry_pickle    (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int class_method_descr_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int class_method_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int dict_items_pickle  (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int dict_keys_pickle   (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int dict_values_pickle (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int method_descr_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int call_iter_pickle   (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int seq_iter_pickle    (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int byte_array_iter_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int dict_iter_item_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int dict_iter_key_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int dict_iter_value_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int field_name_iter_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int formatter_iter_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int list_iter_pickle   (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int list_rev_iter_pickle(struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int set_iter_pickle    (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}
static int tuple_iter_pickle  (struct pickle_ctx *ctx, const PyObject *obj, SDL_RWops *rw) {}

static int load_int(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    char buff[MAX_LINE_LEN];
    READ_LINE(rw, buff, fail);

    errno = 0;
    char *endptr;
    long l = strtol(buff, &endptr, 0);
    if (errno || (*endptr != '\n') || (endptr[1] != '\0'))
        goto fail;

    vec_pobj_push(&ctx->stack, PyInt_FromLong(l));
    return 0;

fail:
    return -1;
}

static int load_stop(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    ctx->stop = true;
    return 0;
}

static bool picklable(const PyObject *obj)
{
    for(int i = 0; i < ARR_SIZE(s_type_dispatch_table); i++) {

        if(obj->ob_type == s_type_dispatch_table[i].type)
            return true;
    }
    return false;
}

static pickle_func_t picklefunc_for_type(const PyObject *obj)
{
    for(int i = 0; i < ARR_SIZE(s_type_dispatch_table); i++) {
    
        if(obj->ob_type == s_type_dispatch_table[i].type)
            return s_type_dispatch_table[i].picklefunc;
    }
    return assert(0), NULL;
}

static bool unpickle_ctx_init(struct unpickle_ctx *ctx)
{
    vec_pobj_init(&ctx->stack);
    ctx->stop = false;
    return true;
}

static void unpickle_ctx_destroy(struct unpickle_ctx *ctx)
{
    vec_pobj_destroy(&ctx->stack);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool S_PickleObjgraph(PyObject *obj, SDL_RWops *stream)
{
    struct pickle_ctx ctx;
    if(0 != picklefunc_for_type(obj)(&ctx, obj, stream))
        return false;

    char term[] = {STOP, '\0'};
    stream->write(stream, term, 1, ARR_SIZE(term));
    return true;
}

bool S_PickleObjgraphByName(const char *module, const char *name, SDL_RWops *stream)
{
    PyObject *modules_dict = PySys_GetObject("modules");
    assert(modules_dict);

    if(!PyMapping_HasKeyString(modules_dict, (char*)module))
        goto fail_mod_key;

    PyObject *mod = PyMapping_GetItemString(modules_dict, (char*)module);
    assert(mod);

    if(!PyObject_HasAttrString(mod, name))
        goto fail_attr_key;

    PyObject *obj = PyObject_GetAttrString(mod, name);
    if(!picklable(obj)) {
        fprintf(stderr, "Could not pickle object of type: %s\n", obj->ob_type->tp_name);
        goto fail_pickle;
    }

    int ret = S_PickleObjgraph(obj, stream);
    Py_DECREF(obj);
    Py_DECREF(mod);
    return ret;

fail_pickle:
    Py_DECREF(obj);
fail_attr_key:
    Py_DECREF(mod);
fail_mod_key:
    return false;
}

//TODO: set exceptions within the picking/unpickling routines
PyObject *S_UnpickleObjgraph(SDL_RWops *stream)
{
    struct unpickle_ctx ctx;
    unpickle_ctx_init(&ctx);

    while(!ctx.stop) {
    
        char op;
        if(!stream->read(stream, &op, 1, 1))
            goto err;

        if(!s_op_dispatch_table[op])
            goto err;

        if(!s_op_dispatch_table[op](&ctx, stream) < 0)
            goto err;
    }

    if(vec_size(&ctx.stack) != 1)
        goto err;

    PyObject *ret = vec_pobj_pop(&ctx.stack);
    unpickle_ctx_destroy(&ctx);
    return ret;

err:
    unpickle_ctx_destroy(&ctx);
    return NULL;
}

bool S_UnpickleObjgraphByName(const char *module, const char *name, SDL_RWops *stream)
{

}

