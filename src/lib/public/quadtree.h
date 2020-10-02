/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2020 Eduard Permyakov 
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

#ifndef QUADTREE_H
#define QUADTREE_H

#include "mpool.h"

#include <stddef.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>


/***********************************************************************************************/

#define QUADTREE_TYPE(name, type)                                                               \
                                                                                                \
    typedef struct qt_##name##_node_s {                                                         \
        size_t   depth;                                                                         \
        mp_ref_t parent;                                                                        \
        /* Head of singly-linked list of leaf nodes, each one holding an additional */          \
        /* record for this key. May be 0 (NULL). */                                             \
        mp_ref_t sibling_next;                                                                  \
        mp_ref_t nw, ne, sw, se;                                                                \
        bool     has_record;                                                                    \
        /* For nodes holding records, this is the position */                                   \
        /* For nodes without records, these are the partitioning coordinates along each axis */ \
        float    x, y;                                                                          \
        type     record;                                                                        \
    } qt_##name##_node_t;                                                                       \
                                                                                                \
    MPOOL_TYPE(name, qt_##name##_node_t)                                                        \
                                                                                                \
    typedef struct qt_##name##_s {                                                              \
        mp(name) node_pool;                                                                     \
        mp_ref_t root;                                                                          \
        size_t   nrecs;                                                                         \
        float xmin, xmax;                                                                       \
        float ymin, ymax;                                                                       \
    } qt_##name##_t;

/***********************************************************************************************/

#define qt(name)                                                                                \
    qt_##name##_t

#define qt_node(name)                                                                           \
    qt_##name##_node_t

#define QT_EPSILON  (1.0f/(1024*1024))
#define QT_EQ(a, b) (fabs(a - b) < QT_EPSILON)

#define _CHK_TRUE_RET(_pred, _ret)                                                              \
    do {                                                                                        \
        if(!_pred)                                                                              \
            return (_ret);                                                                      \
    }while(0)

#define _CHK_TRUE_JMP(_pred, _lbl)                                                              \
    do {                                                                                        \
        if(!_pred)                                                                              \
            goto _lbl;                                                                          \
    }while(0)

#define _MAX(a, b)  ((a) > (b) ? (a) : (b))

/***********************************************************************************************/

#define QUADTREE_PROTOTYPES(scope, name, type)                                                  \
                                                                                                \
    MPOOL_PROTOTYPES(scope, name, qt_node(name))                                                \
                                                                                                \
    scope bool qt_##name##_init(qt(name) *qt,                                                   \
                                float xmin, float xmax,                                         \
                                float ymin, float ymax);                                        \
    scope void qt_##name##_destroy(qt(name) *qt);                                               \
    scope void qt_##name##_clear(qt(name) *qt);                                                 \
    scope bool qt_##name##_insert(qt(name) *qt, float x, float y, type record);                 \
    scope bool qt_##name##_delete(qt(name) *qt, float x, float y, type record);                 \
    scope bool qt_##name##_delete_all(qt(name) *qt, float x, float y);                          \
    scope bool qt_##name##_find(qt(name) *qt, float x, float y, type *out, int maxout);         \
    scope bool qt_##name##_contains(qt(name) *qt, float x, float y);                            \
    scope int  qt_##name##_inrange_circle(qt(name) *qt,                                         \
                                          float x, float y, float range,                        \
                                          type *out, int maxout);                               \
    scope int  qt_##name##_inrange_rect(qt(name) *qt,                                           \
                                           float minx, float maxx,                              \
                                           float miny, float maxy,                              \
                                           type *out, int maxout);                              \
    scope void qt_##name##_print(qt(name) *qt);                                                 \
    scope bool qt_##name##_reserve(qt(name) *qt, size_t size);

/***********************************************************************************************/

