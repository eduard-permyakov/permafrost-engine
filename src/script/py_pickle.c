/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2023 Eduard Permyakov 
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

#include "py_pickle.h"
#include "py_traverse.h"
#include "private_types.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/mem.h"
#include "../asset_load.h"
#include "../sched.h"


/* Additional Python headers */
typedef void *mod_ty; //symtable.h wants this

#include <frameobject.h>
#include <structmember.h>
#include <symtable.h>

#include <assert.h>


#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))
#define TOP(_stk)   (vec_AT(_stk, vec_size(_stk)-1))
#define CHK_TRUE(_pred, _label) do{ if(!(_pred)) goto _label; }while(0)
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#define TP(_p)      ((PyTypeObject*)_p)

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
        printf("[U] %-14s: [stack size: %4u] [mark stack size: %4u] (%s:%d)\n", \
            #op, (unsigned)vec_size(&ctx->stack),                               \
            (unsigned)vec_size(&ctx->mark_stack),                               \
            __FILE__, __LINE__);                                                \
        fflush(stdout);                                                         \
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
        printf("[P] %-24s: (%-36s:%4d) [0x%p] %s\n", obj->ob_type->tp_name,     \
            __FILE__, __LINE__, (void*)obj, PyString_AS_STRING(repr));          \
        fflush(stdout);                                                         \
        Py_DECREF(flag);                                                        \
        Py_DECREF(repr);                                                        \
    }while(0)

/* The subset of the original protocol 0 ASCII opcodes that are used. 
 * Note that some of these have been modified to accept additional arguments
 * via the stack. For example, the EMPTY_LIST opcode takes an additional
 * type argument to support pickling/unpickling of instances of user-defined 
 * subtypes of 'list'. 
 */

#define MARK            '(' /* push special markobject on stack                     */
#define STOP            '.' /* every pickle ends with STOP                          */
#define POP             '0' /* discard topmost stack item                           */
#define POP_MARK        '1' /* discard stack top through topmost markobject         */
#define FLOAT           'F' /* push float object; decimal string argument           */
#define INT             'I' /* push integer or bool; decimal string argument        */
#define LONG            'L' /* push long; decimal string argument                   */
#define NONE            'N' /* push None                                            */
#define STRING          'S' /* push string; NL-terminated string argument           */
#define UNICODE         'V' /* push Unicode string; raw-unicode-escaped'd argument  */
#define EMPTY_DICT      '}' /* push empty dict                                      */
#define APPENDS         'e' /* extend list on stack by topmost stack slice          */
#define GET             'g' /* push item from memo on stack; index is string arg    */
#define EMPTY_LIST      ']' /* push empty list                                      */
#define PUT             'p' /* store stack top in memo; index is string arg         */
#define TUPLE           't' /* build tuple from topmost stack items                 */
#define EMPTY_TUPLE     ')' /* push empty tuple                                     */
#define SETITEMS        'u' /* modify dict by adding topmost key+value pairs        */

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
#define PF_BUILTIN      'i' /* Push new reference to built-in that is identified by its' fully-qualified name */
#define PF_TYPE         'j' /* Push new class created from the top 4 stack items (name, bases, dict, metaclass) */
#define PF_CODE         'k' /* Push code object from the 14 TOS items (the args to PyCode_New) */
#define PF_FUNCTION     'l' /* Push function object from TOS items */
#define PF_EMPTY_CELL   'm' /* Push empty cell */
#define PF_CELL         'n' /* Push cell with TOS contents */
#define PF_BYTEARRAY    'o' /* Push byte array from encoded string and type */
#define PF_SUPER        'p' /* Push super from 3 TOS items */
#define PF_EMPTYFUNC    'q' /* Push dummy function object */
#define PF_BASEOBJ      'r' /* Push an 'object' instance */
#define PF_SYSLONGINFO  's' /* Push a 'sys.long_info' instance */
#define PF_NULLIMPORTER 't' /* Push an imp.NullImporter instance */
#define PF_SYSFLOATINFO 'u' /* Push a 'sys.float_info' instance */
#define PF_SET          'v' /* Push a set from TOS tuple */
#define PF_FROZENSET    'w' /* Push a frozenset from TOS tuple and type */
#define PF_CLASS        'x' /* Push an old class created from the top 3 stack items (name, bases, methods) */
#define PF_INST         'y' /* Push 'instance' from 'classobj' and 'dict' on TOS */
#define PF_GETSETDESC   'z' /* Push 'getset_descriptor' instance from top 3 stack items */
#define PF_MODULE       'A' /* PUsh module with dict on TOS */
#define PF_NEWINST      'B' /* Create new-style instance with type on TOS, args on TOS1 and the outer-most builtin base on TOS2 */
#define PF_CLSMETHOD    'C' /* Push a classmethod instance from TOS */
#define PF_INSTMETHOD   'D' /* Pusn an instancemethod instance from TOS */
#define PF_MEMDESC      'E' /* Push a 'member_descriptor' instance */
#define PF_METHWRAP     'F' /* Push a 'method-wrapper' instance from top 2 stack items */
#define PF_RANGE        'G' /* Push range object from top 3 stack items */
#define PF_SLICE        'H' /* Push a slice object from top 3 stack items */
#define PF_STATMETHOD   'I' /* Push a staticmethod object from stack item and type */
#define PF_BUFFER       'J' /* Push a buffer object from top 4 stack items */
#define PF_MEMVIEW      'K' /* Push a memoryview object from stack item */
#define PF_PROPERTY     'L' /* Push a property object from top 5 stack items */
#define PF_ENUMERATE    'M' /* Push an enuerate object from top 5 stack items */
#define PF_LISTITER     'N' /* Push listiterator object from top 2 stack items */
#define PF_COMPLEX      'O' /* Push a 'complex' object from top 3 stack items */
#define PF_DICTPROXY    'P' /* Push a 'dictproxy' object from the TOS dict */
#define PF_REVERSED     'Q' /* Push a 'reversed' object from the top 3 stack items */
#define PF_GEN          'R' /* Push a generator object from top TOS */
#define PF_FRAME        'S' /* Push a frame object from variable number of TOS items */
#define PF_NULLVAL      'T' /* Push a dummy object onto the stack, indicating a NULL value */
#define PF_TRACEBACK    'U' /* Push a traceback object from top 4 TOS items */
#define PF_EMPTYFRAME   'V' /* Push a dummy frame object with valuestack size from TOS */
#define PF_WEAKREF      'W' /* Push a weakref.ref object from top 2 TOS items */
#define PF_EMPTYMOD     'X' /* Push a dummy module reference */
#define PF_PROXY        'Y' /* Push a weakproxy instance from top 2 TOS items */
#define PF_STENTRY      'Z' /* Push a symtable entry instance from top 16 TOS items */
#define PF_DICTKEYS     '1' /* Push a dict_keys instance from TOS dict */
#define PF_DICTVALS     '2' /* Push a dict_values instance from TOS dict */
#define PF_DICTITEMS    '3' /* Push a dict_items instance from TOS dict */
#define PF_CALLITER     '4' /* Push a callable-iterator instance from top 2 TOS items */
#define PF_SEQITER      '5' /* Push an iterator instance from top 2 TOS items */
#define PF_BYTEARRITER  '6' /* Push a bytearray iterator instance from top 2 TOS items */
#define PF_TUPLEITER    '7' /* Push a tuple iterator instance from top 2 TOS items */
#define PF_LISTREVITER  '8' /* Push a reversed list iterator instance from top 2 TOS items */ 
#define PF_DICTKEYITER  '9' /* Push a dictionary-keyiterator instance from top 5 TOS items */
#define PF_DICTVALITER  '!' /* Push a dictionary-valueiterator instance from top 5 TOS items */
#define PF_DICTITEMITER '@' /* Push a dictionary-itemiterator instance from top 5 TOS items */
#define PF_SETITER      '#' /* Push a setiterator from top 4 TOS items */
#define PF_FIELDNAMEITER '$' /* Push a fieldnameiterator from top 4 TOS items */
#define PF_FORMATITER   '%' /* Push a formatteriterator from top 3 TOS items */
#define PF_EXCEPTION    '^' /* Push an Exception subclass from TOS item */ 
#define PF_METHOD_DESC  '&' /* Push a method_descriptor from the top 2 TOS items */
#define PF_BI_METHOD    '*' /* Push a 'built-in method' (variant of PyCFunctionObject) type from top 3 TOS items */
#define PF_OP_ITEMGET   '(' /* Push an operator.itemgetter instance from top 2 TOS items */
#define PF_OP_ATTRGET   ')' /* Push an operator.attrgetter instance from top 2 TOS items */
#define PF_OP_METHODCALL '-' /* Push an operator.methodcaller instance from top 3 TOS items */
#define PF_CUSTOM       '+' /* Push an instance returned by an __unpickle__ static method of a type */
#define PF_ALLOC        ':' /* Allocate an object (call tp_alloc) using the type on TOS */

#define EXC_START_MAGIC ((void*)0x1234)
#define EXC_END_MAGIC   ((void*)0x4321)

struct memo_entry{
    int idx;
    PyObject *obj;
};

VEC_IMPL(extern, pobj, PyObject*)

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
    vec_pobj_t     to_free;
    bool           stop;
};

typedef int (*pickle_func_t)(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *stream);
typedef int (*unpickle_func_t)(struct unpickle_ctx *ctx, SDL_RWops *stream);

struct pickle_entry{
    PyTypeObject  *type; 
    pickle_func_t  picklefunc;
};


