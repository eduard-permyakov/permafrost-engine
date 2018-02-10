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
#include "../script/public/script.h"

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
};

typedef kvec_t(struct handler_desc) kvec_handler_desc_t;
KHASH_MAP_INIT_INT(handler_desc, kvec_handler_desc_t)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(handler_desc) *s_event_handler_table;

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
    
		if (kh_exist(s_event_handler_table, k)) 
            kv_destroy( kh_value(s_event_handler_table, k) );
    }

    kh_destroy(handler_desc, s_event_handler_table);
}

bool E_Global_Register(enum eventtype event, handler_t handler, void *user_arg)
{
	khiter_t k;
	k = kh_get(handler_desc, s_event_handler_table, event);

    struct handler_desc hd;
    hd.type = HANDLER_TYPE_ENGINE;
    hd.handler.as_function = handler;
    hd.user_arg = user_arg;

	if(k == kh_end(s_event_handler_table)) {

        kvec_handler_desc_t newv;
        kv_init(newv);
        kv_push(struct handler_desc, newv, hd);
    
        int ret;
	    k = kh_put(handler_desc, s_event_handler_table, event, &ret);
        assert(ret == 1);
	    kh_value(s_event_handler_table, k) = newv;

    }else{
    
        kvec_handler_desc_t vec = kh_value(s_event_handler_table, k);
        kv_push(struct handler_desc, vec, hd);
        kh_value(s_event_handler_table, k) = vec;
    }

    return true;
}

bool E_Global_Unregister(enum eventtype event, handler_t handler)
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

        if(elem.type == HANDLER_TYPE_ENGINE && elem.handler.as_function == handler)
            continue;

        kv_push(struct handler_desc, newvec, elem);
    }

    kv_destroy(oldvec);
    kh_value(s_event_handler_table, k) = newvec;

    return true;
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
        }else {
            //TODO
        }
    }
}

