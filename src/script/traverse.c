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

#include "traverse.h"
#include "../lib/public/khash.h"

KHASH_SET_INIT_INT64(id)

struct visit_ctx{
    khash_t(id) *visited;
    int          depth;
    void        *user;
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

void s_traverse(PyObject *root, visitproc visit, struct visit_ctx *ctx)
{
    khiter_t k = kh_get(id, ctx->visited, (uintptr_t)root);
    if(k != kh_end(ctx->visited))
        return;

    visit(root, ctx);

    int ret;
    k = kh_put(id, ctx->visited, (uintptr_t)root, &ret);
    assert(ret != -1 && ret != 0);

    PyObject *attrs = PyObject_Dir(root);
    assert(attrs);

    for(int i = 0; i < PyList_Size(attrs); i++) {

        PyObject *attr = PyList_GET_ITEM(attrs, i);
        assert(PyString_Check(attr));
        if(!PyObject_HasAttr(root, attr))
            continue;

        PyObject *child = PyObject_GetAttr(root, attr);
        assert(child);

        /* When we are the sole owner of something returned by PyObject_GetAttr, it
         * means that it was a brand new object derived to satisfy this very attribute
         * lookup. An example of this is a long object's 'denominator' method
         * which returns a brand new heap-allocated PyObject to satisfy the 
         * PyObject_GetAttr call. Since in that case the type of the returned 
         * attribute is the same as that of the parent object (long), we will get
         * trapped in a cycle of infinite recursion if we traverse down it. So, don't 
         * recurse down 'derived' attributes, which are fulfilled with a unique 
         * object on each lookup.
         */
        if(child->ob_refcnt == 1) {
        
            Py_DECREF(child);
            continue;
        }

        ++(ctx->depth);
        s_traverse(child, visit, ctx);
        --(ctx->depth);

        Py_DECREF(child);
    }

    Py_DECREF(attrs);
}

static int visit_print(PyObject *obj, void *ctx)
{
    struct visit_ctx *vctx = ctx; 
    for(int i = 0; i < vctx->depth; i++)
        printf("  ");

    PyObject *str = PyObject_Repr(obj);
    assert(str && PyString_Check(str));
    printf("%s\n", PyString_AS_STRING(str));
    Py_DECREF(str);

    return 0;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool S_Traverse(PyObject *root, visitproc visit, void *user)
{
    int ret = false;
    struct visit_ctx ctx;

    ctx.visited = kh_init(id);
    if(!ctx.visited)
        goto fail_alloc;

    ctx.depth = 0;
    ctx.user = user;
    s_traverse(root, visit, &ctx);
    ret = true;

    kh_destroy(id, ctx.visited);
fail_alloc:
    return ret;
}

bool S_Traverse_PrintDFT(PyObject *root)
{
    return S_Traverse(root, visit_print, NULL);
}