static bool pickle_obj(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *stream);
static bool pickle_attrs(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw);
static void memoize(struct pickle_ctx *ctx, PyObject *obj);
static bool memo_contains(const struct pickle_ctx *ctx, PyObject *obj);
static int memo_idx(const struct pickle_ctx *ctx, PyObject *obj);
static bool emit_get(const struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw);
static bool emit_put(const struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw);
static bool emit_alloc(const struct pickle_ctx *ctx, SDL_RWops *rw);
static void deferred_free(struct pickle_ctx *ctx, PyObject *obj);

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
static int st_entry_pickle    (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int class_method_descr_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int class_method_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int dict_items_pickle  (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int dict_keys_pickle   (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int dict_values_pickle (struct pickle_ctx *, PyObject *, SDL_RWops *);
static int method_descr_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int method_wrapper_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
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
static int oper_itemgetter_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int oper_attrgetter_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int oper_methodcaller_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int placeholder_inst_pickle(struct pickle_ctx *, PyObject *, SDL_RWops *);
static int exception_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw);
static int custom_pickle      (struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw);

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
static int op_float         (struct unpickle_ctx *, SDL_RWops *);

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
static int op_ext_clsmethod (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_instmethod(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_memdesc   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_method_wrapper(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_range     (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_slice     (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_staticmethod(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_buffer    (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_memview   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_property  (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_enumerate (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_listiter  (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_complex   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_dictproxy (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_reversed  (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_gen       (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_frame     (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_nullval   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_traceback (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_emptyframe(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_weakref   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_emptymod  (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_weakproxy (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_stentry   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_zipimporter(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_dictkeys  (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_dictvalues(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_dictitems (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_calliter  (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_seqiter   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_bytearriter(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_tupleiter (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_revlistiter(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_dictkeyiter(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_dictvaliter(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_dictitemiter(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_setiter   (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_fieldnameiter(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_formatiter(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_exception (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_method_desc(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_bi_method (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_oper_itemgetter(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_oper_attrgetter(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_oper_methodcaller(struct unpickle_ctx *, SDL_RWops *);
static int op_ext_custom    (struct unpickle_ctx *, SDL_RWops *);
static int op_ext_alloc     (struct unpickle_ctx *, SDL_RWops *);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(str) *s_id_qualname_map;

static struct pickle_entry s_type_dispatch_table[] = {
    /* The Python 2.7 public built-in types. Some of these types may be 
     * instantiated directly in any script. Others are additional builtin 
     * types that are used internally in CPython and modules compiled into 
     * the library. Python code may gain references to these 'opaque' objects 
     * but they may not be instantiated directly from scripts. 
     */
    {.type = NULL, /*&PyType_Type*/         .picklefunc = type_pickle                   }, /* type() */
    {.type = NULL, /*&PyBool_Type*/         .picklefunc = bool_pickle                   }, /* bool() */
    {.type = NULL, /*&PyString_Type,*/      .picklefunc = string_pickle                 }, /* str() */
    {.type = NULL, /*&PyByteArray_Type,*/   .picklefunc = bytearray_pickle              }, /* bytearray() */
    {.type = NULL, /*&PyList_Type*/         .picklefunc = list_pickle                   }, /* list() */
    {.type = NULL, /*&PySuper_Type*/        .picklefunc = super_pickle                  }, /* super() */
    {.type = NULL, /*&PyBaseObject_Type*/   .picklefunc = base_obj_pickle               }, /* object() */
    {.type = NULL, /*&PyRange_Type*/        .picklefunc = range_pickle                  }, /* xrange() */
    {.type = NULL, /*&PyDict_Type*/         .picklefunc = dict_pickle                   }, /* dict() */
    {.type = NULL, /*&PySet_Type*/          .picklefunc = set_pickle                    }, /* set() */
#ifdef Py_USING_UNICODE
    {.type = NULL, /*&PyUnicode_Type*/      .picklefunc = unicode_pickle                }, /* unicode() */
#endif
    {.type = NULL, /*&PySlice_Type*/        .picklefunc = slice_pickle                  }, /* slice() */
    {.type = NULL, /*&PyStaticMethod_Type*/ .picklefunc = static_method_pickle          }, /* staticmethod() */
#ifndef WITHOUT_COMPLEX
    {.type = NULL, /*&PyComplex_Type*/      .picklefunc = complex_pickle                }, /* complex() */
#endif
    {.type = NULL, /*&PyFloat_Type*/        .picklefunc = float_pickle                  }, /* float() */
    {.type = NULL, /*&PyBuffer_Type*/       .picklefunc = buffer_pickle                 }, /* buffer() */
    {.type = NULL, /*&PyLong_Type*/         .picklefunc = long_pickle                   }, /* long() */
    {.type = NULL, /*&PyInt_Type*/          .picklefunc = int_pickle                    }, /* int() */
    {.type = NULL, /*&PyFrozenSet_Type*/    .picklefunc = frozen_set_pickle             }, /* frozenset() */
    {.type = NULL, /*&PyProperty_Type*/     .picklefunc = property_pickle               }, /* property() */
    {.type = NULL, /*&PyMemoryView_Type*/   .picklefunc = memory_view_pickle            }, /* memoryview() */
    {.type = NULL, /*&PyTuple_Type*/        .picklefunc = tuple_pickle                  }, /* tuple() */
    {.type = NULL, /*&PyEnum_Type*/         .picklefunc = enum_pickle                   }, /* enumerate() */
    {.type = NULL, /*&PyReversed_Type*/     .picklefunc = reversed_pickle               }, /* reversed(()) */
    {.type = NULL, /*&PyMethod_Type*/       .picklefunc = method_pickle                 }, /* indirectly: instance methods */ 
    {.type = NULL, /*&PyFunction_Type*/     .picklefunc = function_pickle               }, /* indirectly: function */
    {.type = NULL, /*&PyClass_Type*/        .picklefunc = class_pickle                  }, /* indirectly: Old-style class */
    {.type = NULL, /*&PyGen_Type*/          .picklefunc = gen_pickle                    }, /* indirectly: return value from generator function */
    {.type = NULL, /*&PyInstance_Type*/     .picklefunc = instance_pickle               }, /* instance() */
    {.type = NULL, /*&PyFile_Type*/         .picklefunc = file_pickle                   }, /* open() */
    {.type = NULL, /*&PyClassMethod_Type*/  .picklefunc = class_method_pickle           }, /* classmethod() */

    {.type = NULL, /*&PyCell_Type*/         .picklefunc = cell_pickle                   }, /* indirectly: used for closures */
    {.type = NULL, /*&PyModule_Type*/       .picklefunc = module_pickle,                }, /* indirectly: via import */

    /* These are from accessing the attributes of built-in types; created via PyDescr_ API*/
    {.type = NULL, /*&PyGetSetDescr_Type*/  .picklefunc = get_set_descr_pickle          }, /* Wrapper around PyGetSetDef */
    {.type = NULL, /*&PyWrapperDescr_Type*/ .picklefunc = wrapper_descr_pickle          }, /* Wrapper around slot (slotdef) */
    {.type = NULL, /*&PyMemberDescr_Type*/  .picklefunc = member_descr_pickle           }, /* Wrapper around PyMemberDef */
    /* PyClassMethodDescr_Type and PyMethodDescr_Type can only be instantiated during 
     * initialization time for builtin types using PyMemberDefs to implement methods
     * in C. Sublcasses of these types will re-use the same descriptor objects. Thus 
     * all descriptor objects can be indexed as builtins, and no new instances will be
     * created. */
    {.type = NULL, /* &PyClassMethodDescr_Type */ .picklefunc = class_method_descr_pickle},/* Wrapper around PyMethodDef */
    {.type = NULL, /* &PyMethodDescr_Type */      .picklefunc = method_descr_pickle     }, /* Wrapper around PyMethodDef with METH_CLASS set */
    {.type = NULL, /* &wrappertype */             .picklefunc = method_wrapper_pickle   }, /* A PyMethodDescrObject bound to an instance */

    /* This is a reference to C code. As such, we pickle by reference. */
    {.type = NULL, /*&PyCFunction_Type*/    .picklefunc = cfunction_pickle              },
    {.type = NULL, /*&PyCode_Type*/         .picklefunc = code_pickle                   },
    /* These can be retained from sys.exc_info() */
    {.type = NULL, /*&PyTraceBack_Type*/    .picklefunc = traceback_pickle              },
    {.type = NULL, /*&PyFrame_Type*/        .picklefunc = frame_pickle                  },
    {.type = NULL, /*&PyNullImporter_Type*/ .picklefunc = null_importer_pickle          },

    /* Built-in singletons. These may not be instantiated directly  */
    /* The PyNotImplemented_Type and PyNone_Type are not exported. */
    {.type = NULL,                          .picklefunc = not_implemented_pickle        },
    {.type = NULL,                          .picklefunc = none_pickle                   },
    {.type = NULL, /*&PyEllipsis_Type*/     .picklefunc = ellipsis_pickle               },

    /* The following are a result of calling the PyWeakref API with an existing object.
     */
    {.type = NULL, /*&_PyWeakref_RefType*/           .picklefunc = weakref_ref_pickle            },
    {.type = NULL, /*&_PyWeakref_CallableProxyType*/ .picklefunc = weakref_callable_proxy_pickle },
    {.type = NULL, /*&_PyWeakref_ProxyType*/         .picklefunc = weakref_proxy_pickle          },

    {.type = NULL, /*&PySTEntry_Type*/      .picklefunc = st_entry_pickle               },

    /* This is derived from an existing dictionary object using the PyDictProxy API. 
     * The only way to get a dictproxy object via scripting is to access the __dict__
     * attribute of a type object. */
    {.type = NULL, /*&PyDictProxy_Type*/    .picklefunc = dict_proxy_pickle             },

    /* Built-in struct sequences (i.e. 'named tuples') */
    {.type = NULL, /* Long_InfoType */      .picklefunc = long_info_pickle              },
    {.type = NULL, /* FloatInfoType */      .picklefunc = float_info_pickle             },

    /* The following are non-instantiatable named tuple singletons in 'sys' */
    {.type = NULL, /* FlagsType */          .picklefunc = sys_flags_pickle              },
    {.type = NULL, /* VersionInfoType */    .picklefunc = sys_version_pickle            },

    /* Derived with dict built-in methods */
    {.type = NULL, /*&PyDictItems_Type*/    .picklefunc = dict_items_pickle             },
    {.type = NULL, /*&PyDictKeys_Type*/     .picklefunc = dict_keys_pickle              },
    {.type = NULL, /*&PyDictValues_Type*/   .picklefunc = dict_values_pickle            },

    /* Iterator types. Derived by calling 'iter' on an object. */
    {.type = NULL, /*&PyCallIter_Type*/        .picklefunc = call_iter_pickle           },
    {.type = NULL, /*&PySeqIter_Type*/         .picklefunc = seq_iter_pickle            },
    {.type = NULL, /*&PyByteArrayIter_Type*/   .picklefunc = byte_array_iter_pickle     },
    {.type = NULL, /*&PyDictIterItem_Type*/    .picklefunc = dict_iter_item_pickle      },
    {.type = NULL, /*&PyDictIterKey_Type*/     .picklefunc = dict_iter_key_pickle       },
    {.type = NULL, /*&PyDictIterValue_Type*/   .picklefunc = dict_iter_value_pickle     },
    {.type = NULL,                             .picklefunc = list_iter_pickle           },
    {.type = NULL,                             .picklefunc = tuple_iter_pickle          },
    {.type = NULL,                             .picklefunc = list_rev_iter_pickle       },
    {.type = NULL, /* &PySetIter_Type */       .picklefunc = set_iter_pickle            },
    {.type = NULL, /* &PyFieldNameIter_Type */ .picklefunc = field_name_iter_pickle     },
    {.type = NULL, /* &PyFormatterIter_Type */ .picklefunc = formatter_iter_pickle      },

    /* operator module builtins */
    {.type = NULL, /* itemgetter_type */    .picklefunc = oper_itemgetter_pickle        },
    {.type = NULL, /* attrgetter_type */    .picklefunc = oper_attrgetter_pickle        },
    {.type = NULL, /* methodcaller_type */  .picklefunc = oper_methodcaller_pickle      },

    /* A PyCObject cannot be instantiated directly, but may be exported by C
     * extension modules. As it is a wrapper around a raw memory address exported 
     * by some module, we cannot reliablly save and restore it 
     */
    {.type = NULL, /*&PyCObject_Type*/      .picklefunc = NULL                          },
    /* Newer version of CObject */
    {.type = NULL, /*&PyCapsule_Type*/      .picklefunc = NULL                          },

    /* The following built-in types can never be instantiated. 
     */
    {.type = NULL, /*&PyBaseString_Type*/   .picklefunc = NULL                          },

    /* The built-in exception types. All of them can be instantiated directly.  */
    { EXC_START_MAGIC },
    {.type = NULL /* PyExc_BaseException */,              .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_Exception */,                  .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_StandardError */,              .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_TypeError */,                  .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_StopIteration */,              .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_GeneratorExit */,              .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_SystemExit */,                 .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_KeyboardInterrupt */,          .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_ImportError */,                .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_EnvironmentError */,           .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_IOError */,                    .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_OSError */,                    .picklefunc = exception_pickle},
#ifdef MS_WINDOWS
    {.type = NULL /* PyExc_WindowsError */,               .picklefunc = exception_pickle},
#endif
#ifdef __VMS
    {.type = NULL /* PyExc_VMSError */,                   .picklefunc = exception_pickle},
#endif
    {.type = NULL /* PyExc_EOFError */,                   .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_RuntimeError */,               .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_NotImplementedError */,        .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_NameError */,                  .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_UnboundLocalError */,          .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_AttributeError */,             .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_SyntaxError */,                .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_IndentationError */,           .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_TabError */,                   .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_LookupError */,                .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_IndexError */,                 .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_KeyError */,                   .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_ValueError */,                 .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_UnicodeError */,               .picklefunc = exception_pickle},
#ifdef Py_USING_UNICODE
    {.type = NULL /* PyExc_UnicodeEncodeError */,         .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_UnicodeDecodeError */,         .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_UnicodeTranslateError */,      .picklefunc = exception_pickle},
#endif
    {.type = NULL /* PyExc_AssertionError */,             .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_ArithmeticError */,            .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_FloatingPointError */,         .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_OverflowError */,              .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_ZeroDivisionError */,          .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_SystemError */,                .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_ReferenceError */,             .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_MemoryError */,                .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_BufferError */,                .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_Warning */,                    .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_UserWarning */,                .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_DeprecationWarning */,         .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_PendingDeprecationWarning */,  .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_SyntaxWarning */,              .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_RuntimeWarning */,             .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_FutureWarning */,              .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_ImportWarning */,              .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_UnicodeWarning */,             .picklefunc = exception_pickle},
    {.type = NULL /* PyExc_BytesWarning */,               .picklefunc = exception_pickle},
    { EXC_END_MAGIC },
};

/* An 'empty' user-defined type that acts as a placeholder */
static PyObject *s_placeholder_type = NULL;

/* The permafrost engine built-in types */
static struct pickle_entry s_pf_dispatch_table[] = {
    {.type = NULL, /* PyEntity_type */                    .picklefunc = custom_pickle   },
    {.type = NULL, /* PyAnimEntity_type */                .picklefunc = custom_pickle   },
    {.type = NULL, /* PyCombatableEntity_type */          .picklefunc = custom_pickle   },
    {.type = NULL, /* PyTile_type */                      .picklefunc = custom_pickle   },
    {.type = NULL, /* PyWindow_type */                    .picklefunc = custom_pickle   },
    {.type = NULL, /* PyCamera_type */                    .picklefunc = custom_pickle   },
    {.type = NULL, /* PyUIButtonStyle_type */             .picklefunc = custom_pickle   },
    {.type = NULL, /* PyUIHeaderStyle_type */             .picklefunc = custom_pickle   },
    {.type = NULL, /* PyUISelectableStyle_type */         .picklefunc = custom_pickle   },
    {.type = NULL, /* PyUIComboStyle_type */              .picklefunc = custom_pickle   },
    {.type = NULL, /* PyUIToggleStyle_type */             .picklefunc = custom_pickle   },
    {.type = NULL, /* PyUIScrollbarStyle_type*/           .picklefunc = custom_pickle   },
    {.type = NULL, /* PyUIEditStyle_type*/                .picklefunc = custom_pickle   },
    {.type = NULL, /* PyUIPropertyStyle_type*/            .picklefunc = custom_pickle   },
    {.type = NULL, /* PyUISliderStyle_type*/              .picklefunc = custom_pickle   },
    {.type = NULL, /* PyUIProgressStyle_type*/            .picklefunc = custom_pickle   },
    {.type = NULL, /* PyTask_type */                      .picklefunc = custom_pickle   },
    {.type = NULL, /* PyBuildableEntity_type */           .picklefunc = custom_pickle   },
    {.type = NULL, /* PyBuilderEntity_type */             .picklefunc = custom_pickle   },
    {.type = NULL, /* PyResourceEntity_type */            .picklefunc = custom_pickle   },
    {.type = NULL, /* PyHarvesterEntity_type */           .picklefunc = custom_pickle   },
    {.type = NULL, /* PyStorageSiteEntity_type */         .picklefunc = custom_pickle   },
    {.type = NULL, /* PyMovableEntity_type */             .picklefunc = custom_pickle   },
    {.type = NULL, /* PyRegion_type*/                     .picklefunc = custom_pickle   },
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
#ifdef Py_USING_UNICODE
    [UNICODE] = op_unicode,
#endif
    [FLOAT] = op_float,
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
    [PF_CLSMETHOD] = op_ext_clsmethod,
    [PF_INSTMETHOD] = op_ext_instmethod,
    [PF_MEMDESC] = op_ext_memdesc,
    [PF_METHWRAP] = op_ext_method_wrapper,
    [PF_RANGE] = op_ext_range,
    [PF_SLICE] = op_ext_slice,
    [PF_STATMETHOD] = op_ext_staticmethod,
    [PF_BUFFER] = op_ext_buffer,
    [PF_MEMVIEW] = op_ext_memview,
    [PF_PROPERTY] = op_ext_property,
    [PF_ENUMERATE] = op_ext_enumerate,
    [PF_LISTITER] = op_ext_listiter,
#ifndef WITHOUT_COMPLEX
    [PF_COMPLEX] = op_ext_complex,
#endif
    [PF_DICTPROXY] = op_ext_dictproxy,
    [PF_REVERSED] = op_ext_reversed,
    [PF_GEN] = op_ext_gen,
    [PF_FRAME] = op_ext_frame,
    [PF_NULLVAL] = op_ext_nullval,
    [PF_TRACEBACK] = op_ext_traceback,
    [PF_EMPTYFRAME] = op_ext_emptyframe,
    [PF_WEAKREF] = op_ext_weakref,
    [PF_EMPTYMOD] = op_ext_emptymod,
    [PF_PROXY] = op_ext_weakproxy,
    [PF_STENTRY] = op_ext_stentry,
    [PF_DICTKEYS] = op_ext_dictkeys,
    [PF_DICTVALS] = op_ext_dictvalues,
    [PF_DICTITEMS] = op_ext_dictitems,
    [PF_CALLITER] = op_ext_calliter,
    [PF_SEQITER] = op_ext_seqiter,
    [PF_BYTEARRITER] = op_ext_bytearriter,
    [PF_TUPLEITER] = op_ext_tupleiter,
    [PF_LISTREVITER] = op_ext_revlistiter,
    [PF_DICTKEYITER] = op_ext_dictkeyiter,
    [PF_DICTVALITER] = op_ext_dictvaliter,
    [PF_DICTITEMITER] = op_ext_dictitemiter,
    [PF_SETITER] = op_ext_setiter,
    [PF_FIELDNAMEITER] = op_ext_fieldnameiter,
    [PF_FORMATITER] = op_ext_formatiter,
    [PF_EXCEPTION] = op_ext_exception,
    [PF_METHOD_DESC] = op_ext_method_desc,
    [PF_BI_METHOD] = op_ext_bi_method,
    [PF_OP_ITEMGET] = op_ext_oper_itemgetter,
    [PF_OP_ATTRGET] = op_ext_oper_attrgetter,
    [PF_OP_METHODCALL] = op_ext_oper_methodcaller,
    [PF_CUSTOM] = op_ext_custom,
    [PF_ALLOC] = op_ext_alloc,
};

/* Statically-linked builtin modules not imported on initialization which also contain C builtins */
/* (i.e. sys.builtin_module_names) */
static const char *s_extra_indexed_mods[] = {
    "array",
    "_collections",
    "_heapq",
    "exceptions",
    "gc",
    "imp",
    "itertools",
    "math",
    "operator",
    "_warnings",
    "_weakref",
};

/* Create a 'dummy' subclass of every user-subclassable builtin and maintain a 
 * mapping of builtin : subclass. The subclass can be used for creating instances 
 * of user-defined subclasses of builtins. This is the same as using the actual
 * user-created type object for construction, except we avoid any side-effects that 
 * may be present in __new__ or __init__. */
struct sc_map_entry{
    PyTypeObject *builtin;
    PyTypeObject *heap_subtype;
}s_subclassable_builtin_map[] = {
    { NULL, /*&PyBaseObject_Type */         NULL },
    { NULL, /*&PyType_Type */               NULL },
    { NULL, /*&PyString_Type */             NULL },
    { NULL, /*&PyByteArray_Type */          NULL },
    { NULL, /*&PyList_Type */               NULL },
    { NULL, /*&PySuper_Type */              NULL },
    { NULL, /*&PyDict_Type */               NULL },
    { NULL, /*&PySet_Type */                NULL },
    { NULL, /*&PyUnicode_Type */            NULL },
    { NULL, /*&PyStaticMethod_Type */       NULL },
    { NULL, /*&PyComplex_Type */            NULL },
    { NULL, /*&PyFloat_Type */              NULL },
    { NULL, /*&PyLong_Type */               NULL },
    { NULL, /*&PyInt_Type */                NULL },
    { NULL, /*&PyFrozenSet_Type */          NULL },
    { NULL, /*&PyProperty_Type */           NULL },
    { NULL, /*&PyTuple_Type */              NULL },
    { NULL, /*&PyEnum_Type */               NULL },
    { NULL, /*&PyReversed_Type */           NULL },
    { NULL, /*&PyEntity_type*/              NULL },
    { NULL, /*&PyAnimEntity_type*/          NULL },
    { NULL, /*&PyCombatableEntity_type*/    NULL },
    { NULL, /*&PyWindow_type*/              NULL },
    { NULL, /*&PyTile_type*/                NULL },
    { NULL, /*&PyCamera_type*/              NULL },
    { NULL, /*&PyTask_type*/                NULL },
    { NULL, /*&PyBuildableEntity_type*/     NULL },
    { NULL, /*&PyBuilderEntity_type*/       NULL },
    { NULL, /*&PyResourceEntity_type*/      NULL },
    { NULL, /*&PyHarvesterEntity_type*/     NULL },
    { NULL, /*&PyStorageSiteEntity_type*/   NULL },
    { NULL, /*&PyMovableEntity_type*/       NULL },
    { NULL, /*&PyRegion_type*/              NULL },
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/
 
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

static bool strarr_contains(const char **arr, size_t len, const char *item)
{
    for(int i = 0; i < len; i++)
        if(!strcmp(arr[i], item))
            return true;
    return false;
}

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

static bool type_is_subclassable_builtin(PyTypeObject *type)
{
    for(int i = 0; i < ARR_SIZE(s_subclassable_builtin_map); i++) {

        if(s_subclassable_builtin_map[i].builtin == type)
            return true;
    }
    return false;
}

/* Returns a borrowed reference to the type that should be used for 
 * constructing an instance of a particular type. This is either the
 * root builtin type, or a direct subclass of the builtin type that
 * doesn't do anything weird in its' __new__ or __init__. */
static PyObject *constructor_type(PyTypeObject *type)
{
    for(int i = 0; i < ARR_SIZE(s_subclassable_builtin_map); i++) {

        PyTypeObject *builtin = s_subclassable_builtin_map[i].builtin;
        PyTypeObject *sub = s_subclassable_builtin_map[i].heap_subtype;

        /* Every user-defiened type is considered to be a subtype of 
         * 'object', even when PyBaseObject_Type is not one of the bases.
         * This is a Python special case. Leave this check for last. */
        if(builtin == &PyBaseObject_Type)
            continue;

        if(type == builtin)
            return (PyObject*)builtin;
        if(PyType_IsSubtype(type, builtin))
            return (PyObject*)sub;
    }

    if(type == &PyBaseObject_Type)
        return (PyObject*)&PyBaseObject_Type;

    if(PyType_IsSubtype(type, &PyBaseObject_Type)) {
        PyObject *ret = (PyObject*)s_subclassable_builtin_map[0].heap_subtype;
        assert(!strcmp(((PyTypeObject*)ret)->tp_name, "__object_subclass__"));
        return ret;
    }

    return NULL;
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

    /* wrappertype */
    idx = dispatch_idx_for_picklefunc(method_wrapper_pickle);
    assert(s_placeholder_type);
    PyObject *pinst = PyObject_CallFunction(s_placeholder_type, "()", NULL);
    PyObject *mw = PyObject_GetAttrString(pinst, "__setattr__");
    assert(!strcmp(mw->ob_type->tp_name, "method-wrapper"));
    s_type_dispatch_table[idx].type = mw->ob_type;
    Py_DECREF(mw);
    Py_DECREF(pinst);

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

    /* PySetIter_Type */
    idx = dispatch_idx_for_picklefunc(set_iter_pickle);
    PyObject *set = PyObject_CallFunction((PyObject*)&PySet_Type, "()");
    assert(set);
    PyObject *iter = PySet_Type.tp_iter(set);
    assert(iter);
    Py_DECREF(set);
    s_type_dispatch_table[idx].type = iter->ob_type;
    assert(!strcmp(s_type_dispatch_table[idx].type->tp_name, "setiterator"));
    Py_DECREF(iter);

    /* PyFieldNameIter_Type */
    idx = dispatch_idx_for_picklefunc(field_name_iter_pickle);
    PyObject *string = PyString_FromString("test");
    assert(string);
    PyObject *tuple = PyObject_CallMethod(string, "_formatter_field_name_split", "()");
    assert(tuple && PyTuple_GET_SIZE(tuple) > 1);
    s_type_dispatch_table[idx].type = PyTuple_GET_ITEM(tuple, 1)->ob_type;
    assert(!strcmp(s_type_dispatch_table[idx].type->tp_name, "fieldnameiterator"));
    Py_DECREF(tuple);
    Py_DECREF(string);

    /* PyFormatterIter_Type */
    idx = dispatch_idx_for_picklefunc(formatter_iter_pickle);
    string = PyString_FromString("test");
    assert(string);
    iter = PyObject_CallMethod(string, "_formatter_parser", "()");
    assert(iter);
    s_type_dispatch_table[idx].type = iter->ob_type;
    assert(!strcmp(s_type_dispatch_table[idx].type->tp_name, "formatteriterator"));
    Py_DECREF(string);

    /* PyListIter_Type */
    idx = dispatch_idx_for_picklefunc(list_iter_pickle);
    tmp = PyList_New(0);
    assert(tmp);
    iter = PyObject_CallMethod(tmp, "__iter__", "()");
    assert(iter);
    s_type_dispatch_table[idx].type = iter->ob_type;
    assert(!strcmp(s_type_dispatch_table[idx].type->tp_name, "listiterator"));
    Py_DECREF(tmp);

    /* PyListRevIter_Type */
    idx = dispatch_idx_for_picklefunc(list_rev_iter_pickle);
    tmp = PyList_New(0);
    assert(tmp);

    PyObject *reversed = PyObject_CallFunction((PyObject*)&PyReversed_Type, "(O)", tmp);
    Py_DECREF(tmp);
    assert(reversed);

    iter = PyObject_CallMethod(reversed, "__iter__", "()");
    assert(iter);
    s_type_dispatch_table[idx].type = iter->ob_type;
    assert(!strcmp(s_type_dispatch_table[idx].type->tp_name, "listreverseiterator"));
    Py_DECREF(reversed);

    /* PyTupleIter_Type */
    idx = dispatch_idx_for_picklefunc(tuple_iter_pickle);
    tmp = PyTuple_New(0);
    assert(tmp);
    iter = PyObject_CallMethod(tmp, "__iter__", "()");
    assert(iter);
    s_type_dispatch_table[idx].type = iter->ob_type;
    assert(!strcmp(s_type_dispatch_table[idx].type->tp_name, "tupleiterator"));
    Py_DECREF(tmp);

    PyObject *op_mod = PyImport_AddModule("operator"); /* borrowed */

    /* operator.itemgetter */
    idx = dispatch_idx_for_picklefunc(oper_itemgetter_pickle);
    s_type_dispatch_table[idx].type = (PyTypeObject*)PyObject_GetAttrString(op_mod, "itemgetter");
    assert(!strcmp(s_type_dispatch_table[idx].type->tp_name, "operator.itemgetter"));

    /* operator.attrgetter */
    idx = dispatch_idx_for_picklefunc(oper_attrgetter_pickle);
    s_type_dispatch_table[idx].type = (PyTypeObject*)PyObject_GetAttrString(op_mod, "attrgetter");
    assert(!strcmp(s_type_dispatch_table[idx].type->tp_name, "operator.attrgetter"));

    /* operator.methodcaller */
    idx = dispatch_idx_for_picklefunc(oper_methodcaller_pickle);
    s_type_dispatch_table[idx].type = (PyTypeObject*)PyObject_GetAttrString(op_mod, "methodcaller");
    assert(!strcmp(s_type_dispatch_table[idx].type->tp_name, "operator.methodcaller"));

    assert(!PyErr_Occurred());
}

static void load_builtin_types(void)
{
    int base_idx = 0;

    s_type_dispatch_table[base_idx++].type = &PyType_Type;
    s_type_dispatch_table[base_idx++].type = &PyBool_Type;
    s_type_dispatch_table[base_idx++].type = &PyString_Type;
    s_type_dispatch_table[base_idx++].type = &PyByteArray_Type;
    s_type_dispatch_table[base_idx++].type = &PyList_Type;
    s_type_dispatch_table[base_idx++].type = &PySuper_Type;
    s_type_dispatch_table[base_idx++].type = &PyBaseObject_Type;
    s_type_dispatch_table[base_idx++].type = &PyRange_Type;
    s_type_dispatch_table[base_idx++].type = &PyDict_Type;
    s_type_dispatch_table[base_idx++].type = &PySet_Type;
#ifdef Py_USING_UNICODE
    s_type_dispatch_table[base_idx++].type = &PyUnicode_Type;
#endif
    s_type_dispatch_table[base_idx++].type = &PySlice_Type;
    s_type_dispatch_table[base_idx++].type = &PyStaticMethod_Type;
#ifndef WITHOUT_COMPLEX
    s_type_dispatch_table[base_idx++].type = &PyComplex_Type;
#endif
    s_type_dispatch_table[base_idx++].type = &PyFloat_Type;
    s_type_dispatch_table[base_idx++].type = &PyBuffer_Type;
    s_type_dispatch_table[base_idx++].type = &PyLong_Type;
    s_type_dispatch_table[base_idx++].type = &PyInt_Type;
    s_type_dispatch_table[base_idx++].type = &PyFrozenSet_Type;
    s_type_dispatch_table[base_idx++].type = &PyProperty_Type;
    s_type_dispatch_table[base_idx++].type = &PyMemoryView_Type;
    s_type_dispatch_table[base_idx++].type = &PyTuple_Type;
    s_type_dispatch_table[base_idx++].type = &PyEnum_Type;
    s_type_dispatch_table[base_idx++].type = &PyReversed_Type;
    s_type_dispatch_table[base_idx++].type = &PyMethod_Type;
    s_type_dispatch_table[base_idx++].type = &PyFunction_Type;
    s_type_dispatch_table[base_idx++].type = &PyClass_Type;
    s_type_dispatch_table[base_idx++].type = &PyGen_Type;
    s_type_dispatch_table[base_idx++].type = &PyInstance_Type;
    s_type_dispatch_table[base_idx++].type = &PyFile_Type;
    s_type_dispatch_table[base_idx++].type = &PyClassMethod_Type;
    s_type_dispatch_table[base_idx++].type = &PyCell_Type;
    s_type_dispatch_table[base_idx++].type = &PyModule_Type;
    s_type_dispatch_table[base_idx++].type = &PyGetSetDescr_Type;
    s_type_dispatch_table[base_idx++].type = &PyWrapperDescr_Type;
    s_type_dispatch_table[base_idx++].type = &PyMemberDescr_Type;
    base_idx++;
    base_idx++;
    base_idx++;
    s_type_dispatch_table[base_idx++].type = &PyCFunction_Type;
    s_type_dispatch_table[base_idx++].type = &PyCode_Type;
    s_type_dispatch_table[base_idx++].type = &PyTraceBack_Type;
    s_type_dispatch_table[base_idx++].type = &PyFrame_Type;
    s_type_dispatch_table[base_idx++].type = &PyNullImporter_Type;
    base_idx++;
    base_idx++;
    s_type_dispatch_table[base_idx++].type = &PyEllipsis_Type;
    s_type_dispatch_table[base_idx++].type = &_PyWeakref_RefType;
    s_type_dispatch_table[base_idx++].type = &_PyWeakref_CallableProxyType;
    s_type_dispatch_table[base_idx++].type = &_PyWeakref_ProxyType;
    s_type_dispatch_table[base_idx++].type = &PySTEntry_Type;
    s_type_dispatch_table[base_idx++].type = &PyDictProxy_Type;
    base_idx++;
    base_idx++;
    base_idx++;
    base_idx++;
    s_type_dispatch_table[base_idx++].type = &PyDictItems_Type;
    s_type_dispatch_table[base_idx++].type = &PyDictKeys_Type;
    s_type_dispatch_table[base_idx++].type = &PyDictValues_Type;
    s_type_dispatch_table[base_idx++].type = &PyCallIter_Type;
    s_type_dispatch_table[base_idx++].type = &PySeqIter_Type;
    s_type_dispatch_table[base_idx++].type = &PyByteArrayIter_Type;
    s_type_dispatch_table[base_idx++].type = &PyDictIterItem_Type;
    s_type_dispatch_table[base_idx++].type = &PyDictIterKey_Type;
    s_type_dispatch_table[base_idx++].type = &PyDictIterValue_Type;
    base_idx++;
    base_idx++;
    base_idx++;
    base_idx++;
    base_idx++;
    base_idx++;
    base_idx++;
    base_idx++;
    base_idx++;
    s_type_dispatch_table[base_idx++].type = &PyCObject_Type;
    s_type_dispatch_table[base_idx++].type = &PyCapsule_Type;
    s_type_dispatch_table[base_idx++].type = &PyBaseString_Type;

    assert(s_type_dispatch_table[base_idx].type == EXC_START_MAGIC);
}

static void load_subclassable_builtin_refs(void)
{
    int base_idx = 0;
    PyObject *pfmod = PyDict_GetItemString(PySys_GetObject("modules"), "pf");
    assert(pfmod); /* borrowed ref */

    s_subclassable_builtin_map[base_idx++].builtin = &PyBaseObject_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyType_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyString_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyByteArray_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyList_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PySuper_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyDict_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PySet_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyUnicode_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyStaticMethod_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyComplex_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyFloat_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyLong_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyInt_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyFrozenSet_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyProperty_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyTuple_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyEnum_Type;
    s_subclassable_builtin_map[base_idx++].builtin = &PyReversed_Type;

    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "Entity");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "AnimEntity");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "CombatableEntity");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "Window");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "Tile");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "Camera");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "Task");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "BuildableEntity");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "BuilderEntity");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "ResourceEntity");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "HarvesterEntity");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "StorageSiteEntity");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "MovableEntity");
    s_subclassable_builtin_map[base_idx++].builtin =  (PyTypeObject*)PyObject_GetAttrString(pfmod, "Region");

	assert(base_idx == ARR_SIZE(s_subclassable_builtin_map));
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
    int idx = 0;
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "Entity");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "AnimEntity");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "CombatableEntity");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "Tile");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "Window");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "Camera");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "UIButtonStyle");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "UIHeaderStyle");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "UISelectableStyle");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "UIComboStyle");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "UIToggleStyle");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "UIScrollbarStyle");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "UIEditStyle");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "UIPropertyStyle");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "UISliderStyle");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "UIProgressStyle");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "Task");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "BuildableEntity");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "BuilderEntity");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "ResourceEntity");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "HarvesterEntity");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "StorageSiteEntity");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "MovableEntity");
    s_pf_dispatch_table[idx++].type = (PyTypeObject*)PyObject_GetAttrString(pfmod, "Region");

    for(int i = 0; i < ARR_SIZE(s_pf_dispatch_table); i++) {
        assert(s_pf_dispatch_table[i].type);
    }
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

/* Not all types may be directly reachable */
static int reference_all_types(void)
{
    assert(s_placeholder_type);
    PyObject *mapping = PyObject_CallFunction(s_placeholder_type, "()");
    CHK_TRUE(mapping, fail);

    for(int i = 0; i < ARR_SIZE(s_type_dispatch_table); i++) {

        PyObject *type = (PyObject*)s_type_dispatch_table[i].type; 
        if(!type || type == EXC_START_MAGIC || type == EXC_END_MAGIC)
            continue;

        char name[128], *curr = name;
        pf_strlcpy(name, ((PyTypeObject*)type)->tp_name, sizeof(name));
        while(*curr) {
            if(*curr == '.' || isspace(*curr))
                *curr = '-';
            ++curr;
        }

        if(0 != PyObject_SetAttrString(mapping, name, type)) {
            Py_DECREF(mapping); 
            goto fail;
        }
    }

    PyObject *mod = PyImport_AddModule("__builtin__");
    PyObject_SetAttrString(mod, "__all_types__", mapping);
    Py_DECREF(mapping);
    return 0;

fail:
    assert(PyErr_Occurred());
    return -1;
}

static int reference_codecs_builtins(void)
{
    /* force _PyCodecRegistry_Init to get called which will initialize
     * the codec_* fields of the current PyInterpreterState */
    PyCodec_Register(NULL);
    assert(PyErr_ExceptionMatches(PyExc_TypeError));
    PyErr_Clear(); 

    assert(s_placeholder_type);
    PyObject *mapping = PyObject_CallFunction(s_placeholder_type, "()");
    CHK_TRUE(mapping, fail);

    PyInterpreterState *interp = PyThreadState_Get()->interp; 
    assert(interp);
    PyObject *err_registry = interp->codec_error_registry;
    assert(err_registry && PyDict_Check(err_registry));

    PyObject *key, *value;
    Py_ssize_t pos = 0;

    while(PyDict_Next(err_registry, &pos, &key, &value)) {
    
        if(0 != PyObject_SetAttr(mapping, key, value)) {
            Py_DECREF(mapping); 
            goto fail;
        }
    }

    PyObject *mod = PyImport_AddModule("__builtin__");
    PyObject_SetAttrString(mod, "__codecs_builtins__", mapping);
    Py_DECREF(mapping);
    return 0;

fail:
    assert(PyErr_Occurred());
    return -1;
}

static void create_builtin_subclasses(void)
{
    PyObject *args, *sc;
    PyTypeObject *bi;
    PyObject *pfmod = PyDict_GetItemString(PySys_GetObject("modules"), "pf"); /* borrowed */

    for(int idx = 0; idx < ARR_SIZE(s_subclassable_builtin_map); idx++) {

        bi = s_subclassable_builtin_map[idx].builtin;
        assert(bi);

        char name[256];
        pf_snprintf(name, sizeof(name), "__%s_subclass__", bi->tp_name);

        args = Py_BuildValue("s(O){}", name, (PyObject*)bi);
        assert(args);
        sc = PyObject_Call((PyObject*)&PyType_Type, args, NULL);
        assert(sc);
        Py_DECREF(args);
        s_subclassable_builtin_map[idx].heap_subtype = (PyTypeObject*)sc;
    }

    assert(!PyErr_Occurred());
}

static PyObject *qualname_new_ref(const char *qualname)
{
    PyObject *ret = NULL;
    STALLOC(char, copy, strlen(qualname) + 1);
    strcpy(copy, qualname);

    const char *modname = copy;
    char *curr = strstr(copy, ".");
    if(curr)
        *curr++ = '\0';

    PyObject *modules_dict = PySys_GetObject("modules"); /* borrowed */
    assert(modules_dict);
    PyObject *mod = PyDict_GetItemString(modules_dict, modname);
    Py_XINCREF(mod);

    if(!mod && strarr_contains(s_extra_indexed_mods, ARR_SIZE(s_extra_indexed_mods), modname)) {
        mod = PyImport_ImportModule(modname);
    }

    if(!mod) {
        SET_RUNTIME_EXC("Could not find module %s for qualified name %s", modname, qualname);
        goto out;
    }

    PyObject *parent = mod;
    while(curr) {
        char *end = strstr(curr, ".");
        if(end)
            *end++ = '\0';
    
        if(!PyObject_HasAttrString(parent, curr)) {
            Py_DECREF(parent);
            SET_RUNTIME_EXC("Could not look up attribute %s in qualified name %s", curr, qualname);
            goto out;
        }

        PyObject *attr = PyObject_GetAttrString(parent, curr);
        Py_DECREF(parent);
        parent = attr;
        curr = end;
    }
    ret = parent;

out:
    STFREE(copy);
    return ret;
}

/* Due to some attributes implementing the descriptor protocol, it is possible
 * that reading an attribute from an object and then writing it back changes 
 * the attribute. An example is static methods of a class. 'getting' the attribute
 * returns a function, but setting this attribute to a function object will cause
 * Python to wrap it in a method. Thus, when we read back the attribute, it will
 * be an 'unbound method' rather than a function object. This routine handles 
 * such special cases, setting the attribute such that its' original value is 
 * preserved. */
static int setattr_nondestructive(PyObject *obj, PyObject *name, PyObject *val)
{
    assert(PyString_Check(name));
    if(PyType_Check(obj) && PyFunction_Check(val)) {
    
        PyObject *descr =_PyType_Lookup((PyTypeObject*)obj, name);
        assert(descr);
        PyObject_SetAttr(obj, name, descr);
    }else{
        PyObject_SetAttr(obj, name, val);
    }

    if(PyErr_Occurred())
        return -1;

    int ret = 0;
    PyObject *readback = PyObject_GetAttr(obj, name);

    if(readback != val) {

        PyObject *repr = PyObject_Repr(obj);
        SET_RUNTIME_EXC("Unexpected attribute destruction: [%s] of [%s]",
            PyString_AS_STRING(name), PyString_AS_STRING(repr));
        Py_DECREF(repr);
        ret = -1;
    }

    Py_DECREF(readback);
    return ret;
}

/* Query if an attribute of an object is a descriptor with
 * a user-defined __get__ method, begin careful not to actually
 * invoke the method, which may have arbitrary side-effects. */
static bool attr_is_user_descr(PyObject *obj, PyObject *name)
{
    assert(PyString_Check(name));

    PyObject *descr;
    if(PyType_Check(obj)) {
        descr =_PyType_Lookup((PyTypeObject*)obj, name);
    }else {
        descr =_PyType_Lookup(obj->ob_type, name);
    }

    if(!descr)
        return false;

    if(PyType_HasFeature(descr->ob_type, Py_TPFLAGS_HAVE_CLASS)
    && descr->ob_type->tp_descr_get) {

        PyObject *getter = PyObject_GetAttrString(descr, "__get__");
        assert(getter);

        if(!PyMethod_Check(getter)) {
            Py_DECREF(getter);
            return false;
        }

        Py_DECREF(getter);
        return true;
    }
    return false;
}


/* Non-derived attributes are those that don't return a new 
 * object on attribute lookup. 
 * This function returns a new reference.
 */
static PyObject *nonderived_writable_attrs(PyObject *obj)
{
    /* Calling 'dir' on a proxy object will get the attributes of 
     * the object it is referencing. We don't want this. */
    if(PyWeakref_CheckProxy(obj)) {
        return PyDict_New();
    }

    PyObject *attrs = PyObject_Dir(obj);
    assert(attrs);
    PyObject *ret = PyDict_New();
    assert(ret);

    for(int i = 0; i < PyList_Size(attrs); i++) {

        PyObject *name = PyList_GET_ITEM(attrs, i); /* borrowed */
        assert(PyString_Check(name));

        if(attr_is_user_descr(obj, name))
            continue;

        /* Don't touch the frame's locals - getting this causes modification of 
         * the frame. We save the locals when we save the frame */
        if(PyFrame_Check(obj) && !strcmp(PyString_AS_STRING(name), "f_locals"))
            continue;

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
        if(0 != setattr_nondestructive(obj, name, attr)) {
            assert(PyErr_Occurred());        
            if(obj->ob_type != &PyFrame_Type) {
                assert(PyErr_ExceptionMatches(PyExc_TypeError)
                    || PyErr_ExceptionMatches(PyExc_AttributeError)
                    || PyErr_ExceptionMatches(PyExc_RuntimeError));
            }else{
                assert(PyErr_ExceptionMatches(PyExc_TypeError)
                    || PyErr_ExceptionMatches(PyExc_AttributeError)
                    || PyErr_ExceptionMatches(PyExc_ValueError));
            }
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

static int exception_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyExceptionInstance_Check(obj));

    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    const char ops[] = {PF_EXTEND, PF_EXCEPTION};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int custom_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    PyObject *pmeth = PyObject_GetAttrString(obj, "__pickle__");
    if(!pmeth || !PyCallable_Check(pmeth)) {
        SET_RUNTIME_EXC("Object does not have a '__pickle__' method");
        Py_XDECREF(pmeth);
        goto fail;
    }
    Py_DECREF(pmeth);

    PyObject *umeth = PyObject_GetAttrString((PyObject*)obj->ob_type, "__unpickle__");
    if(!umeth || !PyCallable_Check(umeth)) {
        SET_RUNTIME_EXC("Object does not have a class '__unpickle__' method");
        goto fail;
    }
    Py_DECREF(umeth);

    const char mark = MARK;
    CHK_TRUE(rw->write(rw, &mark, 1, 1), fail);

    struct py_pickle_ctx user = (struct py_pickle_ctx) {
        .private_ctx = ctx,
        .stream = rw,
        .memo_contains = (void*)memo_contains,
        .memoize = (void*)memoize,
        .emit_put = (void*)emit_put,
        .emit_get = (void*)emit_get,
        .emit_alloc = (void*)emit_alloc,
        .pickle_obj = (void*)pickle_obj,
        .deferred_free = (void*)deferred_free,
    };

    pmeth = PyObject_GetAttrString(obj, "__pickle__");
    PyObject *args = PyTuple_New(0);
    PyObject *kwargs = Py_BuildValue("{s:s#}", "__ctx__", (void*)&user, sizeof(struct py_pickle_ctx));

    PyObject * ret = NULL;
    if(pmeth && args && kwargs) {
        ret = PyObject_Call(pmeth, args, kwargs);
    }
    Py_XDECREF(pmeth);
    Py_XDECREF(args);
    Py_XDECREF(kwargs);

    if(!ret || !PyString_Check(ret)) {
        SET_RUNTIME_EXC("Error pickling %s instance (%p)", obj->ob_type->tp_name, obj);
        Py_XDECREF(ret);
        goto fail;
    }

    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    vec_pobj_push(&ctx->to_free, ret);
    CHK_TRUE(pickle_obj(ctx, ret, rw), fail);

    /* id(obj) isn't in the memo now. If it shows up there after saving the 
     * type, then the type must recursively reference the object. In that case,
     * just fetch it's value from the memo without pushing anything else onto 
     * the stack.
     */
    if(memo_contains(ctx, obj)) {

        /* pop the stack stuff we pushed */
        const char pmark = POP_MARK;
        CHK_TRUE(rw->write(rw, &pmark, 1, 1), fail);

        /* fetch from memo */
        CHK_TRUE(emit_get(ctx, obj, rw), fail);
        return 0;
    }

    const char ops[] = {PF_EXTEND, PF_CUSTOM};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static PyObject *method_funcs(PyObject *obj)
{
    assert(PyType_Check(obj));
    PyObject *attrs = PyObject_Dir(obj);
    assert(attrs);
    PyObject *ret = PyDict_New();
    assert(ret);

    for(int i = 0; i < PyList_Size(attrs); i++) {

        PyObject *name = PyList_GET_ITEM(attrs, i); /* borrowed */
        assert(PyString_Check(name));

        if(attr_is_user_descr(obj, name)) {
        
            PyObject *desc = _PyType_Lookup((PyTypeObject*)obj, name); /* borrowed */
            assert(desc);
            assert(Py_TYPE(desc)->tp_descr_get);
            PyDict_SetItem(ret, name, desc);
            continue;
        }

        /* If 'dir' gave us the name but PyObject_HasAttr returns
         * false, this means this is a descriptor. It should be
         * passed in to the types's dict at initialization */
        if(!PyObject_HasAttr(obj, name)) {

            PyObject *desc = _PyType_Lookup((PyTypeObject*)obj, name); /* borrowed */
            assert(desc);
            assert(Py_TYPE(desc)->tp_descr_get);
            PyDict_SetItem(ret, name, desc);
            continue;
        }

        PyObject *attr = PyObject_GetAttr(obj, name);
        assert(attr);

        if(PyFunction_Check(attr)) {
            PyObject *sm = PyStaticMethod_New(attr);
            assert(sm);
            PyDict_SetItem(ret, name, sm);
            Py_DECREF(sm);
            Py_DECREF(attr);
            continue;
        }

        if(!PyMethod_Check(attr)) {
            Py_DECREF(attr);
            continue;
        }

        PyMethodObject *meth = (PyMethodObject*)attr;
        if(meth->im_self
        && meth->im_self->ob_type != (PyTypeObject*)obj) {
            PyObject *clsmeth = PyClassMethod_New(meth->im_func);
            PyDict_SetItem(ret, name, clsmeth);
            Py_DECREF(clsmeth);
        }else{
            PyDict_SetItem(ret, name, meth->im_func);
        }

        Py_DECREF(attr);
    }

    Py_DECREF(attrs);
    return ret;
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
    PyObject *dict = method_funcs(obj);
    vec_pobj_push(&ctx->to_free, dict);

    PyObject *str = PyString_FromString("__slots__");
    vec_pobj_push(&ctx->to_free, str);

    if(PyObject_HasAttr(obj, str)) {

        PyObject *slots = PyObject_GetAttr(obj, str);
        PyDict_SetItem(dict, str, slots);
        Py_DECREF(slots);
    }

    CHK_TRUE(pickle_obj(ctx, dict, rw), fail);

    /* Push metaclass */
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    /* If the type is found in the memo now, that means 
     * it recursively references itself via one of its' attributes.
     * In that case, pop the things we were going to use to construct
     * the type, and fetch it from the memo instead.
     */
    if(memo_contains(ctx, obj)) {
        /* Pop the stuff we just pushed */
        const char pops[] = {POP, POP, POP, POP};
        CHK_TRUE(rw->write(rw, pops, ARR_SIZE(pops), 1), fail);
        /* Get the type from the memo */
        CHK_TRUE(emit_get(ctx, obj, rw), fail);
        return 0;
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

    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

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
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    char *buff = PyByteArray_AS_STRING(obj);
    size_t len = PyByteArray_GET_SIZE(obj);

    Py_UNICODE *uni = malloc(Py_UNICODE_SIZE * len);
    for(int i = 0; i < len; i++)
        uni[i] = buff[i];
    PyObject *uniobj = PyUnicode_FromUnicode(uni, len);

    PyObject *str = PyUnicode_EncodeUTF7(PyUnicode_AS_UNICODE(uniobj), PyUnicode_GET_SIZE(uniobj), true, true, "strict"); 
    assert(strlen(PyString_AS_STRING(str)) == PyString_GET_SIZE(str));
    vec_pobj_push(&ctx->to_free, str);

    Py_DECREF(uniobj);
    PF_FREE(uni);
    CHK_TRUE(pickle_obj(ctx, str, rw), fail);
    
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
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);
    CHK_TRUE(rw->write(rw, &empty_list, 1, 1), fail);

    if(PyList_Size(obj) == 0)
        return 0;

    /* Memoize the empty list before pickling the elements. The elements may 
     * reference the list itself. */
    assert(!memo_contains(ctx, obj));
    memoize(ctx, obj);
    CHK_TRUE(emit_put(ctx, obj, rw), fail);

    CHK_TRUE(rw->write(rw, &mark, 1, 1), fail);

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
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);
    CHK_TRUE(pickle_obj(ctx, (PyObject*)su->type, rw), fail);
    if(su->obj) {
        CHK_TRUE(pickle_obj(ctx, su->obj, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

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

    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

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
    assert(PyRange_Check(obj));
    rangeobject *range = (rangeobject*)obj;

    PyObject *ilow = PyLong_FromLong(range->start);
    PyObject *ihigh = PyLong_FromLong(range->start + (range->len * range->step));
    PyObject *step = PyLong_FromLong(range->step);
    vec_pobj_push(&ctx->to_free, ilow);
    vec_pobj_push(&ctx->to_free, ihigh);
    vec_pobj_push(&ctx->to_free, step);

    CHK_TRUE(pickle_obj(ctx, ilow, rw), fail);
    CHK_TRUE(pickle_obj(ctx, ihigh, rw), fail);
    CHK_TRUE(pickle_obj(ctx, step, rw), fail);

    const char ops[] = {PF_EXTEND, PF_RANGE};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int dict_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    const char empty_dict = EMPTY_DICT;
    const char setitems = SETITEMS;
    const char mark = MARK;

    assert(PyDict_Check(obj));
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);
    CHK_TRUE(rw->write(rw, &empty_dict, 1, 1), fail);

    if(PyDict_Size(obj) == 0)
        return 0;

    /* Memoize the empty dict before pickling the elements. The elements may 
     * reference the list itself. */
    assert(!memo_contains(ctx, obj));
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
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    const char ops[] = {PF_EXTEND, PF_SET};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

#ifdef Py_USING_UNICODE
static int unicode_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyUnicode_Check(obj));
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    const char unicode = UNICODE;
    CHK_TRUE(rw->write(rw, &unicode, 1, 1), fail);

    PyObject *str = PyUnicode_EncodeUTF7(PyUnicode_AS_UNICODE(obj), PyUnicode_GET_SIZE(obj), true, true, "strict"); 
    assert(strlen(PyString_AS_STRING(str)) == PyString_GET_SIZE(str));

    int nwritten = rw->write(rw, PyString_AS_STRING(str), 1, PyString_GET_SIZE(str));
    CHK_TRUE(nwritten == PyString_GET_SIZE(str), fail);
    vec_pobj_push(&ctx->to_free, str);

    CHK_TRUE(rw->write(rw, "\0\n", 2, 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}
#endif

static int slice_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PySlice_Check(obj));
    PySliceObject *slice = (PySliceObject*)obj;

    CHK_TRUE(pickle_obj(ctx, slice->start, rw), fail);
    CHK_TRUE(pickle_obj(ctx, slice->stop, rw), fail);
    CHK_TRUE(pickle_obj(ctx, slice->step, rw), fail);

    const char ops[] = {PF_EXTEND, PF_SLICE};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int static_method_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyType_IsSubtype(obj->ob_type, &PyStaticMethod_Type));
    staticmethod *method = (staticmethod*)obj;

    assert(method->sm_callable);
    CHK_TRUE(pickle_obj(ctx, method->sm_callable, rw), fail);
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    const char ops[] = {PF_EXTEND, PF_STATMETHOD};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

#ifndef WITHOUT_COMPLEX
static int complex_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyComplex_Check(obj));
    PyComplexObject *cmplx = (PyComplexObject*)obj;

    PyObject *real = PyFloat_FromDouble(cmplx->cval.real);
    PyObject *imag = PyFloat_FromDouble(cmplx->cval.imag);
    vec_pobj_push(&ctx->to_free, real);
    vec_pobj_push(&ctx->to_free, imag);

    CHK_TRUE(pickle_obj(ctx, real, rw), fail);
    CHK_TRUE(pickle_obj(ctx, imag, rw), fail);
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    const char ops[] = {PF_EXTEND, PF_COMPLEX};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}
#endif

static int float_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyFloat_Check(obj));
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    double d = PyFloat_AS_DOUBLE(obj);

    const char ops[] = {FLOAT};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);

    char *buff = PyOS_double_to_string(d, 'g', 17, 0, NULL);
    if(!buff) {
        PyErr_NoMemory();
        goto fail;
    }

    if(!rw->write(rw, buff, strlen(buff), 1)) {
        PyMem_Free(buff);
        goto fail;
    }
    PyMem_Free(buff);
    buff = NULL;

    CHK_TRUE(rw->write(rw, "\n", 1, 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int buffer_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyBuffer_Check(obj));
    PyBufferObject *buff = (PyBufferObject*)obj;

    /* A buffer object may be created from the C API via the PyBuffer_FromMemory 
     * and PyBuffer_FromReadWriteMemory calls. However, no object of this type 
     * are exposed to the user scripts. As well, it is not possible to create a 
     * raw memory buffer object from scripting. So the only place where these may 
     * pop up is in 3rd party C extensions, which are not supported, in a general 
     * case. */
    if(buff->b_ptr) {
        assert(!buff->b_base);
        SET_RUNTIME_EXC("Picking raw memory buffer objects is not supported. Only buffer objects instantiated "
            "with an object supporting the buffer protocol are supported.");
        return -1;
    }
    assert(buff->b_base);

    PyObject *size = PyLong_FromLong(buff->b_size);
    PyObject *offset = PyLong_FromLong(buff->b_offset);
    PyObject *readonly = PyLong_FromLong(buff->b_readonly);
    vec_pobj_push(&ctx->to_free, size);
    vec_pobj_push(&ctx->to_free, offset);
    vec_pobj_push(&ctx->to_free, readonly);

    CHK_TRUE(pickle_obj(ctx, buff->b_base, rw), fail);
    CHK_TRUE(pickle_obj(ctx, size, rw), fail);
    CHK_TRUE(pickle_obj(ctx, offset, rw), fail);
    CHK_TRUE(pickle_obj(ctx, readonly, rw), fail);

    const char ops[] = {PF_EXTEND, PF_BUFFER};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int long_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyObject *repr = NULL;

    assert(PyLong_Check(obj));
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    repr = PyObject_Repr(obj);
    CHK_TRUE(repr, fail);
    size_t repr_len = strlen(PyString_AS_STRING(repr)) - 1; /* strip L suffix */
    assert(PyString_AS_STRING(repr)[repr_len] == 'L');

    const char ops[] = {LONG};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    CHK_TRUE(rw->write(rw, PyString_AS_STRING(repr), repr_len, 1), fail);
    CHK_TRUE(rw->write(rw, "\n", 1, 1), fail);
    Py_DECREF(repr);
    return 0;

fail:
    Py_XDECREF(repr);
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int int_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    assert(PyInt_Check(obj));
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    char str[32];
    long l = PyInt_AS_LONG((PyIntObject *)obj);

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
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

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
    assert(PyType_IsSubtype(obj->ob_type, &PyProperty_Type));
    propertyobject *prop = (propertyobject*)obj;

    if(prop->prop_get) {
        CHK_TRUE(pickle_obj(ctx, prop->prop_get, rw), fail);
    }else{
        CHK_TRUE(0 == none_pickle(ctx, Py_None, rw), fail);
    }

    if(prop->prop_set) {
        CHK_TRUE(pickle_obj(ctx, prop->prop_set, rw), fail);
    }else{
        CHK_TRUE(0 == none_pickle(ctx, Py_None, rw), fail);
    }

    if(prop->prop_del) {
        CHK_TRUE(pickle_obj(ctx, prop->prop_del, rw), fail);
    }else{
        CHK_TRUE(0 == none_pickle(ctx, Py_None, rw), fail);
    }

    if(prop->prop_doc) {
        CHK_TRUE(pickle_obj(ctx, prop->prop_doc, rw), fail);
    }else{
        CHK_TRUE(0 == none_pickle(ctx, Py_None, rw), fail);
    }
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    const char ops[] = {PF_EXTEND, PF_PROPERTY};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int memory_view_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyMemoryView_Check(obj));
    PyMemoryViewObject *mview = (PyMemoryViewObject*)obj;

    /* Similar to legacy 'buffer' objects, raw-byte based memory views (created 
     * via the PyMemoryView_FromBuffer API) are not able to be created directly 
     * from scripting. They may be used by some C implementations (such as 
     * BufferedIO). However, memory view object handles should not leak to 
     * scripts. If a 3rd party C extension leaks them, we don't support it. */
    if(NULL == mview->base) {
        SET_RUNTIME_EXC("raw-byte memoryview objects are not supported");
        goto fail;
    }

    CHK_TRUE(pickle_obj(ctx, mview->base, rw), fail);

    const char ops[] = {PF_EXTEND, PF_MEMVIEW};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

/* From cPickle:
 * Tuples are the only builtin immutable type that can be recursive
 * (a tuple can be reached from itself), and that requires some subtle
 * magic so that it works in all cases.  IOW, this is a long routine.
 */
static int tuple_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    assert(PyTuple_Check(obj));
    Py_ssize_t len = PyTuple_Size((PyObject*)obj);

    if(len == 0) {

        CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);
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

    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);
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
    assert(PyType_IsSubtype(obj->ob_type, &PyEnum_Type));
    enumobject *en = (enumobject*)obj;

    PyObject *index = PyLong_FromLong(en->en_index);
    vec_pobj_push(&ctx->to_free, index);

    CHK_TRUE(pickle_obj(ctx, index, rw), fail);
    CHK_TRUE(pickle_obj(ctx, en->en_sit, rw), fail);

    if(en->en_result) {
        CHK_TRUE(pickle_obj(ctx, en->en_result, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    if(en->en_longindex) {
        CHK_TRUE(pickle_obj(ctx, en->en_longindex, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    const char ops[] = {PF_EXTEND, PF_ENUMERATE};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int reversed_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyType_IsSubtype(obj->ob_type, &PyReversed_Type));
    reversedobject *rev = (reversedobject*)obj;

    PyObject *index = PyLong_FromSsize_t(rev->index);
    vec_pobj_push(&ctx->to_free, index);

    CHK_TRUE(pickle_obj(ctx, index, rw), fail);
    if(rev->seq) {
        CHK_TRUE(pickle_obj(ctx, rev->seq, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }
    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    const char ops[] = {PF_EXTEND, PF_REVERSED};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int method_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) 
{
    TRACE_PICKLE(obj);
    assert(PyMethod_Check(obj));
    PyMethodObject *meth = (PyMethodObject*)obj;

    CHK_TRUE(pickle_obj(ctx, meth->im_func, rw), fail);
    if(!meth->im_self) {
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, meth->im_self, rw), fail);
    }
    CHK_TRUE(pickle_obj(ctx, meth->im_class, rw), fail);

    const char ops[] = {PF_EXTEND, PF_INSTMETHOD};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
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
    assert(!memo_contains(ctx, obj));
    memoize(ctx, obj);
    CHK_TRUE(emit_put(ctx, obj, rw), fail);

    CHK_TRUE(pickle_obj(ctx, func->func_code, rw), fail);
    CHK_TRUE(pickle_obj(ctx, func->func_globals, rw), fail);

    if(func->func_closure) {
        CHK_TRUE(pickle_obj(ctx, func->func_closure, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    if(func->func_module) {
        CHK_TRUE(pickle_obj(ctx, func->func_module, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    if(func->func_defaults) {
        CHK_TRUE(pickle_obj(ctx, func->func_defaults, rw), fail);
    }else{
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
    assert(PyGen_Check(obj));
    PyGenObject *gen = (PyGenObject*)obj;

    /* When a generator is exhausted, its' 'gi_frame' field is set 
     * to NULL. However, the generator still retains a reference
     * to the code object. In this case, pickle the code object 
     * instead. */
    assert(gen->gi_code);
    if(gen->gi_frame) {
        CHK_TRUE(pickle_obj(ctx, (PyObject*)gen->gi_frame, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, gen->gi_code, rw), fail);
    }

    const char ops[] = {PF_EXTEND, PF_GEN};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
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
    assert(PyFile_Check(obj));
    PyFileObject *file = (PyFileObject*)obj;
    assert(PyString_Check(file->f_name));

    if(file->f_fp == stdin  || !strcmp(PyString_AS_STRING(file->f_name), "<stdin>")
    || file->f_fp == stdout || !strcmp(PyString_AS_STRING(file->f_name), "<stdout>")
    || file->f_fp == stderr || !strcmp(PyString_AS_STRING(file->f_name), "<stderr>")) {
        return builtin_pickle(ctx, obj, rw);    
    }

    TRACE_PICKLE(obj);
    SET_RUNTIME_EXC("Could not pickle file: %s. Only stdin, stdout, and stderr are supported.", 
        PyString_AS_STRING(file->f_name));
    return -1;
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

    /* The module can be self-referencing. Push an empty instance and 
     * memoize it */
    const char emptymod[] = {PF_EXTEND, PF_EMPTYMOD};
    CHK_TRUE(rw->write(rw, emptymod, ARR_SIZE(emptymod), 1), fail);

    assert(!memo_contains(ctx, obj));
    memoize(ctx, obj);
    CHK_TRUE(emit_put(ctx, obj, rw), fail);

    PyModuleObject *mod = (PyModuleObject*)obj;
    CHK_TRUE(pickle_obj(ctx, mod->md_dict, rw), fail);

    /* Now set the module's dict */
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
    assert(obj->ob_type == &PyMemberDescr_Type);
    PyMemberDescrObject *desc = (PyMemberDescrObject*)obj;

    /* These could be dynamically created on a type with __slots__, for example */
    TRACE_PICKLE(obj);

    CHK_TRUE(pickle_obj(ctx, (PyObject*)desc->d_type, rw), fail);

    assert(desc->d_member->name);
    PyObject *str = PyString_FromString(desc->d_member->name);
    vec_pobj_push(&ctx->to_free, str);
    CHK_TRUE(pickle_obj(ctx, str, rw), fail);

    const char ops[] = {PF_EXTEND, PF_MEMDESC};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int dict_proxy_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(obj->ob_type == &PyDictProxy_Type);
    proxyobject *proxy = (proxyobject*)obj;

    CHK_TRUE(pickle_obj(ctx, proxy->dict, rw), fail);

    const char ops[] = {PF_EXTEND, PF_DICTPROXY};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
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
    assert(obj->ob_type == &PyCFunction_Type);
    PyCFunctionObject *func = (PyCFunctionObject*)obj;

    /* Instances of unbounded built-in functions are never re-created. It is sufficient 
     * to pickle them by reference. */
    if(!func->m_self || !strcmp(func->m_ml->ml_name, "__new__")) {
        return builtin_pickle(ctx, obj, rw);
    }

    TRACE_PICKLE(obj);

    CHK_TRUE(pickle_obj(ctx, func->m_self, rw), fail);
    CHK_TRUE(pickle_obj(ctx, (PyObject*)func->m_self->ob_type, rw), fail);

    PyObject *name = PyString_FromString(func->m_ml->ml_name);
    vec_pobj_push(&ctx->to_free, name);
    CHK_TRUE(pickle_obj(ctx, name, rw), fail);

    const char ops[] = {PF_EXTEND, PF_BI_METHOD};
    CHK_TRUE(rw->write(rw, ops, 2, 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
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
    assert(PyTraceBack_Check(obj));
    PyTracebackObject *tb = (PyTracebackObject*)obj;

    if(tb->tb_next) {
        CHK_TRUE(pickle_obj(ctx, (PyObject*)tb->tb_next, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    CHK_TRUE(pickle_obj(ctx, (PyObject*)tb->tb_frame, rw), fail);

    PyObject *lineno = PyInt_FromLong(tb->tb_lineno);
    PyObject *lasti = PyInt_FromLong(tb->tb_lasti);
    vec_pobj_push(&ctx->to_free, lineno);
    vec_pobj_push(&ctx->to_free, lasti);

    CHK_TRUE(pickle_obj(ctx, lineno, rw), fail);
    CHK_TRUE(pickle_obj(ctx, lasti, rw), fail);

    const char ops[] = {PF_EXTEND, PF_TRACEBACK};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static Py_ssize_t frame_extra_size(PyFrameObject *frame)
{
    Py_ssize_t extras, ncells, nfrees;
    ncells = PyTuple_GET_SIZE(frame->f_code->co_cellvars);
    nfrees = PyTuple_GET_SIZE(frame->f_code->co_freevars);
    extras = frame->f_code->co_stacksize + frame->f_code->co_nlocals 
        + ncells + nfrees;
    return extras;
}

static int frame_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyFrame_Check(obj));
    PyFrameObject *f = (PyFrameObject*)obj;

    PyObject *valsize = PyInt_FromSsize_t(frame_extra_size(f));
    vec_pobj_push(&ctx->to_free, valsize);
    CHK_TRUE(pickle_obj(ctx, valsize, rw), fail);

    /* Creating a dummy frame object, memoize it, and set all its' 
     * attributes after. This is to handle self-referencing cases. 
     * Pop it, but keep it in the memo. We will push it again last. */
    const char emptyframe[] = {PF_EXTEND, PF_EMPTYFRAME};
    CHK_TRUE(rw->write(rw, emptyframe, ARR_SIZE(emptyframe), 1), fail);

    assert(!memo_contains(ctx, obj));
    memoize(ctx, obj);
    CHK_TRUE(emit_put(ctx, obj, rw), fail);

    const char pop[] = {POP};
    CHK_TRUE(rw->write(rw, pop, ARR_SIZE(pop), 1), fail);

    /* The code, globals, and locals are all we need to construct a new 
     * frame object. Additionally, pickle the frame's stack workarea 
     * (valuestack), the last instruction pointer, the blockstack, and 
     * the 'fast' locals namespace. This captures the state of frames 
     * that have been suspened by a yeild statement, or have been evaluated 
     * already. 
     */
    size_t nvals = (f->f_stacktop == NULL) ? 0 : (f->f_stacktop - f->f_valuestack);
    /* None to signal a NULL f_stackstop, which indicates that a frame
     * has already been evaluated. */
    PyObject *nv = f->f_stacktop == NULL ? (Py_INCREF(Py_None), Py_None) 
                                         : PyInt_FromLong(nvals);
    vec_pobj_push(&ctx->to_free, nv);
    CHK_TRUE(pickle_obj(ctx, nv, rw), fail);

    const char mark[] = {MARK};
    CHK_TRUE(rw->write(rw, mark, ARR_SIZE(mark), 1), fail);
    
    /* Some of the local state is in the valuestack. For example, if the 
     * yield statement is inside a for loop over a list, the listiterator
     * (which keeps track of the current index) will be on the valuestack. 
     */
    for(int i = nvals-1; i >= 0; i--) {
        CHK_TRUE(pickle_obj(ctx, f->f_valuestack[i], rw), fail);
    }

    CHK_TRUE(rw->write(rw, mark, ARR_SIZE(mark), 1), fail);
    for(int i = f->f_iblock-1; i >= 0; i--) {

        const PyTryBlock *b = &f->f_blockstack[i];
        PyObject *type = PyInt_FromLong(b->b_type);
        PyObject *handler = PyInt_FromLong(b->b_handler);
        PyObject *level = PyInt_FromLong(b->b_level);

        vec_pobj_push(&ctx->to_free, type);
        vec_pobj_push(&ctx->to_free, handler);
        vec_pobj_push(&ctx->to_free, level);

        CHK_TRUE(pickle_obj(ctx, type, rw), fail);
        CHK_TRUE(pickle_obj(ctx, handler, rw), fail);
        CHK_TRUE(pickle_obj(ctx, level, rw), fail);
    }

    /* Pickle the 'fast' locals namespace */
    CHK_TRUE(rw->write(rw, mark, ARR_SIZE(mark), 1), fail);

    size_t nextra = f->f_valuestack - f->f_localsplus;
    for(int i = nextra-1; i >= 0; i--) {
        PyObject *curr = f->f_localsplus[i];
        if(!curr) {
            const char ops[] = {PF_EXTEND, PF_NULLVAL}; 
            CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
        }else{
            CHK_TRUE(pickle_obj(ctx, curr, rw), fail);
        }
    }

    PyObject *lasti = PyInt_FromLong(f->f_lasti);
    vec_pobj_push(&ctx->to_free, lasti);
    CHK_TRUE(pickle_obj(ctx, lasti, rw), fail);

    PyObject *lineno = PyInt_FromLong(f->f_lineno);
    vec_pobj_push(&ctx->to_free, lineno);
    CHK_TRUE(pickle_obj(ctx, lineno, rw), fail);

    if(f->f_back) {
        CHK_TRUE(pickle_obj(ctx, (PyObject*)f->f_back, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    /* Pickle the constructor args last, so that during unpickling 
     * we can construct the frame and pop directly into it. */
    CHK_TRUE(pickle_obj(ctx, (PyObject*)f->f_code, rw), fail);
    CHK_TRUE(pickle_obj(ctx, f->f_globals, rw), fail);

    if(f->f_locals) {
        CHK_TRUE(pickle_obj(ctx, f->f_locals, rw), fail);
    }else {
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    /* Push the dummy frame object */
    assert(memo_contains(ctx, obj));
    CHK_TRUE(emit_get(ctx, obj, rw), fail);

    const char ops[] = {PF_EXTEND, PF_FRAME};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
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
    assert(PyWeakref_CheckRef(obj));
    PyWeakReference *ref = (PyWeakReference*)obj;

    assert(ref->wr_object == Py_None || PyType_SUPPORTS_WEAKREFS(Py_TYPE(ref->wr_object)));
    CHK_TRUE(pickle_obj(ctx, ref->wr_object, rw), fail);

    if(ref->wr_callback) {
        CHK_TRUE(pickle_obj(ctx, ref->wr_callback, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    const char ops[] = {PF_EXTEND, PF_WEAKREF};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int weakref_callable_proxy_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) 
{
    return weakref_proxy_pickle(ctx, obj, rw);
}

static int weakref_proxy_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    PyWeakReference *ref = (PyWeakReference*)obj;

    CHK_TRUE(pickle_obj(ctx, ref->wr_object, rw), fail);

    if(ref->wr_callback) {
        CHK_TRUE(pickle_obj(ctx, ref->wr_callback, rw), fail);
    }else{
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    const char ops[] = {PF_EXTEND, PF_PROXY};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int st_entry_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PySTEntry_Check(obj));
    PySTEntryObject *entry = (PySTEntryObject*)obj;

    CHK_TRUE(pickle_obj(ctx, entry->ste_id, rw), fail);
    CHK_TRUE(pickle_obj(ctx, entry->ste_symbols, rw), fail);
    CHK_TRUE(pickle_obj(ctx, entry->ste_name, rw), fail);
    CHK_TRUE(pickle_obj(ctx, entry->ste_varnames, rw), fail);
    CHK_TRUE(pickle_obj(ctx, entry->ste_children, rw), fail);

    PyObject *ste_type = PyInt_FromLong(entry->ste_type);
    PyObject *ste_unoptimized = PyInt_FromLong(entry->ste_unoptimized);
    PyObject *ste_nested = PyInt_FromLong(entry->ste_nested);
    PyObject *ste_free = PyInt_FromLong(entry->ste_free);
    PyObject *ste_child_free = PyInt_FromLong(entry->ste_child_free);
    PyObject *ste_generator = PyInt_FromLong(entry->ste_generator);
    PyObject *ste_varargs = PyInt_FromLong(entry->ste_varargs);
    PyObject *ste_varkeywords = PyInt_FromLong(entry->ste_varkeywords);
    PyObject *ste_returns_value = PyInt_FromLong(entry->ste_returns_value);
    PyObject *ste_lineno = PyInt_FromLong(entry->ste_lineno);
    PyObject *ste_tmpname = PyInt_FromLong(entry->ste_tmpname);

    vec_pobj_push(&ctx->to_free, ste_type);
    vec_pobj_push(&ctx->to_free, ste_unoptimized);
    vec_pobj_push(&ctx->to_free, ste_nested);
    vec_pobj_push(&ctx->to_free, ste_free);
    vec_pobj_push(&ctx->to_free, ste_child_free);
    vec_pobj_push(&ctx->to_free, ste_generator);
    vec_pobj_push(&ctx->to_free, ste_varargs);
    vec_pobj_push(&ctx->to_free, ste_varkeywords);
    vec_pobj_push(&ctx->to_free, ste_returns_value);
    vec_pobj_push(&ctx->to_free, ste_lineno);
    vec_pobj_push(&ctx->to_free, ste_tmpname);

    CHK_TRUE(pickle_obj(ctx, ste_type, rw), fail);
    CHK_TRUE(pickle_obj(ctx, ste_unoptimized, rw), fail);
    CHK_TRUE(pickle_obj(ctx, ste_nested, rw), fail);
    CHK_TRUE(pickle_obj(ctx, ste_free, rw), fail);
    CHK_TRUE(pickle_obj(ctx, ste_child_free, rw), fail);
    CHK_TRUE(pickle_obj(ctx, ste_generator, rw), fail);
    CHK_TRUE(pickle_obj(ctx, ste_varargs, rw), fail);
    CHK_TRUE(pickle_obj(ctx, ste_varkeywords, rw), fail);
    CHK_TRUE(pickle_obj(ctx, ste_returns_value, rw), fail);
    CHK_TRUE(pickle_obj(ctx, ste_lineno, rw), fail);
    CHK_TRUE(pickle_obj(ctx, ste_tmpname, rw), fail);

    const char ops[] = {PF_EXTEND, PF_STENTRY};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int class_method_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    return builtin_pickle(ctx, obj, rw);
}

static int class_method_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(obj->ob_type == &PyClassMethod_Type);

    PyObject *func = PyObject_GetAttrString(obj, "__func__");
    assert(func);
    vec_pobj_push(&ctx->to_free, func);
    CHK_TRUE(pickle_obj(ctx, func, rw), fail);

    const char ops[] = {PF_EXTEND, PF_CLSMETHOD};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int dict_items_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyDictItems_Check(obj));
    dictviewobject *dv = (dictviewobject*)obj;

    CHK_TRUE(pickle_obj(ctx, (PyObject*)dv->dv_dict, rw), fail);

    const char ops[] = {PF_EXTEND, PF_DICTITEMS};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int dict_keys_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyDictKeys_Check(obj));
    dictviewobject *dv = (dictviewobject*)obj;

    CHK_TRUE(pickle_obj(ctx, (PyObject*)dv->dv_dict, rw), fail);

    const char ops[] = {PF_EXTEND, PF_DICTKEYS};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int dict_values_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyDictValues_Check(obj));
    dictviewobject *dv = (dictviewobject*)obj;

    CHK_TRUE(pickle_obj(ctx, (PyObject*)dv->dv_dict, rw), fail);

    const char ops[] = {PF_EXTEND, PF_DICTVALS};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int method_descr_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    int idx = dispatch_idx_for_picklefunc(method_descr_pickle);
    assert(obj->ob_type == s_type_dispatch_table[idx].type);
    PyMethodDescrObject *desc = (PyMethodDescrObject*)obj;

    TRACE_PICKLE(obj);

    CHK_TRUE(pickle_obj(ctx, (PyObject*)desc->d_type, rw), fail);
    CHK_TRUE(pickle_obj(ctx, desc->d_name, rw), fail);

    const char getattr[] = {PF_EXTEND, PF_METHOD_DESC};
    CHK_TRUE(rw->write(rw, getattr, ARR_SIZE(getattr), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int method_wrapper_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    wrapperobject *wrapper = (wrapperobject*)obj;

    CHK_TRUE(pickle_obj(ctx, (PyObject*)wrapper->descr, rw), fail);
    CHK_TRUE(pickle_obj(ctx, wrapper->self, rw), fail);

    const char ops[] = {PF_EXTEND, PF_METHWRAP};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int call_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    assert(PyCallIter_Check(obj));
    calliterobject *ci = (calliterobject*)obj;

    if(ci->it_callable)
        CHK_TRUE(pickle_obj(ctx, ci->it_callable, rw), fail);
    else
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);

    if(ci->it_sentinel)
        CHK_TRUE(pickle_obj(ctx, ci->it_sentinel, rw), fail);
    else
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);

    const char ops[] = {PF_EXTEND, PF_CALLITER};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int seq_iter_pickle_with_op(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw, 
                                   char ext_op)
{
    seqiterobject *seq  = (seqiterobject*)obj;

    PyObject *index = PyLong_FromLong(seq->it_index);
    CHK_TRUE(index, fail);
    vec_pobj_push(&ctx->to_free, index);
    CHK_TRUE(pickle_obj(ctx, index, rw), fail);

    if(seq->it_seq) {
        CHK_TRUE(pickle_obj(ctx, seq->it_seq, rw), fail);
    }else {
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);
    }

    const char ops[]= {PF_EXTEND, ext_op};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int dict_iter_pickle_with_op(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw,
                                    char ext_op)
{
    dictiterobject *iter = (dictiterobject*)obj;

    if(iter->di_dict)
        CHK_TRUE(pickle_obj(ctx, (PyObject*)iter->di_dict, rw), fail);
    else
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);

    PyObject *di_used = PyLong_FromSsize_t(iter->di_used);
    CHK_TRUE(di_used, fail);
    vec_pobj_push(&ctx->to_free, di_used);

    PyObject *di_pos = PyLong_FromSsize_t(iter->di_pos);
    CHK_TRUE(di_pos, fail);
    vec_pobj_push(&ctx->to_free, di_pos);

    CHK_TRUE(pickle_obj(ctx, di_used, rw), fail);
    CHK_TRUE(pickle_obj(ctx, di_pos, rw), fail);
    if(iter->di_result)
        CHK_TRUE(pickle_obj(ctx, iter->di_result, rw), fail);
    else
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);

    PyObject *len = PyLong_FromSsize_t(iter->len);
    CHK_TRUE(len, fail);
    vec_pobj_push(&ctx->to_free, len);

    CHK_TRUE(pickle_obj(ctx, len, rw), fail);

    const char ops[] = {PF_EXTEND, ext_op};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int seq_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    return seq_iter_pickle_with_op(ctx, obj, rw, PF_SEQITER);
}

static int byte_array_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    return seq_iter_pickle_with_op(ctx, obj, rw, PF_BYTEARRITER);
}

static int dict_iter_item_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    return dict_iter_pickle_with_op(ctx, obj, rw, PF_DICTITEMITER);
}

static int dict_iter_key_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) 
{
    TRACE_PICKLE(obj);
    return dict_iter_pickle_with_op(ctx, obj, rw, PF_DICTKEYITER);
}

static int dict_iter_value_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    return dict_iter_pickle_with_op(ctx, obj, rw, PF_DICTVALITER);
}

static int field_name_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    fieldnameiterobject *iter = (fieldnameiterobject*)obj;

    CHK_TRUE(pickle_obj(ctx, (PyObject*)iter->str, rw), fail);

    const char *raw = PyString_AS_STRING(iter->str);
    const size_t rawlen = strlen(raw);

    assert(iter->it_field.ptr >= raw && iter->it_field.ptr < raw + rawlen);
    assert(iter->it_field.str.ptr >= raw && iter->it_field.str.ptr < raw + rawlen);
    assert(iter->it_field.str.end >= raw && iter->it_field.str.end <= raw + rawlen);

    size_t swiz_ptr = iter->it_field.ptr - raw;
    size_t swiz_str_ptr = iter->it_field.str.ptr - raw;
    size_t swiz_str_end = iter->it_field.str.end - raw;

    assert(swiz_ptr < rawlen);
    assert(swiz_str_ptr < rawlen);
    assert(swiz_str_end <= rawlen);

    PyObject *ptr = PyLong_FromSsize_t(swiz_ptr);
    CHK_TRUE(ptr, fail);
    vec_pobj_push(&ctx->to_free, ptr);

    PyObject *str_ptr = PyLong_FromSsize_t(swiz_str_ptr);
    CHK_TRUE(str_ptr, fail);
    vec_pobj_push(&ctx->to_free, str_ptr);

    PyObject *str_end = PyLong_FromSsize_t(swiz_str_end);
    CHK_TRUE(str_end, fail);
    vec_pobj_push(&ctx->to_free, str_end);

    CHK_TRUE(pickle_obj(ctx, ptr, rw), fail);
    CHK_TRUE(pickle_obj(ctx, str_ptr, rw), fail);
    CHK_TRUE(pickle_obj(ctx, str_end, rw), fail);

    const char ops[] = {PF_EXTEND, PF_FIELDNAMEITER};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int formatter_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    formatteriterobject *iter = (formatteriterobject*)obj;

    CHK_TRUE(pickle_obj(ctx, (PyObject*)iter->str, rw), fail);

    const char *raw = PyString_AS_STRING(iter->str);
    const size_t rawlen = strlen(raw);

    assert(iter->it_markup.str.ptr >= raw && iter->it_markup.str.ptr < raw + rawlen);
    assert(iter->it_markup.str.end >= raw && iter->it_markup.str.end <= raw + rawlen);

    size_t swiz_str_ptr = iter->it_markup.str.ptr - raw;
    size_t swiz_str_end = iter->it_markup.str.end - raw;

    assert(swiz_str_ptr < rawlen);
    assert(swiz_str_end <= rawlen);

    PyObject *str_ptr = PyLong_FromSsize_t(swiz_str_ptr);
    CHK_TRUE(str_ptr, fail);
    vec_pobj_push(&ctx->to_free, str_ptr);

    PyObject *str_end = PyLong_FromSsize_t(swiz_str_end);
    CHK_TRUE(str_end, fail);
    vec_pobj_push(&ctx->to_free, str_end);

    CHK_TRUE(pickle_obj(ctx, str_ptr, rw), fail);
    CHK_TRUE(pickle_obj(ctx, str_end, rw), fail);

    const char ops[] = {PF_EXTEND, PF_FORMATITER};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int list_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    return seq_iter_pickle_with_op(ctx, obj, rw, PF_LISTITER);
}

static int list_rev_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    return seq_iter_pickle_with_op(ctx, obj, rw, PF_LISTREVITER);
}

static int set_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw) 
{
    TRACE_PICKLE(obj);
    setiterobject *si = (setiterobject*)obj;

    if(si->si_set)
        CHK_TRUE(pickle_obj(ctx, (PyObject*)si->si_set, rw), fail);
    else
        CHK_TRUE(pickle_obj(ctx, Py_None, rw), fail);

    PyObject *si_used = PyLong_FromSsize_t(si->si_used);
    CHK_TRUE(si_used, fail);
    vec_pobj_push(&ctx->to_free, si_used);

    PyObject *si_pos = PyLong_FromSsize_t(si->si_pos);
    CHK_TRUE(si_pos, fail);
    vec_pobj_push(&ctx->to_free, si_pos);

    PyObject *len = PyLong_FromSsize_t(si->len);
    CHK_TRUE(len, fail);
    vec_pobj_push(&ctx->to_free, len);

    CHK_TRUE(pickle_obj(ctx, si_used, rw), fail);
    CHK_TRUE(pickle_obj(ctx, si_pos, rw), fail);
    CHK_TRUE(pickle_obj(ctx, len, rw), fail);

    const char ops[] = {PF_EXTEND, PF_SETITER};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int tuple_iter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    return seq_iter_pickle_with_op(ctx, obj, rw, PF_TUPLEITER);
}

static int newclass_instance_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    PyTypeObject *type = obj->ob_type, *basetype = NULL;
    assert(type->tp_flags & Py_TPFLAGS_HEAPTYPE);
    assert(type->tp_mro);
    assert(PyTuple_Check(type->tp_mro));
    assert(PyTuple_GET_SIZE(type->tp_mro) >= 1);

    for(int i = 0; i < PyTuple_GET_SIZE(type->tp_mro); i++) {

        PyObject *base = PyTuple_GET_ITEM(type->tp_mro, i);
        if(!type_is_builtin(base))
            continue;
        
        basetype = (PyTypeObject*)base;
        break;
    }
    assert(basetype);
    assert(type_is_subclassable_builtin(basetype));

    Py_TYPE(obj) = basetype;
    pickle_func_t pf = picklefunc_for_type(obj);
    assert(pf);
    Py_TYPE(obj) = type;

    if(0 != pf(ctx, obj, rw)) {
        assert(PyErr_Occurred());
        goto fail; 
    }

    CHK_TRUE(pickle_obj(ctx, (PyObject*)obj->ob_type, rw), fail);

    const char ops[] = {PF_EXTEND, PF_NEWINST};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int oper_itemgetter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    itemgetterobject *ig = (itemgetterobject*)obj;

    PyObject *nitems = PyInt_FromSsize_t(ig->nitems);
    vec_pobj_push(&ctx->to_free, nitems);
    CHK_TRUE(pickle_obj(ctx, nitems, rw), fail);

    assert(ig->item);
    CHK_TRUE(pickle_obj(ctx, ig->item, rw), fail);

    const char ops[] = {PF_EXTEND, PF_OP_ITEMGET};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int oper_attrgetter_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    attrgetterobject *ag = (attrgetterobject*)obj;

    PyObject *nattrs = PyInt_FromSsize_t(ag->nattrs);
    vec_pobj_push(&ctx->to_free, nattrs);
    CHK_TRUE(pickle_obj(ctx, nattrs, rw), fail);

    assert(ag->attr);
    CHK_TRUE(pickle_obj(ctx, ag->attr, rw), fail);

    const char ops[] = {PF_EXTEND, PF_OP_ATTRGET};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int oper_methodcaller_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);
    methodcallerobject *mc = (methodcallerobject*)obj;

    CHK_TRUE(pickle_obj(ctx, mc->name, rw), fail);
    CHK_TRUE(pickle_obj(ctx, mc->args, rw), fail);

    if(mc->kwds) {
        CHK_TRUE(pickle_obj(ctx, mc->kwds, rw), fail);
    }else{
        CHK_TRUE(0 == none_pickle(ctx, Py_None, rw), fail);
    }

    const char ops[] = {PF_EXTEND, PF_OP_METHODCALL};
    CHK_TRUE(rw->write(rw, ops, ARR_SIZE(ops), 1), fail);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    return -1;
}

static int placeholder_inst_pickle(struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    TRACE_PICKLE(obj);

    PyObject *ph = PyObject_CallFunction(s_placeholder_type, "()");
    if(!ph) {
        assert(PyErr_Occurred());
        return -1;
    }
    vec_pobj_push(&ctx->to_free, ph);
    return newclass_instance_pickle(ctx, ph, rw);
}

static int op_int(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(INT, ctx);

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyInt_Type)) {
        SET_RUNTIME_EXC("STRING: Expecting str type or subtype on TOS");
        goto fail_typecheck;
    }
    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    char buff[MAX_LINE_LEN];
    READ_LINE(rw, buff, fail);

    errno = 0;
    char *endptr;
    long l = strtol(buff, &endptr, 0);
    if (errno || !isspace(*endptr)) {
        SET_RUNTIME_EXC("Bad int in pickle stream [offset: %ld]", (long)rw->seek(rw, RW_SEEK_CUR, 0));
        goto fail;
    }

    PyObject *val = PyObject_CallFunction(ctype, "l", l);
    CHK_TRUE(val, fail);
    vec_pobj_push(&ctx->stack, val);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
fail_typecheck:
    Py_DECREF(type);
fail_underflow:
    return -1;
}

static int op_long(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(LONG, ctx);

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyLong_Type)) {
        SET_RUNTIME_EXC("STRING: Expecting str type or subtype on TOS");
        goto fail_typecheck;
    }
    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    char buff[MAX_LINE_LEN];
    READ_LINE(rw, buff, fail);

    errno = 0;
    char *endptr;
    long long l = strtoll(buff, &endptr, 0);
    if (errno || !isspace(*endptr)) {
        SET_RUNTIME_EXC("Bad long in pickle stream [offset: %ld]", (long)rw->seek(rw, RW_SEEK_CUR, 0));
        goto fail;
    }

    PyObject *val = PyObject_CallFunction(ctype, "L", l);
    CHK_TRUE(val, fail);
    vec_pobj_push(&ctx->stack, val);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
fail_typecheck:
    Py_DECREF(type);
fail_underflow:
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

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyString_Type)) {
        SET_RUNTIME_EXC("STRING: Expecting str type or subtype on TOS");
        goto fail_typecheck;
    }
    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

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

    PyObject *tmp = PyString_DecodeEscape(p, len, NULL, 0, NULL);
    if(!tmp) {
        assert(PyErr_Occurred()); 
        goto fail;
    }
    assert(PyString_Check(tmp));
    PyObject *strobj = PyObject_CallFunctionObjArgs(ctype, tmp, NULL);
    Py_DECREF(tmp);

    if(!strobj) {
        assert(PyErr_Occurred()); 
        goto fail;
    }

    Py_DECREF(type);
    vec_char_destroy(&str);

    vec_pobj_push(&ctx->stack, strobj);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    vec_char_destroy(&str);
fail_typecheck:
    Py_DECREF(type);
fail_underflow:
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
    if(!idx && !isspace(*end)) {
        SET_RUNTIME_EXC("Bad index in pickle stream: [offset: %ld]", (long)rw->seek(rw, RW_SEEK_CUR, 0));
        return -1;
    }

    if(idx != ctx->memo.size) {
        SET_RUNTIME_EXC("Bad index %d (expected %d)", idx, (int)ctx->memo.size);
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
    if(!idx && !isspace(*end)) {
        SET_RUNTIME_EXC("Bad index in pickle stream: [offset: %ld]", (long)rw->seek(rw, RW_SEEK_CUR, 0));
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
    int ret = -1;

    if(vec_size(&ctx->mark_stack) == 0) {
        SET_RUNTIME_EXC("Mark stack underflow");
        goto fail_underflow;
    }

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow"); 
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyTuple_Type)) {
        SET_RUNTIME_EXC("Expecting 'tuple' type or subtype on TOS"); 
        goto fail_typecheck;
    }
    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    int mark = vec_int_pop(&ctx->mark_stack);
    if(vec_size(&ctx->stack) < mark) {
        SET_RUNTIME_EXC("Popped mark beyond stack limits: %d", mark);
        return -1;
    }

    size_t tup_len = vec_size(&ctx->stack) - mark;
    PyObject *tmp = PyTuple_New(tup_len);
    CHK_TRUE(tmp, fail_tuple);

    for(int i = 0; i < tup_len; i++) {
        PyObject *elem = vec_pobj_pop(&ctx->stack);
        PyTuple_SET_ITEM(tmp, tup_len - i - 1, elem);
    }

    PyObject *tuple = PyObject_CallFunctionObjArgs(ctype, tmp, NULL);
    Py_DECREF(tmp);
    CHK_TRUE(tuple, fail_tuple);

    vec_pobj_push(&ctx->stack, tuple);
    ret = 0;

fail_tuple:
fail_typecheck:
    Py_DECREF(type);
fail_underflow:
    return ret;
}

static int op_empty_tuple(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(EMPTY_TUPLE, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow"); 
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyTuple_Type)) {
        SET_RUNTIME_EXC("Expecting 'tuple' type or subtype on TOS"); 
        goto fail_typecheck;
    }
    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    PyObject *tuple = PyObject_CallFunctionObjArgs(ctype, NULL);
    if(!tuple) {
        assert(PyErr_Occurred());
        return -1;
    }
    vec_pobj_push(&ctx->stack, tuple);
    return 0;

fail_typecheck:
    Py_DECREF(type);
fail_underflow:
    return ret;
}

static int op_empty_list(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(EMPTY_LIST, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow"); 
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type) 
    || !PyType_IsSubtype((PyTypeObject*)type, &PyList_Type)) {
        SET_RUNTIME_EXC("Expecting list type or subtype on TOS");
        goto fail_typecheck;
    }
    PyObject *args = PyTuple_New(0);
	CHK_TRUE(args, fail_list);

    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    PyObject *list = PyObject_Call(ctype, args, NULL);
    if(!list) {
        assert(PyErr_Occurred());
        goto fail_list;
    }
    vec_pobj_push(&ctx->stack, list);
    ret = 0;

fail_list:
    Py_XDECREF(args);
fail_typecheck:
    Py_DECREF(type);
fail_underflow:
    return ret;
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
        /* steal the reference from the stack */
        PyObject *elem = vec_pobj_pop(&ctx->stack);
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
    int ret = -1;

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyDict_Type)) {
        SET_RUNTIME_EXC("EMPTY_DICT: Exepcting 'dict' type or subtype at TOS");
        goto fail_typecheck;
    }
    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    PyObject *dict = PyObject_CallFunctionObjArgs(ctype, NULL);
    CHK_TRUE(dict, fail_dict);

    vec_pobj_push(&ctx->stack, dict);
    ret = 0;

