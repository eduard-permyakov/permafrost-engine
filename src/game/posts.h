/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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

#ifndef POSTS_H
#define POSTS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct SDL_RWops;
struct map;

/* An open post of a workshop that has work for it */
struct post_vacancy{
    uint32_t workshop;
    int      faction;
    int      district;
    int      priority;
    /* The holder that went to eat, whom the post waits for; NULL_UID for anyone */
    uint32_t awaited;
    /* One that could not get in a short while ago, not to be sent again yet */
    uint32_t shunned;
    /* Ticks the workshop has waited for hands */
    uint32_t waited_ticks;
};

bool   G_Posts_Init(const struct map *map);
void   G_Posts_Shutdown(void);
void   G_Posts_RemoveEntity(uint32_t uid);
/* Whether the unit holds a post, inside its workshop or on its way there */
bool   G_Posts_Holds(uint32_t unit);
/* Sends the unit to take up a post of the workshop */
bool   G_Posts_Take(uint32_t workshop, uint32_t unit);
/* What a right click on a workshop with posts to fill does with the selection */
int    G_Posts_CurrContextualAction(void);
/* The open posts of workshops with work, the most urgent first; gathered once a tick */
size_t G_Posts_Vacancies(size_t maxout, struct post_vacancy out[]);
bool   G_Posts_SaveState(struct SDL_RWops *stream);
bool   G_Posts_LoadState(struct SDL_RWops *stream);

#endif
