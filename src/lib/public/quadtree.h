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
        size_t   height;                                                                        \
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
    scope bool qt_##name##_delete(qt(name) *qt, float x, float y);                              \
    scope bool qt_##name##_find(qt(name) *qt, float x, float y, type *out);                     \
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
        printf("[%10.4f, %10.4f]", node->x, node->y);                                           \
        if(node->has_record)                                                                    \
            printf(" (has record)");                                                            \
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
        if(ymin1 >= ymin2 && ymin1 <= ymax2)                                                    \
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
        _qt_##name##_node_bounds(qt, ref, &xmin, &xmax, &ymin, &ymax);                          \
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
            if(root->has_record                                                                 \
            && (_qt_##name##_dist(x, y, root->x, root->y) <= range)) {                          \
                                                                                                \
                *out = root->record;                                                            \
                *inout_maxout -= 1;                                                             \
                return 1;                                                                       \
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
                *out = root->record;                                                            \
                *inout_maxout -= 1;                                                             \
                return 1;                                                                       \
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
        mp_ref_t nw = 0, ne = 0, sw = 0, se = 0;                                                \
        _CHK_TRUE_JMP((nw = mp_##name##_alloc(&qt->node_pool)), fail);                          \
        _CHK_TRUE_JMP((ne = mp_##name##_alloc(&qt->node_pool)), fail);                          \
        _CHK_TRUE_JMP((sw = mp_##name##_alloc(&qt->node_pool)), fail);                          \
        _CHK_TRUE_JMP((se = mp_##name##_alloc(&qt->node_pool)), fail);                          \
        qt->height = _MAX(qt->height, node->depth + 2);                                         \
                                                                                                \
        /* NW node */                                                                           \
        node->nw = nw;                                                                          \
        curr = mp_##name##_entry(&qt->node_pool, nw);                                           \
        _qt_##name##_node_init(curr, node->depth + 1);                                          \
        _qt_##name##_set_divide_coords(qt, node, nw);                                           \
        curr->parent = ref;                                                                     \
                                                                                                \
        /* NE node */                                                                           \
        node->ne = ne;                                                                          \
        curr = mp_##name##_entry(&qt->node_pool, ne);                                           \
        _qt_##name##_node_init(curr, node->depth + 1);                                          \
        _qt_##name##_set_divide_coords(qt, node, ne);                                           \
        curr->parent = ref;                                                                     \
                                                                                                \
        /* SW node */                                                                           \
        node->sw = sw;                                                                          \
        curr = mp_##name##_entry(&qt->node_pool, sw);                                           \
        _qt_##name##_node_init(curr, node->depth + 1);                                          \
        _qt_##name##_set_divide_coords(qt, node, sw);                                           \
        curr->parent = ref;                                                                     \
                                                                                                \
        /* SE node */                                                                           \
        node->se = se;                                                                          \
        curr = mp_##name##_entry(&qt->node_pool, se);                                           \
        _qt_##name##_node_init(curr, node->depth + 1);                                          \
        _qt_##name##_set_divide_coords(qt, node, se);                                           \
        curr->parent = ref;                                                                     \
                                                                                                \
        /* Set the record for in one of the quadrants */                                        \
        mp_ref_t rec_ref = _qt_##name##_quadrant(node, saved_x, saved_y);                       \
        qt_node(name) *rec_node = mp_##name##_entry(&qt->node_pool, rec_ref);                   \
        rec_node->x = saved_x;                                                                  \
        rec_node->y = saved_y;                                                                  \
        rec_node->record = saved_record;                                                        \
        rec_node->has_record = true;                                                            \
                                                                                                \
        return true;                                                                            \
                                                                                                \
    fail:                                                                                       \
        nw ? mp_##name##_free(&qt->node_pool, nw), 0 : 0;                                       \
        ne ? mp_##name##_free(&qt->node_pool, ne), 0 : 0;                                       \
        sw ? mp_##name##_free(&qt->node_pool, sw), 0 : 0;                                       \
        se ? mp_##name##_free(&qt->node_pool, se), 0 : 0;                                       \
        return false;                                                                           \
    }                                                                                           \
                                                                                                \
    static bool _qt_##name##_merge(qt(name) *qt, mp_ref_t ref)                                  \
    {                                                                                           \
        assert(ref > 0);                                                                        \
        qt_node(name) *node = mp_##name##_entry(&qt->node_pool, ref);                           \
        assert(!_qt_##name##_node_isleaf(node));                                                \
                                                                                                \
        int nrecs = !!mp_##name##_entry(&qt->node_pool, node->nw)->has_record                   \
                  + !!mp_##name##_entry(&qt->node_pool, node->ne)->has_record                   \
                  + !!mp_##name##_entry(&qt->node_pool, node->sw)->has_record                   \
                  + !!mp_##name##_entry(&qt->node_pool, node->se)->has_record;                  \
                                                                                                \
        assert(nrecs > 0);                                                                      \
        if(nrecs > 1)                                                                           \
            return false;                                                                       \
                                                                                                \
        mp_ref_t rec = mp_##name##_entry(&qt->node_pool, node->nw)->has_record ? node->nw       \
                     : mp_##name##_entry(&qt->node_pool, node->ne)->has_record ? node->ne       \
                     : mp_##name##_entry(&qt->node_pool, node->sw)->has_record ? node->sw       \
                     : mp_##name##_entry(&qt->node_pool, node->se)->has_record ? node->se       \
                     : (assert(0), 0);                                                          \
                                                                                                \
        mp_##name##_free(&qt->node_pool, node->nw);                                             \
        mp_##name##_free(&qt->node_pool, node->ne);                                             \
        mp_##name##_free(&qt->node_pool, node->sw);                                             \
        mp_##name##_free(&qt->node_pool, node->se);                                             \
                                                                                                \
        node->nw = node->ne = node->sw = node->se = 0;                                          \
        node->has_record = true;                                                                \
        node->record = mp_##name##_entry(&qt->node_pool, rec)->record;                          \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool qt_##name##_init(qt(name) *qt,                                                   \
                                float xmin, float xmax,                                         \
                                float ymin, float ymax)                                         \
    {                                                                                           \
        mp_##name##_init(&qt->node_pool);                                                       \
        qt->root = 0;                                                                           \
        qt->nrecs = 0;                                                                          \
        qt->height = 0;                                                                         \
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
            assert(qt->height == 0);                                                            \
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
            qt->height++;                                                                       \
            return true;                                                                        \
        }                                                                                       \
                                                                                                \
        assert(qt->root > 0);                                                                   \
        mp_ref_t curr_ref = _qt_##name##_find_leaf(qt, x, y);                                   \
        qt_node(name) *curr_node = mp_##name##_entry(&qt->node_pool, curr_ref);                 \
        assert(_qt_##name##_node_isleaf(curr_node));                                            \
                                                                                                \
        /* If the leaf has a record at the same position, overwrite it. */                      \
        /* If it does not, then it is the region containing the new node. Set its' record. */   \
        if((curr_node->has_record && QT_EQ(x, curr_node->x) && QT_EQ(y, curr_node->y))          \
        || !curr_node->has_record) {                                                            \
                                                                                                \
            curr_node->x = x;                                                                   \
            curr_node->y = y;                                                                   \
            curr_node->has_record = true;                                                       \
            curr_node->record = record;                                                         \
            return true;                                                                        \
        }                                                                                       \
        assert(curr_node->has_record);                                                          \
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
    scope bool qt_##name##_delete(qt(name) *qt, float x, float y)                               \
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
        curr_node->has_record = false;                                                          \
        --qt->nrecs;                                                                            \
                                                                                                \
        if(!curr_node->parent)                                                                  \
            return true;                                                                        \
                                                                                                \
        do {                                                                                    \
            curr_ref = curr_node->parent;                                                       \
            curr_node = mp_##name##_entry(&qt->node_pool, curr_ref);                            \
                                                                                                \
        }while(_qt_##name##_merge(qt, curr_ref));                                               \
                                                                                                \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool qt_##name##_find(qt(name) *qt, float x, float y, type *out)                      \
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
        return qt_##name##_find(qt, x, y, &dummy);                                              \
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

