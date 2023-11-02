/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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

#include <Python.h> /* must be first */
#include "py_entity.h" 
#include "py_pickle.h"
#include "../main.h"
#include "../entity.h"
#include "../event.h"
#include "../asset_load.h"
#include "../anim/public/anim.h"
#include "../game/public/game.h"
#include "../phys/public/phys.h"
#include "../lib/public/khash.h"
#include "../lib/public/SDL_vec_rwops.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/mem.h"

#include <assert.h>


#define CHK_TRUE(_pred, _label) do{ if(!(_pred)) goto _label; }while(0)
#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

KHASH_MAP_INIT_INT(PyObject, PyObject*)

/*****************************************************************************/
/* pf.Entity                                                                 */
/*****************************************************************************/

typedef struct {
    PyObject_HEAD
    uint32_t ent;
}PyEntityObject;

static PyObject *PyEntity_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyEntity_dealloc(PyEntityObject *self);
static PyObject *PyEntity_del(PyEntityObject *self);
static PyObject *PyEntity_get_uid(PyEntityObject *self, void *closure);
static PyObject *PyEntity_get_name(PyEntityObject *self, void *closure);
static int       PyEntity_set_name(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_zombie(PyEntityObject *self, void *closure);
static PyObject *PyEntity_get_height(PyEntityObject *self, void *closure);
static PyObject *PyEntity_get_pos(PyEntityObject *self, void *closure);
static int       PyEntity_set_pos(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_scale(PyEntityObject *self, void *closure);
static int       PyEntity_set_scale(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_rotation(PyEntityObject *self, void *closure);
static int       PyEntity_set_rotation(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_selectable(PyEntityObject *self, void *closure);
static int       PyEntity_set_selectable(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_selection_radius(PyEntityObject *self, void *closure);
static int       PyEntity_set_selection_radius(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_pfobj_path(PyEntityObject *self, void *closure);
static PyObject *PyEntity_get_top_screen_pos(PyEntityObject *self, void *closure);
static PyObject *PyEntity_get_faction_id(PyEntityObject *self, void *closure);
static int       PyEntity_set_faction_id(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_vision_range(PyEntityObject *self, void *closure);
static int       PyEntity_set_vision_range(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_tags(PyEntityObject *self, void *closure);
static PyObject *PyEntity_get_bounds(PyEntityObject *self, void *closure);
static PyObject *PyEntity_register(PyEntityObject *self, PyObject *args);
static PyObject *PyEntity_unregister(PyEntityObject *self, PyObject *args);
static PyObject *PyEntity_notify(PyEntityObject *self, PyObject *args);
static PyObject *PyEntity_select(PyEntityObject *self);
static PyObject *PyEntity_deselect(PyEntityObject *self);
static PyObject *PyEntity_stop(PyEntityObject *self);
static PyObject *PyEntity_face_towards(PyEntityObject *self, PyObject *args);
static PyObject *PyEntity_set_model(PyEntityObject *self, PyObject *args);
static PyObject *PyEntity_ping(PyEntityObject *self);
static PyObject *PyEntity_zombiefy(PyEntityObject *self);
static PyObject *PyEntity_add_tag(PyEntityObject *self, PyObject *args);
static PyObject *PyEntity_remove_tag(PyEntityObject *self, PyObject *args);
static PyObject *PyEntity_pickle(PyEntityObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

static PyMethodDef PyEntity_methods[] = {
    {"__del__", 
    (PyCFunction)PyEntity_del, METH_NOARGS,
    "Calls the next __del__ in the MRO if there is one, otherwise do nothing."},

    {"register", 
    (PyCFunction)PyEntity_register, METH_VARARGS,
    "Registers the specified callable to be invoked when an event of the specified type "
    "is sent to this entity." },

    {"unregister", 
    (PyCFunction)PyEntity_unregister, METH_VARARGS,
    "Unregisters a callable previously registered to be invoked on the specified event."},

    {"notify", 
    (PyCFunction)PyEntity_notify, METH_VARARGS,
    "Send a specific event to an entity in order to invoke the entity's event handlers. Weakref "
    "arguments are automatically unpacked before being passed to the handler."},

    {"select", 
    (PyCFunction)PyEntity_select, METH_NOARGS,
    "Adds the entity to the current unit selection, if it is not present there already."},

    {"deselect", 
    (PyCFunction)PyEntity_deselect, METH_NOARGS,
    "Removes the entity from the current unit selection, if it is selected."},

    {"stop", 
    (PyCFunction)PyEntity_stop, METH_NOARGS,
    "Issues a 'stop' command to the entity, stopping its' movement and attack. Cancels 'hold position' order."},

    {"face_towards", 
    (PyCFunction)PyEntity_face_towards, METH_VARARGS,
    "Make the entity face towards the specified point."},

    {"set_model", 
    (PyCFunction)PyEntity_set_model, METH_VARARGS,
    "Replace the current entity's current model and animation data with the specified PFOBJ data."},

    {"ping", 
    (PyCFunction)PyEntity_ping, METH_NOARGS,
    "Temporarily blink the enitity's selection circle."},

    {"zombiefy", 
    (PyCFunction)PyEntity_zombiefy, METH_NOARGS,
    "Make the entity a 'zombie', effectively removing it from the game simulation but allowing "
    "the scripting object to persist."},

    {"add_tag", 
    (PyCFunction)PyEntity_add_tag, METH_VARARGS,
    "Add a string tag to this entity's list of tags."},

    {"remove_tag", 
    (PyCFunction)PyEntity_remove_tag, METH_VARARGS,
    "Remove a string tag from this entity's list of tags."},

    {"__pickle__", 
    (PyCFunction)PyEntity_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine entity to a string."},

    {"__unpickle__", 
    (PyCFunction)PyEntity_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.Entity instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyEntity_getset[] = {
    {"uid",
    (getter)PyEntity_get_uid, NULL,
    "The unique integer ID of this entity",
    NULL},
    {"name",
    (getter)PyEntity_get_name, (setter)PyEntity_set_name,
    "Custom name given to this enity.",
    NULL},
    {"zombie",
    (getter)PyEntity_get_zombie, NULL,
    "Returns True if the entity is a zombie (destroyed in the game simulation, but retained via a scripting reference).",
    NULL},
    {"height",
    (getter)PyEntity_get_height, NULL,
    "Returns the scaled height of the entity, in OpenGL coordinates.",
    NULL},
    {"pos",
    (getter)PyEntity_get_pos, (setter)PyEntity_set_pos,
    "The XYZ position in worldspace coordinates.",
    NULL},
    {"scale",
    (getter)PyEntity_get_scale, (setter)PyEntity_set_scale,
    "The XYZ scaling factors.",
    NULL},
    {"rotation",
    (getter)PyEntity_get_rotation, (setter)PyEntity_set_rotation,
    "XYZW quaternion for rotaion about local origin.",
    NULL},
    {"selectable",
    (getter)PyEntity_get_selectable, (setter)PyEntity_set_selectable,
    "Flag indicating whether this entity can be selected with the mouse.",
    NULL},
    {"selection_radius",
    (getter)PyEntity_get_selection_radius, (setter)PyEntity_set_selection_radius,
    "Radius (in OpenGL coordinates) of the unit selection circle for this entity.",
    NULL},
    {"pfobj_path",
    (getter)PyEntity_get_pfobj_path, NULL,
    "The relative path of the PFOBJ file used to instantiate the entity. Readonly.",
    NULL},
    {"top_screen_pos",
    (getter)PyEntity_get_top_screen_pos, NULL,
    "Get the location of the top center point of the entity, in screenspace coordinates.",
    NULL},
    {"faction_id",
    (getter)PyEntity_get_faction_id, (setter)PyEntity_set_faction_id,
    "Index of the faction that the entity belongs to.",
    NULL},
    {"vision_range",
    (getter)PyEntity_get_vision_range, (setter)PyEntity_set_vision_range,
    "The radius (in OpenGL coordinates) that the entity sees around itself.",
    NULL},
    {"tags",
    (getter)PyEntity_get_tags, NULL,
    "Return a tuple with all the entity's tags.",
    NULL},
    {"bounds",
    (getter)PyEntity_get_bounds, NULL,
    "Return an (X, Y, Z) tuple of the dimensions of the entity's bounding box (in OpenGL coordinates).",
    NULL},
    {NULL}  /* Sentinel */
};

static PyTypeObject PyEntity_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.Entity",               /* tp_name */
    sizeof(PyEntityObject),    /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)PyEntity_dealloc,/* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "Permafrost Engine generic game entity.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyEntity_methods,          /* tp_methods */
    0,                         /* tp_members */
    PyEntity_getset,           /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    PyEntity_new,              /* tp_new */
};

/*****************************************************************************/
/* pf.AnimEntity                                                             */
/*****************************************************************************/

typedef struct {
    PyEntityObject super; 
}PyAnimEntityObject;

static int       PyAnimEntity_init(PyAnimEntityObject *self, PyObject *args, PyObject *kwds);
static PyObject *PyAnimEntity_del(PyAnimEntityObject *self);
static PyObject *PyAnimEntity_play_anim(PyAnimEntityObject *self, PyObject *args, PyObject *kwds);
static PyObject *PyAnimEntity_get_anim(PyAnimEntityObject *self);
static PyObject *PyAnimEntity_pickle(PyAnimEntityObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyAnimEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

static PyMethodDef PyAnimEntity_methods[] = {
    {"play_anim", 
    (PyCFunction)PyAnimEntity_play_anim, METH_VARARGS | METH_KEYWORDS,
    "Play the animation clip with the specified name. "
    "Set kwarg 'mode=%d' to set the animation mode. The default is ANIM_MODE_LOOP."},

    {"get_anim", 
    (PyCFunction)PyAnimEntity_get_anim, METH_NOARGS,
    "Get the name of the currently playing animation clip."},

    {"__del__", 
    (PyCFunction)PyAnimEntity_del, METH_NOARGS,
    "Calls the next __del__ in the MRO if there is one, otherwise do nothing."},

    {"__pickle__", 
    (PyCFunction)PyAnimEntity_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine animated entity to a string."},

    {"__unpickle__", 
    (PyCFunction)PyAnimEntity_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.AnimEntity instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyAnimEntity_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "pf.AnimEntity",
    .tp_basicsize = sizeof(PyAnimEntityObject), 
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "Permafrost Engine animated entity. This type requires the "
                    "'idle_clip' keyword argument to be passed to __init__. "
                    "This is a subclass of pf.Entity.",
    .tp_methods   = PyAnimEntity_methods,
    .tp_base      = &PyEntity_type,
    .tp_init      = (initproc)PyAnimEntity_init,
};
 
/*****************************************************************************/
/* pf.CombatableEntity                                                       */
/*****************************************************************************/

typedef struct {
    PyEntityObject super; 
}PyCombatableEntityObject;

static int       PyCombatableEntity_init(PyCombatableEntityObject *self, PyObject *args, PyObject *kwds);
static PyObject *PyCombatableEntity_del(PyCombatableEntityObject *self);
static PyObject *PyCombatableEntity_get_hp(PyCombatableEntityObject *self, void *closure);
static int       PyCombatableEntity_set_hp(PyCombatableEntityObject *self, PyObject *value, void *closure);
static PyObject *PyCombatableEntity_get_max_hp(PyCombatableEntityObject *self, void *closure);
static int       PyCombatableEntity_set_max_hp(PyCombatableEntityObject *self, PyObject *value, void *closure);
static PyObject *PyCombatableEntity_get_base_dmg(PyCombatableEntityObject *self, void *closure);
static int       PyCombatableEntity_set_base_dmg(PyCombatableEntityObject *self, PyObject *value, void *closure);
static PyObject *PyCombatableEntity_get_base_armour(PyCombatableEntityObject *self, void *closure);
static int       PyCombatableEntity_set_base_armour(PyCombatableEntityObject *self, PyObject *value, void *closure);
static PyObject *PyCombatableEntity_get_attack_range(PyCombatableEntityObject *self, void *closure);
static int       PyCombatableEntity_set_attack_range(PyCombatableEntityObject *self, PyObject *value, void *closure);
static PyObject *PyCombatableEntity_hold_position(PyCombatableEntityObject *self);
static PyObject *PyCombatableEntity_attack(PyCombatableEntityObject *self, PyObject *args);
static PyObject *PyCombatableEntity_pickle(PyCombatableEntityObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyCombatableEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

static PyMethodDef PyCombatableEntity_methods[] = {
    {"hold_position", 
    (PyCFunction)PyCombatableEntity_hold_position, METH_NOARGS,
    "Issues a 'hold position' order to the entity, stopping it and preventing it from moving to attack."},

    {"attack", 
    (PyCFunction)PyCombatableEntity_attack, METH_VARARGS,
    "Issues an 'attack move' order to the entity at the XZ position specified by the argument."},

    {"__del__", 
    (PyCFunction)PyCombatableEntity_del, METH_NOARGS,
    "Calls the next __del__ in the MRO if there is one, otherwise do nothing."},

    {"__pickle__", 
    (PyCFunction)PyCombatableEntity_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine combatable entity to a string."},

    {"__unpickle__", 
    (PyCFunction)PyCombatableEntity_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.CombatableEntity instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyCombatableEntity_getset[] = {
    {"hp",
    (getter)PyCombatableEntity_get_hp, (setter)PyCombatableEntity_set_hp,
    "The current number of hitpoints that the entity has.",
    NULL},
    {"max_hp",
    (getter)PyCombatableEntity_get_max_hp, (setter)PyCombatableEntity_set_max_hp,
    "The maximum number of hitpoints that the entity starts out with.",
    NULL},
    {"base_dmg",
    (getter)PyCombatableEntity_get_base_dmg, (setter)PyCombatableEntity_set_base_dmg,
    "The base damage for which this entity's attacks hit.",
    NULL},
    {"base_armour",
    (getter)PyCombatableEntity_get_base_armour, (setter)PyCombatableEntity_set_base_armour,
    "The base armour (as a fraction from 0.0 to 1.0) specifying which percentage of incoming "
    "damage is blocked.",
    NULL},
    {"attack_range",
    (getter)PyCombatableEntity_get_attack_range, (setter)PyCombatableEntity_set_attack_range,
    "The distance from which an entity can attack. 0 for melee units.",
    NULL},
    {NULL}  /* Sentinel */
};

static PyTypeObject PyCombatableEntity_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "pf.CombatableEntity",
    .tp_basicsize = sizeof(PyCombatableEntityObject), 
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "Permafrost Engine entity which is able to take part in combat. This type "
                    "requires the 'max_hp', 'base_dmg', and 'base_armour' keyword arguments to be "
                    "passed to __init__. An optional 'attack_range' keyword argument may also be "
                    "passed. This is a subclass of pf.Entity.",
    .tp_methods   = PyCombatableEntity_methods,
    .tp_base      = &PyEntity_type,
    .tp_getset    = PyCombatableEntity_getset,
    .tp_init      = (initproc)PyCombatableEntity_init,
};

/*****************************************************************************/
/* pf.BuildableEntity                                                        */
/*****************************************************************************/

typedef struct {
    PyEntityObject super; 
}PyBuildableEntityObject;

static int       PyBuildableEntity_init(PyBuildableEntityObject *self, PyObject *args, PyObject *kwds);
static PyObject *PyBuildableEntity_del(PyBuildableEntityObject *self);
static PyObject *PyBuildableEntity_mark(PyBuildableEntityObject *self);
static PyObject *PyBuildableEntity_found(PyBuildableEntityObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyBuildableEntity_supply(PyBuildableEntityObject *self);
static PyObject *PyBuildableEntity_complete(PyBuildableEntityObject *self);
static PyObject *PyBuildableEntity_unobstructed(PyBuildableEntityObject *self);
static PyObject *PyBuildableEntity_get_pos(PyBuildableEntityObject *self, void *closure);
static int       PyBuildableEntity_set_pos(PyBuildableEntityObject *self, PyObject *value, void *closure);
static PyObject *PyBuildableEntity_get_founded(PyBuildableEntityObject *self, void *closure);
static PyObject *PyBuildableEntity_get_supplied(PyBuildableEntityObject *self, void *closure);
static PyObject *PyBuildableEntity_get_completed(PyBuildableEntityObject *self, void *closure);
static PyObject *PyBuildableEntity_get_vision_range(PyBuildableEntityObject *self, void *closure);
static int       PyBuildableEntity_set_vision_range(PyBuildableEntityObject *self, PyObject *value, void *closure);
static PyObject *PyBuildableEntity_get_required_resources(PyBuildableEntityObject *self, void *closure);
static PyObject *PyBuildableEntity_pickle(PyBuildableEntityObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyBuildableEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

static PyMethodDef PyBuildableEntity_methods[] = {
    {"mark", 
    (PyCFunction)PyBuildableEntity_mark, METH_NOARGS,
    "Advance a building to the 'MARKED' state from the initial 'PLACEMENT' state, "
    "where it will wait for a worker to found it."},

    {"found", 
    (PyCFunction)PyBuildableEntity_found, METH_VARARGS | METH_KEYWORDS,
    "Advance a building to the 'FOUNDED' state from the 'MARKED' state, "
    "where it becomes a build site and wait for workers to supply it."},

    {"supply", 
    (PyCFunction)PyBuildableEntity_supply, METH_NOARGS,
    "Advance a building to the 'SUPPLIED' state from the 'FOUNDED' state, "
    "where it meets the construction resource requirements and wait for workers to finish constructing it."},

    {"complete", 
    (PyCFunction)PyBuildableEntity_complete, METH_NOARGS,
    "Advance a building to the 'COMPLETED' state from the 'SUPPLIED' state."},

    {"unobstructed", 
    (PyCFunction)PyBuildableEntity_unobstructed, METH_NOARGS,
    "Returns True if there is no obstruction under any of the building's tiles."},

    {"__del__", 
    (PyCFunction)PyBuildableEntity_del, METH_NOARGS,
    "Calls the next __del__ in the MRO if there is one, otherwise do nothing."},

    {"__pickle__", 
    (PyCFunction)PyBuildableEntity_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine buildable entity to a string."},

    {"__unpickle__", 
    (PyCFunction)PyBuildableEntity_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.BuildableEntity instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyBuildableEntity_getset[] = {
    {"pos",
    (getter)PyBuildableEntity_get_pos, (setter)PyBuildableEntity_set_pos,
    "The XYZ position in worldspace coordinates.",
    NULL},
    {"vision_range",
    (getter)PyBuildableEntity_get_vision_range, (setter)PyBuildableEntity_set_vision_range,
    "The radius (in OpenGL coordinates) that the entity sees around itself.",
    NULL},
    {"founded",
    (getter)PyBuildableEntity_get_founded, NULL,
    "Boolean indicating if the building is at or past the 'FOUNDED' state.",
    NULL},
    {"supplied",
    (getter)PyBuildableEntity_get_supplied, NULL,
    "Boolean indicating if the building is at or past the 'SUPPLIED' state.",
    NULL},
    {"completed",
    (getter)PyBuildableEntity_get_completed, NULL,
    "Boolean indicating if the building is at or past the 'COMPLETED' state.",
    NULL},
    {"selectable",
    (getter)PyEntity_get_selectable, NULL,
    "Flag indicating whether this entity can be selected with the mouse.",
    NULL},
    {"required_resources",
    (getter)PyBuildableEntity_get_required_resources, NULL,
    "Get a dictionary of the resources required to supply this building.",
    NULL},
    {NULL}  /* Sentinel */
};

static PyTypeObject PyBuildableEntity_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "pf.BuildableEntity",
    .tp_basicsize = sizeof(PyBuildableEntityObject), 
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "Permafrost Engine entity buildable entity. This is a subclass of pf.Entity. "
                    "The building starts out in the 'PLACEMENT' state. It must then go through the 'MARKED', "
                    "'FOUNDED', 'SUPPLIED', and 'COMPLETED' states. This type requires the 'required_resources' "
                    "keyword argument to be passed to __init__.",
    .tp_methods   = PyBuildableEntity_methods,
    .tp_base      = &PyEntity_type,
    .tp_getset    = PyBuildableEntity_getset,
    .tp_init      = (initproc)PyBuildableEntity_init,
};

/*****************************************************************************/
/* pf.BuilderEntity                                                          */
/*****************************************************************************/

typedef struct {
    PyEntityObject super; 
}PyBuilderEntityObject;

static PyObject *PyBuilderEntity_del(PyBuilderEntityObject *self);
static int       PyBuilderEntity_init(PyBuilderEntityObject *self, PyObject *args, PyObject *kwds);
static PyObject *PyBuilderEntity_build(PyBuilderEntityObject *self, PyObject *args);
static PyObject *PyBuilderEntity_pickle(PyBuilderEntityObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyBuilderEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

static PyMethodDef PyBuilderEntity_methods[] = {
    {"build", 
    (PyCFunction)PyBuilderEntity_build, METH_VARARGS,
    "Issue an order to build a specific buildable entity."},

    {"__del__", 
    (PyCFunction)PyBuilderEntity_del, METH_NOARGS,
    "Calls the next __del__ in the MRO if there is one, otherwise do nothing."},

    {"__pickle__", 
    (PyCFunction)PyBuilderEntity_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine builder entity to a string."},

    {"__unpickle__", 
    (PyCFunction)PyBuilderEntity_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.BuilderEntity instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyBuilderEntity_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "pf.BuilderEntity",
    .tp_basicsize = sizeof(PyBuilderEntityObject), 
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "Permafrost Engine builder entity. This is a subclass of pf.Entity. This kind of "
                    "entity is able to construct and repair pf.BuildableEntity instances. This type "
                    "requires the 'build_speed' keyword argument to be passed to '__init__'.",
    .tp_methods   = PyBuilderEntity_methods,
    .tp_base      = &PyEntity_type,
    .tp_init      = (initproc)PyBuilderEntity_init,
};

/*****************************************************************************/
/* pf.ResourceEntity                                                         */
/*****************************************************************************/

typedef struct {
    PyEntityObject super;
}PyResourceEntityObject;

static PyObject *PyResourceEntity_del(PyResourceEntityObject *self);
static int       PyResourceEntity_init(PyResourceEntityObject *self, PyObject *args, PyObject *kwds);
static PyObject *PyResourceEntity_pickle(PyResourceEntityObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyResourceEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);
static PyObject *PyResourceEntity_get_cursor(PyResourceEntityObject *self, void *closure);
static int       PyResourceEntity_set_cursor(PyResourceEntityObject *self, PyObject *value, void *closure);
static PyObject *PyResourceEntity_get_name(PyResourceEntityObject *self, void *closure);
static PyObject *PyResourceEntity_get_amount(PyResourceEntityObject *self, void *closure);
static int       PyResourceEntity_set_amount(PyResourceEntityObject *self, PyObject *value, void *closure);

static PyMethodDef PyResourceEntity_methods[] = {

    {"__del__", 
    (PyCFunction)PyResourceEntity_del, METH_NOARGS,
    "Calls the next __del__ in the MRO if there is one, otherwise do nothing."},

    {"__pickle__", 
    (PyCFunction)PyResourceEntity_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine combatable entity to a string."},

    {"__unpickle__", 
    (PyCFunction)PyResourceEntity_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.ResourceEntity instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyResourceEntity_getset[] = {
    {"cursor",
    (getter)PyResourceEntity_get_cursor, (setter)PyResourceEntity_set_cursor,
    "The name of the cursor to display as a contextual gather command indicator when hovering over the resource.",
    NULL},
    {"resource_name",
    (getter)PyResourceEntity_get_name, NULL,
    "The name of resource that can be harvested from this entity",
    NULL},
    {"resource_amount",
    (getter)PyResourceEntity_get_amount, (setter)PyResourceEntity_set_amount,
    "The amount of resources that this entity currently holds",
    NULL},
    {NULL}  /* Sentinel */
};

static PyTypeObject PyResourceEntity_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "pf.ResourceEntity",
    .tp_basicsize = sizeof(PyResourceEntityObject), 
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "Permafrost Engine resource entity. This is a subclass of pf.Entity. This type "
                    "requires the 'resource_name' and 'resource_amount' keyword arguments to be passed to '__init__'.",
    .tp_methods   = PyResourceEntity_methods,
    .tp_base      = &PyEntity_type,
    .tp_getset    = PyResourceEntity_getset,
    .tp_init      = (initproc)PyResourceEntity_init,
};

/*****************************************************************************/
/* pf.HarvesterEntity                                                        */
/*****************************************************************************/

typedef struct {
    PyEntityObject super;
}PyHarvesterEntityObject;

static PyObject *PyHarvesterEntity_del(PyHarvesterEntityObject *self);
static PyObject *PyHarvesterEntity_gather(PyHarvesterEntityObject *self, PyObject *args);
static PyObject *PyHarvesterEntity_drop_off(PyHarvesterEntityObject *self, PyObject *args);
static PyObject *PyHarvesterEntity_transport(PyHarvesterEntityObject *self, PyObject *args);
static PyObject *PyHarvesterEntity_get_curr_carry(PyHarvesterEntityObject *self, PyObject *args);
static PyObject *PyHarvesterEntity_clear_curr_carry(PyHarvesterEntityObject *self);
static PyObject *PyHarvesterEntity_get_max_carry(PyHarvesterEntityObject *self, PyObject *args);
static PyObject *PyHarvesterEntity_set_max_carry(PyHarvesterEntityObject *self, PyObject *args);
static PyObject *PyHarvesterEntity_get_gather_speed(PyHarvesterEntityObject *self, PyObject *args);
static PyObject *PyHarvesterEntity_set_gather_speed(PyHarvesterEntityObject *self, PyObject *args);
static PyObject *PyHarvesterEntity_increase_transport_priority(PyHarvesterEntityObject *self, PyObject *args);
static PyObject *PyHarvesterEntity_decrease_transport_priority(PyHarvesterEntityObject *self, PyObject *args);
static PyObject *PyHarvesterEntity_pickle(PyHarvesterEntityObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyHarvesterEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);
static PyObject *PyHarvesterEntity_get_strategy(PyHarvesterEntityObject *self, void *closure);
static int       PyHarvesterEntity_set_strategy(PyHarvesterEntityObject *self, PyObject *value, void *closure);
static PyObject *PyHarvesterEntity_get_total_carry(PyHarvesterEntityObject *self, void *closure);
static PyObject *PyHarvesterEntity_get_transport_priority(PyHarvesterEntityObject *self, void *closure);

static PyMethodDef PyHarvesterEntity_methods[] = {

    {"__del__", 
    (PyCFunction)PyHarvesterEntity_del, METH_NOARGS,
    "Calls the next __del__ in the MRO if there is one, otherwise do nothing."},

    {"gather", 
    (PyCFunction)PyHarvesterEntity_gather, METH_VARARGS,
    "Instruct an entity to harvest a particular resource."},

    {"drop_off", 
    (PyCFunction)PyHarvesterEntity_drop_off, METH_VARARGS,
    "Instruct an entity to bring the resources it is currently holding to the specified storage site."},

    {"transport", 
    (PyCFunction)PyHarvesterEntity_transport, METH_VARARGS,
    "Instruct an entity to bring resources to the target storage site, using its' strategy and priority list "
    "to select the appropriate source storage sites."},

    {"get_curr_carry", 
    (PyCFunction)PyHarvesterEntity_get_curr_carry, METH_VARARGS,
    "Get the amount of a particular resources that this entity is currently carrying."},

    {"clear_curr_carry", 
    (PyCFunction)PyHarvesterEntity_clear_curr_carry, METH_NOARGS,
    "Clear any resources that the unit is currently carrying."},

    {"get_max_carry", 
    (PyCFunction)PyHarvesterEntity_get_max_carry, METH_VARARGS,
    "Get the maximum amount of a particular resources that this entity is able to carry."},

    {"set_max_carry", 
    (PyCFunction)PyHarvesterEntity_set_max_carry, METH_VARARGS,
    "Set how much of the specified resource the entity is able to carry at a time."},

    {"get_gather_speed", 
    (PyCFunction)PyHarvesterEntity_get_gather_speed, METH_VARARGS,
    "Get how much of the specified resource the entity gathers in a single animation."},

    {"set_gather_speed", 
    (PyCFunction)PyHarvesterEntity_set_gather_speed, METH_VARARGS,
    "Set how much of the specified resource the entity gathers in a single animation."},

    {"increase_transport_priority", 
    (PyCFunction)PyHarvesterEntity_increase_transport_priority, METH_VARARGS,
    "Move the specified resource up in the priority list the peasant uses for selecting which resource "
    "to bring next to the target storage site."},

    {"decrease_transport_priority", 
    (PyCFunction)PyHarvesterEntity_decrease_transport_priority, METH_VARARGS,
    "Move the specified resource down in the priority list the peasant uses for selecting which resource "
    "to bring next to the target storage site."},

    {"__pickle__", 
    (PyCFunction)PyHarvesterEntity_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine combatable entity to a string."},

    {"__unpickle__", 
    (PyCFunction)PyHarvesterEntity_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.HarvesterEntity instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyHarvesterEntity_getset[] = {
    {"total_carry",
    (getter)PyHarvesterEntity_get_total_carry, NULL,
    "Get the total amount of resources currently carried by the entity.",
    NULL},
    {"transport_priority",
    (getter)PyHarvesterEntity_get_transport_priority, NULL,
    "Get the ordered list of the resource names that the harvester will prioritize transporting.",
    NULL},
    {"strategy",
    (getter)PyHarvesterEntity_get_strategy, (setter)PyHarvesterEntity_set_strategy,
    "The approach used by the harvester to pick the next storage site to get resources from. "
    "Must be a pf.TRANSPORT_ enum value.",
    NULL},
    {NULL}  /* Sentinel */
};

static PyTypeObject PyHarvesterEntity_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "pf.HarvesterEntity",
    .tp_basicsize = sizeof(PyHarvesterEntityObject), 
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "Permafrost Engine resource entity. This is a subclass of pf.Entity. This kind of "
                    "entity is able to gather and transport resources (from pf.ResourceEntity types).",
    .tp_methods   = PyHarvesterEntity_methods,
    .tp_base      = &PyEntity_type,
    .tp_getset    = PyHarvesterEntity_getset,
};

/*****************************************************************************/
/* pf.StorageSiteEntity                                                      */
/*****************************************************************************/

typedef struct {
    PyEntityObject super;
}PyStorageSiteEntityObject;

static PyObject *PyStorageSiteEntity_del(PyStorageSiteEntityObject *self);
static PyObject *PyStorageSiteEntity_get_curr_amount(PyStorageSiteEntityObject *self, PyObject *args);
static PyObject *PyStorageSiteEntity_set_curr_amount(PyStorageSiteEntityObject *self, PyObject *args);
static PyObject *PyStorageSiteEntity_get_capacity(PyStorageSiteEntityObject *self, PyObject *args);
static PyObject *PyStorageSiteEntity_set_capacity(PyStorageSiteEntityObject *self, PyObject *args);
static PyObject *PyStorageSiteEntity_get_storable(PyStorageSiteEntityObject *self, void *closure);
static PyObject *PyStorageSiteEntity_get_desired(PyStorageSiteEntityObject *self, PyObject *args);
static PyObject *PyStorageSiteEntity_set_desired(PyStorageSiteEntityObject *self, PyObject *args);
static PyObject *PyStorageSiteEntity_get_do_not_take(PyStorageSiteEntityObject *self, void *closure);
static int       PyStorageSiteEntity_set_do_not_take(PyStorageSiteEntityObject *self, PyObject *value, void *closure);
static PyObject *PyStorageSiteEntity_pickle(PyStorageSiteEntityObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyStorageSiteEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

static PyMethodDef PyStorageSiteEntity_methods[] = {

    {"__del__", 
    (PyCFunction)PyStorageSiteEntity_del, METH_NOARGS,
    "Calls the next __del__ in the MRO if there is one, otherwise do nothing."},

    {"get_curr_amount", 
    (PyCFunction)PyStorageSiteEntity_get_curr_amount, METH_VARARGS,
    "Gets the amount of the specified resource currently stored in the storage site."},

    {"set_curr_amount", 
    (PyCFunction)PyStorageSiteEntity_set_curr_amount, METH_VARARGS,
    "Sets the amount of the specified resource currently stored in the storage site."},

    {"get_capacity", 
    (PyCFunction)PyStorageSiteEntity_get_capacity, METH_VARARGS,
    "Gets the maximum amount of the specified resource that can be stored in the storage site."},

    {"set_capacity", 
    (PyCFunction)PyStorageSiteEntity_set_capacity, METH_VARARGS,
    "Sets the maximum amount of the specified resource that can be stored in the storage site."},

    {"get_desired", 
    (PyCFunction)PyStorageSiteEntity_get_desired, METH_VARARGS,
    "Gets the target amount of the specified resource that harvesters will aim to store there."},

    {"set_desired", 
    (PyCFunction)PyStorageSiteEntity_set_desired, METH_VARARGS,
    "Sets the target amount of the specified resource that harvesters will aim to store there."},

    {"__pickle__", 
    (PyCFunction)PyStorageSiteEntity_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine combatable entity to a string."},

    {"__unpickle__", 
    (PyCFunction)PyStorageSiteEntity_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.StorageSiteEntity instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyStorageSiteEntity_getset[] = {
    {"storable",
    (getter)PyStorageSiteEntity_get_storable, NULL,
    "The list of resources that are currently able to be held at this storage site.",
    NULL},
    {"do_not_take",
    (getter)PyStorageSiteEntity_get_do_not_take, (setter)PyStorageSiteEntity_set_do_not_take,
    "The list of resources that are currently able to be held at this storage site.",
    NULL},
    {NULL}  /* Sentinel */
};

static PyTypeObject PyStorageSiteEntity_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "pf.StorageSiteEntity",
    .tp_basicsize = sizeof(PyStorageSiteEntityObject), 
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "Permafrost Engine storage site entity. This is a subclass of pf.Entity. This kind of "
                    "entity is able to hold resources that can be dropped off by pf.HarvesterEntity types.",
    .tp_methods   = PyStorageSiteEntity_methods,
    .tp_base      = &PyEntity_type,
    .tp_getset    = PyStorageSiteEntity_getset,
};

/*****************************************************************************/
/* pf.MovableEntity                                                          */
/*****************************************************************************/

typedef struct {
    PyEntityObject super; 
}PyMovableEntityObject;

static PyObject *PyMovableEntity_move(PyMovableEntityObject *self, PyObject *args);
static PyObject *PyMovableEntity_del(PyMovableEntityObject *self);
static PyObject *PyMovableEntity_pickle(PyMovableEntityObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyMovableEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

static PyObject *PyMovableEntity_get_speed(PyMovableEntityObject *self, void *closure);
static int       PyMovableEntity_set_speed(PyMovableEntityObject *self, PyObject *value, void *closure);

static PyGetSetDef PyMovableEntity_getset[] = {
    {"speed",
    (getter)PyMovableEntity_get_speed, (setter)PyMovableEntity_set_speed,
    "Entity's movement speed (in OpenGL coordinates per second).",
    NULL},
    {NULL}  /* Sentinel */
};

static PyMethodDef PyMovableEntity_methods[] = {

    {"move", 
    (PyCFunction)PyMovableEntity_move, METH_VARARGS,
    "Issues a 'move' order to the entity at the XZ position specified by the argument."},

    {"__del__", 
    (PyCFunction)PyMovableEntity_del, METH_NOARGS,
    "Calls the next __del__ in the MRO if there is one, otherwise do nothing."},

    {"__pickle__", 
    (PyCFunction)PyMovableEntity_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine combatable entity to a string."},

    {"__unpickle__", 
    (PyCFunction)PyMovableEntity_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.MovableEntity instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyMovableEntity_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "pf.MovableEntity",
    .tp_basicsize = sizeof(PyMovableEntityObject), 
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "Permafrost Engine movable entity. This is a subclass of pf.Entity. This kind of "
                    "entity is able to receive move orders and travel around the map.",
    .tp_methods   = PyMovableEntity_methods,
    .tp_getset    = PyMovableEntity_getset,
    .tp_base      = &PyEntity_type,
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(PyObject) *s_uid_pyobj_table;
static PyObject          *s_loaded;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool s_has_super_method(const char *method_name, PyObject *type, PyObject *self)
{
    bool ret = false;

    PyObject *super_obj = PyObject_CallFunction((PyObject*)&PySuper_Type, "(OO)", type, self);
    if(!super_obj) {
        assert(PyErr_Occurred());
        goto fail_call_super;
    }

    ret = PyObject_HasAttrString(super_obj, method_name);
    Py_DECREF(super_obj);

fail_call_super:
    return ret;
}

static PyObject *s_call_super_method(const char *method_name, PyObject *type, 
                                     PyObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *ret = NULL;

    PyObject *super_obj = PyObject_CallFunction((PyObject*)&PySuper_Type, "(OO)", type, self);
    if(!super_obj) {
        assert(PyErr_Occurred());
        goto fail_call_super;
    }

    PyObject *method = PyObject_GetAttrString(super_obj, method_name);
    if(!method) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to get super method."); 
        goto fail_get_method;
    }

    ret = PyObject_Call(method, args, kwds); 
    Py_DECREF(method);

fail_get_method:
    Py_DECREF(super_obj);
fail_call_super:
    return ret;
}

static PyObject *s_super_del(PyObject *self, PyTypeObject *type)
{
    if(!s_has_super_method("__del__", (PyObject*)type, self))
        Py_RETURN_NONE;

    PyObject *args = PyTuple_New(0);
    PyObject *ret = s_call_super_method("__del__", (PyObject*)type, self, args, NULL);
    Py_DECREF(args);
    return ret;
}

static PyObject *PyEntity_del(PyEntityObject *self)
{
    return s_super_del((PyObject*)self, &PyEntity_type);
}

static PyObject *PyEntity_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyEntityObject *self;
    const char *dirpath, *filename, *name;

    /* First, extract the first 3 args to handle the cases where subclasses get 
     * intialized with more arguments */
    PyObject *first_args = PyTuple_GetSlice(args, 0, 3);
    if(!first_args) {
        return NULL;
    }

    if(!PyArg_ParseTuple(first_args, "sss", &dirpath, &filename, &name)) {
        PyErr_SetString(PyExc_TypeError, "First 3 arguments must be strings.");
        return NULL;
    }
    Py_DECREF(first_args);

    PyObject *uidobj = NULL;
    uint32_t uid;

    if(kwds) {
        uidobj = PyDict_GetItemString(kwds, "__uid__");
    }
    if(uidobj && PyInt_Check(uidobj)) {
        uid = PyInt_AS_LONG(uidobj);
    }else {
        uid = Entity_NewUID();
    }

    uint32_t flags;
    bool success = AL_EntityFromPFObj(dirpath, filename, name, uid, &flags);
    if(!success) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to load specified pf.Entity PFOBJ model.");
        return NULL;
    }

    self = (PyEntityObject*)type->tp_alloc(type, 0);
    if(!self) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to allocate new pf.Entity.");
        AL_EntityFree(uid); 
        return NULL;
    }else{
        self->ent = uid; 
    }

    uint32_t extra_flags = 0;
    if(kwds) {
        PyObject *flags = PyDict_GetItemString(kwds, "__extra_flags__");
        if(flags && PyInt_Check(flags))
            extra_flags = PyInt_AS_LONG(flags);
    }
    bool zombie = (extra_flags & ENTITY_FLAG_ZOMBIE);

    if(!zombie && PyType_IsSubtype(type, &PyCombatableEntity_type))
        extra_flags |= ENTITY_FLAG_COMBATABLE;

    if(!zombie && PyType_IsSubtype(type, &PyBuildableEntity_type))
        extra_flags |= ENTITY_FLAG_BUILDING;

    if(!zombie && PyType_IsSubtype(type, &PyBuilderEntity_type))
        extra_flags |= ENTITY_FLAG_BUILDER;

    if(!zombie && PyType_IsSubtype(type, &PyResourceEntity_type))
        extra_flags |= ENTITY_FLAG_RESOURCE;

    if(!zombie && PyType_IsSubtype(type, &PyHarvesterEntity_type))
        extra_flags |= ENTITY_FLAG_HARVESTER;

    if(!zombie && PyType_IsSubtype(type, &PyStorageSiteEntity_type))
        extra_flags |= ENTITY_FLAG_STORAGE_SITE;

    if(!zombie && PyType_IsSubtype(type, &PyMovableEntity_type))
        extra_flags |= ENTITY_FLAG_MOVABLE;

    vec3_t pos = (vec3_t){0.0f, 0.0f, 0.0f};
    if(kwds) {
        PyObject *posobj = PyDict_GetItemString(kwds, "pos");
        if(posobj) {
            if(!PyTuple_Check(posobj) || !PyArg_ParseTuple(posobj, "fff", &pos.x, &pos.y, &pos.z)) {
                PyErr_SetString(PyExc_TypeError, "'pos' keyword argument must be a tuple of 3 floats.");
                return NULL; 
            }
        }
    }

    flags |= extra_flags;
    G_AddEntity(self->ent, flags, pos);

    int ret;
    khiter_t k = kh_put(PyObject, s_uid_pyobj_table, uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_uid_pyobj_table, k) = (PyObject*)self;

    return (PyObject*)self;
}

static void PyEntity_dealloc(PyEntityObject *self)
{
    khiter_t k = kh_get(PyObject, s_uid_pyobj_table, self->ent);
    assert(k != kh_end(s_uid_pyobj_table));
    kh_del(PyObject, s_uid_pyobj_table, k);

    /* Fully remove the entity from the simulation at the end of the frame 
     * instead of immediately. This approach guarantees that an entity will 
     * never disappear from the simulation until the whole session loading 
     * process is completed, This is a nice property which allows us to 
     * skip handling this edge case elsewhere. 
     */
    G_DeferredRemove(self->ent);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *PyEntity_get_uid(PyEntityObject *self, void *closure)
{
    return PyInt_FromLong(self->ent);
}

static PyObject *PyEntity_get_name(PyEntityObject *self, void *closure)
{
    const struct entity *ent = AL_EntityGet(self->ent);
    assert(ent);
    return Py_BuildValue("s", ent->name);
}

static PyObject *PyEntity_get_zombie(PyEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->ent) & ENTITY_FLAG_ZOMBIE) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *PyEntity_get_height(PyEntityObject *self, void *closure)
{
    struct obb obb;
    Entity_CurrentOBB(self->ent, &obb, true);
    float height = obb.half_lengths[1] * 2.0f;
    return PyFloat_FromDouble(height);
}

static int PyEntity_set_name(PyEntityObject *self, PyObject *value, void *closure)
{
    if(!PyObject_IsInstance(value, (PyObject*)&PyString_Type)){
        PyErr_SetString(PyExc_TypeError, "Argument must be a string.");
        return -1;
    }

    const char *s = pf_strdup(PyString_AsString(value));
    if(!s) {
        PyErr_NoMemory();
        return -1;
    }

    struct entity *ent = AL_EntityGet(self->ent);
    assert(ent);

    PF_FREE(ent->name);
    ent->name = s;

    return 0;
}

static PyObject *PyEntity_get_pos(PyEntityObject *self, void *closure)
{
    vec3_t pos = G_Pos_Get(self->ent);
    return Py_BuildValue("(f,f,f)", pos.x, pos.y, pos.z);
}

static int PyEntity_set_pos(PyEntityObject *self, PyObject *value, void *closure)
{
    if(!PyTuple_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple.");
        return -1;
    }
    
    vec3_t newpos;
    if(!PyArg_ParseTuple(value, "fff", 
        &newpos.raw[0], &newpos.raw[1], &newpos.raw[2])) {
        return -1;
    }

    G_Pos_Set(self->ent, newpos);
    return 0;
}

static PyObject *PyEntity_get_scale(PyEntityObject *self, void *closure)
{
    vec3_t scale = Entity_GetScale(self->ent);
    return Py_BuildValue("(f,f,f)", scale.x, scale.y, scale.z);
}

static int PyEntity_set_scale(PyEntityObject *self, PyObject *value, void *closure)
{
    if(!PyTuple_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple.");
        return -1;
    }

    vec3_t scale;
    if(!PyArg_ParseTuple(value, "fff", &scale.x, &scale.y, &scale.z))
        return -1;

    Entity_SetScale(self->ent, scale);
    return 0;
}

static PyObject *PyEntity_get_rotation(PyEntityObject *self, void *closure)
{
    quat_t rot = Entity_GetRot(self->ent);
    return Py_BuildValue("(f,f,f,f)", rot.x, rot.y, rot.z, rot.w);
}

static int PyEntity_set_rotation(PyEntityObject *self, PyObject *value, void *closure)
{
    if(!PyTuple_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple.");
        return -1;
    }
   
    quat_t rot;
    if(!PyArg_ParseTuple(value, "ffff", &rot.x, &rot.y, &rot.z, &rot.w))
        return -1;

    Entity_SetRot(self->ent, rot);
    return 0;
}

static PyObject *PyEntity_get_selectable(PyEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->ent) & ENTITY_FLAG_SELECTABLE)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static int PyEntity_set_selectable(PyEntityObject *self, PyObject *value, void *closure)
{
    int result = PyObject_IsTrue(value);

    if(-1 == result) {
        PyErr_SetString(PyExc_TypeError, "Argument must evaluate to True or False.");
        return -1;
    }else if(1 == result) {
        G_FlagsSet(self->ent, G_FlagsGet(self->ent) | ENTITY_FLAG_SELECTABLE);
    }else {
        G_FlagsSet(self->ent, G_FlagsGet(self->ent) & ~ENTITY_FLAG_SELECTABLE);
    }

    return 0;
}

static PyObject *PyEntity_get_selection_radius(PyEntityObject *self, void *closure)
{
    return Py_BuildValue("f", G_GetSelectionRadius(self->ent));
}

static int PyEntity_set_selection_radius(PyEntityObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    G_SetSelectionRadius(self->ent, PyFloat_AsDouble(value));
    return 0;
}

static PyObject *PyEntity_get_pfobj_path(PyEntityObject *self, void *closure)
{
    struct entity *ent = AL_EntityGet(self->ent);
    assert(ent);

    STALLOC(char, buff, strlen(ent->basedir) + strlen(ent->filename) + 2);
    strcpy(buff, ent->basedir);
    strcat(buff, "/");
    strcat(buff, ent->filename);
    PyObject *ret = PyString_FromString(buff);
    STFREE(buff);

    return ret;
}

static PyObject *PyEntity_get_top_screen_pos(PyEntityObject *self, void *closure)
{
    int width, height;
    Engine_WinDrawableSize(&width, &height);
    vec2_t coord = Entity_TopScreenPos(self->ent, width, height);
    return Py_BuildValue("ii", (int)coord.x, (int)coord.y);
}

static PyObject *PyEntity_get_faction_id(PyEntityObject *self, void *closure)
{
    return Py_BuildValue("i", G_GetFactionID(self->ent));
}

static int PyEntity_set_faction_id(PyEntityObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "faction_id attribute must be an integer.");
        return -1;
    }

    int faction_id = PyInt_AS_LONG(value);
    uint16_t factions = G_GetFactions(NULL, NULL, NULL);
    if(faction_id < 0 || faction_id >= MAX_FACTIONS || !(factions & (0x1 << faction_id))) {
        PyErr_SetString(PyExc_TypeError, "Invalid faction ID.");
        return -1;
    }

    G_SetFactionID(self->ent, faction_id);
    return 0;
}

static PyObject *PyEntity_get_vision_range(PyEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }
    return Py_BuildValue("f", G_GetVisionRange(self->ent));
}

static int PyEntity_set_vision_range(PyEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return -1;
    }

    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "vision_range attribute must be an float.");
        return -1;
    }

    G_SetVisionRange(self->ent, PyFloat_AS_DOUBLE(value));
    return 0;
}

static PyObject *PyEntity_get_tags(PyEntityObject *self, void *closure)
{
    const char *tags[MAX_TAGS];
    size_t ntags = Entity_TagsForEnt(self->ent, ARR_SIZE(tags), tags);

    PyObject *ret = PyTuple_New(ntags);
    if(!ret)
        return NULL;

    for(int i = 0; i < ntags; i++) {
        PyObject *str = PyString_FromString(tags[i]);
        if(!str) {
            Py_DECREF(ret);
            return NULL;
        }
        PyTuple_SET_ITEM(ret, i, str);
    }

    return ret;
}

static PyObject *PyEntity_get_bounds(PyEntityObject *self, void *closure)
{
    struct obb obb;
    Entity_CurrentOBB(self->ent, &obb, true);
    return Py_BuildValue("(fff)", 
        obb.half_lengths[0] * 2, 
        obb.half_lengths[1] * 2,
        obb.half_lengths[2] * 2);
}

static PyObject *PyEntity_register(PyEntityObject *self, PyObject *args)
{
    enum eventtype event;
    PyObject *callable, *user_arg;

    if(!PyArg_ParseTuple(args, "iOO", &event, &callable, &user_arg)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an integer and two objects.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Second argument must be callable.");
        return NULL;
    }

    Py_INCREF(callable);
    Py_INCREF(user_arg);

    bool ret = E_Entity_ScriptRegister(event, self->ent, callable, user_arg, G_RUNNING);
    if(!ret) {
        Py_DECREF(callable);
        Py_DECREF(user_arg);
        PyErr_SetString(PyExc_TypeError, "Unable to register the specified handler.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyEntity_unregister(PyEntityObject *self, PyObject *args)
{
    enum eventtype event;
    PyObject *callable;

    if(!PyArg_ParseTuple(args, "iO", &event, &callable)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must an integer and one object.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Second argument must be callable.");
        return NULL;
    }

    E_Entity_ScriptUnregister(event, self->ent, callable);
    Py_RETURN_NONE;
}

static PyObject *PyEntity_notify(PyEntityObject *self, PyObject *args)
{
    enum eventtype event;
    PyObject *arg;

    if(!PyArg_ParseTuple(args, "iO", &event, &arg)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an integer and one object.");
        return NULL;
    }

    Py_INCREF(arg);

    E_Entity_Notify(event, self->ent, arg, ES_SCRIPT);
    Py_RETURN_NONE;
}

static PyObject *PyEntity_select(PyEntityObject *self)
{
    assert(self->ent);
    G_Sel_Add(self->ent);
    Py_RETURN_NONE;
}

static PyObject *PyEntity_deselect(PyEntityObject *self)
{
    assert(self->ent);
    G_Sel_Remove(self->ent);
    Py_RETURN_NONE;
}

static PyObject *PyEntity_stop(PyEntityObject *self)
{
    assert(self->ent);
    G_StopEntity(self->ent, true);
    Py_RETURN_NONE;
}

static PyObject *PyEntity_face_towards(PyEntityObject *self, PyObject *args)
{
    vec3_t pos;
    if(!PyArg_ParseTuple(args, "(fff)", &pos.raw[0], &pos.raw[1], &pos.raw[2])) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of 3 floats (position to face).");
        return NULL;
    }

    Entity_FaceTowards(self->ent, (vec2_t){pos.x, pos.z});
    Py_RETURN_NONE;
}

static PyObject *PyEntity_set_model(PyEntityObject *self, PyObject *args)
{
    const char *dirpath, *filename;
    if(!PyArg_ParseTuple(args, "ss", &dirpath, &filename)) {
        PyErr_SetString(PyExc_TypeError, "Expecting two string arguments: directory path and PFOBJ file name.");
        return NULL;
    }

    struct entity *ent = AL_EntityGet(self->ent);
    assert(ent);

    if(!strcmp(ent->basedir, dirpath)
    && !strcmp(ent->filename, filename)) {
        Py_RETURN_NONE;
    }

    if(!AL_EntitySetPFObj(self->ent, dirpath, filename)) {
        PyErr_SetString(PyExc_RuntimeError, "Could not set the model to the specified PFOBJ file.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyEntity_ping(PyEntityObject *self)
{
    Entity_Ping(self->ent);
    Py_RETURN_NONE;
}

static PyObject *PyEntity_zombiefy(PyEntityObject *self)
{
    G_Zombiefy(self->ent, true);
    Py_RETURN_NONE;
}

static PyObject *PyEntity_add_tag(PyEntityObject *self, PyObject *args)
{
    const char *tag;
    if(!PyArg_ParseTuple(args, "s", &tag)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a string (tag) argument.");
        return NULL;
    }

    if(!Entity_AddTag(self->ent, tag)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to set tag for entity.");
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *PyEntity_remove_tag(PyEntityObject *self, PyObject *args)
{
    const char *tag;
    if(!PyArg_ParseTuple(args, "s", &tag)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a string (tag) argument.");
        return NULL;
    }

    Entity_RemoveTag(self->ent, tag);
    Py_RETURN_NONE;
}

static PyObject *PyEntity_pickle(PyEntityObject *self, PyObject *args, PyObject *kwargs)
{
    bool status;
    PyObject *ret = NULL;
    const struct entity *ent = AL_EntityGet(self->ent);
    assert(ent);

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);

    PyObject *basedir = PyString_FromString(ent->basedir);
    CHK_TRUE(basedir, fail_pickle);
    status = S_PickleObjgraph(basedir, stream);
    Py_DECREF(basedir);
    CHK_TRUE(status, fail_pickle);

    PyObject *filename = PyString_FromString(ent->filename);
    CHK_TRUE(filename, fail_pickle);
    status = S_PickleObjgraph(filename, stream);
    Py_DECREF(filename);
    CHK_TRUE(status, fail_pickle);

    PyObject *name = PyString_FromString(ent->name);
    CHK_TRUE(name, fail_pickle);
    status = S_PickleObjgraph(name, stream);
    Py_DECREF(name);
    CHK_TRUE(status, fail_pickle);

    PyObject *uid = PyInt_FromLong(self->ent);
    CHK_TRUE(uid, fail_pickle);
    status = S_PickleObjgraph(uid, stream);
    Py_DECREF(uid);
    CHK_TRUE(status, fail_pickle);

    vec3_t rawpos = G_Pos_Get(self->ent);
    PyObject *pos = Py_BuildValue("(fff)", rawpos.x, rawpos.y, rawpos.z);
    CHK_TRUE(pos, fail_pickle);
    status = S_PickleObjgraph(pos, stream);
    Py_DECREF(pos);
    CHK_TRUE(status, fail_pickle);

    vec3_t vscale = Entity_GetScale(self->ent);
    PyObject *scale = Py_BuildValue("(fff)", vscale.x, vscale.y, vscale.z);
    CHK_TRUE(scale, fail_pickle);
    status = S_PickleObjgraph(scale, stream);
    Py_DECREF(scale);
    CHK_TRUE(status, fail_pickle);

    quat_t qrot = Entity_GetRot(self->ent);
    PyObject *rotation = Py_BuildValue("(ffff)", qrot.x, qrot.y, qrot.z, qrot.w);
    CHK_TRUE(rotation, fail_pickle);
    status = S_PickleObjgraph(rotation, stream);
    Py_DECREF(rotation);
    CHK_TRUE(status, fail_pickle);

    PyObject *flags = Py_BuildValue("i", G_FlagsGet(self->ent));
    CHK_TRUE(flags, fail_pickle);
    status = S_PickleObjgraph(flags, stream);
    Py_DECREF(flags);
    CHK_TRUE(status, fail_pickle);

    PyObject *sel_radius = Py_BuildValue("f", G_GetSelectionRadius(self->ent));
    CHK_TRUE(sel_radius, fail_pickle);
    status = S_PickleObjgraph(sel_radius, stream);
    Py_DECREF(sel_radius);
    CHK_TRUE(status, fail_pickle);

    PyObject *faction_id = Py_BuildValue("i", G_GetFactionID(self->ent));
    CHK_TRUE(faction_id, fail_pickle);
    status = S_PickleObjgraph(faction_id, stream);
    Py_DECREF(faction_id);
    CHK_TRUE(status, fail_pickle);

    PyObject *vision_range = Py_BuildValue("f", G_GetVisionRange(self->ent));
    CHK_TRUE(vision_range, fail_pickle);
    status = S_PickleObjgraph(vision_range, stream);
    Py_DECREF(vision_range);
    CHK_TRUE(status, fail_pickle);

    PyObject *tags = PyEntity_get_tags(self, NULL);
    CHK_TRUE(tags, fail_pickle);
    status = S_PickleObjgraph(tags, stream);
    Py_DECREF(tags);
    CHK_TRUE(status, fail_pickle);

    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    return ret;
}

static PyObject *PyEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = NULL;
    const char *str;
    Py_ssize_t len;
    int status;
    char tmp;

    if(!PyArg_ParseTuple(args, "s#", &str, &len)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single string.");
        goto fail_args;
    }

    SDL_RWops *stream = SDL_RWFromConstMem(str, len);
    CHK_TRUE(stream, fail_args);

    PyObject *basedir = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *filename = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *name = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *uid = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    if(!basedir || !filename || !name || !uid) {
        PyErr_SetString(PyExc_RuntimeError, "Could not unpickle internal state of pf.Entity instance");
        goto fail_unpickle;
    }

    PyObject *pos = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *scale = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *rotation = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *flags = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *sel_radius = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *faction_id = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *vision_range = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *tags = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    if(!pos || !scale
    || !rotation || !flags 
    || !sel_radius || !faction_id
    || !vision_range || !tags) {

        PyErr_SetString(PyExc_RuntimeError, "Could not unpickle attributes of pf.Entity instance");
        goto fail_unpickle_atts;
    }

    PyObject *ent_args = Py_BuildValue("(OOO)", basedir, filename, name);
    PyObject *ent_kwargs = Py_BuildValue("{s:O, s:O}", "__uid__", uid, "__extra_flags__", flags);

    /* Use the 'plain' (i.e. direct successor with no extra overrides) as the arugment
     * to avoid calling any magic that might be risiding in the user-implemented __new__ 
     */
    PyTypeObject *heap_subtype = (PyTypeObject*)S_Pickle_PlainHeapSubtype((PyTypeObject*)cls);
    assert(heap_subtype->tp_new);

    PyObject *entobj = heap_subtype->tp_new((struct _typeobject*)cls, ent_args, ent_kwargs);
    assert(entobj || PyErr_Occurred());

    Py_DECREF(ent_args);
    Py_DECREF(ent_kwargs);
    CHK_TRUE(entobj, fail_unpickle);

    uint32_t ent = ((PyEntityObject*)entobj)->ent;
    vec3_t rawpos;
    CHK_TRUE(PyArg_ParseTuple(pos, "fff", &rawpos.x, &rawpos.y, &rawpos.z), fail_unpickle_atts);
    G_Pos_Set(ent, rawpos);

    vec3_t vscale;
    CHK_TRUE(PyArg_ParseTuple(scale, "fff", &vscale.x, &vscale.y, &vscale.z), fail_unpickle_atts);
    Entity_SetScale(ent, vscale);

    quat_t qrot;
    CHK_TRUE(PyArg_ParseTuple(rotation, "ffff", &qrot.x, &qrot.y, &qrot.z, &qrot.w), fail_unpickle_atts);
    Entity_SetRot(ent, qrot);

    status = PyObject_SetAttrString(entobj, "faction_id", faction_id);
    CHK_TRUE(0 == status, fail_unpickle_atts);

    status = PyObject_SetAttrString(entobj, "selection_radius", sel_radius);
    CHK_TRUE(0 == status, fail_unpickle_atts);

    if(!(G_FlagsGet(ent) & ENTITY_FLAG_ZOMBIE)) {
        status = PyObject_SetAttrString(entobj, "vision_range", vision_range);
        CHK_TRUE(0 == status, fail_unpickle_atts);

        CHK_TRUE(PyTuple_Check(tags), fail_unpickle_atts);
        for(int i = 0; i < PyTuple_GET_SIZE(tags); i++) {
            PyObject *tag = PyTuple_GET_ITEM(tags, i);
            CHK_TRUE(PyString_Check(tag), fail_unpickle_atts);
            Entity_AddTag(ent, PyString_AS_STRING(tag));
        }
    }

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", entobj, (int)nread);
    Py_DECREF(entobj);

fail_unpickle_atts:
    Py_XDECREF(pos);
    Py_XDECREF(scale);
    Py_XDECREF(rotation);
    Py_XDECREF(flags);
    Py_XDECREF(sel_radius);
    Py_XDECREF(faction_id);
    Py_XDECREF(vision_range);
    Py_XDECREF(tags);
fail_unpickle:
    Py_XDECREF(basedir);
    Py_XDECREF(filename);
    Py_XDECREF(name);
    Py_XDECREF(uid);
    SDL_RWclose(stream);
fail_args:
    return ret;
}

static PyObject *PyCombatableEntity_hold_position(PyCombatableEntityObject *self)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    G_StopEntity(self->super.ent, true);

    assert(G_FlagsGet(self->super.ent) & ENTITY_FLAG_COMBATABLE);
    G_Combat_SetStance(self->super.ent, COMBAT_STANCE_HOLD_POSITION);
    Py_RETURN_NONE;
}

static PyObject *PyCombatableEntity_attack(PyCombatableEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    vec2_t xz_pos;
    if(!PyArg_ParseTuple(args, "(ff)", &xz_pos.x, &xz_pos.z)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of 2 floats.");
        return NULL;
    }

    if(!G_PointInsideMap(xz_pos)) {
        PyErr_SetString(PyExc_RuntimeError, "The movement point must be within the map bounds.");
        return NULL;
    }

    assert(G_FlagsGet(self->super.ent) & ENTITY_FLAG_COMBATABLE);
    G_Combat_SetStance(self->super.ent, COMBAT_STANCE_AGGRESSIVE);

    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_MOVABLE) {
        G_Move_SetDest(self->super.ent, xz_pos, true);
    }

    Py_RETURN_NONE;
}

static PyObject *PyCombatableEntity_pickle(PyCombatableEntityObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = s_call_super_method("__pickle__", (PyObject*)&PyCombatableEntity_type, 
        (PyObject*)self, args, kwargs);

    assert(PyString_Check(ret));
    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_stream);
    CHK_TRUE(SDL_RWwrite(stream, PyString_AS_STRING(ret), PyString_GET_SIZE(ret), 1), fail_stream);

    PyObject *max_hp = NULL;
    PyObject *base_dmg = NULL;
    PyObject *base_armour = NULL;
    PyObject *curr_hp = NULL;
    PyObject *attack_range = NULL;

    if(!(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE)) {
    
        max_hp = PyInt_FromLong(G_Combat_GetMaxHP(self->super.ent));
        base_dmg = PyInt_FromLong(G_Combat_GetBaseDamage(self->super.ent));
        base_armour = PyFloat_FromDouble(G_Combat_GetBaseArmour(self->super.ent));
        curr_hp = PyInt_FromLong(G_Combat_GetCurrentHP(self->super.ent));
        attack_range = PyFloat_FromDouble(G_Combat_GetRange(self->super.ent));
        CHK_TRUE(max_hp && base_dmg && base_armour && curr_hp && attack_range, fail_pickle);

        bool status;
        status = S_PickleObjgraph(max_hp, stream);
        CHK_TRUE(status, fail_pickle);
        status = S_PickleObjgraph(base_dmg, stream);
        CHK_TRUE(status, fail_pickle);
        status = S_PickleObjgraph(base_armour, stream);
        CHK_TRUE(status, fail_pickle);
        status = S_PickleObjgraph(curr_hp, stream);
        CHK_TRUE(status, fail_pickle);
        status = S_PickleObjgraph(attack_range, stream);
        CHK_TRUE(status, fail_pickle);

        Py_DECREF(max_hp);
        Py_DECREF(base_dmg);
        Py_DECREF(base_armour);
        Py_DECREF(curr_hp);
        Py_DECREF(attack_range);
    }

    Py_DECREF(ret);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));
    SDL_RWclose(stream);
    return ret;

fail_pickle:
    Py_XDECREF(max_hp);
    Py_XDECREF(base_dmg);
    Py_XDECREF(base_armour);
    Py_XDECREF(curr_hp);
    Py_XDECREF(attack_range);
    SDL_RWclose(stream);
fail_stream:
    Py_XDECREF(ret);
    return NULL;
}

static PyObject *PyCombatableEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    char tmp;
    PyObject *max_hp = NULL, *base_dmg = NULL, *base_armour = NULL, *curr_hp = NULL, *attack_range = NULL;
    PyObject *ret = NULL;
    int status;

    PyObject *tuple = s_call_super_method("__unpickle__", (PyObject*)&PyCombatableEntity_type, cls, args, kwargs);
    if(!tuple)
        return NULL;

    PyObject *ent;
    int nread; 
    if(!PyArg_ParseTuple(tuple, "Oi", &ent, &nread)) {
        Py_DECREF(tuple);
        return NULL;
    }
    Py_INCREF(ent);
    Py_DECREF(tuple);

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_stream);

    PyObject *str = PyTuple_GET_ITEM(args, 0); /* borrowed */
    CHK_TRUE(SDL_RWwrite(stream, PyString_AS_STRING(str), PyString_GET_SIZE(str), 1), fail_unpickle);
    SDL_RWseek(stream, nread, RW_SEEK_SET);

    if(!(G_FlagsGet(((PyCombatableEntityObject*)ent)->super.ent) & ENTITY_FLAG_ZOMBIE)) {

        max_hp = S_UnpickleObjgraph(stream);
        SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

        status = PyObject_SetAttrString(ent, "max_hp", max_hp);
        CHK_TRUE(0 == status, fail_unpickle);

        base_dmg = S_UnpickleObjgraph(stream);
        SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

        status = PyObject_SetAttrString(ent, "base_dmg", base_dmg);
        CHK_TRUE(0 == status, fail_unpickle);

        base_armour = S_UnpickleObjgraph(stream);
        SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

        status = PyObject_SetAttrString(ent, "base_armour", base_armour);
        CHK_TRUE(0 == status, fail_unpickle);

        curr_hp = S_UnpickleObjgraph(stream);
        SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

        CHK_TRUE(PyInt_Check(curr_hp), fail_unpickle);
        G_Combat_SetCurrentHP(((PyEntityObject*)ent)->ent, PyInt_AS_LONG(curr_hp));

        attack_range = S_UnpickleObjgraph(stream);
        SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

        status = PyObject_SetAttrString(ent, "attack_range", attack_range);
        CHK_TRUE(0 == status, fail_unpickle);
    }

    nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("Oi", ent, nread);

fail_unpickle:
    Py_XDECREF(max_hp);
    Py_XDECREF(base_dmg);
    Py_XDECREF(base_armour);
    Py_XDECREF(curr_hp);
    Py_XDECREF(attack_range);
    SDL_RWclose(stream);
fail_stream:
    Py_DECREF(ent);
    return ret;
}

static int PyAnimEntity_init(PyAnimEntityObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *idle_clip;

    if(!kwds || ((idle_clip = PyDict_GetItemString(kwds, "idle_clip")) == NULL)) {
        PyErr_SetString(PyExc_TypeError, "'idle_clip' keyword argument required for initializing pf.AnimEntity types.");
        return -1;
    }

    if(!PyString_Check(idle_clip)) {
        PyErr_SetString(PyExc_TypeError, "'idle_clip' keyword argument must be a string.");
        return -1; 
    }

    if(!A_HasClip(self->super.ent, PyString_AS_STRING(idle_clip))) {

        const struct entity *ent = AL_EntityGet(self->super.ent);
        assert(ent);

        char errbuff[256];
        pf_snprintf(errbuff, sizeof(errbuff), "%s instance has no animation clip named '%s'.", 
            ent->filename, PyString_AS_STRING(idle_clip));
        PyErr_SetString(PyExc_RuntimeError, errbuff);
        return -1;
    }

    A_SetIdleClip(self->super.ent, PyString_AS_STRING(idle_clip), 24);

    /* Call the next __init__ method in the MRO. This is required for all __init__ calls in the 
     * MRO to complete in cases when this class is one of multiple base classes of another type. 
     * This allows this type to be used as one of many mix-in bases. */
    PyObject *ret = s_call_super_method("__init__", (PyObject*)&PyAnimEntity_type, (PyObject*)self, args, kwds);
    if(!ret)
        return -1; /* Exception already set */
    Py_DECREF(ret);
    return 0;
}

static PyObject *PyAnimEntity_del(PyAnimEntityObject *self)
{
    return s_super_del((PyObject*)self, &PyAnimEntity_type);
}

static PyObject *PyAnimEntity_play_anim(PyAnimEntityObject *self, PyObject *args, PyObject *kwds)
{
    const char *clipname;
    if(!PyArg_ParseTuple(args, "s", &clipname)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string.");
        return NULL;
    }

    enum anim_mode mode = ANIM_MODE_LOOP; /* default */
    PyObject *mode_obj;

    if(kwds && (mode_obj = PyDict_GetItemString(kwds, "mode"))) {
    
        if(!PyInt_Check(mode_obj)
        || (mode = PyInt_AS_LONG(mode_obj)) > ANIM_MODE_ONCE) {
        
            PyErr_SetString(PyExc_TypeError, "Mode kwarg must be a valid animation mode (int).");
            return NULL;
        }
    }

    if(!A_HasClip(self->super.ent, clipname)) {

        const struct entity *ent = AL_EntityGet(self->super.ent);
        assert(ent);

        char errbuff[256];
        pf_snprintf(errbuff, sizeof(errbuff), "%s instance has no animation clip named '%s'.", 
            ent->filename, clipname);
        PyErr_SetString(PyExc_RuntimeError, errbuff);
        return NULL;
    }

    A_SetActiveClip(self->super.ent, clipname, mode, 24);
    Py_RETURN_NONE;
}

static PyObject *PyAnimEntity_get_anim(PyAnimEntityObject *self)
{
    return PyString_FromString(A_GetCurrClip(self->super.ent));
}

static PyObject *PyAnimEntity_pickle(PyAnimEntityObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = s_call_super_method("__pickle__", (PyObject*)&PyAnimEntity_type, 
        (PyObject*)self, args, kwargs);

    assert(PyString_Check(ret));
    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_stream);
    CHK_TRUE(SDL_RWwrite(stream, PyString_AS_STRING(ret), PyString_GET_SIZE(ret), 1), fail_stream);

    PyObject *idle_clip = PyString_FromString(A_GetIdleClip(self->super.ent));
    CHK_TRUE(idle_clip, fail_pickle);
    bool status = S_PickleObjgraph(idle_clip, stream);
    Py_DECREF(idle_clip);
    CHK_TRUE(status, fail_pickle);

    Py_DECREF(ret);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));
    SDL_RWclose(stream);
    return ret;

fail_pickle:
    SDL_RWclose(stream);
fail_stream:
    Py_XDECREF(ret);
    return NULL;
}

static PyObject *PyAnimEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    char tmp;
    PyObject *ret = NULL;

    PyObject *tuple = s_call_super_method("__unpickle__", (PyObject*)&PyAnimEntity_type, cls, args, kwargs);
    if(!tuple)
        return NULL;

    PyObject *ent;
    int nread; 
    if(!PyArg_ParseTuple(tuple, "Oi", &ent, &nread)) {
        Py_DECREF(tuple);
        return NULL;
    }
    Py_INCREF(ent);
    Py_DECREF(tuple);

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_stream);

    PyObject *str = PyTuple_GET_ITEM(args, 0); /* borrowed */
    CHK_TRUE(SDL_RWwrite(stream, PyString_AS_STRING(str), PyString_GET_SIZE(str), 1), fail_unpickle);

    SDL_RWseek(stream, nread, RW_SEEK_SET);
    PyObject *idle_clip = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */
    CHK_TRUE(idle_clip, fail_unpickle);
    CHK_TRUE(PyString_Check(idle_clip), fail_parse);

    nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    A_SetIdleClip(((PyAnimEntityObject*)ent)->super.ent, PyString_AS_STRING(idle_clip), 24);

    ret = Py_BuildValue("Oi", ent, nread);

fail_parse:
    Py_DECREF(idle_clip);
fail_unpickle:
    SDL_RWclose(stream);
fail_stream:
    Py_DECREF(ent);
    return ret;
}

static int PyCombatableEntity_init(PyCombatableEntityObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *max_hp, *base_dmg, *base_armour;
    assert(G_FlagsGet(self->super.ent) & ENTITY_FLAG_COMBATABLE);

    if(!kwds 
    || ((max_hp = PyDict_GetItemString(kwds, "max_hp")) == NULL)
    || ((base_dmg = PyDict_GetItemString(kwds, "base_dmg")) == NULL)
    || ((base_armour = PyDict_GetItemString(kwds, "base_armour")) == NULL)) {
        PyErr_SetString(PyExc_TypeError, "'max_hp', 'base_dmg', and 'base_armour' keyword arguments "
            "required for initializing pf.CombatableEntity types.");
        return -1;
    }

    PyObject *attack_range = PyDict_GetItemString(kwds, "attack_range");
    if(attack_range && (PyCombatableEntity_set_attack_range(self, attack_range, NULL) != 0))
        return -1; /* Exception already set */ 

    PyObject *proj_desc = PyDict_GetItemString(kwds, "projectile_descriptor");
    if(proj_desc) {

        const char *dir, *pfobj;
        vec3_t scale;
        float speed;

        if(!PyTuple_Check(proj_desc) || !PyArg_ParseTuple(proj_desc, "ss(fff)f", 
            &dir, &pfobj, &scale.x, &scale.y, &scale.z, &speed)) {
            PyErr_SetString(PyExc_TypeError, "Optional 'projectile_descriptor' keyword argument must be a tuple of 4 items: "
                "pfobj directory (string), pfobj name (string), scale (tuple of 3 floats), speed (float).");
            return -1;
        }
        struct proj_desc pd = (struct proj_desc) {
            .basedir = dir,
            .pfobj = pfobj,
            .scale = scale,
            .speed = speed,
        };
        G_Combat_SetProjDesc(self->super.ent, &pd);
    }

    if((PyCombatableEntity_set_max_hp(self, max_hp, NULL) != 0)
    || (PyCombatableEntity_set_base_dmg(self, base_dmg, NULL) != 0)
    || (PyCombatableEntity_set_base_armour(self, base_armour, NULL) != 0))
        return -1; /* Exception already set */ 

    G_Combat_SetCurrentHP(self->super.ent, PyInt_AS_LONG(max_hp));

    /* Call the next __init__ method in the MRO. This is required for all __init__ calls in the 
     * MRO to complete in cases when this class is one of multiple base classes of another type. 
     * This allows this type to be used as one of many mix-in bases. */
    PyObject *ret = s_call_super_method("__init__", (PyObject*)&PyCombatableEntity_type, (PyObject*)self, args, kwds);
    if(!ret)
        return -1; /* Exception already set */
    Py_DECREF(ret);
    return 0;
}

static PyObject *PyCombatableEntity_del(PyCombatableEntityObject *self)
{
    return s_super_del((PyObject*)self, &PyCombatableEntity_type);
}

static PyObject *PyCombatableEntity_get_hp(PyCombatableEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }
    return PyInt_FromLong(G_Combat_GetCurrentHP(self->super.ent));
}

static int PyCombatableEntity_set_hp(PyCombatableEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return -1;
    }

    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "hp attribute must be an integer.");
        return -1;
    }
    
    int hp = PyInt_AS_LONG(value);
    if(hp <= 0) {
        PyErr_SetString(PyExc_RuntimeError, "hp must be greater than 0.");
        return -1;
    }

    G_Combat_SetCurrentHP(self->super.ent, hp);
    return 0;
}

static PyObject *PyCombatableEntity_get_max_hp(PyCombatableEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    int max_hp = G_Combat_GetMaxHP(self->super.ent);
    return Py_BuildValue("i", max_hp);
}

static int PyCombatableEntity_set_max_hp(PyCombatableEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return -1;
    }

    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "max_hp attribute must be an integer.");
        return -1;
    }
    
    int max_hp = PyInt_AS_LONG(value);
    if(max_hp < 0) {
        PyErr_SetString(PyExc_RuntimeError, "max_hp must be greater or equal to 0 (0 = invulnerable).");
        return -1;
    }

    G_Combat_SetMaxHP(self->super.ent, max_hp);
    return 0;
}

static PyObject *PyCombatableEntity_get_base_dmg(PyCombatableEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }
    return PyInt_FromLong(G_Combat_GetBaseDamage(self->super.ent));
}

static int PyCombatableEntity_set_base_dmg(PyCombatableEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return -1;
    }

    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "base_dmg attribute must be an integer.");
        return -1;
    }
    
    int base_dmg = PyInt_AS_LONG(value);
    if(base_dmg < 0) {
        PyErr_SetString(PyExc_RuntimeError, "base_dmg must be greater than or equal to 0.");
        return -1;
    }

    G_Combat_SetBaseDamage(self->super.ent, base_dmg);
    return 0;
}

static PyObject *PyCombatableEntity_get_base_armour(PyCombatableEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    return PyFloat_FromDouble(G_Combat_GetBaseArmour(self->super.ent));
}

static int PyCombatableEntity_set_base_armour(PyCombatableEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return -1;
    }

    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "base_armour attribute must be a float.");
        return -1;
    }
    
    float base_armour = PyFloat_AS_DOUBLE(value);
    if(base_armour < 0.0f || base_armour > 1.0f) {
        PyErr_SetString(PyExc_RuntimeError, "base_armour must be in the range of [0.0, 1.0].");
        return -1;
    }

    G_Combat_SetBaseArmour(self->super.ent, base_armour);
    return 0;
}

static PyObject *PyCombatableEntity_get_attack_range(PyCombatableEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    return PyFloat_FromDouble(G_Combat_GetRange(self->super.ent));
}

static int PyCombatableEntity_set_attack_range(PyCombatableEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return -1;
    }

    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "attack_range attribute must be a float.");
        return -1;
    }

    float range = PyFloat_AS_DOUBLE(value);
    if(range < 0.0f) {
        PyErr_SetString(PyExc_TypeError, "attack_range attribute must be a positive value.");
        return -1;
    }

    G_Combat_SetRange(self->super.ent, range);
    return 0;
}