fail_dict:
fail_typecheck:
    Py_DECREF(type);
fail_underflow:
    return ret;
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

#ifdef Py_USING_UNICODE
static int op_unicode(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(UNICODE, ctx);

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyUnicode_Type)) {
        SET_RUNTIME_EXC("STRING: Expecting unicode type or subtype on TOS");
        goto fail_typecheck;
    }
    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    vec_char_t str;
    vec_char_init(&str);
    char c;
    do { 
        CHK_TRUE(SDL_RWread(rw, &c, 1, 1), fail);
        CHK_TRUE(vec_char_push(&str, c), fail);
    }while(c != '\0');

    CHK_TRUE(SDL_RWread(rw, &c, 1, 1), fail); /* consume newline */

    assert(strlen(str.array) == vec_size(&str) - 1);
    PyObject *tmp = PyUnicode_DecodeUTF7(str.array, vec_size(&str)-1, "strict");
    CHK_TRUE(tmp, fail);

    PyObject *unicode = PyObject_CallFunctionObjArgs(ctype, tmp, NULL);
    Py_DECREF(tmp);
    CHK_TRUE(unicode, fail);

    Py_DECREF(type);
    vec_char_destroy(&str);

    vec_pobj_push(&ctx->stack, unicode);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
    vec_char_destroy(&str);
