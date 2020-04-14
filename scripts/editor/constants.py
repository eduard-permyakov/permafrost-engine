#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2018-2020 Eduard Permyakov 
#
#  Permafrost Engine is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  Permafrost Engine is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
# 
#  Linking this software statically or dynamically with other modules is making 
#  a combined work based on this software. Thus, the terms and conditions of 
#  the GNU General Public License cover the whole combination. 
#  
#  As a special exception, the copyright holders of Permafrost Engine give 
#  you permission to link Permafrost Engine with independent modules to produce 
#  an executable, regardless of the license terms of these independent 
#  modules, and to copy and distribute the resulting executable under 
#  terms of your choice, provided that you also meet, for each linked 
#  independent module, the terms and conditions of the license of that 
#  module. An independent module is a module which is not derived from 
#  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
#  extend this exception to your version of Permafrost Engine, but you are not 
#  obliged to do so. If you do not wish to do so, delete this exception 
#  statement from your version.
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

DEFAULT_FACTION_COLOR = (255, 255, 255, 255)
DEFAULT_FACTION_NAME = "Mother Nature"

############################################################
# Editor-specific events                                   #
############################################################

EVENT_TEXTURE_SELECTION_CHANGED    = 0x20000
EVENT_TOP_TAB_SELECTION_CHANGED    = 0x20001
EVENT_TERRAIN_BRUSH_SIZE_CHANGED   = 0x20002
EVENT_TERRAIN_BRUSH_TYPE_CHANGED   = 0x20003
EVENT_TERRAIN_EDGE_TYPE_CHANGED    = 0x20004
EVENT_HEIGHT_SELECTION_CHANGED     = 0x20005
EVENT_TERRAIN_TEX_BLEND_CHANGED    = 0x20006
EVENT_TERRAIN_NORMAL_BLEND_CHANGED = 0x20007
EVENT_SIDE_MAT_SEL_CHANGED         = 0x20008

EVENT_MENU_LOAD                    = 0x20010
EVENT_MENU_SAVE                    = 0x20011
EVENT_MENU_SAVE_AS                 = 0x20012
EVENT_MENU_NEW                     = 0x20013
EVENT_MENU_SETTINGS_SHOW           = 0x20014
EVENT_MENU_PERF_SHOW               = 0x20015
EVENT_MENU_EXIT                    = 0x20016
EVENT_MENU_CANCEL                  = 0x20017
EVENT_MENU_SESSION_SHOW            = 0x20018

EVENT_OBJECTS_TAB_MODE_CHANGED     = 0x20020
EVENT_OBJECT_SELECTION_CHANGED     = 0x20021
EVENT_OBJECT_SELECTED_UNIT_PICKED  = 0x20022
EVENT_OBJECT_DELETE_SELECTION      = 0x20023

EVENT_MOUSE_ENTERED_MAP            = 0x20030
EVENT_MOUSE_EXITED_MAP             = 0x20031

EVENT_OLD_GAME_TEARDOWN_BEGIN      = 0x20040
EVENT_OLD_GAME_TEARDOWN_END        = 0x20041

EVENT_DIPLO_FAC_SELECTION_CHANGED  = 0x20050
EVENT_DIPLO_FAC_REMOVED            = 0x20051
EVENT_DIPLO_FAC_CHANGED            = 0x20052
EVENT_DIPLO_FAC_NEW                = 0x20053

EVENT_FILE_CHOOSER_OKAY            = 0x20060
EVENT_FILE_CHOOSER_CANCEL          = 0x20061

