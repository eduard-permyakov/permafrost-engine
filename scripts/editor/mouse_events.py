#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2018 Eduard Permyakov 
#
#  Permafrost Engine is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation either version 3 of the License or
#  (at your option) any later version.
#
#  Permafrost Engine is distributed in the hope that it will be useful
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not see <http://www.gnu.org/licenses/>.
#

import pf
from constants import *

mouse_over_map =  None

def __on_mousemove(user, event):
    new_mouse_over_map = not pf.mouse_over_ui() and not pf.mouse_over_minimap()

    global mouse_over_map
    if new_mouse_over_map != mouse_over_map:
        if new_mouse_over_map:
            pf.global_event(EVENT_MOUSE_ENTERED_MAP, None)
        else:
            pf.global_event(EVENT_MOUSE_EXITED_MAP, None)
        mouse_over_map = new_mouse_over_map
         
def install():
    global mouse_over_map
    mouse_over_map = not pf.mouse_over_ui() and not pf.mouse_over_minimap()
    pf.register_event_handler(EVENT_SDL_MOUSEMOTION, __on_mousemove, None)