fail_typecheck:
    Py_DECREF(type);
fail_underflow:
    return -1;
}
#endif

static int op_float(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(FLOAT, ctx);

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyFloat_Type)) {
        SET_RUNTIME_EXC("STRING: Expecting 'float' type or subtype on TOS");
        goto fail_typecheck;
    }
    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    char line[MAX_LINE_LEN];
    READ_LINE(rw, line, fail);

    char *curr = line;
    while(*curr && !isspace(*curr))
        curr++;
    *curr = '\0'; /* Strip newline */

    double d = PyOS_string_to_double(line, NULL, PyExc_OverflowError);
    CHK_TRUE(!PyErr_Occurred(), fail);

    PyObject *retval = PyObject_CallFunction(ctype, "d", d);
    CHK_TRUE(retval, fail);
    vec_pobj_push(&ctx->stack, retval);

    Py_DECREF(type);
    return 0;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error reading from pickle stream");
fail_typecheck:
    Py_DECREF(type);
fail_underflow:
    return -1;
}

static int op_ext_builtin(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_BUILTIN, ctx);

    char buff[MAX_LINE_LEN];
    READ_LINE(rw, buff, fail);

    char *curr = buff;
    while(*curr && !isspace(*curr))
        curr++;
    *curr = '\0'; /* Strip newline */

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
    PyObject *retval = PyObject_Call(meta, args, NULL);
    CHK_TRUE(retval, fail_build);

    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_build:
    Py_DECREF(args);