static int PyBuildableEntity_init(PyBuildableEntityObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *required_resources;

    if(!kwds || ((required_resources = PyDict_GetItemString(kwds, "required_resources")) == NULL)) {
        PyErr_SetString(PyExc_TypeError, 
            "'required_resources' keyword argument required for initializing pf.BuildableEntity types.");
        return -1;
    }

    if(!PyDict_Check(required_resources))
        goto fail_type;

    PyObject *key, *value;
    Py_ssize_t pos = 0;

    while (PyDict_Next(required_resources, &pos, &key, &value)) {

        if(!PyString_Check(key))
            goto fail_type;
        if(!PyInt_Check(value))
            goto fail_type;

        const char *rname = PyString_AS_STRING(key);
        int ramount = PyInt_AS_LONG(value);
        G_Building_SetRequired(self->super.ent, rname, ramount);
    }

    /* Call the next __init__ method in the MRO. This is required for all __init__ calls in the 
     * MRO to complete in cases when this class is one of multiple base classes of another type. 
     * This allows this type to be used as one of many mix-in bases. */
    PyObject *ret = s_call_super_method("__init__", (PyObject*)&PyBuildableEntity_type, 
        (PyObject*)self, args, kwds);
    if(!ret)
        return -1; /* Exception already set */
    Py_DECREF(ret);
    return 0;

fail_type:
    PyErr_SetString(PyExc_TypeError, 
        "'required_resources' must be a dictionary mapping strings to integers.");
    return -1;
}

