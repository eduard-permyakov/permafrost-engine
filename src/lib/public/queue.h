/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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

#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>

typedef struct queue queue_t;

queue_t *queue_init(size_t entry_size, int init_capacity);
queue_t *queue_copy(const queue_t *queue);
void     queue_free(queue_t *queue);
int      queue_push(queue_t *queue, void *entry);
int      queue_pop(queue_t *queue, void *out);
size_t   queue_get_size(queue_t *queue);

#endif