fail_typecheck:
    Py_DECREF(meta);
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
    if(vec_size(&ctx->stack) < 6) {
        SET_RUNTIME_EXC("Stack underflow"); 
        return -1;
    }

    PyObject *defaults = vec_pobj_pop(&ctx->stack);
    PyObject *module = vec_pobj_pop(&ctx->stack);
    PyObject *closure = vec_pobj_pop(&ctx->stack);
    PyObject *globals = vec_pobj_pop(&ctx->stack);
    PyObject *code = vec_pobj_pop(&ctx->stack);
    PyFunctionObject *op = (PyFunctionObject*)vec_pobj_pop(&ctx->stack);

    /* Make sure we don't traverse the function object's fields mid-surgery */
    PyObject_GC_UnTrack(op);

    /* Clear the placeholder values */
    Py_CLEAR(op->func_code); 
    Py_CLEAR(op->func_globals);
    Py_CLEAR(op->func_name);
    Py_CLEAR(op->func_doc);
    assert(!op->func_defaults);
    assert(!op->func_module);
    Py_CLEAR(op->func_dict);

    /* Set the code and globals of the empty function object. This is 
     * the exact same flow as if the code and globals were passed to
     * 'PyFunction_New'. We just wanted to memoize the function object
     * before pickling its' members to handle self-referencing. 
     *
     * The following is ripped from PyFunction_New in funcobject.c:
     */

    PyObject *doc;
    PyObject *consts;

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

    if(module != Py_None) {
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

    Py_DECREF(module);
    Py_DECREF(closure);
    Py_DECREF(defaults);

    PyObject_GC_Track(op);
    vec_pobj_push(&ctx->stack, (PyObject*)op);
    return 0;

fail:
    Py_DECREF(module);
    Py_DECREF(code);
    Py_DECREF(globals);
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

    PyObject *val = vec_pobj_pop(&ctx->stack);
    PyObject *cell = PyCell_New(val);
    assert(cell);
    Py_DECREF(val);
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
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow"); 
        goto fail_underflow;
    }

    PyObject *encoded = vec_pobj_pop(&ctx->stack);
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyString_Check(encoded)) {
        SET_RUNTIME_EXC("PF_BYTEARRAY: Expecting string at TOS");
        goto fail_typecheck;
    }

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyByteArray_Type)) {
        SET_RUNTIME_EXC("PF_BYTEARRAY: Expecting bytearray type of subtype at TOS1");
        goto fail_typecheck;
    }

    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    PyObject *raw = PyUnicode_DecodeUTF7(PyString_AS_STRING(encoded), PyString_GET_SIZE(encoded), "strict");
    size_t len = PyUnicode_GET_SIZE(raw);
    char *buff = malloc(PyUnicode_GET_SIZE(raw));

    for(int i = 0; i < len; i++) {
        assert(PyUnicode_AS_UNICODE(raw)[i] < (Py_UNICODE)256);
        buff[i] = (char)PyUnicode_AS_UNICODE(raw)[i];
    }

    PyObject *tmp = PyByteArray_FromStringAndSize(buff, len);
    PyObject *ba = PyObject_CallFunctionObjArgs(ctype, tmp, NULL);
    Py_DECREF(tmp);
    CHK_TRUE(ba, fail_ba);

    vec_pobj_push(&ctx->stack, ba);
    ret = 0;

fail_ba:
    free(buff);
    Py_DECREF(raw);
fail_typecheck:
    Py_DECREF(encoded);
fail_underflow:
    return ret;
}

static int op_ext_super(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_SUPER, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 3) {
        SET_RUNTIME_EXC("Stack underflow"); 
        goto fail_underflow;
    }

    PyObject *obj = vec_pobj_pop(&ctx->stack);
    PyObject *type = vec_pobj_pop(&ctx->stack);
    PyObject *metatype = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(metatype)
    && !PyType_IsSubtype((PyTypeObject*)metatype, &PySuper_Type)) {
        SET_RUNTIME_EXC("PF_SUPER: Expecting 'super' type or subtype at TOS2");
        goto fail_typecheck;
    }

    PyObject *ctype = constructor_type((PyTypeObject*)metatype);
    assert(ctype);

    PyObject *super = PyObject_CallFunctionObjArgs(ctype, type, obj, NULL);
    CHK_TRUE(ret, fail_super);

    vec_pobj_push(&ctx->stack, super);
    ret = 0;

fail_super:
fail_typecheck:
    Py_DECREF(obj);
    Py_DECREF(type);
    Py_DECREF(metatype);
fail_underflow:
    return ret;
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
    int ret = -1;

    PyObject *code = NULL, *globals = NULL;
    code = (PyObject*)PyCode_NewEmpty("__placeholder__", "__placeholder__", 0);
    CHK_TRUE(code, fail);
    globals = PyDict_New();
    CHK_TRUE(globals, fail);

    PyObject *func = PyFunction_New(code, globals);
    CHK_TRUE(func, fail);

    vec_pobj_push(&ctx->stack, func);
    ret = 0;

fail:
    Py_XDECREF(globals);
    Py_XDECREF(code);
    return ret;
}

static int op_ext_baseobj(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_BASEOBJ, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow"); 
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || (!PyType_IsSubtype((PyTypeObject*)type, &PyBaseObject_Type))) {
        SET_RUNTIME_EXC("Expecting object type or subtype on TOS");
        goto fail_typecheck;
    }

    PyObject *ctype = constructor_type((PyTypeObject*)type); /* borrowed */
    assert(ctype);

    PyObject *args = PyTuple_New(0);
    PyObject *retval = PyObject_Call(ctype, args, NULL);
    Py_DECREF(args);
    assert(retval);

    vec_pobj_push(&ctx->stack, retval);
    return 0;

fail_typecheck:
    Py_DECREF(type);
fail_underflow:
    return ret;
}