static PyObject *PyBuildableEntity_del(PyBuildableEntityObject *self)
{
    return s_super_del((PyObject*)self, &PyBuildableEntity_type);
}

static PyObject *PyBuildableEntity_mark(PyBuildableEntityObject *self)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    if(!G_Building_Mark(self->super.ent)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to mark building. It must be in the PLACEMENT state.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyBuildableEntity_found(PyBuildableEntityObject *self, PyObject *args, PyObject *kwargs)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    static char *kwlist[] = {"blocking", "force", NULL};
    int blocking = 1;
    int force = 0;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "|ii", kwlist, &blocking, &force)) {
        PyErr_SetString(PyExc_TypeError, "Two (optional) arguments: blocking (int) and force (int)");
        return NULL;
    }

    if(!force && !G_Building_Unobstructed(self->super.ent)) {
        PyErr_SetString(PyExc_RuntimeError, "The tiles under the building must not be obstructed by any objects.");
        return NULL;
    }

    if(!G_Building_Found(self->super.ent, blocking)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to found building. It must be in the MARKED state.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyBuildableEntity_supply(PyBuildableEntityObject *self)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    if(!G_Building_Supply(self->super.ent)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to supply building. It must be in the FOUNDED state.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyBuildableEntity_complete(PyBuildableEntityObject *self)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    if(!G_Building_Complete(self->super.ent)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to complete building. It must be in the SUPPLIED state.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyBuildableEntity_unobstructed(PyBuildableEntityObject *self)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    if(G_Building_Unobstructed(self->super.ent)) {
        Py_RETURN_TRUE;
    }else{
        Py_RETURN_FALSE;
    }
}

static PyObject *PyBuildableEntity_get_pos(PyBuildableEntityObject *self, void *closure)
{
    vec3_t pos = G_Pos_Get(self->super.ent);
    return Py_BuildValue("(f,f,f)", pos.x, pos.y, pos.z);
}

static int PyBuildableEntity_set_pos(PyBuildableEntityObject *self, PyObject *value, void *closure)
{
    if(!PyTuple_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple.");
        return -1;
    }
    
    vec3_t newpos;
    if(!PyArg_ParseTuple(value, "fff", 
        &newpos.raw[0], &newpos.raw[1], &newpos.raw[2])) {
        return -1;
    }

    newpos.x -= fmod(newpos.x, X_COORDS_PER_TILE / 2.0);
    newpos.z -= fmod(newpos.z, Z_COORDS_PER_TILE / 2.0);

    G_Pos_Set(self->super.ent, newpos);
    return 0;
}

static PyObject *PyBuildableEntity_get_founded(PyBuildableEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot access attribute of zombie entity.");
        return NULL;
    }
    if(G_Building_IsFounded(self->super.ent)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *PyBuildableEntity_get_supplied(PyBuildableEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot access attribute of zombie entity.");
        return NULL;
    }
    if(G_Building_IsSupplied(self->super.ent)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *PyBuildableEntity_get_completed(PyBuildableEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot access attribute of zombie entity.");
        return NULL;
    }
    if(G_Building_IsCompleted(self->super.ent)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *PyBuildableEntity_get_vision_range(PyBuildableEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot access attribute of zombie entity.");
        return NULL;
    }
    return Py_BuildValue("f", G_Building_GetVisionRange(self->super.ent));
}

static int PyBuildableEntity_set_vision_range(PyBuildableEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot access attribute of zombie entity.");
        return -1;
    }

    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "vision_range attribute must be an float.");
        return -1;
    }

    G_Building_SetVisionRange(self->super.ent, PyFloat_AS_DOUBLE(value));
    return 0;
}

static PyObject *PyBuildableEntity_get_required_resources(PyBuildableEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot access attribute of zombie entity.");
        return NULL;
    }

    const size_t max = 64;
    STALLOC(char*, names, max);
    STALLOC(int, amounts, max);

    size_t nreq = G_Building_GetAllRequired(self->super.ent, max, (const char**)names, amounts);
    PyObject *ret = PyDict_New();
    if(!ret)
        return NULL; 

    for(int i = 0; i < nreq; i++) {
        PyObject *amount = PyInt_FromLong(amounts[i]);
        if(!amount) {
            Py_CLEAR(ret);
            goto out;
        }
        if(!PyDict_SetItemString(ret, names[i], amount)) {
            Py_CLEAR(ret);
            goto out;
        }
    }
out:
    STFREE(names);
    STFREE(amounts);
    return ret;
}

