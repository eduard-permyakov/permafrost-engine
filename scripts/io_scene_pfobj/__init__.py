#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2017 Eduard Permyakov 
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
#

bl_info = {
    "name": "Permafrost Engine Object (.pfobj)",
    "author": "Eduard Permyakov",
    "version": (1, 0, 0),
    "blender": (2, 72),
    "location": "File > Import-Export",
    "description": "Exports and imports models in Permafrost Engine Object format",
    "category": "Import-Export"
}

import bpy
from bpy_extras.io_utils import(
    ImportHelper,
    ExportHelper,
    axis_conversion
)
from bpy.props import(
    StringProperty
)


class ImportPFOBJ(bpy.types.Operator, ImportHelper):
    """Load a Permafrost Engine OBJ File"""

    bl_idname    = "import_scene.pfobj"
    bl_label     = "Import PFOBJ"
    filename_ext = ".pfobj"
    filter_glob  = StringProperty(
        default="*.pfobj",
        options={'HIDDEN'}
    )

    def execute(self, context):
        # TODO
        return {'FINISHED'}

class ExportPFOBJ(bpy.types.Operator, ExportHelper):
    """Save a Permafrost Engine Object File"""

    bl_idname    = "export_scene.pfobj"
    bl_label     = 'Export PFOBJ'

    filter_glob = StringProperty(
        default="*.pfobj",
        options={'HIDDEN'}
    )
    check_extension = True
    filename_ext    = ".pfobj"

    def execute(self, context):
        from . import export_pfobj
        from mathutils import Matrix

        keywords = self.as_keywords(ignore=("check_existing",
                                            "filter_glob"))

        # Convert to OpenGL coordinate system
        global_matrix = axis_conversion(to_forward='-Z', to_up='Y').to_4x4()
        keywords["global_matrix"] = global_matrix

        return export_pfobj.save(self, context, **keywords)


def menu_func_import(self, context):
    self.layout.operator(ImportPFOBJ.bl_idname, 
                         text="Permafrost Engine Object (.pfobj)")


def menu_func_export(self, context):
    self.layout.operator(ExportPFOBJ.bl_idname, 
                         text="Permafrost Engine Object (.pfobj)")

def register():
    bpy.utils.register_module(__name__)

    bpy.types.INFO_MT_file_import.append(menu_func_import)
    bpy.types.INFO_MT_file_export.append(menu_func_export)

def unregister():
    bpy.utils.unregister_module(__name__)

    bpy.types.INFO_MT_file_import.remove(menu_func_import)
    bpy.types.INFO_MT_file_export.remove(menu_func_export)

if __name__ == "__main__":
    register()