static void del_extra_attrs(PyObject *obj, PyObject **attrs_base, size_t npairs)
{
    PyObject *ndw_attrs = nonderived_writable_attrs(obj);

    PyObject *key, *value;
    Py_ssize_t pos = 0;

    size_t ndel = 0;
    STALLOC(PyObject*, todel, PyDict_Size(ndw_attrs));

    while(PyDict_Next(ndw_attrs, &pos, &key, &value)) {

        bool contains = false;
        for(int i = 0; i < npairs * 2; i += 2) {

            PyObject *attr_key = attrs_base[i];

            if(PyObject_RichCompareBool(key, attr_key, Py_EQ)) {
                contains = true;
                break;
            }
        }

        if(!contains) {
            Py_INCREF(key);
            todel[ndel++] = key;
        }
    }

    for(int i = 0; i < ndel; i++) {

        assert(!PyErr_Occurred());
        PyObject_DelAttr(obj, todel[i]);
        Py_DECREF(todel[i]);

        if(PyErr_Occurred() 
        && (PyErr_ExceptionMatches(PyExc_AttributeError) || PyErr_ExceptionMatches(PyExc_TypeError))) {
            PyErr_Clear(); 
        }
    }

    STFREE(todel);
    Py_DECREF(ndw_attrs);
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
    del_extra_attrs(obj, &vec_AT(&ctx->stack, mark), nitems);

    for(int i = 0; i < nitems; i++) {
    
        PyObject *val = vec_pobj_pop(&ctx->stack);
        PyObject *key = vec_pobj_pop(&ctx->stack);

        ret = (0 == setattr_nondestructive(obj, key, val)) ? 0 : -1;

        Py_DECREF(key);
        Py_DECREF(val);
        if(ret)
            return -1;
    }


    PyObject *top = vec_pobj_pop(&ctx->stack);
    assert(obj == top);

    vec_pobj_push(&ctx->stack, obj);
    return 0;
}

static int op_ext_notimpl(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_NOTIMPL, ctx);
    Py_INCREF(Py_NotImplemented);
    vec_pobj_push(&ctx->stack, Py_NotImplemented);
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
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);
    PyObject *items = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PySet_Type)) {
        SET_RUNTIME_EXC("PF_SET: Expecting a 'set' type or subtype on TOS");
        goto fail_typecheck;
    }

    if(!PyTuple_Check(items)) {
        SET_RUNTIME_EXC("PF_SET: Expecting a tuple of set items on TOS1");
        goto fail_typecheck;
    }

    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    PyObject *set = PyObject_CallFunctionObjArgs(ctype, items, NULL);
    if(!set) {
        assert(PyErr_Occurred());
        goto fail_set;
    }

    vec_pobj_push(&ctx->stack, set);
    ret = 0;

fail_set:
fail_typecheck:
    Py_DECREF(type);
    Py_DECREF(items);
fail_underflow:
    return ret;
}

static int op_ext_frozenset(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_SET, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);
    PyObject *items = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyFrozenSet_Type)) {
        SET_RUNTIME_EXC("PF_FROZENSET: Expecting a tuple of set items on TOS");
        goto fail_typecheck;
    }

    if(!PyTuple_Check(items)) {
        SET_RUNTIME_EXC("PF_FROZENSET: Expecting a tuple of set items on TOS");
        goto fail_typecheck;
    }

    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    PyObject *set = PyObject_CallFunctionObjArgs(ctype, items, NULL);
    if(!set) {
        assert(PyErr_Occurred());
        goto fail_set;
    }

    vec_pobj_push(&ctx->stack, set);
    ret = 0;

fail_set:
fail_typecheck:
    Py_DECREF(type);
    Py_DECREF(items);
fail_underflow:
    return ret;
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
        SET_RUNTIME_EXC("Could not find getset_descriptor (%s) of type (%s) [%p]",
            PyString_AS_STRING(name), tp_type->tp_name, tp_type);
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

static int op_ext_emptymod(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_EMPTYMOD, ctx);

    PyModuleObject *mod = PyObject_GC_New(PyModuleObject, &PyModule_Type);
    if(!mod)
        return -1;
    mod->md_dict = NULL;
    vec_pobj_push(&ctx->stack, (PyObject*)mod);
    return 0;
}

static int op_ext_module(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_MODULE, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    PyObject *dict = vec_pobj_pop(&ctx->stack);
    PyObject *emptymod = vec_pobj_pop(&ctx->stack);

    if(!PyDict_Check(dict)) {
        SET_RUNTIME_EXC("PF_MODULE: Expecting dict on TOS");
        goto fail_typecheck;
    }

    if(!PyModule_Check(emptymod)) {
        SET_RUNTIME_EXC("PF_MODULE: Expecting module instance on TOS1");
        goto fail_typecheck;
    }

    ((PyModuleObject*)emptymod)->md_dict = dict;
    Py_INCREF(((PyModuleObject*)emptymod)->md_dict);

    Py_INCREF(emptymod);
    PyObject_GC_Track(emptymod);
    vec_pobj_push(&ctx->stack, emptymod);
    ret = 0;

fail_typecheck:
    Py_DECREF(dict);
    Py_DECREF(emptymod);
fail_underflow:
    assert((ret && PyErr_Occurred()) || (!ret && !PyErr_Occurred()));
    return ret;
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
    PyObject *inst = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)) {
        SET_RUNTIME_EXC("PF_NEWINST: Expecting type on TOS");
        goto fail_typecheck;
    }

    PyTypeObject *tp_type = (PyTypeObject*)type;

    /* This is assigning to __class__, but with no error checking */
    Py_DECREF(Py_TYPE(inst));
    Py_TYPE(inst) = tp_type;
    Py_INCREF(Py_TYPE(inst));

    Py_INCREF(inst);
    vec_pobj_push(&ctx->stack, inst);
    ret = 0;

fail_typecheck:
    Py_DECREF(type);
    Py_DECREF(inst);
fail_underflow:
    assert((ret && PyErr_Occurred()) || (!ret && !PyErr_Occurred()));
    return ret;
}

static int op_ext_clsmethod(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_CLSMETHOD, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    PyObject *callable = vec_pobj_pop(&ctx->stack); 
    if(!PyCallable_Check(callable)) {
        SET_RUNTIME_EXC("PF_CLSMETHOD: Expecting callable object on TOS");
        goto fail;
    }

    PyObject *newmeth = PyClassMethod_New(callable);
    CHK_TRUE(newmeth, fail);
    vec_pobj_push(&ctx->stack, newmeth);
    ret = 0;

fail:
    Py_DECREF(callable);
fail_underflow:
    return ret;
}

static int op_ext_instmethod(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_INSTMETHOD, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 3) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    PyObject *klass = vec_pobj_pop(&ctx->stack);
    PyObject *self = vec_pobj_pop(&ctx->stack);
    PyObject *func = vec_pobj_pop(&ctx->stack);

    PyObject *newmeth = PyMethod_New(func, (self == Py_None ? NULL : self), klass);
    CHK_TRUE(newmeth, fail);
    vec_pobj_push(&ctx->stack, newmeth);
    ret = 0;

fail:
    Py_DECREF(klass);
    Py_DECREF(self);
    Py_DECREF(func);
fail_underflow:
    return ret;
}

static int op_ext_memdesc(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_MEMDESC, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    PyObject *name = vec_pobj_pop(&ctx->stack);
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyString_Check(name)) {
        SET_RUNTIME_EXC("PF_MEMDESC: Expecting string at TOS");
        goto fail_typecheck;
    }

    if(!PyType_Check(type)) {
        SET_RUNTIME_EXC("PF_MEMDESC: Expecting type at TOS1");
        goto fail_typecheck;
    }

    PyTypeObject *tp_type = (PyTypeObject*)type;
    PyMemberDef *found = NULL;
    for(PyMemberDef *curr = tp_type->tp_members; curr && curr->name; curr++) {
    
        if(0 == strcmp(curr->name, PyString_AS_STRING(name))) {
            found = curr; 
            break;
        }
    }

    if(!found) {
        SET_RUNTIME_EXC("Could not find member_descriptor (%s) of type (%s)",
            PyString_AS_STRING(name), tp_type->tp_name);
        goto fail_desc;
    }

    PyObject *desc = PyDescr_NewMember(tp_type, found);
    CHK_TRUE(desc, fail_desc);
    vec_pobj_push(&ctx->stack, desc);
    ret = 0;

fail_desc:
fail_typecheck:
    Py_DECREF(name);
    Py_DECREF(type);
fail_underflow:
    return ret;
}

static int op_ext_method_wrapper(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_METHWRAP, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *self = vec_pobj_pop(&ctx->stack);
    PyObject *desc = vec_pobj_pop(&ctx->stack);

    if(desc->ob_type != &PyWrapperDescr_Type) {
        SET_RUNTIME_EXC("PF_METHWRAP: Expecting wrapper_descriptor at TOS1");
        goto fail_typecheck;
    }

    PyObject *method = PyWrapper_New(desc, self);
    CHK_TRUE(method, fail_method);
    vec_pobj_push(&ctx->stack, method);
    ret = 0;

fail_method:
fail_typecheck:
    Py_DECREF(self);
    Py_DECREF(desc);
fail_underflow:
    return ret;
}

static int op_ext_range(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_RANGE, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 3) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *step = vec_pobj_pop(&ctx->stack);
    PyObject *ihigh = vec_pobj_pop(&ctx->stack);
    PyObject *ilow = vec_pobj_pop(&ctx->stack);

    if(!PyLong_Check(ilow)
    || !PyLong_Check(ihigh)
    || !PyLong_Check(step)) {

        SET_RUNTIME_EXC("PF_RANGE: Expecting long objects as the top 3 TOS items");
        goto fail_typecheck;
    }

    PyObject *retval = PyObject_CallFunction((PyObject*)&PyRange_Type, "(OOO)", ilow, ihigh, step);
    CHK_TRUE(retval, fail_range);
    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_range:
fail_typecheck:
    Py_DECREF(ilow);
    Py_DECREF(ihigh);
    Py_DECREF(step);
fail_underflow:
    return ret;
}

static int op_ext_slice(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_SLICE, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 3) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    /* These can actually be of any type */
    PyObject *step = vec_pobj_pop(&ctx->stack);
    PyObject *stop = vec_pobj_pop(&ctx->stack);
    PyObject *start = vec_pobj_pop(&ctx->stack);

    PyObject *retval = PySlice_New(start, stop, step);
    CHK_TRUE(retval, fail_slice);
    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_slice:
    Py_DECREF(step);
    Py_DECREF(stop);
    Py_DECREF(start);
fail_underflow:
    return ret;
}


static int op_ext_staticmethod(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_STATMETHOD, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);
    PyObject *callable = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyStaticMethod_Type)) {
        SET_RUNTIME_EXC("PF_STATMETHOD: Expecting 'staticmethod' type or subtype on TOS");
        goto fail_typecheck;
    }

    if(!PyCallable_Check(callable)) {
        SET_RUNTIME_EXC("PF_STATMETHOD: Expecting callable object on TOS1");
        goto fail_typecheck;
    }

    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    PyObject *retval = PyObject_CallFunctionObjArgs(ctype, callable, NULL);
    CHK_TRUE(retval, fail_method);
    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_method:
fail_typecheck:
    Py_DECREF(type);
    Py_DECREF(callable);
fail_underflow:
    return ret;
}

static int op_ext_buffer(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_BUFFER, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 4) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *readonly = vec_pobj_pop(&ctx->stack);
    PyObject *offset = vec_pobj_pop(&ctx->stack);
    PyObject *size = vec_pobj_pop(&ctx->stack);
    PyObject *base = vec_pobj_pop(&ctx->stack);

    if(!PyLong_Check(readonly)
    || !PyLong_Check(offset)
    || !PyLong_Check(size)) {
        SET_RUNTIME_EXC("PF_BUFFER: Expecting long objects as top 3 stack items");
        goto fail_typecheck;
    }

    PyObject *retval = NULL;
    if(PyLong_AsLong(readonly)) {
        retval = PyBuffer_FromObject(base, PyLong_AsLong(offset), PyLong_AsLong(size));
    }else{
        retval = PyBuffer_FromReadWriteObject(base, PyLong_AsLong(offset), PyLong_AsLong(size));
    }
    CHK_TRUE(retval, fail_buffer);
    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_buffer:
fail_typecheck:
    Py_DECREF(readonly);
    Py_DECREF(offset);
    Py_DECREF(size);
    Py_DECREF(base);
fail_underflow:
    return ret;
}

static int op_ext_memview(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_MEMVIEW, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *base = vec_pobj_pop(&ctx->stack);
    PyObject *retval = PyMemoryView_FromObject(base);
    CHK_TRUE(retval, fail_memview);
    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_memview:
    Py_DECREF(base);
fail_underflow:
    return ret;
}

static int op_ext_property(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_PROPERTY, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 5) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);
    PyObject *doc = vec_pobj_pop(&ctx->stack);
    PyObject *del = vec_pobj_pop(&ctx->stack);
    PyObject *set = vec_pobj_pop(&ctx->stack);
    PyObject *get = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyProperty_Type)) {
        SET_RUNTIME_EXC("PF_PROPERTY: Expecting 'property' type or subtype on TOS");
        goto fail_typecheck;
    }

    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    PyObject *prop = PyObject_CallFunction(ctype, "(OOOO)", get, set, del, doc);
    CHK_TRUE(prop, fail_prop);
    vec_pobj_push(&ctx->stack, prop);
    ret = 0;

fail_prop:
fail_typecheck:
    Py_DECREF(doc);
    Py_DECREF(del);
    Py_DECREF(set);
    Py_DECREF(get);
fail_underflow:
    return ret;
}

static int op_ext_enumerate(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_ENUMERATE, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 5) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);
    PyObject *longindex = vec_pobj_pop(&ctx->stack);
    PyObject *result = vec_pobj_pop(&ctx->stack);
    PyObject *sit = vec_pobj_pop(&ctx->stack);
    PyObject *index = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyEnum_Type)) {
        SET_RUNTIME_EXC("PF_ENUMERATE: Expecting 'enumerate' type or subtype at TOS");
        goto fail_typecheck;
    }

    if(longindex != Py_None && !_PyAnyInt_Check(longindex)) {
        SET_RUNTIME_EXC("PF_ENUMERATE: expecting integer (int,long) or None type on TOS1");
        goto fail_typecheck;
    }

    if(result != Py_None && !PyTuple_Check(result)) {
        SET_RUNTIME_EXC("PF_ENUMERATE: expecting tuple or None on TOS2");
        goto fail_typecheck;
    }

    if(!PyIter_Check(sit)) {
        SET_RUNTIME_EXC("PF_ENUMERATE: expecting iterator on TOS2");
        goto fail_typecheck;
    }

    if(!PyLong_Check(index)) {
        SET_RUNTIME_EXC("PF_ENUMERATE: expecting long on TOS3");
        goto fail_typecheck;
    }

    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    /* The following is ripped from enumobject.c:enum_new
     * (The 'enumerate' new method takes a sequence and a start index.
     * The sequence is not saved. So hijack the creation path and set
     * the iterator directly. */
    enumobject *en = (enumobject *)((PyTypeObject*)ctype)->tp_alloc((PyTypeObject*)ctype, 0);
    CHK_TRUE(en, fail_en);

    en->en_index = PyLong_AsSsize_t(index);
    en->en_sit = sit;
    Py_INCREF(en->en_sit);
    en->en_result = (result != Py_None ? result : NULL);
    if(en->en_result) {
        Py_INCREF(en->en_result); 
    }
    en->en_longindex = (longindex != Py_None ? longindex : NULL);
    if(en->en_longindex) {
        Py_INCREF(en->en_longindex);
    }

    vec_pobj_push(&ctx->stack, (PyObject*)en);
    ret = 0;

fail_en:
fail_typecheck:
    Py_DECREF(longindex);
    Py_DECREF(result);
    Py_DECREF(sit);
    Py_DECREF(index);
fail_underflow:
    return ret;
}

static int op_ext_seqiter_with_type(struct unpickle_ctx *ctx, SDL_RWops *rw, PyTypeObject *type)
{
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *seq = vec_pobj_pop(&ctx->stack);
    PyObject *index = vec_pobj_pop(&ctx->stack);

    if(seq != Py_None && !PySequence_Check(seq)) {
        SET_RUNTIME_EXC("Expecting sequence or None at TOS");
        goto fail_typecheck;
    }

    if(!PyLong_Check(index)) {
        SET_RUNTIME_EXC("Expecting long at TOS1"); 
        goto fail_typecheck;
    }

    seqiterobject *retval = PyObject_GC_New(seqiterobject, type);
    CHK_TRUE(retval, fail_seq);
    if(seq != Py_None) {
        retval->it_seq = seq;
        Py_INCREF(seq);
    }else{
        retval->it_seq = NULL; 
    }
    retval->it_index = PyLong_AsLong(index);
    PyObject_GC_Track(retval);

    vec_pobj_push(&ctx->stack, (PyObject*)retval);
    ret = 0;

fail_seq:
fail_typecheck:
    Py_DECREF(seq);
    Py_DECREF(index);
fail_underflow:
    return ret;
}

static int op_ext_listiter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_LISTITER, ctx);

    int idx = dispatch_idx_for_picklefunc(list_iter_pickle);
    PyTypeObject *type = s_type_dispatch_table[idx].type;
    return op_ext_seqiter_with_type(ctx, rw, type);
}

#ifndef WITHOUT_COMPLEX
static int op_ext_complex(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_COMPLEX, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 3) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);
    PyObject *imag = vec_pobj_pop(&ctx->stack);
    PyObject *real = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyComplex_Type)) {
        SET_RUNTIME_EXC("PF_COMPLEX: Expecting 'complex' type or subtype at TOS");
        goto fail_typecheck;
    }

    if(!PyFloat_Check(imag)
    || !PyFloat_Check(real)) {
        SET_RUNTIME_EXC("PF_COMPLEX: Expecting float objects at TOS1 and TOS2");
        goto fail_typecheck;
    }

    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    PyObject *retval = PyObject_CallFunctionObjArgs(ctype, real, imag, NULL);
    CHK_TRUE(retval, fail_complex);
    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_complex:
fail_typecheck:
    Py_DECREF(imag);
    Py_DECREF(real);
fail_underflow:
    return ret;
}
#endif

static int op_ext_dictproxy(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_DICTPROXY, ctx); 

    int ret = -1;
    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *dict = vec_pobj_pop(&ctx->stack);

    if(!PyDict_Check(dict)) {
        SET_RUNTIME_EXC("PF_DICTPROXY: Expecting dict on TOS");
        goto fail_typecheck;
    }

    PyObject *retval = PyDictProxy_New(dict);
    CHK_TRUE(retval, fail_dictproxy);
    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_dictproxy:
fail_typecheck:
    Py_DECREF(dict);
fail_underflow:
    return ret;
}

static int op_ext_reversed(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_REVERSED, ctx); 

    int ret = -1;
    if(vec_size(&ctx->stack) < 3) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);
    PyObject *seq = vec_pobj_pop(&ctx->stack);
    PyObject *index = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type)
    || !PyType_IsSubtype((PyTypeObject*)type, &PyReversed_Type)) {
        SET_RUNTIME_EXC("PF_REVERSED: Expecting 'reversed' type or subtype at TOS");
        goto fail_typecheck;
    }

    if(seq != Py_None && !PySequence_Check(seq)) {
        SET_RUNTIME_EXC("PF_REVERSED: TOS1 item must be None or a sequence");
        goto fail_typecheck;
    }

    if(!PyLong_Check(index)) {
        SET_RUNTIME_EXC("PF_REVERSED: Expecting long object on TOS2"); 
        goto fail_typecheck;
    }

    PyObject *ctype = constructor_type((PyTypeObject*)type);
    assert(ctype);

    reversedobject *rev = (reversedobject*)((PyTypeObject*)ctype)->tp_alloc((PyTypeObject*)ctype, 0);
    CHK_TRUE(rev, fail_rev);
    rev->index = PyLong_AsSsize_t(index);
    if(seq != Py_None) {
        rev->seq = seq;
        Py_INCREF(seq);
    }else{
        rev->seq = NULL; 
    }

    vec_pobj_push(&ctx->stack, (PyObject*)rev);
    ret = 0;
 
fail_rev:
fail_typecheck:
    Py_DECREF(seq);
    Py_DECREF(index);
fail_underflow:
    return ret;
}

static int op_ext_gen(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_GEN, ctx);

    int ret = -1;
    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *top = vec_pobj_pop(&ctx->stack);

    if(!PyCode_Check(top) && !PyFrame_Check(top)) {
        SET_RUNTIME_EXC("PF_GEN: Expecting code or frame object on TOS");
        goto fail_typecheck;
    }

    PyObject *retval = NULL;
    Py_INCREF(top);

    if(PyFrame_Check(top)) {

        retval = PyGen_New((PyFrameObject*)top); /* steals 'top' ref */
        CHK_TRUE(retval, fail_gen);
    }else{
        PyGenObject *gen = PyObject_GC_New(PyGenObject, &PyGen_Type);
        CHK_TRUE(gen, fail_gen);

        gen->gi_frame = NULL;
        gen->gi_code = top; /* steals 'top' ref */
        gen->gi_running = 0;
        gen->gi_weakreflist = NULL;
        PyObject_GC_Track(gen);
        retval = (PyObject*)gen;
    }

    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_gen:
fail_typecheck:
    Py_DECREF(top);
fail_underflow:
    return ret;
}

/* Patch a dummy frame. Some of this logic is ripped from frameobject.c */
static int convert_frame(PyFrameObject *frame, PyCodeObject *code,
                         PyObject *globals, PyObject *locals)
{
    assert(frame->f_code && frame->f_globals && frame->f_locals);
    size_t old_valsize = frame->f_code->co_stacksize;
    Py_CLEAR(frame->f_code);
    Py_CLEAR(frame->f_globals);
    Py_CLEAR(frame->f_locals);
    Py_CLEAR(frame->f_builtins);

    PyThreadState *tstate = PyThreadState_GET();
    PyFrameObject *back = tstate->frame;
    PyObject *builtins;

    /* set builtins */
    if (back == NULL || back->f_globals != globals) {
        builtins = PyDict_GetItemString(globals, "__builtins__");
        if (builtins) {
            if (PyModule_Check(builtins)) {
                builtins = PyModule_GetDict(builtins);
                assert(!builtins || PyDict_Check(builtins));
            }
            else if (!PyDict_Check(builtins))
                builtins = NULL;
        }
        if (builtins == NULL) {
            /* No builtins!              Make up a minimal one
               Give them 'None', at least. */
            builtins = PyDict_New();
            if (builtins == NULL
            || PyDict_SetItemString(builtins, "None", Py_None) < 0)
                return -1;
        }
        else
            Py_INCREF(builtins);
    }
    else {
        /* If we share the globals, we share the builtins.
           Save a lookup and a call. */
        builtins = back->f_builtins;
        assert(builtins != NULL && PyDict_Check(builtins));
        Py_INCREF(builtins);
    }
    assert(builtins);
    frame->f_builtins = builtins;

    /* Set code */
    frame->f_code = code;
    Py_INCREF(frame->f_code);

    /* Set globals */
    frame->f_globals = globals;
    Py_INCREF(frame->f_globals);

    /* Set locals */
    if ((code->co_flags & (CO_NEWLOCALS | CO_OPTIMIZED)) ==
        (CO_NEWLOCALS | CO_OPTIMIZED))
        ; /* f_locals = NULL; will be set by PyFrame_FastToLocals() */
    else if (code->co_flags & CO_NEWLOCALS) {
        frame->f_locals = PyDict_New();
        if(!frame->f_locals)
            return -1;
    }
    else {
        frame->f_locals = locals;
        if(locals == Py_None)
            frame->f_locals = globals;
        Py_INCREF(frame->f_locals);
    }

    /* Setup valuestack */
    if(old_valsize != frame_extra_size(frame))
        return -1;

    const size_t ncells = PyTuple_GET_SIZE(code->co_cellvars);
    const size_t nfrees = PyTuple_GET_SIZE(code->co_freevars);
    size_t extras = code->co_nlocals + ncells + nfrees;

    frame->f_valuestack = frame->f_localsplus + extras;
    frame->f_stacktop = frame->f_valuestack;

    for(int i = 0; i < extras; i++)
        frame->f_localsplus[i] = NULL;

    assert(frame->f_builtins && frame->f_code && frame->f_globals);
    frame->f_iblock = 0;
    return 0;
}

