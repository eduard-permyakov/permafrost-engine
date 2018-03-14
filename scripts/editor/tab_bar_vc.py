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
from ui import ViewController

class TabBarViewController(ViewController):

    def __init__(self, view):
        self.view = view
        self.active_idx = 0
        self.labels = []
        self.children = []

    def __on_tab_changed(self, event):
        assert self.active_idx >= 0 and self.active_idx < len(self.children)
        assert event >= 0 and event < len(self.children)
        self.children[self.active_idx].deactivate()
        self.active_idx = event
        self.children[self.active_idx].activate()

    def push_child(self, label, vc):
        assert isinstance(vc, ViewController)
        assert isinstance(label, basestring)
        self.children.append(vc)
        self.view.labels.append(label)
        self.view.child_windows.append(vc.view)

    def activate(self):
        pf.register_event_handler(EVENT_TOP_TAB_SELECTION_CHANGED, TabBarViewController.__on_tab_changed, self)
        self.children[self.active_idx].activate()

    def deactivate(self):
        self.children[self.active_idx].deactivate()
        pf.unregister_event_handler(EVENT_TOP_TAB_SELECTION_CHANGED, TabBarViewController.__on_tab_changed)

