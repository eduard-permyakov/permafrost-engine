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