static int op_ext_frame(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_FRAME, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 7) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    if(vec_size(&ctx->mark_stack) < 3) {
        SET_RUNTIME_EXC("Mark stack underflow");
        goto fail_underflow;
    }

    PyFrameObject *frame = (PyFrameObject*)vec_pobj_pop(&ctx->stack);
    PyObject *locals = vec_pobj_pop(&ctx->stack);
    PyObject *globals = vec_pobj_pop(&ctx->stack);
    PyCodeObject *code = (PyCodeObject*)vec_pobj_pop(&ctx->stack);
    PyObject *back = vec_pobj_pop(&ctx->stack);
    PyObject *lineno = vec_pobj_pop(&ctx->stack);
    PyObject *lasti = vec_pobj_pop(&ctx->stack);

    if(!PyFrame_Check((PyObject*)frame)) {
        SET_RUNTIME_EXC("PF_FRAME: Expecting frame on TOS");
        goto fail_typecheck;
    }

    if(locals != Py_None && !PyMapping_Check(locals)) {
        SET_RUNTIME_EXC("PF_FRAME: Expecting mapping or None on TOS1");
        goto fail_typecheck;
    }

    if(!PyDict_Check(globals)) {
        SET_RUNTIME_EXC("PF_FRAME: Expecting dict on TOS2");
        goto fail_typecheck;
    }

    if(!PyCode_Check((PyObject*)code)) {
        SET_RUNTIME_EXC("PF_FRAME: Expecting code object on TOS3");
        goto fail_typecheck;
    }

    if(back != Py_None && !PyFrame_Check(back)) {
        SET_RUNTIME_EXC("PF_FRAME: Expecting frame or None on TOS4");
        goto fail_typecheck;
    }

    if(!PyInt_Check(lineno)
    || !PyInt_Check(lasti)) {
        SET_RUNTIME_EXC("PF_FRAME: Expecting int objects on TOS5 and TOS6");
        goto fail_typecheck;
    }

    /* Forcefully set attrs */
    PyObject_GC_UnTrack(frame);
    CHK_TRUE(0 == convert_frame(frame, code, globals, locals == Py_None ? NULL : locals), fail_frame);

    Py_XDECREF(frame->f_back);
    frame->f_back = back == Py_None ? NULL : (PyFrameObject*)back;
    Py_XINCREF(frame->f_back);
    frame->f_lasti = PyInt_AsLong(lasti);
    frame->f_lineno = PyInt_AsLong(lineno);

    /* Pop the fast locals namespace */
    int mark = vec_int_pop(&ctx->mark_stack);
    size_t nitems = vec_size(&ctx->stack) - mark;

    if(vec_size(&ctx->stack) < mark) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_frame;
    }

    for(int i = 0; i < nitems; i++) {

        PyObject *val = vec_pobj_pop(&ctx->stack);
        if(!val)
            continue;
        frame->f_localsplus[i] = val;
    }

    /* Pop all the PyTryBlocks */
    mark = vec_int_pop(&ctx->mark_stack);
    nitems = vec_size(&ctx->stack) - mark;

    if(vec_size(&ctx->stack) < mark) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_frame;
    }
    if(nitems % 3 != 0) {
        SET_RUNTIME_EXC("PF_FRAME: Number of stack items for the block stack not divisible by 3");
        goto fail_frame;
    }
    nitems /= 3;

    for(int i = 0; i < nitems; i++) {

        PyObject *level = vec_pobj_pop(&ctx->stack);
        PyObject *handler = vec_pobj_pop(&ctx->stack);
        PyObject *type = vec_pobj_pop(&ctx->stack);

        if(!PyInt_Check(level)
        || !PyInt_Check(handler)
        || !PyInt_Check(type)) {
            SET_RUNTIME_EXC("PF_FRAME: Got non-int blockstack item fields");
            goto fail_inner;
        }

        PyFrame_BlockSetup(frame, PyInt_AS_LONG(type),
            PyInt_AS_LONG(handler), PyInt_AS_LONG(level));

    fail_inner:
        Py_DECREF(level);
        Py_DECREF(handler);
        Py_DECREF(type);
        if(PyErr_Occurred())
            goto fail_frame;
    }

    /* Pop the valuestack */
    mark = vec_int_pop(&ctx->mark_stack);
    nitems = vec_size(&ctx->stack) - mark;

    if(vec_size(&ctx->stack) < mark-1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_frame;
    }

    assert(frame->f_stacktop == frame->f_valuestack);
    for(int i = 0; i < nitems; i++) {
        *(frame->f_stacktop++) = vec_pobj_pop(&ctx->stack);
    }

    PyObject *sent = vec_pobj_pop(&ctx->stack);
    if(PyInt_Check(sent) && PyInt_AS_LONG(sent) != nitems) {

        SET_RUNTIME_EXC("PF_FRAME: Sentinel reports incorrect number of items on valuestack [exp: %u, act: %ld]",
            (unsigned)nitems, PyInt_AS_LONG(sent));
        goto fail_sent;
    }

    if(sent == Py_None) {
        frame->f_stacktop = NULL;
    }

    Py_INCREF(frame);
    PyObject_GC_Track(frame);
    vec_pobj_push(&ctx->stack, (PyObject*)frame);
    ret = 0;

fail_sent:
    Py_DECREF(sent);
fail_frame: 
fail_typecheck:
    Py_DECREF(locals);
    Py_DECREF(globals);
    Py_DECREF(code);
    Py_DECREF(back);
    Py_DECREF(lasti);
    Py_DECREF(frame);
fail_underflow:
    return ret;
}

static int op_ext_nullval(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_NULLVAL, ctx);
    vec_pobj_push(&ctx->stack, NULL);
    return 0;
}

static int op_ext_traceback(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_TRACEBACK, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 4) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *lasti = vec_pobj_pop(&ctx->stack);
    PyObject *lineno = vec_pobj_pop(&ctx->stack);
    PyObject *frame = vec_pobj_pop(&ctx->stack);
    PyObject *next = vec_pobj_pop(&ctx->stack);

    if(!PyInt_Check(lasti)
    || !PyInt_Check(lineno)) {
        SET_RUNTIME_EXC("PF_TRACEBACK: Expecting int objects on TOS and TOS1");
        goto fail_typecheck;
    }

    if(!PyFrame_Check(frame)) {
        SET_RUNTIME_EXC("PF_TRACEBACK: Expecting frame object on TOS2");
        goto fail_typecheck;
    }

    if(next != Py_None && !PyTraceBack_Check(next)) {
        SET_RUNTIME_EXC("PF_TRACEBACK: Expecting tracebakc or None object on TOS3");
        goto fail_typecheck;
    }

    PyTracebackObject *tb = PyObject_GC_New(PyTracebackObject, &PyTraceBack_Type);
    CHK_TRUE(tb, fail_tb);
    if(next != Py_None) {
        Py_INCREF(next);
        tb->tb_next = (struct _traceback*)next; 
    }else{
        tb->tb_next = NULL;
    }
    Py_INCREF(frame);
    tb->tb_frame = (struct _frame*)frame;
    tb->tb_lasti = PyInt_AsLong(lasti);
    tb->tb_lineno = PyInt_AsLong(lineno);
    PyObject_GC_Track(tb);

    vec_pobj_push(&ctx->stack, (PyObject*)tb);
    ret = 0;

fail_tb:
fail_typecheck:
    Py_DECREF(frame);
    Py_DECREF(next);
fail_underflow:
    return ret;
}

static int op_ext_emptyframe(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_EMPTYFRAME, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *valsize = vec_pobj_pop(&ctx->stack);

    if(!PyInt_Check(valsize)) {
        SET_RUNTIME_EXC("PF_EMPTYFRAME: Expecting integer object on TOS"); 
        goto fail_typecheck;
    }

    PyObject *code = (PyObject*)PyCode_NewEmpty("__placeholder__", "__placeholder__", 0);
    CHK_TRUE(code, fail_typecheck);
    /* Patch the stacksize so that enough memory is allocated for the frame 
     * to house the code object that will eventually be there */
    ((PyCodeObject*)code)->co_stacksize = PyInt_AsSsize_t(valsize);

    PyObject *globals = PyDict_New();
    CHK_TRUE(globals, fail_globals);

    PyObject *locals = PyDict_New();
    CHK_TRUE(locals, fail_locals);

    PyThreadState *tstate = PyThreadState_GET();
    PyFrameObject *frame = PyFrame_New(tstate, (PyCodeObject*)code, globals, locals);
    CHK_TRUE(frame, fail_frame);

    vec_pobj_push(&ctx->stack, (PyObject*)frame);
    ret = 0;

fail_frame:
    Py_DECREF(locals);
fail_locals:
    Py_DECREF(globals);
fail_globals:
    Py_DECREF(code);
fail_typecheck:
    Py_DECREF(valsize);
fail_underflow:
    return ret;
}

static int op_ext_weakref(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_WEAKREF, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *callback = vec_pobj_pop(&ctx->stack);
    PyObject *referent = vec_pobj_pop(&ctx->stack);

    if(callback != Py_None && !PyCallable_Check(callback)) {
        SET_RUNTIME_EXC("PF_WEAKREF: Expecting callable or none on TOS");
        goto fail_typecheck;
    }

    if(!PyType_SUPPORTS_WEAKREFS(Py_TYPE(referent)) && referent != Py_None) {
        SET_RUNTIME_EXC("PF_WEAKREF: Expecting object of type that supports weakrefs on TOS1");
        goto fail_typecheck;
    }

    /* If the referent is 'None', that means it has been GC'd already */
    PyObject *retval = NULL;
    if(referent == Py_None) {

        PyObject *dummy = PyObject_CallFunction(s_placeholder_type, "()");
        retval = PyWeakref_NewRef(dummy, callback);
        Py_DECREF(dummy);
    }else{
        retval = PyWeakref_NewRef(referent, callback);
    }
    CHK_TRUE(retval, fail_ref);

    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_ref:
fail_typecheck:
    Py_DECREF(callback);
    vec_pobj_push(&ctx->to_free, referent);
fail_underflow:
    return ret;
}

static int op_ext_weakproxy(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_PROXY, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *callback = vec_pobj_pop(&ctx->stack);
    PyObject *referent = vec_pobj_pop(&ctx->stack);

    if(callback != Py_None && !PyCallable_Check(callback)) {
        SET_RUNTIME_EXC("PF_PROXY: Expecting callable or none on TOS");
        goto fail_typecheck;
    }

    /* If the referent is 'None', that means it has been GC'd already */
    PyObject *retval = NULL;
    if(referent == Py_None) {

        PyObject *dummy = PyObject_CallFunction(s_placeholder_type, "()");
        retval = PyWeakref_NewProxy(dummy, callback);
        Py_DECREF(dummy);
    }else{
        retval = PyWeakref_NewProxy(referent, callback);
    }
    CHK_TRUE(retval, fail_proxy);

    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_proxy:
fail_typecheck:
    Py_DECREF(callback);
    Py_DECREF(referent);
fail_underflow:
    return ret;
}

static int op_ext_stentry(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_STENTRY, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 16) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *ste_tmpname = vec_pobj_pop(&ctx->stack);
    PyObject *ste_lineno = vec_pobj_pop(&ctx->stack);
    PyObject *ste_returns_value = vec_pobj_pop(&ctx->stack);
    PyObject *ste_varkeywords = vec_pobj_pop(&ctx->stack);
    PyObject *ste_varargs = vec_pobj_pop(&ctx->stack);
    PyObject *ste_generator = vec_pobj_pop(&ctx->stack);
    PyObject *ste_child_free = vec_pobj_pop(&ctx->stack);
    PyObject *ste_free = vec_pobj_pop(&ctx->stack);
    PyObject *ste_nested = vec_pobj_pop(&ctx->stack);
    PyObject *ste_unoptimized = vec_pobj_pop(&ctx->stack);
    PyObject *ste_type = vec_pobj_pop(&ctx->stack);
    PyObject *ste_children = vec_pobj_pop(&ctx->stack);
    PyObject *ste_varnames = vec_pobj_pop(&ctx->stack);
    PyObject *ste_name = vec_pobj_pop(&ctx->stack);
    PyObject *ste_symbols = vec_pobj_pop(&ctx->stack);
    PyObject *ste_id = vec_pobj_pop(&ctx->stack);

    if(!PyInt_Check(ste_id) && !PyLong_Check(ste_id)) {
        SET_RUNTIME_EXC("PF_STENTRY: Expecting int or long object on TOS15");
        goto fail_typecheck;
    }

    if(!PyDict_Check(ste_symbols)) {
        SET_RUNTIME_EXC("PF_STENTRY: Expecting dict object on TOS14");
        goto fail_typecheck;
    }

    if(!PyString_Check(ste_name)) {
        SET_RUNTIME_EXC("PF_STENTRY: Expecting string object on TOS13");
        goto fail_typecheck;
    }

    if(!PyList_Check(ste_varnames) && !PyList_Check(ste_children)) {
        SET_RUNTIME_EXC("PF_STENTRY: Expecting list objects on TOS12 and TOS11");
        goto fail_typecheck;
    }

    if(!PyInt_Check(ste_type) 
    && !PyInt_Check(ste_unoptimized)
    && !PyInt_Check(ste_nested)
    && !PyInt_Check(ste_free)
    && !PyInt_Check(ste_child_free)
    && !PyInt_Check(ste_generator)
    && !PyInt_Check(ste_varargs)
    && !PyInt_Check(ste_returns_value)
    && !PyInt_Check(ste_lineno)
    && !PyInt_Check(ste_tmpname)) {
        SET_RUNTIME_EXC("PF_STENTRY: Expecting int objects on TOS10 through TOS");
        goto fail_typecheck;
    }

    PySTEntryObject *retval = PyObject_New(PySTEntryObject, &PySTEntry_Type);
    CHK_TRUE(retval, fail_entry);

    retval->ste_id = ste_id;
    retval->ste_symbols = ste_symbols;
    retval->ste_name = ste_name;
    retval->ste_varnames = ste_varnames;
    retval->ste_children = ste_children;
    retval->ste_type = PyInt_AS_LONG(ste_type);
    retval->ste_unoptimized = PyInt_AS_LONG(ste_unoptimized);
    retval->ste_nested = PyInt_AS_LONG(ste_nested);
    retval->ste_free = PyInt_AS_LONG(ste_free);
    retval->ste_child_free = PyInt_AS_LONG(ste_child_free);
    retval->ste_generator = PyInt_AS_LONG(ste_generator);
    retval->ste_varargs = PyInt_AS_LONG(ste_varargs);
    retval->ste_varkeywords = PyInt_AS_LONG(ste_varkeywords);
    retval->ste_returns_value = PyInt_AS_LONG(ste_returns_value);
    retval->ste_lineno = PyInt_AS_LONG(ste_lineno);
    retval->ste_tmpname = PyInt_AS_LONG(ste_tmpname);

    Py_INCREF(ste_id);
    Py_INCREF(ste_symbols);
    Py_INCREF(ste_name);
    Py_INCREF(ste_varnames);
    Py_INCREF(ste_children);

    vec_pobj_push(&ctx->stack, (PyObject*)retval);
    ret = 0;

fail_entry:
fail_typecheck:
    Py_DECREF(ste_tmpname);
    Py_DECREF(ste_lineno);
    Py_DECREF(ste_returns_value);
    Py_DECREF(ste_varkeywords);
    Py_DECREF(ste_varargs);
    Py_DECREF(ste_generator);
    Py_DECREF(ste_child_free);
    Py_DECREF(ste_free);
    Py_DECREF(ste_nested);
    Py_DECREF(ste_unoptimized);
    Py_DECREF(ste_type);
fail_underflow:
    return ret;
}

static int op_ext_dictkeys(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_DICTKEYS, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *dict = vec_pobj_pop(&ctx->stack);

    if(!PyDict_Check(dict)) {
        SET_RUNTIME_EXC("PF_DICTKEYS: Expecting dict object at TOS");
        goto fail_typecheck;
    }

    PyObject *method = PyObject_GetAttrString(dict, "viewkeys");
    PyObject *retval = PyObject_CallFunction(method, "()");
    Py_DECREF(method);
    CHK_TRUE(retval, fail_view);

    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_view:
fail_typecheck:
    Py_DECREF(dict);
fail_underflow:
    return ret;
}

static int op_ext_dictvalues(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_DICTVALUES, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *dict = vec_pobj_pop(&ctx->stack);

    if(!PyDict_Check(dict)) {
        SET_RUNTIME_EXC("PF_VALUES: Expecting dict object at TOS");
        goto fail_typecheck;
    }

    PyObject *method = PyObject_GetAttrString(dict, "viewvalues");
    PyObject *retval = PyObject_CallFunction(method, "()");
    Py_DECREF(method);
    CHK_TRUE(retval, fail_view);

    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_view:
fail_typecheck:
    Py_DECREF(dict);
fail_underflow:
    return ret;
}

static int op_ext_dictitems(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_DICTITEMS, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *dict = vec_pobj_pop(&ctx->stack);

    if(!PyDict_Check(dict)) {
        SET_RUNTIME_EXC("PF_DICTKEYS: Expecting dict object at TOS");
        goto fail_typecheck;
    }

    PyObject *method = PyObject_GetAttrString(dict, "viewitems");
    PyObject *retval = PyObject_CallFunction(method, "()");
    Py_DECREF(method);
    CHK_TRUE(retval, fail_view);

    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_view:
fail_typecheck:
    Py_DECREF(dict);
fail_underflow:
    return ret;
}

static int op_ext_calliter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_CALLITER, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *sent = vec_pobj_pop(&ctx->stack);
    PyObject *call = vec_pobj_pop(&ctx->stack);

    /* Don't check for 'callable' - placeholders may not have 
     * had their '__call__ attribute set yet' */

    calliterobject *retval = PyObject_GC_New(calliterobject, &PyCallIter_Type);
    CHK_TRUE(retval, fail_iter);

    if(call != Py_None) {
        retval->it_callable = call; 
        Py_INCREF(call);
    }else{
        retval->it_callable = NULL;
    }

    if(sent != Py_None) {
        retval->it_sentinel = sent; 
        Py_INCREF(sent);
    }else{
        retval->it_sentinel = NULL;
    }
    PyObject_GC_Track(retval);

    vec_pobj_push(&ctx->stack, (PyObject*)retval);
    ret = 0;

fail_iter:
    Py_DECREF(sent);
    Py_DECREF(call);
fail_underflow:
    return ret;
}

static int op_ext_seqiter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_SEQITER, ctx);
    return op_ext_seqiter_with_type(ctx, rw, &PySeqIter_Type);
}

static int op_ext_bytearriter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_BYTEARRITER, ctx);
    return op_ext_seqiter_with_type(ctx, rw, &PyByteArrayIter_Type);
}

static int op_ext_tupleiter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_TUPLEITER, ctx);

    int idx = dispatch_idx_for_picklefunc(tuple_iter_pickle);
    PyTypeObject *type = s_type_dispatch_table[idx].type;
    return op_ext_seqiter_with_type(ctx, rw, type);
}

static int op_ext_revlistiter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_LISTREVITER, ctx);

    int idx = dispatch_idx_for_picklefunc(list_rev_iter_pickle);
    PyTypeObject *type = s_type_dispatch_table[idx].type;
    return op_ext_seqiter_with_type(ctx, rw, type);
}

static int op_ext_dictiter_with_type(struct unpickle_ctx *ctx, SDL_RWops *rw,
                                     PyTypeObject *type)
{
    int ret = -1;

    if(vec_size(&ctx->stack) < 5) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *len = vec_pobj_pop(&ctx->stack);
    PyObject *di_result = vec_pobj_pop(&ctx->stack);
    PyObject *di_pos = vec_pobj_pop(&ctx->stack);
    PyObject *di_used = vec_pobj_pop(&ctx->stack);
    PyObject *di_dict = vec_pobj_pop(&ctx->stack);

    if(!PyLong_Check(len)
    || !PyLong_Check(di_pos)
    || !PyLong_Check(di_pos)) {
        SET_RUNTIME_EXC("Expecting long objects on TOS, TOS2, and TOS3");
        goto fail_typecheck;
    }

    if(di_result != Py_None && !PyTuple_Check(di_result)) {
        SET_RUNTIME_EXC("Expecting tuple object or None on TOS1");
        goto fail_typecheck;
    }

    if(di_dict != Py_None && !PyDict_Check(di_dict)) {
        SET_RUNTIME_EXC("Expecting dict object on TOS1");
        goto fail_typecheck;
    }

    dictiterobject *retval = PyObject_GC_New(dictiterobject, type);
    CHK_TRUE(retval, fail_iter);
    if(di_dict != Py_None) {
        retval->di_dict = (PyDictObject*)di_dict; 
        Py_INCREF(di_dict);
    }else{
        retval->di_dict = NULL; 
    }
    retval->di_used = PyLong_AsSsize_t(di_used);
    retval->di_pos = PyLong_AsSsize_t(di_pos);
    if(di_result != Py_None) {
        retval->di_result = di_result;
        Py_INCREF(di_result);
    }else{
        retval->di_result = NULL; 
    }
    retval->len = PyLong_AsSsize_t(len);
    PyObject_GC_Track(retval);

    vec_pobj_push(&ctx->stack, (PyObject*)retval);
    ret = 0;

fail_iter:
fail_typecheck:
    Py_DECREF(len);
    Py_DECREF(di_result);
    Py_DECREF(di_pos);
    Py_DECREF(di_used);
    Py_DECREF(di_dict);
fail_underflow:
    return ret;
}

static int op_ext_dictkeyiter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_DICTKEYITER, ctx);
    return op_ext_dictiter_with_type(ctx, rw, &PyDictIterKey_Type);
}

static int op_ext_dictvaliter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_DICTVALITER, ctx);
    return op_ext_dictiter_with_type(ctx, rw, &PyDictIterValue_Type);
}

