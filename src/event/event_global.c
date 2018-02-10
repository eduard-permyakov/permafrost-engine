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

#include "public/event.h"
#include "../lib/public/khash.h"
#include "../lib/public/kvec.h"

#include <assert.h>


enum handler_type{
    HANDLER_TYPE_ENGINE,
    HANDLER_TYPE_SCRIPT,
};

struct handler_desc{
    enum handler_type type;
    union {
        handler_t       as_function;
        script_opaque_t as_script_callable;
    }handler;
    void *user_arg;
    void (*freefunc)(script_opaque_t, script_opaque_t);
};

typedef kvec_t(struct handler_desc) kvec_handler_desc_t;
KHASH_MAP_INIT_INT(handler_desc, kvec_handler_desc_t)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(handler_desc) *s_event_handler_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool e_register_handler(enum eventtype event, struct handler_desc *desc)
{
	khiter_t k;
	k = kh_get(handler_desc, s_event_handler_table, event);

	if(k == kh_end(s_event_handler_table)) {

        kvec_handler_desc_t newv;
        kv_init(newv);
        kv_push(struct handler_desc, newv, *desc);
    
        int ret;
	    k = kh_put(handler_desc, s_event_handler_table, event, &ret);
        assert(ret == 1);
	    kh_value(s_event_handler_table, k) = newv;

    }else{
    
        kvec_handler_desc_t vec = kh_value(s_event_handler_table, k);
        kv_push(struct handler_desc, vec, *desc);
        kh_value(s_event_handler_table, k) = vec;
    }

    return true;
}

static bool e_unregister_handler(enum eventtype event, struct handler_desc *desc)
{
	khiter_t k;
	k = kh_get(handler_desc, s_event_handler_table, event);

	if(k == kh_end(s_event_handler_table))
        return false;

    kvec_handler_desc_t oldvec = kh_value(s_event_handler_table, k);
    kvec_handler_desc_t newvec;
    kv_init(newvec);
    while(kv_size(oldvec) > 0) {
        struct handler_desc elem = kv_pop(oldvec); 

        if( ((elem.type == desc->type == HANDLER_TYPE_ENGINE) && elem.handler.as_function        == desc->handler.as_function) )
            continue;
        if( ((elem.type == desc->type == HANDLER_TYPE_SCRIPT) && elem.handler.as_script_callable == desc->handler.as_script_callable) ) {
        
            if(elem.freefunc)
                elem.freefunc(elem.handler.as_script_callable, elem.user_arg);
            continue;
        }

        kv_push(struct handler_desc, newvec, elem);
    }

    kv_destroy(oldvec);
    kh_value(s_event_handler_table, k) = newvec;

    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool E_Global_Init(void)
{
    s_event_handler_table = kh_init(handler_desc);
    return true;
}

void E_Global_Shutdown(void)
{
	khiter_t k;
	for (k = kh_begin(s_event_handler_table); k != kh_end(s_event_handler_table); ++k) {
    
		if (kh_exist(s_event_handler_table, k)) {
        
            kvec_handler_desc_t vec = kh_value(s_event_handler_table, k);
            for(int i = 0; i < kv_size(vec); i++) {
               
                struct handler_desc hd = kv_A(vec, i);
                if(hd.freefunc)
                    hd.freefunc(hd.handler.as_script_callable, hd.user_arg);
            }

            kv_destroy(vec);
        }
    }

    kh_destroy(handler_desc, s_event_handler_table);
}

bool E_Global_Register(enum eventtype event, handler_t handler, void *user_arg)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_ENGINE;
    hd.handler.as_function = handler;
    hd.user_arg = user_arg;
    hd.freefunc = NULL;

    return e_register_handler(event, &hd);
}

bool E_Global_Unregister(enum eventtype event, handler_t handler)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_ENGINE;
    hd.handler.as_function = handler;

    return e_unregister_handler(event, &hd);
}

bool E_Global_ScriptRegister(enum eventtype event, script_opaque_t handler, script_opaque_t user_arg,
                             void (*freefunc)(script_opaque_t, script_opaque_t))
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_SCRIPT;
    hd.handler.as_script_callable = handler;
    hd.user_arg = user_arg;
    hd.freefunc = freefunc;

    return e_register_handler(event, &hd);
}

bool E_Global_ScriptUnregister(enum eventtype event, script_opaque_t handler)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_SCRIPT;
    hd.handler.as_script_callable = handler;

    return e_unregister_handler(event, &hd);
}

void E_Global_Broadcast(enum eventtype event, void *event_arg)
{
	khiter_t k;
	k = kh_get(handler_desc, s_event_handler_table, event);

	if(k == kh_end(s_event_handler_table))
        return;

    kvec_handler_desc_t vec = kh_value(s_event_handler_table, k);
    for(int i = 0; i < kv_size(vec); i++) {

        struct handler_desc *elem = &kv_A(vec, i);
    
        if(elem->type == HANDLER_TYPE_ENGINE) {
            elem->handler.as_function(elem->user_arg, event_arg);
        }else if(elem->type == HANDLER_TYPE_SCRIPT) {
            S_RunEventHandler(elem->handler.as_script_callable, elem->user_arg, event_arg);
        }
    }
}

