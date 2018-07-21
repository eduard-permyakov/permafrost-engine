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

############################################################
# Editor shared constants                                  #
############################################################

MODELS_PREFIX_DIR = "assets/models/"

UI_TAB_BAR_HEIGHT = 40
UI_LEFT_PANE_WIDTH = 250
UI_TAB_BAR_NUM_COLS = 5
UI_TAB_BAR_COL_WIDTH = 120

MINIMAP_PX_WIDTH = (pf.MINIMAP_SIZE + 6) #hard-coded 3px border

############################################################
# Editor-specific events                                   #
############################################################

EVENT_TEXTURE_SELECTION_CHANGED    = 0x20000
EVENT_TOP_TAB_SELECTION_CHANGED    = 0x20001
EVENT_TERRAIN_BRUSH_SIZE_CHANGED   = 0x20002
EVENT_TERRAIN_BRUSH_TYPE_CHANGED   = 0x20003
EVENT_TERRAIN_EDGE_TYPE_CHANGED    = 0x20004
EVENT_HEIGHT_SELECTION_CHANGED     = 0x20005

EVENT_FILE_CHOOSER_OKAY            = 0x20008
EVENT_FILE_CHOOSER_CANCEL          = 0x20009

EVENT_MENU_LOAD                    = 0x20010
EVENT_MENU_SAVE                    = 0x20011
EVENT_MENU_SAVE_AS                 = 0x20012
EVENT_MENU_NEW                     = 0x20013
EVENT_MENU_EXIT                    = 0x20014
EVENT_MENU_CANCEL                  = 0x20015

EVENT_OBJECTS_TAB_MODE_CHANGED     = 0x20020
EVENT_OBJECT_SELECTION_CHANGED     = 0x20021
EVENT_OBJECT_SELECTED_UNIT_PICKED  = 0x20022
EVENT_OBJECT_DELETE_SELECTION      = 0x20023

EVENT_MOUSE_ENTERED_MAP            = 0x20030
EVENT_MOUSE_EXITED_MAP             = 0x20031

EVENT_OLD_GAME_TEARDOWN_BEGIN      = 0x20040
EVENT_OLD_GAME_TEARDOWN_END        = 0x20041