static int op_ext_dictitemiter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_DICTITEMITER, ctx);
    return op_ext_dictiter_with_type(ctx, rw, &PyDictIterItem_Type);
}

static int op_ext_setiter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_SETITER, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 4) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *len = vec_pobj_pop(&ctx->stack);
    PyObject *si_pos = vec_pobj_pop(&ctx->stack);
    PyObject *si_used = vec_pobj_pop(&ctx->stack);
    PyObject *si_set = vec_pobj_pop(&ctx->stack);

    if(!PyLong_Check(len)
    || !PyLong_Check(si_pos)
    || !PyLong_Check(si_used)) {
        SET_RUNTIME_EXC("PF_SETITER: Expecting long objects for the top 3 TOS items");
        goto fail_typecheck;
    }

    if(!PySet_Check(si_set)) {
        SET_RUNTIME_EXC("PF_SETITER: Expecting set object at TOS3"); 
        goto fail_typecheck;
    }

    int idx = dispatch_idx_for_picklefunc(set_iter_pickle);
    PyTypeObject *setiter_type = s_type_dispatch_table[idx].type;
    setiterobject *retval = PyObject_GC_New(setiterobject, setiter_type);
    CHK_TRUE(retval, fail_iter);

    if(si_set != Py_None) {
        retval->si_set = (PySetObject*)si_set;
        Py_INCREF(si_set);
    }else{
        retval->si_set = NULL;
    }
    retval->si_used = PyLong_AsSsize_t(si_used);
    retval->si_pos = PyLong_AsSsize_t(si_pos);
    retval->len = PyLong_AsSsize_t(len);
    PyObject_GC_Track(retval);

    vec_pobj_push(&ctx->stack, (PyObject*)retval);
    ret = 0;

fail_iter:
fail_typecheck:
    Py_DECREF(len);
    Py_DECREF(si_pos);
    Py_DECREF(si_used);
    Py_DECREF(si_set);
fail_underflow:
    return ret;
}

static int op_ext_fieldnameiter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_FIELDNAMEITER, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 4) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *str_end = vec_pobj_pop(&ctx->stack);
    PyObject *str_ptr = vec_pobj_pop(&ctx->stack);
    PyObject *ptr = vec_pobj_pop(&ctx->stack);
    PyObject *str = vec_pobj_pop(&ctx->stack);

    if(!PyLong_Check(str_end)
    || !PyLong_Check(str_ptr)
    || !PyLong_Check(ptr)) {
        SET_RUNTIME_EXC("PF_FIELDNAMEITER: Expecting long objects as top 3 TOS items"); 
        goto fail_typecheck;
    }

    if(!PyString_Check(str)) {
        SET_RUNTIME_EXC("PF_FIELDNAMEITER: Expecting string object at TOS3");
        goto fail_typecheck;
    }

    int idx = dispatch_idx_for_picklefunc(field_name_iter_pickle);
    PyTypeObject *iter_type = s_type_dispatch_table[idx].type;
    fieldnameiterobject *retval = PyObject_New(fieldnameiterobject, iter_type);
    CHK_TRUE(retval, fail_iter);

    retval->str = (PyStringObject*)str;
    Py_INCREF(str);
    char *raw = PyString_AS_STRING(str);
    retval->it_field.ptr = raw + PyLong_AsLong(ptr);
    retval->it_field.str.ptr = raw + PyLong_AsLong(str_ptr);
    retval->it_field.str.end = raw + PyLong_AsLong(str_end);

    vec_pobj_push(&ctx->stack, (PyObject*)retval);
    ret = 0;

fail_iter:
fail_typecheck:
    Py_DECREF(str_end);
    Py_DECREF(str_ptr);
    Py_DECREF(ptr);
    Py_DECREF(str);
fail_underflow:
    return ret;
}

static int op_ext_formatiter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_FIELDNAMEITER, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 3) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *str_end = vec_pobj_pop(&ctx->stack);
    PyObject *str_ptr = vec_pobj_pop(&ctx->stack);
    PyObject *str = vec_pobj_pop(&ctx->stack);

    if(!PyLong_Check(str_end)
    || !PyLong_Check(str_ptr)) {
        SET_RUNTIME_EXC("PF_FIELDNAMEITER: Expecting long objects as top 2 TOS items"); 
        goto fail_typecheck;
    }

    if(!PyString_Check(str)) {
        SET_RUNTIME_EXC("PF_FIELDNAMEITER: Expecting string object at TOS2");
        goto fail_typecheck;
    }

    int idx = dispatch_idx_for_picklefunc(formatter_iter_pickle);
    PyTypeObject *iter_type = s_type_dispatch_table[idx].type;
    formatteriterobject *retval = PyObject_New(formatteriterobject, iter_type);
    CHK_TRUE(retval, fail_iter);

    retval->str = (PyStringObject*)str;
    Py_INCREF(str);
    char *raw = PyString_AS_STRING(str);
    retval->it_markup.str.ptr = raw + PyLong_AsLong(str_ptr);
    retval->it_markup.str.end = raw + PyLong_AsLong(str_end);

    vec_pobj_push(&ctx->stack, (PyObject*)retval);
    ret = 0;

fail_iter:
fail_typecheck:
    Py_DECREF(str_end);
    Py_DECREF(str_ptr);
    Py_DECREF(str);
fail_underflow:
    return ret;
}

static int op_ext_exception(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_EXCEPTION, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(type) || !PyExceptionClass_Check(type)) {
        SET_RUNTIME_EXC("PF_EXCEPTION: Expecting type that is a non-strict subclass of Exception on TOS"); 
        goto fail_typecheck;
    }

    PyObject *retval = PyObject_CallFunction(type, "()");
    CHK_TRUE(retval, fail_exc);

    vec_pobj_push(&ctx->stack, retval);
    ret = 0;

fail_exc:
fail_typecheck:
    Py_DECREF(type);
fail_underflow:
    return ret;
}

static int op_ext_method_desc(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_METHOD_DESC, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    PyObject *name = vec_pobj_pop(&ctx->stack);
    PyObject *type = vec_pobj_pop(&ctx->stack);

    if(!PyString_Check(name)) {
        SET_RUNTIME_EXC("PF_METHOD_DESC: Expecting string at TOS");
        goto fail_typecheck;
    }

    if(!PyType_Check(type)) {
        SET_RUNTIME_EXC("PF_METHOD_DESC: Expecting type at TOS1");
        goto fail_typecheck;
    }

    PyTypeObject *tp_type = (PyTypeObject*)type;
    PyMethodDef *found = NULL;
    for(PyMethodDef *curr = tp_type->tp_methods; curr && curr->ml_name; curr++) {
    
        if(0 == strcmp(curr->ml_name, PyString_AS_STRING(name))) {
            found = curr; 
            break;
        }
    }

    if(!found) {
        SET_RUNTIME_EXC("Could not find method_descriptor (%s) of type (%s)",
            PyString_AS_STRING(name), tp_type->tp_name);
        goto fail_desc;
    }

    PyObject *desc = PyDescr_NewMethod(tp_type, found);
    CHK_TRUE(desc, fail_desc);
    vec_pobj_push(&ctx->stack, desc);
    ret = 0;

fail_desc:
fail_typecheck:
    Py_DECREF(name);
    Py_DECREF(type);
fail_underflow:
    return ret;
}

static int op_ext_bi_method(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_BI_METHOD, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 3) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    PyObject *name = vec_pobj_pop(&ctx->stack);
    PyObject *type = vec_pobj_pop(&ctx->stack);
    PyObject *inst = vec_pobj_pop(&ctx->stack);

    if(!PyString_Check(name)) {
        SET_RUNTIME_EXC("PF_BI_METHOD: Expecting string at TOS");
        goto fail_typecheck;
    }

    if(!PyType_Check(type)) {
        SET_RUNTIME_EXC("PF_BI_METHOD: Expecting type at TOS1");
        goto fail_typecheck;
    }

    PyTypeObject *tp_type = (PyTypeObject*)type;
    PyMethodDef *found = NULL;
    for(PyMethodDef *curr = tp_type->tp_methods; curr && curr->ml_name; curr++) {
    
        if(0 == strcmp(curr->ml_name, PyString_AS_STRING(name))) {
            found = curr; 
            break;
        }
    }

    PyObject *bases;
    if(!found && (bases = tp_type->tp_bases)) {

        assert(PyTuple_Check(bases));

        for(int i = 0; i < PyTuple_GET_SIZE(bases); i++) {

            tp_type = (PyTypeObject*)PyTuple_GET_ITEM(bases, i);
            assert(PyType_Check(tp_type));

            for(PyMethodDef *curr = tp_type->tp_methods; curr && curr->ml_name; curr++) {
            
                if(0 == strcmp(curr->ml_name, PyString_AS_STRING(name))) {
                    found = curr; 
                    goto done;
                }
            }
        }
    }
done:

    if(!found) {
        SET_RUNTIME_EXC("Could not find method (%s) of type (%s)",
            PyString_AS_STRING(name), tp_type->tp_name);
        goto fail_method;
    }

    PyObject *meth = PyCFunction_New(found, inst);
    CHK_TRUE(meth, fail_method);
    vec_pobj_push(&ctx->stack, meth);
    ret = 0;

fail_method:
fail_typecheck:
    Py_DECREF(name);
    Py_DECREF(type);
    Py_DECREF(inst);
fail_underflow:
    return ret;
}

static int op_ext_oper_itemgetter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_OP_ITEMGET, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    int idx = dispatch_idx_for_picklefunc(oper_itemgetter_pickle);
    PyTypeObject *itemgettertype = s_type_dispatch_table[idx].type;

    PyObject *item = vec_pobj_pop(&ctx->stack);
    PyObject *nitems = vec_pobj_pop(&ctx->stack);

    if(!PyInt_Check(nitems)) {
        SET_RUNTIME_EXC("PF_OP_ITEMGET: Expecting int at TOS1");
        goto fail_typecheck;
    }

    itemgetterobject *rval = PyObject_GC_New(itemgetterobject, itemgettertype);
    Py_INCREF(item);
    rval->item = item;
    rval->nitems = PyInt_AsSsize_t(nitems);
    PyObject_GC_Track(rval);

    vec_pobj_push(&ctx->stack, (PyObject*)rval);
    ret = 0;

fail_typecheck:
    Py_DECREF(item);
    Py_DECREF(nitems);
fail_underflow:
    return ret;
}

static int op_ext_oper_attrgetter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_OP_ATTRGET, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    int idx = dispatch_idx_for_picklefunc(oper_attrgetter_pickle);
    PyTypeObject *attrgettertype = s_type_dispatch_table[idx].type;

    PyObject *attr = vec_pobj_pop(&ctx->stack);
    PyObject *nattrs = vec_pobj_pop(&ctx->stack);

    if(!PyInt_Check(nattrs)) {
        SET_RUNTIME_EXC("PF_OP_ATTRGET: Expecting int at TOS1");
        goto fail_typecheck;
    }

    attrgetterobject *rval = PyObject_GC_New(attrgetterobject, attrgettertype);
    Py_INCREF(attr);
    rval->attr = attr;
    rval->nattrs = PyInt_AsSsize_t(nattrs);
    PyObject_GC_Track(rval);

    vec_pobj_push(&ctx->stack, (PyObject*)rval);
    ret = 0;

fail_typecheck:
    Py_DECREF(attr);
    Py_DECREF(nattrs);
fail_underflow:
    return ret;
}

static int op_ext_oper_methodcaller(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_OP_METHODCALL, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 3) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    int idx = dispatch_idx_for_picklefunc(oper_methodcaller_pickle);
    PyTypeObject *methodcallertype = s_type_dispatch_table[idx].type;

    PyObject *kwds = vec_pobj_pop(&ctx->stack);
    PyObject *args = vec_pobj_pop(&ctx->stack);
    PyObject *name = vec_pobj_pop(&ctx->stack);

    if(kwds != Py_None && !PyDict_Check(kwds)) {
        SET_RUNTIME_EXC("PF_OP_METHODCALL: Expecting dictionary of None at TOS");
        goto fail_typecheck;
    }

    if(!PyTuple_Check(args)) {
        SET_RUNTIME_EXC("PF_OP_METHODCALL: Expecting tuple at TOS1");
        goto fail_typecheck;
    }

    if(!PyString_Check(name)) {
        SET_RUNTIME_EXC("PF_OP_METHODCALL: Expecting string at TOS2");
        goto fail_typecheck;
    }

    methodcallerobject *rval = PyObject_GC_New(methodcallerobject, methodcallertype);
    Py_INCREF(name);
    rval->name = name;
    Py_INCREF(args);
    rval->args = args;
    if(kwds != Py_None) {
        Py_INCREF(kwds);
        rval->kwds = kwds;
    }else{
        rval->kwds = NULL;
    }
    PyObject_GC_Track(rval);

    vec_pobj_push(&ctx->stack, (PyObject*)rval);
    ret = 0;

fail_typecheck:
    Py_DECREF(name);
    Py_DECREF(args);
    Py_DECREF(kwds);
fail_underflow:
    return ret;
}

static int op_ext_custom(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_CUSTOM, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 2) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }

    if(vec_size(&ctx->mark_stack) == 0) {
        SET_RUNTIME_EXC("Mark stack underflow");
        return -1;
    }
    vec_int_pop(&ctx->mark_stack);

    PyObject *str = vec_pobj_pop(&ctx->stack);
    PyObject *klass = vec_pobj_pop(&ctx->stack);

    if(!PyString_Check(str)) {
        SET_RUNTIME_EXC("PF_CUSTOM: Expecting string at TOS");
        goto fail_typecheck;
    }

    if(!PyType_Check(klass)) {
        SET_RUNTIME_EXC("PF_CUSTOM: Expecting type at TOS1");
        goto fail_typecheck;
    }
    PyObject *ctype = constructor_type((PyTypeObject*)klass);
    assert(ctype);

    struct py_unpickle_ctx user = (struct py_unpickle_ctx) {
        .stack = &ctx->stack
    };

    PyObject *pmeth = PyObject_GetAttrString(klass, "__unpickle__");
    PyObject *args = Py_BuildValue("(O)", str);
    PyObject *kwargs = Py_BuildValue("{s:s#}", "__ctx__", (void*)&user, sizeof(user));

    PyObject *tuple = NULL;
    if(pmeth && args && kwargs) {
        tuple = PyObject_Call(pmeth, args, kwargs);
    }
    Py_XDECREF(pmeth);
    Py_XDECREF(args);
    Py_XDECREF(kwargs);

    if(!tuple || !PyTuple_Check(tuple)) {
        assert(PyErr_Occurred());
        goto fail_inst;
    }
    PyObject *rval = PyTuple_GetItem(tuple, 0);
    Py_INCREF(rval);
    Py_DECREF(tuple);
    CHK_TRUE(rval, fail_inst);

    vec_pobj_push(&ctx->stack, rval);
    ret = 0;

fail_inst:
fail_typecheck:
    Py_DECREF(str);
    Py_DECREF(klass);
fail_underflow:
    return ret;
}

static int op_ext_alloc(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_ALLOC, ctx);
    int ret = -1;

    if(vec_size(&ctx->stack) < 1) {
        SET_RUNTIME_EXC("Stack underflow");
        goto fail_underflow;
    }
    PyObject *klass = vec_pobj_pop(&ctx->stack);

    if(!PyType_Check(klass)) {
        SET_RUNTIME_EXC("PF_CUSTOM: Expecting type at TOS1");
        goto fail_typecheck;
    }

    PyObject *rval = ((PyTypeObject*)klass)->tp_alloc((struct _typeobject*)klass, 0);
    CHK_TRUE(rval, fail_inst);

    vec_pobj_push(&ctx->stack, rval);
    ret = 0;

fail_inst:
fail_typecheck:
    Py_DECREF(klass);
fail_underflow:
    return ret;
}

static int op_ext_nullimporter(struct unpickle_ctx *ctx, SDL_RWops *rw)
{
    TRACE_OP(PF_NULLIMPORTER, ctx);

    PyObject *args = PyTuple_Pack(1, PyString_FromString("__test__")); /* Pass any invalid path; it's not saved */
    PyObject *ret = PyObject_Call((PyObject*)&PyNullImporter_Type, args, NULL);
    Py_DECREF(args);
    assert(ret);

    vec_pobj_push(&ctx->stack, ret);
    return 0;
}

static bool pickle_ctx_init(struct pickle_ctx *ctx)
{
    if(NULL == (ctx->memo = kh_init(memo))) {
        SET_EXC(PyExc_MemoryError, "Memo table allocation");
        goto fail_memo;
    }

    vec_pobj_init(&ctx->to_free);
    vec_pobj_resize(&ctx->to_free, 16 * 1024);
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
    vec_pobj_init(&ctx->to_free);

    vec_pobj_resize(&ctx->stack, 4 * 1024);
    vec_pobj_resize(&ctx->memo, 16 * 1024);
    vec_int_resize(&ctx->mark_stack, 1024);
    vec_pobj_resize(&ctx->to_free, 16 * 1024);

    ctx->stop = false;
    return true;
}

static void unpickle_ctx_destroy(struct unpickle_ctx *ctx)
{
    for(int i = 0; i < vec_size(&ctx->memo); i++) {
        Py_DECREF(vec_AT(&ctx->memo, i));
    }

    for(int i = 0; i < vec_size(&ctx->to_free); i++) {
        Py_DECREF(vec_AT(&ctx->to_free, i));
    }
    
    vec_pobj_destroy(&ctx->to_free);
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
    pf_snprintf(str, ARR_SIZE(str), "%c%d\n", GET, memo_idx(ctx, obj));
    str[ARR_SIZE(str)-1] = '\0';
    return rw->write(rw, str, 1, strlen(str));
}

static bool emit_put(const struct pickle_ctx *ctx, PyObject *obj, SDL_RWops *rw)
{
    char str[32];
    pf_snprintf(str, ARR_SIZE(str), "%c%d\n", PUT, memo_idx(ctx, obj));
    str[ARR_SIZE(str)-1] = '\0';
    return rw->write(rw, str, 1, strlen(str));
}

static bool emit_alloc(const struct pickle_ctx *ctx, SDL_RWops *rw)
{
    const char ops[] = {PF_EXTEND, PF_ALLOC};
    return rw->write(rw, ops, sizeof(ops), 1);
}

static void deferred_free(struct pickle_ctx *ctx, PyObject *obj)
{
    vec_pobj_push(&ctx->to_free, obj);
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
        vec_pobj_push(&ctx->to_free, key);
        value = PyDict_GetItem(ndw_attrs, key);
        assert(value);

        CHK_TRUE(pickle_obj(ctx, key, rw), fail);
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
    Sched_TryYield();
    pickle_func_t pf;

    if(0 != Py_EnterRecursiveCall("pickle_obj")) {
        PyErr_SetObject(PyExc_RuntimeError, PyExc_RecursionErrorInst);
        goto fail;
    }

    if(memo_contains(ctx, obj)) {
        CHK_TRUE(emit_get(ctx, obj, stream), fail);
        goto out;
    }

    pf = picklefunc_for_type(obj);
    /* It's not one of the known builtins */
    if(NULL == pf) {

        if(obj->ob_type->tp_flags & Py_TPFLAGS_HEAPTYPE) {
            pf = newclass_instance_pickle;
        }else{
            SET_RUNTIME_EXC("Cannot pickle object of type:%s", obj->ob_type->tp_name);
            goto fail;
        }
    }
    assert(pf);

    if(0 != pf(ctx, obj, stream)) {
        assert(PyErr_Occurred());
        goto fail; 
    }

    /* Some objects (eg. lists) may already be memoized */
    if(!memo_contains(ctx, obj)) {
        memoize(ctx, obj);
        CHK_TRUE(emit_put(ctx, obj, stream), fail);
    }

    if(pickle_attrs(ctx, obj, stream)) {
        assert(PyErr_Occurred());
        goto fail; 
    }

out:
    Py_LeaveRecursiveCall();
    assert(!PyErr_Occurred());
    return true;

fail:
    DEFAULT_ERR(PyExc_IOError, "Error writing to pickle stream");
    Py_LeaveRecursiveCall();
    assert(PyErr_Occurred());
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

    /* Dummy user-defined class to use for creating stubs */
    PyObject *args = Py_BuildValue("(s(O){})", "__placeholder__", (PyObject*)&PyBaseObject_Type);
    s_placeholder_type = PyObject_Call((PyObject*)&PyType_Type, args, NULL);
    Py_DECREF(args);
    assert(s_placeholder_type);

    pre_build_index();

    load_private_type_refs();
    load_builtin_types();
    load_exception_types();
    load_engine_builtin_types();
    reference_all_types();
    reference_codecs_builtins();
	load_subclassable_builtin_refs();
    create_builtin_subclasses();

    if(!S_Traverse_IndexQualnames(s_id_qualname_map))
        goto fail_traverse;

    post_build_index();

    return true;

fail_traverse:
    kh_destroy(str, s_id_qualname_map);
fail_id_qualname:
    return false;
}

void S_Pickle_Clear(void)
{
    for(int i = 0; i < ARR_SIZE(s_subclassable_builtin_map); i++) {
        Py_DECREF(s_subclassable_builtin_map[i].heap_subtype);
        s_subclassable_builtin_map[i].heap_subtype = NULL;
    }
    memset(s_subclassable_builtin_map, 0, sizeof(s_subclassable_builtin_map));
    Py_CLEAR(s_placeholder_type);
}

void S_Pickle_Shutdown(void)
{
    const char *curr;
    kh_foreach(s_id_qualname_map, (uint64_t){0}, curr, {
        PF_FREE(curr);
    });
    kh_destroy(str, s_id_qualname_map);
}

PyObject *S_Pickle_PlainHeapSubtype(PyTypeObject *type)
{
    return constructor_type(type);
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

PyObject *S_UnpickleObjgraph(SDL_RWops *stream)
{
    struct unpickle_ctx ctx;
    unpickle_ctx_init(&ctx);
    unsigned long opcount = 0;

    while(!ctx.stop) {
    
        unsigned char op;
        bool xtend = false;

        CHK_TRUE(stream->read(stream, &op, 1, 1), err);

        if(op == PF_EXTEND) {

            CHK_TRUE(stream->read(stream, &op, 1, 1), err);
            xtend =true;
        }

        unpickle_func_t upf = xtend ? s_ext_op_dispatch_table[op]
                                    : s_op_dispatch_table[op];
        if(!upf) {
            SET_RUNTIME_EXC("Bad %sopcode %c[%d]", (xtend ? "extended " : ""), op, (int)op);
            goto err;
        }
        CHK_TRUE(upf(&ctx, stream) == 0, err);

        opcount++;
        if(opcount % 10)
            Sched_TryYield();
    }

    if(vec_size(&ctx.stack) != 1) {
        SET_RUNTIME_EXC("Unexpected stack size [%u] after 'STOP'", (unsigned)vec_size(&ctx.stack));
        goto err;
    }

    if(vec_size(&ctx.mark_stack) != 0) {
        SET_RUNTIME_EXC("Unexpected mark stack size [%u] after 'STOP'", (unsigned)vec_size(&ctx.mark_stack));
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