static PyObject *PyBuildableEntity_pickle(PyBuildableEntityObject *self, PyObject *args, PyObject *kwargs)
{
    return s_call_super_method("__pickle__", (PyObject*)&PyBuildableEntity_type, 
        (PyObject*)self, args, kwargs);
}

static PyObject *PyBuildableEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    return s_call_super_method("__unpickle__", (PyObject*)&PyBuildableEntity_type, 
        cls, args, kwargs);
}

static PyObject *PyBuilderEntity_del(PyBuilderEntityObject *self)
{
    return s_super_del((PyObject*)self, &PyBuilderEntity_type);
}

static int PyBuilderEntity_init(PyBuilderEntityObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *build_speed;

    if(!kwds || ((build_speed = PyDict_GetItemString(kwds, "build_speed")) == NULL)) {
        PyErr_SetString(PyExc_TypeError, 
            "'build_speed' keyword argument required for initializing pf.BuilderEntity types.");
        return -1;
    }

    if(!PyInt_Check(build_speed)) {
        PyErr_SetString(PyExc_TypeError, "'build_speed' keyword argument must be an integer.");
        return -1; 
    }

    G_Builder_SetBuildSpeed(self->super.ent, PyInt_AS_LONG(build_speed));

    /* Call the next __init__ method in the MRO. This is required for all __init__ calls in the 
     * MRO to complete in cases when this class is one of multiple base classes of another type. 
     * This allows this type to be used as one of many mix-in bases. */
    PyObject *ret = s_call_super_method("__init__", (PyObject*)&PyBuilderEntity_type, (PyObject*)self, args, kwds);
    if(!ret)
        return -1; /* Exception already set */
    Py_DECREF(ret);
    return 0;
}

