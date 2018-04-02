/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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
 */


#include "tile_script.h"
#include "../map/public/tile.h"
#include "../map/public/map.h"

#include <structmember.h>

typedef struct {
    PyObject_HEAD
    struct tile tile; 
}PyTileObject;


static int PyTile_init(PyTileObject *self, PyObject *args);
static PyObject *PyTile_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

#define BASE (offsetof(PyTileObject, tile))
static PyMemberDef PyTileMembers[] = {
    {"pathable",        T_INT, BASE + offsetof(struct tile, pathable),         0,
    "Whether or not units can travel through this tile."},
    {"type",            T_INT, BASE + offsetof(struct tile, type),             0,
    "Integer value specifying whether this tile is a ramp, which direction it faces, etc."},
    {"base_height",     T_INT, BASE + offsetof(struct tile, base_height),      0,
    "The height level of the bottom plane of the tile."},
    {"top_mat_idx",     T_INT, BASE + offsetof(struct tile, top_mat_idx),      0,
    "Material index for the top face of the tile."},
    {"sides_mat_idx",   T_INT, BASE + offsetof(struct tile, sides_mat_idx),    0,
    "Material index for the side faces of the tile."},
    {"ramp_height",     T_INT, BASE + offsetof(struct tile, ramp_height),      0,
    "The height of the top edge of the ramp or corner above the base_height."},
    {NULL}  /* Sentinel */
};

static PyTypeObject PyTile_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name        = "pf.Tile",
    .tp_basicsize   = sizeof(PyTileObject),
    .tp_flags       = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc         = "Map tile representation for Permafrost Engine maps.",
    .tp_members     = PyTileMembers,
    .tp_init        = (initproc)PyTile_init,
    .tp_new         = PyTile_new,
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int PyTile_init(PyTileObject *self, PyObject *args)
{
    self->tile.pathable = true;
    self->tile.type = TILETYPE_FLAT;
    self->tile.base_height = 0;
    self->tile.ramp_height = 0;
    self->tile.top_mat_idx = 0;
    self->tile.sides_mat_idx = 1;

    return 0;
}

static PyObject *PyTile_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *self = type->tp_alloc(type, 0);
    return self;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void S_Tile_PyRegister(PyObject *module)
{
    if(PyType_Ready(&PyTile_type) < 0)
        return;
    Py_INCREF(&PyTile_type);
    PyModule_AddObject(module, "Tile", (PyObject*)&PyTile_type);

    int ret = PyObject_SetAttrString(module, "TILES_PER_CHUNK_WIDTH", 
        Py_BuildValue("i", TILES_PER_CHUNK_WIDTH));
    assert(0 == ret);

    ret = PyObject_SetAttrString(module, "TILES_PER_CHUNK_HEIGHT", 
        Py_BuildValue("i", TILES_PER_CHUNK_HEIGHT));
    assert(0 == ret);

    ret = PyObject_SetAttrString(module, "MATERIALS_PER_CHUNK", 
        Py_BuildValue("i", MATERIALS_PER_CHUNK));
    assert(0 == ret);
}


const struct tile *S_Tile_GetTile(PyObject *tile_obj)
{
    if(!PyObject_IsInstance(tile_obj, (PyObject*)&PyTile_type))
        return NULL;

    return &((PyTileObject*)tile_obj)->tile;
}

