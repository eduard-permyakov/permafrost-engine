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

#ifndef CURSOR_H
#define CURSOR_H

#include <stdbool.h>

enum cursortype{
    CURSOR_POINTER = 0,
    CURSOR_SCROLL_TOP,
    CURSOR_SCROLL_TOP_RIGHT,
    CURSOR_SCROLL_RIGHT,
    CURSOR_SCROLL_BOT_RIGHT,
    CURSOR_SCROLL_BOT,
    CURSOR_SCROLL_BOT_LEFT,
    CURSOR_SCROLL_LEFT,
    CURSOR_SCROLL_TOP_LEFT
};

bool Cursor_InitAll(const char *basedir);
void Cursor_FreeAll(void);
void Cursor_SetActive(enum cursortype type);

/* Set the cursor icon based on which corner or edge of the screen the 
 * cursor currently is */
void Cursor_RTS_SetActive(int mouse_x, int mouse_y);

#endif