static PyObject *PyBuilderEntity_build(PyBuilderEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    PyObject *building;
    if(!PyArg_ParseTuple(args, "O", &building)
    || !PyObject_IsInstance(building, (PyObject*)&PyBuildableEntity_type)) {
        PyErr_SetString(PyExc_TypeError, "Expecting 1 argument: a pf.BuildableEntity instance");
        return NULL;
    }

    G_Builder_Build(self->super.ent, ((PyBuildableEntityObject*)building)->super.ent);
    Py_RETURN_NONE;
}

static PyObject *PyBuilderEntity_pickle(PyBuilderEntityObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = s_call_super_method("__pickle__", (PyObject*)&PyBuilderEntity_type, 
        (PyObject*)self, args, kwargs);

    assert(PyString_Check(ret));
    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_stream);
    CHK_TRUE(SDL_RWwrite(stream, PyString_AS_STRING(ret), PyString_GET_SIZE(ret), 1), fail_stream);

    if(!(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE)) {
    
        PyObject *build_speed = PyInt_FromLong(G_Builder_GetBuildSpeed(self->super.ent));
        CHK_TRUE(build_speed, fail_pickle);
        bool status = S_PickleObjgraph(build_speed, stream);
        Py_DECREF(build_speed);
        CHK_TRUE(status, fail_pickle);
    }

    Py_DECREF(ret);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));
    SDL_RWclose(stream);
    return ret;