#define QUADTREE_IMPL(scope, name, type)                                                        \
                                                                                                \
    MPOOL_IMPL(static, name, qt_node(name))                                                     \
                                                                                                \
    static bool _qt_##name##_node_isleaf(qt_node(name) *node)                                   \
    {                                                                                           \
        return (0 == node->nw                                                                   \
            &&  0 == node->ne                                                                   \
            &&  0 == node->sw                                                                   \
            &&  0 == node->se);                                                                 \
    }                                                                                           \
                                                                                                \
    static void _qt_##name##_node_init(qt_node(name) *node, int depth)                          \
    {                                                                                           \
        node->depth = depth;                                                                    \
        node->nw = node->ne = node->sw = node->se = 0;                                          \
        node->has_record = false;                                                               \
        node->parent = 0;                                                                       \
        node->sibling_next = 0;                                                                 \
    }                                                                                           \
                                                                                                \
    static int _qt_##name##_node_nsibs(qt(name) *qt, qt_node(name) *node)                       \
    {                                                                                           \
        int ret = 0;                                                                            \
        mp_ref_t curr = node->sibling_next;                                                     \
        while(curr) {                                                                           \
            qt_node(name) *curr_node = mp_##name##_entry(&qt->node_pool, curr);                 \
            curr = curr_node->sibling_next;                                                     \
            ++ret;                                                                              \
        }                                                                                       \
        return ret;                                                                             \
    }                                                                                           \
                                                                                                \
    static void _qt_##name##_node_print(qt(name) *qt, mp_ref_t ref, int indent)                 \
    {                                                                                           \
        qt_node(name) *node = mp_##name##_entry(&qt->node_pool, ref);                           \
        for(int i = 0; i < indent; i++)                                                         \
            printf("  ");                                                                       \
        if(indent)                                                                              \
            printf("|-> ");                                                                     \
                                                                                                \
        printf("[%12.6f, %12.6f]", node->x, node->y);                                           \
        if(node->has_record)                                                                    \
            printf(" (has record) (%d siblings)", _qt_##name##_node_nsibs(qt, node));           \
        printf("\n");                                                                           \
                                                                                                \
        if(_qt_##name##_node_isleaf(node))                                                      \
            return;                                                                             \
                                                                                                \
        _qt_##name##_node_print(qt, node->nw, indent + 1);                                      \
        _qt_##name##_node_print(qt, node->ne, indent + 1);                                      \
        _qt_##name##_node_print(qt, node->sw, indent + 1);                                      \
        _qt_##name##_node_print(qt, node->se, indent + 1);                                      \
    }                                                                                           \
                                                                                                \
    static void _qt_##name##_node_bounds(qt(name) *qt, mp_ref_t ref,                            \
                                         float *out_xmin, float *out_xmax,                      \
                                         float *out_ymin, float *out_ymax)                      \
    {                                                                                           \
        qt_node(name) *node = mp_##name##_entry(&qt->node_pool, ref);                           \
                                                                                                \
        assert(qt->root > 0);                                                                   \
        if(!node->parent) {                                                                     \
                                                                                                \
            assert(node->depth == 0);                                                           \
            *out_xmin = qt->xmin;                                                               \
            *out_xmax = qt->xmax;                                                               \
            *out_ymin = qt->ymin;                                                               \
            *out_ymax = qt->ymax;                                                               \
            return;                                                                             \
        }                                                                                       \
                                                                                                \
        qt_node(name) *parent = mp_##name##_entry(&qt->node_pool, node->parent);                \
        float pxmin, pxmax, pymin, pymax;                                                       \
        _qt_##name##_node_bounds(qt, node->parent, &pxmin, &pxmax, &pymin, &pymax);             \
                                                                                                \
        if(parent->nw == ref) {                                                                 \
            *out_xmin = pxmin;                                                                  \
            *out_xmax = (pxmax + pxmin) / 2.0f;                                                 \
            *out_ymin = (pymax + pymin) / 2.0f;                                                 \
            *out_ymax = pymax;                                                                  \
        }else if(parent->ne == ref) {                                                           \
            *out_xmin = (pxmax + pxmin) / 2.0f;                                                 \
            *out_xmax = pxmax;                                                                  \
            *out_ymin = (pymax + pymin) / 2.0f;                                                 \
            *out_ymax = pymax;                                                                  \
        }else if(parent->sw == ref) {                                                           \
            *out_xmin = pxmin;                                                                  \
            *out_xmax = (pxmax + pxmin) / 2.0f;                                                 \
            *out_ymin = pymin;                                                                  \
            *out_ymax = (pymax + pymin) / 2.0f;                                                 \
        }else if(parent->se == ref) {                                                           \
            *out_xmin = (pxmax + pxmin) / 2.0f;                                                 \
            *out_xmax = pxmax;                                                                  \
            *out_ymin = pymin;                                                                  \
            *out_ymax = (pymax + pymin) / 2.0f;                                                 \
        }else                                                                                   \
            assert(0);                                                                          \
    }                                                                                           \
                                                                                                \
    static void _qt_##name##_extend_bounds(float *inout_xmin, float *inout_xmax,                \
                                           float *inout_ymin, float *inout_ymax,                \
                                           float border)                                        \
    {                                                                                           \
        *inout_xmin -= border;                                                                  \
        *inout_xmax += border;                                                                  \
        *inout_ymin -= border;                                                                  \
        *inout_ymax += border;                                                                  \
    }                                                                                           \
                                                                                                \
    static bool _qt_##name##_point_in_bounds(float xmin, float xmax,                            \
                                             float ymin, float ymax,                            \
                                             float x, float y)                                  \
    {                                                                                           \
        if(x < xmin || x > xmax)                                                                \
            return false;                                                                       \
        if(y < ymin || y > ymax)                                                                \
            return false;                                                                       \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    static bool _qt_##name##_bounds_intersect(float xmin1, float xmax1,                         \
                                              float ymin1, float ymax1,                         \
                                              float xmin2, float xmax2,                         \
                                              float ymin2, float ymax2)                         \
    {                                                                                           \
        if(xmin1 >= xmin2 && xmin1 <= xmax2)                                                    \
            return true;                                                                        \
        if(xmax1 >= xmin2 && xmax1 <= xmax2)                                                    \
            return true;                                                                        \
        if(ymin1 >= ymin2 && ymin1 <= ymax2)                                                    \
            return true;                                                                        \
        if(ymax1 >= ymin2 && ymax1 <= ymax2)                                                    \
            return true;                                                                        \
        return false;                                                                           \
    }                                                                                           \
                                                                                                \
    static float _qt_##name##_dist(float x1, float y1, float x2, float y2)                      \
    {                                                                                           \
        return sqrt(pow(x2-x1, 2) + pow(y2-y1, 2));                                             \
    }                                                                                           \
                                                                                                \
    static int _qt_##name##_node_inrange_circle(qt(name)*, qt_node(name)*,                      \
                                         float, float, float, type*, int*);                     \
                                                                                                \
    static int _qt_##name##_node_add_circle(qt(name) *qt, mp_ref_t ref,                         \
                                            float x, float y, float range,                      \
                                            type *out, int *inout_maxout)                       \
    {                                                                                           \
        float xmin, xmax, ymin, ymax;                                                           \
                                                                                                \
        qt_node(name) *curr = mp_##name##_entry(&qt->node_pool, ref);                       \
        _qt_##name##_node_bounds(qt, ref, &xmin, &xmax, &ymin, &ymax);                          \
                                                                                                \
        _qt_##name##_extend_bounds(&xmin, &xmax, &ymin, &ymax, range);                          \
                                                                                                \
        if(_qt_##name##_point_in_bounds(xmin, xmax, ymin, ymax, x, y)) {                        \
                                                                                                \
            qt_node(name) *curr = mp_##name##_entry(&qt->node_pool, ref);                       \
            return _qt_##name##_node_inrange_circle(qt, curr, x, y, range, out, inout_maxout);  \
        }                                                                                       \
        return 0;                                                                               \
    }                                                                                           \
                                                                                                \
    static int _qt_##name##_node_inrange_circle(qt(name) *qt, qt_node(name) *root,              \
                                        float x, float y, float range,                          \
                                        type *out, int *inout_maxout)                           \
    {                                                                                           \
        if(!*inout_maxout)                                                                      \
            return 0;                                                                           \
                                                                                                \
        int orig_maxout = *inout_maxout;                                                        \
        if(_qt_##name##_node_isleaf(root)) {                                                    \
                                                                                                \
            if(root->has_record) {                                                              \
                                                                                                \
                *out++ = root->record;                                                          \
                *inout_maxout -= 1;                                                             \
                                                                                                \
                while(root->sibling_next && *inout_maxout) {                                    \
                    root = mp_##name##_entry(&qt->node_pool, root->sibling_next);               \
                    *out++ = root->record;                                                      \
                    *inout_maxout -= 1;                                                         \
                }                                                                               \
                return (orig_maxout - *inout_maxout);                                           \
            }                                                                                   \
            return 0;                                                                           \
        }                                                                                       \
                                                                                                \
        assert(!_qt_##name##_node_isleaf(root));                                                \
        out += _qt_##name##_node_add_circle(qt, root->nw, x, y, range, out, inout_maxout);      \
        out += _qt_##name##_node_add_circle(qt, root->ne, x, y, range, out, inout_maxout);      \
        out += _qt_##name##_node_add_circle(qt, root->sw, x, y, range, out, inout_maxout);      \
        out += _qt_##name##_node_add_circle(qt, root->se, x, y, range, out, inout_maxout);      \
                                                                                                \
        assert(orig_maxout >= *inout_maxout);                                                   \
        return (orig_maxout - *inout_maxout);                                                   \
    }                                                                                           \
                                                                                                \
    static int _qt_##name##_node_inrange_rect(qt(name)*, qt_node(name)*,                        \
                                              float, float, float, float, type*, int*);         \
                                                                                                \
    static int _qt_##name##_node_add_rect(qt(name) *qt, mp_ref_t ref,                           \
                                          float axmin, float axmax, float aymin, float aymax,   \
                                          type *out, int *inout_maxout)                         \
    {                                                                                           \
        float xmin, xmax, ymin, ymax;                                                           \
        _qt_##name##_node_bounds(qt, ref, &xmin, &xmax, &ymin, &ymax);                          \
                                                                                                \
        if(_qt_##name##_bounds_intersect(xmin,  xmax,  ymin,  ymax,                             \
                                         axmin, axmax, aymin, aymax)) {                         \
                                                                                                \
            qt_node(name) *curr = mp_##name##_entry(&qt->node_pool, ref);                       \
            return _qt_##name##_node_inrange_rect(qt, curr, axmin, axmax, aymin, aymax,         \
                out, inout_maxout);                                                             \
        }                                                                                       \
        return 0;                                                                               \
    }                                                                                           \
                                                                                                \
    static int _qt_##name##_node_inrange_rect(qt(name) *qt, qt_node(name) *root,                \
                                        float xmin, float xmax, float ymin, float ymax,         \
                                        type *out, int *inout_maxout)                           \
    {                                                                                           \
        if(!*inout_maxout)                                                                      \
            return 0;                                                                           \
                                                                                                \
        int orig_maxout = *inout_maxout;                                                        \
        if(_qt_##name##_node_isleaf(root)) {                                                    \
                                                                                                \
            if(root->has_record                                                                 \
            && _qt_##name##_point_in_bounds(xmin, xmax, ymin, ymax, root->x, root->y)) {        \
                                                                                                \
                *out++ = root->record;                                                          \
                *inout_maxout -= 1;                                                             \
                                                                                                \
                while(root->sibling_next && *inout_maxout) {                                    \
                    root = mp_##name##_entry(&qt->node_pool, root->sibling_next);               \
                    *out++ = root->record;                                                      \
                    *inout_maxout -= 1;                                                         \
                }                                                                               \
                return (orig_maxout - *inout_maxout);                                           \
            }                                                                                   \
            return 0;                                                                           \
        }                                                                                       \
                                                                                                \
        assert(!_qt_##name##_node_isleaf(root));                                                \
        out += _qt_##name##_node_add_rect(qt, root->nw, xmin, xmax, ymin, ymax, out, inout_maxout); \
        out += _qt_##name##_node_add_rect(qt, root->ne, xmin, xmax, ymin, ymax, out, inout_maxout); \
        out += _qt_##name##_node_add_rect(qt, root->sw, xmin, xmax, ymin, ymax, out, inout_maxout); \
        out += _qt_##name##_node_add_rect(qt, root->se, xmin, xmax, ymin, ymax, out, inout_maxout); \
                                                                                                \
        assert(orig_maxout >= *inout_maxout);                                                   \
        return (orig_maxout - *inout_maxout);                                                   \
    }                                                                                           \
                                                                                                \
    static bool _qt_##name##_node_sib_append(qt(name) *qt, mp_ref_t ref, type record)           \
    {                                                                                           \
        qt_node(name) *node = mp_##name##_entry(&qt->node_pool, ref);                           \
                                                                                                \
        mp_ref_t sib = mp_##name##_alloc(&qt->node_pool);                                       \
        _CHK_TRUE_RET(sib, false);                                                              \
                                                                                                \
        qt_node(name) *sib_node = mp_##name##_entry(&qt->node_pool, sib);                       \
        _qt_##name##_node_init(sib_node, node->depth);                                          \
        sib_node->x = node->x;                                                                  \
        sib_node->y = node->y;                                                                  \
        sib_node->parent = node->parent;                                                        \
                                                                                                \
        mp_ref_t curr = node->sibling_next;                                                     \
        mp_ref_t prev = ref;                                                                    \
                                                                                                \
        while(curr) {                                                                           \
            node = mp_##name##_entry(&qt->node_pool, curr);                                     \
            prev = curr;                                                                        \
            curr = node->sibling_next;                                                          \
        }                                                                                       \
                                                                                                \
        node = mp_##name##_entry(&qt->node_pool, prev);                                         \
        assert(!node->sibling_next);                                                            \
                                                                                                \
        sib_node->record = record;                                                              \
        sib_node->has_record = true;                                                            \
        node->sibling_next = sib;                                                               \
        ++qt->nrecs;                                                                            \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    static bool _qt_##name##_delete_sib(qt(name) *qt, mp_ref_t head,                            \
                                        float x, float y, type record)                          \
    {                                                                                           \
        qt_node(name) *head_node = mp_##name##_entry(&qt->node_pool, head);                     \
        qt_node(name) *curr_node, *prev_node;                                                   \
        if(!head_node->sibling_next)                                                            \
            return false;                                                                       \
                                                                                                \
        mp_ref_t curr = head_node->sibling_next;                                                \
        mp_ref_t prev = head;                                                                   \
                                                                                                \
        while(curr) {                                                                           \
            curr_node = mp_##name##_entry(&qt->node_pool, curr);                                \
            prev_node = mp_##name##_entry(&qt->node_pool, prev);                                \
                                                                                                \
            if(0 == memcmp(&record, &curr_node->record, sizeof(record))) {                      \
                prev_node->sibling_next = curr_node->sibling_next;                              \
                mp_##name##_free(&qt->node_pool, curr);                                         \
                --qt->nrecs;                                                                    \
                return true;                                                                    \
            }                                                                                   \
            prev = curr;                                                                        \
            curr = curr_node->sibling_next;                                                     \
        }                                                                                       \
        return false;                                                                           \
    }                                                                                           \
                                                                                                \
    static mp_ref_t _qt_##name##_quadrant(qt_node(name) *node, float x, float y)                \
    {                                                                                           \
        if(x <= node->x && y >= node->y)                                                        \
            return node->nw;                                                                    \
        if(x >= node->x && y >= node->y)                                                        \
            return node->ne;                                                                    \
        if(x <= node->x && y <= node->y)                                                        \
            return node->sw;                                                                    \
        if(x >= node->x && y <= node->y)                                                        \
            return node->se;                                                                    \
        assert(0);                                                                              \
        return 0;                                                                               \
    }                                                                                           \
                                                                                                \
    scope mp_ref_t _qt_##name##_find_leaf(qt(name) *qt, float x, float y)                       \
    {                                                                                           \
        if(qt->root == 0)                                                                       \
            return 0;                                                                           \
                                                                                                \
        assert(qt->root > 0);                                                                   \
        mp_ref_t curr_ref = qt->root;                                                           \
        qt_node(name) *curr_node = mp_##name##_entry(&qt->node_pool, curr_ref);                 \
                                                                                                \
        while(!_qt_##name##_node_isleaf(curr_node)) {                                           \
                                                                                                \
            assert(curr_node->nw && curr_node->ne                                               \
                && curr_node->sw && curr_node->se);                                             \
                                                                                                \
            curr_ref = _qt_##name##_quadrant(curr_node, x, y);                                  \
            curr_node = mp_##name##_entry(&qt->node_pool, curr_ref);                            \
        }                                                                                       \
        assert(_qt_##name##_node_isleaf(curr_node));                                            \
        return curr_ref;                                                                        \
    }                                                                                           \
                                                                                                \
    static void _qt_##name##_set_divide_coords(qt(name) *qt,                                    \
                                               qt_node(name) *parent, mp_ref_t child_ref)       \
    {                                                                                           \
        assert(child_ref > 0);                                                                  \
        qt_node(name) *child = mp_##name##_entry(&qt->node_pool, child_ref);                    \
                                                                                                \
        const float x_len = qt->xmax - qt->xmin;                                                \
        const float y_len = qt->ymax - qt->ymin;                                                \
        assert(x_len && y_len);                                                                 \
        const float region_x_len = x_len * 1.0f/pow(2, child->depth + 1);                       \
        const float region_y_len = y_len * 1.0f/pow(2, child->depth + 1);                       \
                                                                                                \
        if(parent->nw == child_ref) {                                                           \
            child->x = parent->x - region_x_len;                                                \
            child->y = parent->y + region_y_len;                                                \
        }else if(parent->ne == child_ref) {                                                     \
            child->x = parent->x + region_x_len;                                                \
            child->y = parent->y + region_y_len;                                                \
        }else if(parent->sw == child_ref) {                                                     \
            child->x = parent->x - region_x_len;                                                \
            child->y = parent->y - region_y_len;                                                \
        }else if(parent->se == child_ref) {                                                     \
            child->x = parent->x + region_x_len;                                                \
            child->y = parent->y - region_y_len;                                                \
        }else                                                                                   \
            assert(0);                                                                          \
    }                                                                                           \
                                                                                                \
    static bool _qt_##name##_partition(qt(name) *qt, mp_ref_t ref)                              \
    {                                                                                           \
        qt_node(name) *node = mp_##name##_entry(&qt->node_pool, ref);                           \
        qt_node(name) *curr = NULL;                                                             \
                                                                                                \
        assert(_qt_##name##_node_isleaf(node));                                                 \
        assert(node->has_record);                                                               \
                                                                                                \
        float saved_x = node->x;                                                                \
        float saved_y = node->y;                                                                \
        type saved_record = node->record;                                                       \
        mp_ref_t saved_sibnext = node->sibling_next;                                            \
                                                                                                \
        node->sibling_next = 0;                                                                 \
        node->has_record = false;                                                               \
                                                                                                \
        if(!node->parent) {                                                                     \
                                                                                                \
            assert(node->depth == 0);                                                           \
            node->x = 0.0f;                                                                     \
            node->y = 0.0f;                                                                     \
        }else{                                                                                  \
                                                                                                \
            qt_node(name) *parent = mp_##name##_entry(&qt->node_pool, node->parent);            \
            _qt_##name##_set_divide_coords(qt, parent, ref);                                    \
        }                                                                                       \
                                                                                                \
        _CHK_TRUE_JMP((node->nw = mp_##name##_alloc(&qt->node_pool)), fail);                    \
        _CHK_TRUE_JMP((node->ne = mp_##name##_alloc(&qt->node_pool)), fail);                    \
        _CHK_TRUE_JMP((node->sw = mp_##name##_alloc(&qt->node_pool)), fail);                    \
        _CHK_TRUE_JMP((node->se = mp_##name##_alloc(&qt->node_pool)), fail);                    \
                                                                                                \
        /* NW node */                                                                           \
        curr = mp_##name##_entry(&qt->node_pool, node->nw);                                     \
        _qt_##name##_node_init(curr, node->depth + 1);                                          \
        _qt_##name##_set_divide_coords(qt, node, node->nw);                                     \
        curr->parent = ref;                                                                     \
                                                                                                \
        /* NE node */                                                                           \
        curr = mp_##name##_entry(&qt->node_pool, node->ne);                                     \
        _qt_##name##_node_init(curr, node->depth + 1);                                          \
        _qt_##name##_set_divide_coords(qt, node, node->ne);                                     \
        curr->parent = ref;                                                                     \
                                                                                                \
        /* SW node */                                                                           \
        curr = mp_##name##_entry(&qt->node_pool, node->sw);                                     \
        _qt_##name##_node_init(curr, node->depth + 1);                                          \
        _qt_##name##_set_divide_coords(qt, node, node->sw);                                     \
        curr->parent = ref;                                                                     \
                                                                                                \
        /* SE node */                                                                           \
        curr = mp_##name##_entry(&qt->node_pool, node->se);                                     \
        _qt_##name##_node_init(curr, node->depth + 1);                                          \
        _qt_##name##_set_divide_coords(qt, node, node->se);                                     \
        curr->parent = ref;                                                                     \
                                                                                                \
        /* Set the record for in one of the quadrants */                                        \
        mp_ref_t rec_ref = _qt_##name##_quadrant(node, saved_x, saved_y);                       \
        qt_node(name) *rec_node = mp_##name##_entry(&qt->node_pool, rec_ref);                   \
        rec_node->x = saved_x;                                                                  \
        rec_node->y = saved_y;                                                                  \
        rec_node->record = saved_record;                                                        \
        rec_node->has_record = true;                                                            \
        rec_node->sibling_next = saved_sibnext;                                                 \
                                                                                                \
        mp_ref_t sib = rec_node->sibling_next;                                                  \
        while(sib) {                                                                            \
            qt_node(name) *sib_node = mp_##name##_entry(&qt->node_pool, sib);                   \
            sib_node->depth = rec_node->depth;                                                  \
            sib_node->parent = rec_node->parent;                                                \
            sib = sib_node->sibling_next;                                                       \
        }                                                                                       \
                                                                                                \
        return true;                                                                            \
                                                                                                \
    fail:                                                                                       \
        node->nw ? mp_##name##_free(&qt->node_pool, node->nw), 0 : 0;                           \
        node->ne ? mp_##name##_free(&qt->node_pool, node->ne), 0 : 0;                           \
        node->sw ? mp_##name##_free(&qt->node_pool, node->sw), 0 : 0;                           \
        node->se ? mp_##name##_free(&qt->node_pool, node->se), 0 : 0;                           \
        return false;                                                                           \
    }                                                                                           \
                                                                                                \
    static bool _qt_##name##_rec_node(qt(name) *qt, mp_ref_t ref)                               \
    {                                                                                           \
        qt_node(name) *node = mp_##name##_entry(&qt->node_pool, ref);                           \
        if(!_qt_##name##_node_isleaf(node))                                                     \
            return true;                                                                        \
        return (node->has_record);                                                              \
    }                                                                                           \
                                                                                                \
    static bool _qt_##name##_merge(qt(name) *qt, mp_ref_t ref)                                  \
    {                                                                                           \
        assert(ref > 0);                                                                        \
        qt_node(name) *node = mp_##name##_entry(&qt->node_pool, ref);                           \
        assert(!_qt_##name##_node_isleaf(node));                                                \
                                                                                                \
        int nrecs = !!_qt_##name##_rec_node(qt, node->nw)                                       \
                  + !!_qt_##name##_rec_node(qt, node->ne)                                       \
                  + !!_qt_##name##_rec_node(qt, node->sw)                                       \
                  + !!_qt_##name##_rec_node(qt, node->se);                                      \
                                                                                                \
        assert(nrecs > 0);                                                                      \
        if(nrecs > 1)                                                                           \
            return false;                                                                       \
                                                                                                \
        mp_ref_t rec = _qt_##name##_rec_node(qt, node->nw) ? node->nw                           \
                     : _qt_##name##_rec_node(qt, node->ne) ? node->ne                           \
                     : _qt_##name##_rec_node(qt, node->sw) ? node->sw                           \
                     : _qt_##name##_rec_node(qt, node->se) ? node->se                           \
                     : (assert(0), 0);                                                          \
                                                                                                \
        qt_node(name) *rec_node = mp_##name##_entry(&qt->node_pool, rec);                       \
        if(!_qt_##name##_node_isleaf(rec_node))                                                 \
            return false;                                                                       \
                                                                                                \
        mp_##name##_free(&qt->node_pool, node->nw);                                             \
        mp_##name##_free(&qt->node_pool, node->ne);                                             \
        mp_##name##_free(&qt->node_pool, node->sw);                                             \
        mp_##name##_free(&qt->node_pool, node->se);                                             \
                                                                                                \
        node->nw = node->ne = node->sw = node->se = 0;                                          \
        node->has_record = true;                                                                \
        node->record = rec_node->record;                                                        \
        node->x = mp_##name##_entry(&qt->node_pool, rec)->x;                                    \
        node->y = mp_##name##_entry(&qt->node_pool, rec)->y;                                    \
        node->sibling_next = rec_node->sibling_next;                                            \
                                                                                                \
        mp_ref_t curr = node->sibling_next;                                                     \
        while(curr) {                                                                           \
            qt_node(name) *curr_sib_node = mp_##name##_entry(&qt->node_pool, curr);             \
            curr_sib_node->depth = node->depth;                                                 \
            curr_sib_node->parent = node->parent;                                               \
            curr = curr_sib_node->sibling_next;                                                 \
        }                                                                                       \
                                                                                                \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    static void _qt_##name##_update_ref(qt_node(name) *node, mp_ref_t oldref, mp_ref_t newref)  \
    {                                                                                           \
        if(node->nw == oldref) node->nw = newref;                                               \
        if(node->ne == oldref) node->ne = newref;                                               \
        if(node->sw == oldref) node->sw = newref;                                               \
        if(node->se == oldref) node->se = newref;                                               \
    }                                                                                           \
                                                                                                \
    static void _qt_##name##_node_check_consistent(qt(name) *qt, mp_ref_t ref)                  \
    {                                                                                           \
        qt_node(name) *node = mp_##name##_entry(&qt->node_pool, ref);                           \
        float xmin, xmax, ymin, ymax;                                                           \
        _qt_##name##_node_bounds(qt, ref, &xmin, &xmax, &ymin, &ymax);                          \
                                                                                                \
        if(!(xmin <= node->x)                                                                   \
        || !(xmax >= node->x)                                                                   \
        || !(ymin <= node->y)                                                                   \
        || !(ymax >= node->y)) {                                                                \
            printf("Inconsistent Quadtree: [node (%f, %f) misplaced]\n", node->x, node->y);     \
            qt_##name##_print(qt);                                                              \
            fflush(stdout);                                                                     \
        }                                                                                       \
                                                                                                \
        assert(xmin <= node->x);                                                                \
        assert(xmax >= node->x);                                                                \
        assert(ymin <= node->y);                                                                \
        assert(ymax >= node->y);                                                                \
                                                                                                \
        mp_ref_t sib = node->sibling_next;                                                      \
        while(sib) {                                                                            \
            qt_node(name) *sib_node = mp_##name##_entry(&qt->node_pool, sib);                   \
            if(sib_node->depth != node->depth || sib_node->parent != node->parent) {            \
                printf("Mismatching sibling depths/parents for node (%f, %f) [%d] and its' sibling (%f, %f) [%d]\n", \
                    node->x, node->y, (int)node->depth, sib_node->x, sib_node->y, (int)sib_node->depth); \
                qt_##name##_print(qt);                                                          \
                fflush(stdout);                                                                 \
            }                                                                                   \
            assert(sib_node->depth == node->depth);                                             \
            assert(sib_node->parent == node->parent);                                           \
            sib = sib_node->sibling_next;                                                       \
        }                                                                                       \
                                                                                                \
        if(node->parent) {                                                                      \
            qt_node(name) *parent = mp_##name##_entry(&qt->node_pool, node->parent);            \
            if(node->depth - parent->depth != 1) {                                              \
                printf("Mismatching depths for node (%f, %f) [%d] and its' parent (%f, %f) [%d]\n", \
                    node->x, node->y, (int)node->depth, parent->x, parent->y, (int)parent->depth); \
                qt_##name##_print(qt);                                                          \
                fflush(stdout);                                                                 \
            }                                                                                   \
            assert(node->depth - parent->depth == 1);                                           \
        }                                                                                       \
                                                                                                \
        if(_qt_##name##_node_isleaf(node))                                                      \
            return;                                                                             \
                                                                                                \
        _qt_##name##_node_check_consistent(qt, node->nw);                                       \
        _qt_##name##_node_check_consistent(qt, node->ne);                                       \
        _qt_##name##_node_check_consistent(qt, node->sw);                                       \
        _qt_##name##_node_check_consistent(qt, node->se);                                       \
    }                                                                                           \
                                                                                                \
    static void _qt_##name##_check_consistent(qt(name) *qt)                                     \
    {                                                                                           \
        if(!qt->root)                                                                           \
            return;                                                                             \
        _qt_##name##_node_check_consistent(qt, qt->root);                                       \
    }                                                                                           \
                                                                                                \
    scope bool qt_##name##_init(qt(name) *qt,                                                   \
                                float xmin, float xmax,                                         \
                                float ymin, float ymax)                                         \
    {                                                                                           \
        mp_##name##_init(&qt->node_pool);                                                       \
        qt->root = 0;                                                                           \
        qt->nrecs = 0;                                                                          \
        qt->xmin = xmin;                                                                        \
        qt->xmax = xmax;                                                                        \
        qt->ymin = ymin;                                                                        \
        qt->ymax = ymax;                                                                        \
        return 0;                                                                               \
    }                                                                                           \
                                                                                                \
    scope void qt_##name##_destroy(qt(name) *qt)                                                \
    {                                                                                           \
        qt_##name##_clear(qt);                                                                  \
        mp_##name##_destroy(&qt->node_pool);                                                    \
        memset(qt, 0, sizeof(*qt));                                                             \
    }                                                                                           \
                                                                                                \
    scope void qt_##name##_clear(qt(name) *qt)                                                  \
    {                                                                                           \
        mp_##name##_clear(&qt->node_pool);                                                      \
        qt->root = 0;                                                                           \
        qt->nrecs = 0;                                                                          \
    }                                                                                           \
                                                                                                \
    scope bool qt_##name##_insert(qt(name) *qt, float x, float y, type record)                  \
    {                                                                                           \
        if(qt->root == 0) {                                                                     \
                                                                                                \
            assert(qt->nrecs == 0);                                                             \
                                                                                                \
            _CHK_TRUE_RET((qt->root = mp_##name##_alloc(&qt->node_pool)), false);               \
            qt_node(name) *node = mp_##name##_entry(&qt->node_pool, qt->root);                  \
            _qt_##name##_node_init(node, 0);                                                    \
                                                                                                \
            node->x = x;                                                                        \
            node->y = y;                                                                        \
            node->has_record = true;                                                            \
            node->record = record;                                                              \
            assert(_qt_##name##_node_isleaf(node));                                             \
                                                                                                \
            qt->nrecs++;                                                                        \
            return true;                                                                        \
        }                                                                                       \
                                                                                                \
        assert(qt->root > 0);                                                                   \
        mp_ref_t curr_ref = _qt_##name##_find_leaf(qt, x, y);                                   \
        qt_node(name) *curr_node = mp_##name##_entry(&qt->node_pool, curr_ref);                 \
        assert(_qt_##name##_node_isleaf(curr_node));                                            \
                                                                                                \
        /* If the leaf node has no record, then it is the region containing the new node.*/     \
        if(!curr_node->has_record) {                                                            \
                                                                                                \
            curr_node->x = x;                                                                   \
            curr_node->y = y;                                                                   \
            curr_node->has_record = true;                                                       \
            curr_node->record = record;                                                         \
            qt->nrecs++;                                                                        \
            return true;                                                                        \
        }                                                                                       \
        assert(curr_node->has_record);                                                          \
                                                                                                \
        if(QT_EQ(x, curr_node->x) && QT_EQ(y, curr_node->y))                                    \
            return _qt_##name##_node_sib_append(qt, curr_ref, record);                          \
                                                                                                \
        /* 'curr_node' is the closest leaf node. Keep partitioning it until the */              \
        /* existing point and the new point lie in different quadrants */                       \
        do{                                                                                     \
            _CHK_TRUE_RET(_qt_##name##_partition(qt, curr_ref), false);                         \
            assert(!curr_node->has_record);                                                     \
                                                                                                \
            curr_ref = _qt_##name##_quadrant(curr_node, x, y);                                  \
            assert(curr_ref > 0);                                                               \
            curr_node = mp_##name##_entry(&qt->node_pool, curr_ref);                            \
                                                                                                \
        }while(curr_node->has_record);                                                          \
                                                                                                \
        /* curr_node is now a record-less leaf node. Set its' record. */                        \
        curr_node->record = record;                                                             \
        curr_node->has_record = true;                                                           \
        curr_node->x = x;                                                                       \
        curr_node->y = y;                                                                       \
                                                                                                \
        qt->nrecs++;                                                                            \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool qt_##name##_delete(qt(name) *qt, float x, float y, type record)                  \
    {                                                                                           \
        mp_ref_t curr_ref = _qt_##name##_find_leaf(qt, x, y);                                   \
        if(!curr_ref)                                                                           \
            return false;                                                                       \
                                                                                                \
        qt_node(name) *curr_node = mp_##name##_entry(&qt->node_pool, curr_ref);                 \
        if(!curr_node->has_record)                                                              \
            return false;                                                                       \
        if(!QT_EQ(curr_node->x, x) || !QT_EQ(curr_node->y, y))                                  \
            return false;                                                                       \
                                                                                                \
        /* The record isn't in the head */                                                      \
        if(0 != memcmp(&record, &curr_node->record, sizeof(record)))                            \
            return _qt_##name##_delete_sib(qt, curr_ref, x, y, record);                         \
                                                                                                \
        --qt->nrecs;                                                                            \
                                                                                                \
        /* If deleted node has siblings, make the next sibling the head */                      \
        if(curr_node->sibling_next) {                                                           \
                                                                                                \
            mp_ref_t new_head = curr_node->sibling_next;                                        \
            qt_node(name) *new_head_node = mp_##name##_entry(&qt->node_pool, new_head);         \
            assert(new_head_node->has_record);                                                  \
            assert(_qt_##name##_node_isleaf(new_head_node));                                    \
                                                                                                \
            new_head_node->nw = curr_node->nw;                                                  \
            new_head_node->ne = curr_node->ne;                                                  \
            new_head_node->sw = curr_node->sw;                                                  \
            new_head_node->se = curr_node->se;                                                  \
            new_head_node->parent = curr_node->parent;                                          \
                                                                                                \
            mp_##name##_free(&qt->node_pool, curr_ref);                                         \
                                                                                                \
            if(!curr_node->parent) {                                                            \
                assert(qt->root == curr_ref);                                                   \
                qt->root = new_head;                                                            \
                return true;                                                                    \
            }                                                                                   \
                                                                                                \
            qt_node(name) *parent = mp_##name##_entry(&qt->node_pool, curr_node->parent);       \
            _qt_##name##_update_ref(parent, curr_ref, new_head);                                \
            return true;                                                                        \
        }                                                                                       \
                                                                                                \
        if(!curr_node->parent) {                                                                \
            mp_##name##_free(&qt->node_pool, curr_ref);                                         \
            assert(qt->root == curr_ref);                                                       \
            qt->root = 0;                                                                       \
            return true;                                                                        \
        }                                                                                       \
                                                                                                \
        qt_node(name) *parent = mp_##name##_entry(&qt->node_pool, curr_node->parent);           \
        _qt_##name##_set_divide_coords(qt, parent, curr_ref);                                   \
        curr_node->has_record = false;                                                          \
                                                                                                \
        do {                                                                                    \
            curr_ref = curr_node->parent;                                                       \
            if(!curr_ref)                                                                       \
                break;                                                                          \
            curr_node = mp_##name##_entry(&qt->node_pool, curr_ref);                            \
                                                                                                \
        }while(_qt_##name##_merge(qt, curr_ref));                                               \
                                                                                                \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool qt_##name##_delete_all(qt(name) *qt, float x, float y)                           \
    {                                                                                           \
        mp_ref_t curr_ref = _qt_##name##_find_leaf(qt, x, y);                                   \
        if(!curr_ref)                                                                           \
            return false;                                                                       \
                                                                                                \
        qt_node(name) *curr_node = mp_##name##_entry(&qt->node_pool, curr_ref);                 \
        if(!curr_node->has_record)                                                              \
            return false;                                                                       \
        if(!QT_EQ(curr_node->x, x) || !QT_EQ(curr_node->y, y))                                  \
            return false;                                                                       \
                                                                                                \
        if(!curr_node->sibling_next)                                                            \
            return qt_##name##_delete(qt, x, y, curr_node->record);                             \
                                                                                                \
        mp_ref_t curr_sib = curr_node->sibling_next;                                            \
        while(curr_sib) {                                                                       \
            qt_node(name) *curr_sib_node = mp_##name##_entry(&qt->node_pool, curr_sib);         \
            bool ret = _qt_##name##_delete_sib(qt, curr_ref, x, y, curr_sib_node->record);      \
            (void)ret;                                                                          \
            assert(ret);                                                                        \
            curr_sib = curr_sib_node->sibling_next;                                             \
        }                                                                                       \
                                                                                                \
        assert(!curr_node->sibling_next);                                                       \
        return qt_##name##_delete(qt, x, y, curr_node->record);                                 \
    }                                                                                           \
                                                                                                \
    scope bool qt_##name##_find(qt(name) *qt, float x, float y, type *out, int maxout)          \
    {                                                                                           \
        mp_ref_t curr_ref = _qt_##name##_find_leaf(qt, x, y);                                   \
        if(!curr_ref)                                                                           \
            return false;                                                                       \
                                                                                                \
        qt_node(name) *curr_node = mp_##name##_entry(&qt->node_pool, curr_ref);                 \
        if(!curr_node->has_record)                                                              \
            return false;                                                                       \
        if(!QT_EQ(curr_node->x, x) || !QT_EQ(curr_node->y, y))                                  \
            return false;                                                                       \
                                                                                                \
        *out = curr_node->record;                                                               \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool qt_##name##_contains(qt(name) *qt, float x, float y)                             \
    {                                                                                           \
        type dummy;                                                                             \
        return qt_##name##_find(qt, x, y, &dummy, 1);                                           \
    }                                                                                           \
                                                                                                \
    scope int qt_##name##_inrange_circle(qt(name) *qt,                                          \
                                  float x, float y, float range,                                \
                                  type *out, int maxout)                                        \
    {                                                                                           \
        if(!qt->root)                                                                           \
            return 0;                                                                           \
        qt_node(name) *root = mp_##name##_entry(&qt->node_pool, qt->root);                      \
        return _qt_##name##_node_inrange_circle(qt, root, x, y, range, out, &maxout);           \
    }                                                                                           \
                                                                                                \
    scope int  qt_##name##_inrange_rect(qt(name) *qt,                                           \
                                        float minx, float maxx,                                 \
                                        float miny, float maxy,                                 \
                                        type *out, int maxout)                                  \
    {                                                                                           \
        if(!qt->root)                                                                           \
            return 0;                                                                           \
        qt_node(name) *root = mp_##name##_entry(&qt->node_pool, qt->root);                      \
        return _qt_##name##_node_inrange_rect(qt, root, minx, maxx, miny, maxy,out, &maxout);   \
    }                                                                                           \
                                                                                                \
    scope void qt_##name##_print(qt(name) *qt)                                                  \
    {                                                                                           \
        printf("number of records: %u\n", (unsigned)qt->nrecs);                                 \
        printf("mempool nodes: %u\n", (unsigned)qt->node_pool.num_allocd);                      \
        if(qt->root == 0) {                                                                     \
            printf("(empty)\n");                                                                \
            return;                                                                             \
        }                                                                                       \
        _qt_##name##_node_print(qt, qt->root, 0);                                               \
    }                                                                                           \
                                                                                                \
    scope bool qt_##name##_reserve(qt(name) *qt, size_t new_cap)                                \
    {                                                                                           \
        return mp_##name##_reserve(&qt->node_pool, new_cap);                                    \
    }

#endif

