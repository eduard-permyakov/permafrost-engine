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

#ifndef PRIVATE_TYPES_H
#define PRIVATE_TYPES_H

#include <Python.h>

/* from Objects/descrobject.c */
typedef struct {
    PyObject_HEAD
    PyObject *dict;
} proxyobject;

/* from Objects/descrobject.c */
typedef struct {
    PyObject_HEAD
    PyWrapperDescrObject *descr;
    PyObject *self;
} wrapperobject;

/* from Objects/typeobject.c */
typedef struct {
    PyObject_HEAD
    PyTypeObject *type;
    PyObject *obj;
    PyTypeObject *obj_type;
} superobject;

/* from Objects/rangeobject.c */
typedef struct {
    PyObject_HEAD
    long        start;
    long        step;
    long        len;
} rangeobject;

/* from Objects/funcobject.c */
typedef struct {
    PyObject_HEAD
    PyObject *sm_callable;
} staticmethod;

/* from Objects/bufferobject.c */
typedef struct {
    PyObject_HEAD
    PyObject *b_base;
    void *b_ptr;
    Py_ssize_t b_size;
    Py_ssize_t b_offset;
    int b_readonly;
    long b_hash;
} PyBufferObject;

/* from Objects/descrobject.c */
typedef struct {
    PyObject_HEAD
    PyObject *prop_get;
    PyObject *prop_set;
    PyObject *prop_del;
    PyObject *prop_doc;
    int getter_doc;
} propertyobject;

/* from Objects/enumobject.c */
typedef struct {
    PyObject_HEAD
    Py_ssize_t en_index;           /* current index of enumeration */
    PyObject* en_sit;          /* secondary iterator of enumeration */
    PyObject* en_result;           /* result tuple  */
    PyObject* en_longindex;        /* index for sequences >= PY_SSIZE_T_MAX */
} enumobject;

/* from Objects/listobject.c */
typedef struct {
    PyObject_HEAD
    long it_index;
    PyListObject *it_seq; /* Set to NULL when iterator is exhausted */
} listiterobject;

/* from Objects/enumobject.c */
typedef struct {
    PyObject_HEAD
    Py_ssize_t      index;
    PyObject* seq;
} reversedobject;

#endif