fail_pickle:
    SDL_RWclose(stream);
fail_stream:
    Py_XDECREF(ret);
    PyErr_SetString(PyExc_RuntimeError, "Unable to pickle pf.BuilderEntity state");
    return NULL;
}

static PyObject *PyBuilderEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    char tmp;
    PyObject *ret = NULL;

    PyObject *tuple = s_call_super_method("__unpickle__", (PyObject*)&PyBuilderEntity_type, cls, args, kwargs);
    if(!tuple)
        return NULL;

    PyObject *ent;
    int nread; 
    if(!PyArg_ParseTuple(tuple, "Oi", &ent, &nread)) {
        Py_DECREF(tuple);
        return NULL;
    }
    Py_INCREF(ent);
    Py_DECREF(tuple);

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_stream);

    PyObject *str = PyTuple_GET_ITEM(args, 0); /* borrowed */
    CHK_TRUE(SDL_RWwrite(stream, PyString_AS_STRING(str), PyString_GET_SIZE(str), 1), fail_unpickle);
    PyObject *build_speed = NULL;

    if(!(G_FlagsGet(((PyBuilderEntityObject*)ent)->super.ent) & ENTITY_FLAG_ZOMBIE)) {

        SDL_RWseek(stream, nread, RW_SEEK_SET);
        build_speed = S_UnpickleObjgraph(stream);
        SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */
        CHK_TRUE(build_speed, fail_unpickle);
        CHK_TRUE(PyInt_Check(build_speed), fail_parse);

        nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
        G_Builder_SetBuildSpeed(((PyBuilderEntityObject*)ent)->super.ent, PyInt_AS_LONG(build_speed));
    }

    ret = Py_BuildValue("Oi", ent, nread);

fail_parse:
    Py_XDECREF(build_speed);
fail_unpickle:
    SDL_RWclose(stream);
fail_stream:
    Py_DECREF(ent);
    if(!ret && !PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to unpickle pf.BuilderEntity state");
    }
    return ret;
}

static PyObject *PyResourceEntity_del(PyResourceEntityObject *self)
{
    return s_super_del((PyObject*)self, &PyResourceEntity_type);
}

static int PyResourceEntity_init(PyResourceEntityObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *name, *amount;

    if(!kwds 
    || ((name = PyDict_GetItemString(kwds, "resource_name")) == NULL)
    || ((amount = PyDict_GetItemString(kwds, "resource_amount")) == NULL)) {
        PyErr_SetString(PyExc_TypeError, 
            "'resource_name' and 'resource_amount' keyword arguments required for initializing pf.ResourceEntity types.");
        return -1;
    }

    if(!PyString_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "'resource_name' keyword argument must be a string.");
        return -1;
    }

    if(!PyInt_Check(amount)) {
        PyErr_SetString(PyExc_TypeError, "'resource_amount' keyword argument must be a string.");
        return -1;
    }

    G_Resource_SetName(self->super.ent, PyString_AS_STRING(name));
    G_Resource_SetAmount(self->super.ent, PyInt_AS_LONG(amount));

    /* Call the next __init__ method in the MRO. This is required for all __init__ calls in the 
     * MRO to complete in cases when this class is one of multiple base classes of another type. 
     * This allows this type to be used as one of many mix-in bases. */
    PyObject *ret = s_call_super_method("__init__", (PyObject*)&PyResourceEntity_type, (PyObject*)self, args, kwds);
    if(!ret)
        return -1; /* Exception already set */
    Py_DECREF(ret);
    return 0;
}

static PyObject *PyResourceEntity_pickle(PyResourceEntityObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = s_call_super_method("__pickle__", (PyObject*)&PyResourceEntity_type, 
        (PyObject*)self, args, kwargs);

    assert(PyString_Check(ret));
    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_stream);
    CHK_TRUE(SDL_RWwrite(stream, PyString_AS_STRING(ret), PyString_GET_SIZE(ret), 1), fail_stream);

    if(!(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE)) {
    
        PyObject *name = PyString_FromString(G_Resource_GetName(self->super.ent));
        CHK_TRUE(name, fail_pickle);
        bool status = S_PickleObjgraph(name, stream);
        Py_DECREF(name);
        CHK_TRUE(status, fail_pickle);

        PyObject *amount = PyInt_FromLong(G_Resource_GetAmount(self->super.ent));
        CHK_TRUE(amount, fail_pickle);
        status = S_PickleObjgraph(amount, stream);
        Py_DECREF(amount);
        CHK_TRUE(status, fail_pickle);
    }

    Py_DECREF(ret);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));
    SDL_RWclose(stream);
    return ret;

fail_pickle:
    SDL_RWclose(stream);
fail_stream:
    Py_XDECREF(ret);
    return NULL;
}

static PyObject *PyResourceEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    char tmp;
    PyObject *ret = NULL;

    PyObject *tuple = s_call_super_method("__unpickle__", (PyObject*)&PyResourceEntity_type, 
        cls, args, kwargs);
    if(!tuple)
        return NULL;

    PyObject *ent;
    int nread; 
    if(!PyArg_ParseTuple(tuple, "Oi", &ent, &nread)) {
        Py_DECREF(tuple);
        return NULL;
    }
    Py_INCREF(ent);
    Py_DECREF(tuple);

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_stream);

    PyObject *str = PyTuple_GET_ITEM(args, 0); /* borrowed */
    CHK_TRUE(SDL_RWwrite(stream, PyString_AS_STRING(str), PyString_GET_SIZE(str), 1), fail_unpickle);
    SDL_RWseek(stream, nread, RW_SEEK_SET);

    PyObject *name = NULL;
    PyObject *amount = NULL;

    if(!(G_FlagsGet(((PyResourceEntityObject*)ent)->super.ent) & ENTITY_FLAG_ZOMBIE)) {

        name = S_UnpickleObjgraph(stream);
        SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */
        CHK_TRUE(name, fail_unpickle);
        CHK_TRUE(PyString_Check(name), fail_name);
        G_Resource_SetName(((PyResourceEntityObject*)ent)->super.ent, PyString_AS_STRING(name));

        amount = S_UnpickleObjgraph(stream);
        SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */
        CHK_TRUE(amount, fail_name);
        CHK_TRUE(PyInt_Check(amount), fail_amount);
        G_Resource_SetAmount(((PyResourceEntityObject*)ent)->super.ent, PyInt_AS_LONG(amount));
    }

    nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("Oi", ent, nread);

fail_amount:
    Py_XDECREF(amount);
fail_name:
    Py_XDECREF(name);
fail_unpickle:
    SDL_RWclose(stream);
fail_stream:
    Py_DECREF(ent);
    return ret;
}

static PyObject *PyResourceEntity_get_cursor(PyResourceEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    return Py_BuildValue("s", G_Resource_GetCursor(self->super.ent));
}

static int PyResourceEntity_set_cursor(PyResourceEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return -1;
    }

    if(value == NULL) {
        PyErr_SetString(PyExc_AttributeError, "Cannot delete 'cursor' attribute.");
        return -1;
    }

    if(!PyObject_IsInstance(value, (PyObject*)&PyString_Type)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string.");
        return -1;
    }

    const char *s = PyString_AsString(value);
    G_Resource_SetCursor(self->super.ent, s);
    return 0;
}

static PyObject *PyResourceEntity_get_name(PyResourceEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    return PyString_FromString(G_Resource_GetName(self->super.ent));
}

static PyObject *PyResourceEntity_get_amount(PyResourceEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    return PyInt_FromLong(G_Resource_GetAmount(self->super.ent));
}

static int PyResourceEntity_set_amount(PyResourceEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return -1;
    }

    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer.");
        return -1;
    }

    G_Resource_SetAmount(self->super.ent, PyInt_AS_LONG(value));
    return 0;
}

static PyObject *PyHarvesterEntity_del(PyHarvesterEntityObject *self)
{
    return s_super_del((PyObject*)self, &PyHarvesterEntity_type);
}

static PyObject *PyHarvesterEntity_gather(PyHarvesterEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    PyResourceEntityObject *resource;
    if(!PyArg_ParseTuple(args, "O", &resource)
    || !PyObject_IsInstance((PyObject*)resource, (PyObject*)&PyResourceEntity_type)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a pf.ResourceEntity instance.");
        return NULL;
    }

    G_StopEntity(self->super.ent, true);
    if(!G_Harvester_Gather(self->super.ent, resource->super.ent)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to gather the specified resource.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyHarvesterEntity_drop_off(PyHarvesterEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    PyStorageSiteEntityObject *storage;
    if(!PyArg_ParseTuple(args, "O", &storage)
    || !PyObject_IsInstance((PyObject*)storage, (PyObject*)&PyStorageSiteEntity_type)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a pf.StorageSiteEntity instance.");
        return NULL;
    }

    G_StopEntity(self->super.ent, true);
    if(!G_Harvester_DropOff(self->super.ent, storage->super.ent)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to drop off resource at the specified storage site.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyHarvesterEntity_transport(PyHarvesterEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    PyStorageSiteEntityObject *storage;
    if(!PyArg_ParseTuple(args, "O", &storage)
    || !PyObject_IsInstance((PyObject*)storage, (PyObject*)&PyStorageSiteEntity_type)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a pf.StorageSiteEntity instance.");
        return NULL;
    }

    G_StopEntity(self->super.ent, true);
    if(!G_Harvester_Transport(self->super.ent, storage->super.ent)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to transport resources to the specified storage site.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyHarvesterEntity_get_curr_carry(PyHarvesterEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return NULL;
    }

    const char *rname;
    if(!PyArg_ParseTuple(args, "s", &rname)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one arguments: resource name (string).");
        return NULL;
    }

    int ret = G_Harvester_GetCurrCarry(self->super.ent, rname);
    return PyInt_FromLong(ret);
}

static PyObject *PyHarvesterEntity_clear_curr_carry(PyHarvesterEntityObject *self)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot invoke method of zombie entity.");
        return NULL;
    }

    G_Harvester_ClearCurrCarry(self->super.ent);
    Py_RETURN_NONE;
}

static PyObject *PyHarvesterEntity_get_max_carry(PyHarvesterEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return NULL;
    }

    const char *rname;
    if(!PyArg_ParseTuple(args, "s", &rname)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one arguments: resource name (string).");
        return NULL;
    }

    int ret = G_Harvester_GetMaxCarry(self->super.ent, rname);
    return PyInt_FromLong(ret);
}

static PyObject *PyHarvesterEntity_set_max_carry(PyHarvesterEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return NULL;
    }

    const char *name;
    int amount;

    if(!PyArg_ParseTuple(args, "si", &name, &amount)) {
        PyErr_SetString(PyExc_TypeError, "Expecting two arguments: name (string) and amount (integer).");
        return NULL;
    }

    if(!G_Harvester_SetMaxCarry(self->super.ent, name, amount)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to set the max carry amount.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyHarvesterEntity_get_gather_speed(PyHarvesterEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return NULL;
    }

    const char *rname;
    if(!PyArg_ParseTuple(args, "s", &rname)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one arguments: resource name (string).");
        return NULL;
    }

    float ret = G_Harvester_GetGatherSpeed(self->super.ent, rname);
    return PyFloat_FromDouble(ret);
}

static PyObject *PyHarvesterEntity_set_gather_speed(PyHarvesterEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return NULL;
    }

    const char *name;
    float amount;

    if(!PyArg_ParseTuple(args, "sf", &name, &amount)) {
        PyErr_SetString(PyExc_TypeError, "Expecting two arguments: name (string) and amount (float).");
        return NULL;
    }

    if(!G_Harvester_SetGatherSpeed(self->super.ent, name, amount)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to set the gathering speed.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyHarvesterEntity_increase_transport_priority(PyHarvesterEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a string arument (resource name).");
        return NULL;
    }

    if(G_Harvester_IncreaseTransportPrio(self->super.ent, name)) {
        Py_RETURN_TRUE;
    }else {
        Py_RETURN_FALSE;
    }
}

static PyObject *PyHarvesterEntity_decrease_transport_priority(PyHarvesterEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a string arument (resource name).");
        return NULL;
    }

    if(G_Harvester_DecreaseTransportPrio(self->super.ent, name)) {
        Py_RETURN_TRUE;
    }else {
        Py_RETURN_FALSE;
    }
}

static PyObject *PyHarvesterEntity_pickle(PyHarvesterEntityObject *self, PyObject *args, PyObject *kwargs)
{
    return s_call_super_method("__pickle__", (PyObject*)&PyHarvesterEntity_type, 
        (PyObject*)self, args, kwargs);
}

static PyObject *PyHarvesterEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    return s_call_super_method("__unpickle__", (PyObject*)&PyHarvesterEntity_type, cls, args, kwargs);
}

static PyObject *PyHarvesterEntity_get_total_carry(PyHarvesterEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    int total = G_Harvester_GetCurrTotalCarry(self->super.ent);
    return PyInt_FromLong(total);
}

static PyObject *PyHarvesterEntity_get_transport_priority(PyHarvesterEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    const char *names[64];
    int nres = G_Harvester_GetTransportPrio(self->super.ent, ARR_SIZE(names), names);

    PyObject *ret = PyList_New(nres);
    if(!ret)
        return NULL;

    for(int i = 0; i < nres; i++) {
        PyObject *str = PyString_FromString(names[i]);
        if(!str) {
            Py_DECREF(ret);
            return NULL;
        }
        PyList_SetItem(ret, i, str);
    }
    return ret;
}

static PyObject *PyHarvesterEntity_get_strategy(PyHarvesterEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    enum tstrategy strat = G_Harvester_GetStrategy(self->super.ent);
    return PyInt_FromLong(strat);
}

static int PyHarvesterEntity_set_strategy(PyHarvesterEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return -1;
    }

    if(!PyInt_Check(value)
    || (PyInt_AS_LONG(value) > TRANSPORT_STRATEGY_GATHERING)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a pf.TRANSPORT_ enum value.");
        return -1;
    }

    G_Harvester_SetStrategy(self->super.ent, PyInt_AS_LONG(value));
    return 0;
}

static PyObject *PyStorageSiteEntity_del(PyStorageSiteEntityObject *self)
{
    return s_super_del((PyObject*)self, &PyStorageSiteEntity_type);
}

static PyObject *PyStorageSiteEntity_get_curr_amount(PyStorageSiteEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one argument: name (string).");
        return NULL;
    }

    int curr = G_StorageSite_GetCurr(self->super.ent, name);
    return PyInt_FromLong(curr);
}

static PyObject *PyStorageSiteEntity_set_curr_amount(PyStorageSiteEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    const char *name;
    int amount;
    if(!PyArg_ParseTuple(args, "si", &name, &amount)) {
        PyErr_SetString(PyExc_TypeError, "Expecting two argument: name (string) and amount (int).");
        return NULL;
    }

    if(!G_StorageSite_SetCurr(self->super.ent, name, amount)) {
        char buff[256];
        pf_snprintf(buff, sizeof(buff), "Unable to set amount (%d) for resource (%s).", amount, name);
        PyErr_SetString(PyExc_RuntimeError, buff);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *PyStorageSiteEntity_get_capacity(PyStorageSiteEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one argument: name (string).");
        return NULL;
    }

    int cap = G_StorageSite_GetCapacity(self->super.ent, name);
    return PyInt_FromLong(cap);
}

static PyObject *PyStorageSiteEntity_set_capacity(PyStorageSiteEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return NULL;
    }

    const char *name;
    int amount;

    if(!PyArg_ParseTuple(args, "si", &name, &amount)) {
        PyErr_SetString(PyExc_TypeError, "Expecting two arguments: name (string) and amount (integer).");
        return NULL;
    }

    if(!G_StorageSite_SetCapacity(self->super.ent, name, amount)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to set the resource capacity.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyStorageSiteEntity_get_desired(PyStorageSiteEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one argument: name (string).");
        return NULL;
    }

    int cap = G_StorageSite_GetDesired(self->super.ent, name);
    return PyInt_FromLong(cap);
}

static PyObject *PyStorageSiteEntity_set_desired(PyStorageSiteEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return NULL;
    }

    const char *name;
    int amount;

    if(!PyArg_ParseTuple(args, "si", &name, &amount)) {
        PyErr_SetString(PyExc_TypeError, "Expecting two arguments: name (string) and amount (integer).");
        return NULL;
    }

    if(!G_StorageSite_SetDesired(self->super.ent, name, amount)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to set the resource capacity.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyStorageSiteEntity_get_do_not_take(PyStorageSiteEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    bool do_not_take = G_StorageSite_GetDoNotTake(self->super.ent);
    if(do_not_take) {
        Py_RETURN_TRUE;
    }else{
        Py_RETURN_FALSE;
    }
}

static int PyStorageSiteEntity_set_do_not_take(PyStorageSiteEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return -1;
    }

    bool do_not_take = PyObject_IsTrue(value);
    G_StorageSite_SetDoNotTake(self->super.ent, do_not_take);
    return 0;
}

static PyObject *PyStorageSiteEntity_get_storable(PyStorageSiteEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot set attribute of zombie entity.");
        return NULL;
    }

    const char *names[64];
    int nres = G_StorageSite_GetStorableResources(self->super.ent, ARR_SIZE(names), names);

    PyObject *ret = PyList_New(nres);
    if(!ret)
        return NULL;

    for(int i = 0; i < nres; i++) {
        PyObject *str = PyString_FromString(names[i]);
        if(!str) {
            Py_DECREF(ret);
            return NULL;
        }
        PyList_SetItem(ret, i, str);
    }
    return ret;
}

static PyObject *PyStorageSiteEntity_pickle(PyStorageSiteEntityObject *self, PyObject *args, PyObject *kwargs)
{
    return s_call_super_method("__pickle__", (PyObject*)&PyStorageSiteEntity_type, 
        (PyObject*)self, args, kwargs);
}

static PyObject *PyStorageSiteEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    return s_call_super_method("__unpickle__", (PyObject*)&PyStorageSiteEntity_type, cls, args, kwargs);
}

static PyObject *PyMovableEntity_get_speed(PyMovableEntityObject *self, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return NULL;
    }

    float speed = 0.0f;
    G_Move_GetMaxSpeed(self->super.ent, &speed);
    return PyFloat_FromDouble(speed);
}

static int PyMovableEntity_set_speed(PyMovableEntityObject *self, PyObject *value, void *closure)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot get attribute of zombie entity.");
        return -1;
    }

    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Speed attribute must be a float.");
        return -1;
    }

    G_Move_SetMaxSpeed(self->super.ent, PyFloat_AS_DOUBLE(value));
    return 0;
}

static PyObject *PyMovableEntity_move(PyMovableEntityObject *self, PyObject *args)
{
    if(G_FlagsGet(self->super.ent) & ENTITY_FLAG_ZOMBIE) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot call method on zombie entity.");
        return NULL;
    }

    vec2_t xz_pos;
    if(!PyArg_ParseTuple(args, "(ff)", &xz_pos.x, &xz_pos.z)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of 2 floats.");
        return NULL;
    }

    if(!G_PointInsideMap(xz_pos)) {
        PyErr_SetString(PyExc_RuntimeError, "The movement point must be within the map bounds.");
        return NULL;
    }

    G_Move_SetDest(self->super.ent, xz_pos, false);
    Py_RETURN_NONE;
}


static PyObject *PyMovableEntity_del(PyMovableEntityObject *self)
{
    return s_super_del((PyObject*)self, &PyMovableEntity_type);
}

static PyObject *PyMovableEntity_pickle(PyMovableEntityObject *self, PyObject *args, PyObject *kwargs)
{
    return s_call_super_method("__pickle__", (PyObject*)&PyMovableEntity_type, 
        (PyObject*)self, args, kwargs);
}

static PyObject *PyMovableEntity_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    return s_call_super_method("__unpickle__", (PyObject*)&PyMovableEntity_type, cls, args, kwargs);
}

static PyObject *s_obj_from_attr(const struct attr *attr)
{
    switch(attr->type){
    case TYPE_STRING:   return Py_BuildValue("s", attr->val.as_string);
    case TYPE_FLOAT:    return Py_BuildValue("f", attr->val.as_float);
    case TYPE_INT:      return Py_BuildValue("i", attr->val.as_int);
    case TYPE_VEC3:     return Py_BuildValue("(f,f,f)", attr->val.as_vec3.x, attr->val.as_vec3.y, attr->val.as_vec3.z);
    case TYPE_QUAT:     return Py_BuildValue("(f,f,f,f)", 
                               attr->val.as_quat.x, attr->val.as_quat.y, attr->val.as_quat.z, attr->val.as_quat.w);
    case TYPE_BOOL:     return Py_BuildValue("i", attr->val.as_bool);
    default: assert(0); return NULL;
    }
}

static PyObject *s_tuple_from_attr_vec(const vec_attr_t *attr_vec)
{
    PyObject *ret = PyTuple_New(vec_size(attr_vec));
    if(!ret)
        return NULL;

    for(int i = 0; i < vec_size(attr_vec); i++) {
        PyTuple_SetItem(ret, i, s_obj_from_attr(&vec_AT(attr_vec, i)));
    }

    return ret;
}

static PyObject *s_entity_from_atts(const char *path, const char *name, 
                                    const khash_t(attr) *attr_table, uint32_t extra_flags)
{
    PyObject *ret;

    char copy[256];
    if(strlen(path) >= sizeof(copy)) 
        return NULL;
    strcpy(copy, path);
    char *end = copy + strlen(path);
    assert(*end == '\0');
    while(end > copy && *end != '/')
        end--;
    if(end == copy)
        return NULL;
    *end = '\0';
    const char *filename = end + 1;

    khiter_t k = kh_get(attr, attr_table, "animated");
    if(k == kh_end(attr_table))
        return NULL;
    bool anim = kh_value(attr_table, k).val.as_bool;

    PyObject *args = PyTuple_New(anim ? 4 : 3);
    if(!args)
        return NULL;

    PyTuple_SetItem(args, 0, PyString_FromString(copy));
    PyTuple_SetItem(args, 1, PyString_FromString(filename));
    PyTuple_SetItem(args, 2, PyString_FromString(name));

    PyObject *kwargs = Py_BuildValue("{s:I}", "__extra_flags__", extra_flags);
    if(!kwargs) {
        Py_DECREF(args);
        return NULL;
    }

    if((k = kh_get(attr, attr_table, "position")) != kh_end(attr_table)) {
        vec3_t pos = kh_value(attr_table, k).val.as_vec3;
        PyObject *posobj = Py_BuildValue("fff", pos.x, pos.y, pos.z);
        if(!posobj) {
            Py_DECREF(args);
            Py_DECREF(kwargs);
            return NULL;
        }
        PyDict_SetItemString(kwargs, "pos", posobj);
        Py_DECREF(posobj);
    }

    if(anim) {
        PyObject *idle_clip;
        k = kh_get(attr, attr_table, "idle_clip");

        if(k == kh_end(attr_table) || !(idle_clip = s_obj_from_attr(&kh_value(attr_table, k)))) {
            Py_DECREF(args);
            Py_DECREF(kwargs);
            return NULL;
        }

        PyDict_SetItemString(kwargs, "idle_clip", idle_clip);
        Py_DECREF(idle_clip);
        ret = PyObject_Call((PyObject*)&PyAnimEntity_type, args, kwargs);
    }else{
        ret = PyObject_Call((PyObject*)&PyEntity_type, args, kwargs);
    }

    Py_DECREF(kwargs);
    Py_DECREF(args);
    return ret;
}

static PyObject *s_new_custom_class(const char *name, const vec_attr_t *construct_args, 
                                    const khash_t(attr) *attr_table, uint32_t extra_flags)
{
    PyObject *ret = NULL;
    PyObject *sys_mod_dict = PyImport_GetModuleDict();
    PyObject *modules = PyMapping_Values(sys_mod_dict);
    PyObject *class = NULL;
    for(int i = 0; i < PyList_Size(modules); i++) {
        if(PyObject_HasAttrString(PyList_GetItem(modules, i), name)) {
            class = PyObject_GetAttrString(PyList_GetItem(modules, i), name);
            break;
        }
    }
    Py_DECREF(modules);
    if(!class) {
        char buff[256];
        pf_snprintf(buff, sizeof(buff), "Unable to find class %s", name);
        PyErr_SetString(PyExc_RuntimeError, buff);
        return NULL;
    }
    if(!PyType_Check(class)) {
        char buff[256];
        pf_snprintf(buff, sizeof(buff), "%s is not a 'type' instance", name);
        Py_DECREF(class);
        return NULL;
    }

    PyObject *args = s_tuple_from_attr_vec(construct_args);
    if(!args)
        goto fail_args;

    PyObject *kwargs = Py_BuildValue("{s:I}", "__extra_flags__", extra_flags);
    if(!kwargs)
        goto fail_kwargs;

    khiter_t k;
    if((k = kh_get(attr, attr_table, "position")) != kh_end(attr_table)) {
        vec3_t pos = kh_value(attr_table, k).val.as_vec3;
        PyObject *posobj = Py_BuildValue("fff", pos.x, pos.y, pos.z);
        if(!posobj)
            goto fail_pos;
        PyDict_SetItemString(kwargs, "pos", posobj);
        Py_DECREF(posobj);
    }

    PyTypeObject *tp_class = (PyTypeObject*)class;
    ret = tp_class->tp_new(tp_class, args, kwargs);
    if(ret) {
        int status = tp_class->tp_init(ret, args, NULL);
        if(status) {
            Py_CLEAR(ret);
        }
    }

fail_pos:
    Py_DECREF(kwargs);
fail_kwargs:
    Py_DECREF(args);
fail_args:
    Py_DECREF(class);

    return ret;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void S_Entity_PyRegister(PyObject *module)
{
    if(PyType_Ready(&PyEntity_type) < 0)
        Py_FatalError("Can't initialize pf.Entity type");
    Py_INCREF(&PyEntity_type);
    PyModule_AddObject(module, "Entity", (PyObject*)&PyEntity_type);

    if(PyType_Ready(&PyAnimEntity_type) < 0)
        Py_FatalError("Can't initialize pf.AnimEntity type");
    Py_INCREF(&PyAnimEntity_type);
    PyModule_AddObject(module, "AnimEntity", (PyObject*)&PyAnimEntity_type);

    if(PyType_Ready(&PyCombatableEntity_type) < 0)
        Py_FatalError("Can't initialize pf.CombatableEntity type");
    Py_INCREF(&PyCombatableEntity_type);
    PyModule_AddObject(module, "CombatableEntity", (PyObject*)&PyCombatableEntity_type);

    if(PyType_Ready(&PyBuildableEntity_type) < 0)
        Py_FatalError("Can't initialize pf.BuildableEntity type");
    Py_INCREF(&PyBuildableEntity_type);
    PyModule_AddObject(module, "BuildableEntity", (PyObject*)&PyBuildableEntity_type);

    if(PyType_Ready(&PyBuilderEntity_type) < 0)
        Py_FatalError("Can't initialize pf.BuilderEntity type");
    Py_INCREF(&PyBuilderEntity_type);
    PyModule_AddObject(module, "BuilderEntity", (PyObject*)&PyBuilderEntity_type);

    if(PyType_Ready(&PyResourceEntity_type) < 0)
        Py_FatalError("Can't initialize pf.ResourceEntity type");
    Py_INCREF(&PyResourceEntity_type);
    PyModule_AddObject(module, "ResourceEntity", (PyObject*)&PyResourceEntity_type);

    if(PyType_Ready(&PyHarvesterEntity_type) < 0)
        Py_FatalError("Can't initialize pf.HarvesterEntity type");
    Py_INCREF(&PyHarvesterEntity_type);
    PyModule_AddObject(module, "HarvesterEntity", (PyObject*)&PyHarvesterEntity_type);

    if(PyType_Ready(&PyStorageSiteEntity_type) < 0)
        Py_FatalError("Can't initialize pf.StorageSiteEntity type");
    Py_INCREF(&PyStorageSiteEntity_type);
    PyModule_AddObject(module, "StorageSiteEntity", (PyObject*)&PyStorageSiteEntity_type);

    if(PyType_Ready(&PyMovableEntity_type) < 0)
        Py_FatalError("Can't initialize pf.MovableEntity type");
    Py_INCREF(&PyMovableEntity_type);
    PyModule_AddObject(module, "MovableEntity", (PyObject*)&PyMovableEntity_type);
}

bool S_Entity_Init(void)
{
    s_loaded = PyList_New(0);
    if(!s_loaded)
        return false;

    s_uid_pyobj_table = kh_init(PyObject);
    if(!s_uid_pyobj_table) {
        Py_DECREF(s_loaded);
        return false;
    }
    return true;
}

void S_Entity_Clear(void)
{
    Py_CLEAR(s_loaded);
}

void S_Entity_Shutdown(void)
{
    kh_destroy(PyObject, s_uid_pyobj_table);
}

bool S_Entity_Check(PyObject *obj)
{
    return PyObject_IsInstance(obj, (PyObject*)&PyEntity_type);
}

bool S_Entity_UIDForObj(script_opaque_t obj, uint32_t *out)
{
    if(!PyObject_IsInstance(obj, (PyObject*)&PyEntity_type))
        return false;

    *out = ((PyEntityObject*)obj)->ent;
    return true;
}

script_opaque_t S_Entity_ObjForUID(uint32_t uid)
{
    khiter_t k = kh_get(PyObject, s_uid_pyobj_table, uid);
    if(k == kh_end(s_uid_pyobj_table))
        return NULL;

    return kh_value(s_uid_pyobj_table, k);
}

script_opaque_t S_Entity_ObjFromAtts(const char *path, const char *name,
                                     const khash_t(attr) *attr_table, 
                                     const vec_attr_t *construct_args)
{
    khiter_t k;
    PyObject *ret = NULL;
    uint32_t extra_flags = 0;

    if(((k = kh_get(attr, attr_table, "static")) != kh_end(attr_table))
    && !kh_value(attr_table, k).val.as_bool) {
        extra_flags |= ENTITY_FLAG_MOVABLE;
    }

    /* First, attempt to make an instance of the custom class specified in the 
     * attributes dictionary, if the key is present. */
    if((k = kh_get(attr, attr_table, "class")) != kh_end(attr_table)) {

        const char *cls = kh_value(attr_table, k).val.as_string;
        ret = s_new_custom_class(cls, construct_args, attr_table, extra_flags);

        if(PyErr_Occurred()) {
            PyThreadState *tstate = PyThreadState_GET();
            PyObject *repr = PyObject_Repr(tstate->curexc_value);
            printf("[IMPORT] Unable to make %s instance: %s\n", cls, PyString_AS_STRING(repr));
            Py_DECREF(repr);
            PyErr_Clear();
        }
    }

    /* If we could not make a custom class, fall back to instantiating a basic entity */
    if(!ret) {
        ret = s_entity_from_atts(path, name, attr_table, extra_flags); 
    }

    if(!ret){
        return NULL;
    }
    uint32_t ent = ((PyEntityObject*)ret)->ent;

    if(((k = kh_get(attr, attr_table, "collision")) != kh_end(attr_table))
    && kh_value(attr_table, k).val.as_bool) {
        G_FlagsSet(ent, G_FlagsGet(ent) | ENTITY_FLAG_COLLISION);
    }

    if(PyObject_IsInstance(ret, (PyObject*)&PyBuildableEntity_type)) {
        G_Building_Mark(ent);
        G_Building_Found(ent, true);
        G_Building_Supply(ent);
        G_Building_Complete(ent);
    }

    const char *attrs[][2] = {
        {"selection_radius",    "selection_radius"  },
        {"pos",                 "position"          },
        {"scale",               "scale"             },
        {"rotation",            "rotation"          },
        {"selectable",          "selectable"        },
        {"faction_id",          "faction_id"        },
        {"vision_range",        "vision_range"      },
        {"hp",                  "hp"                },
    };

    for(int i = 0; i < ARR_SIZE(attrs); i++) {

        const char *ent_attr = attrs[i][0];
        const char *scene_attr = attrs[i][1];

        if(PyObject_HasAttrString(ret, ent_attr) 
        && ((k = kh_get(attr, attr_table, scene_attr)) != kh_end(attr_table))) {

            PyObject *val = s_obj_from_attr(&kh_value(attr_table, k));
            if(val) {
                PyObject_SetAttrString(ret, ent_attr, val);
                PyErr_Clear();
            }
            Py_XDECREF(val);
        }
    }

    PyList_Append(s_loaded, ret);
    Py_DECREF(ret);
    return ret;
}

PyObject *S_Entity_GetLoaded(void)
{
    PyObject *ret = s_loaded;
    if(!ret)
        return NULL;

    s_loaded = PyList_New(0);
    assert(s_loaded);
    
    return ret;
}

