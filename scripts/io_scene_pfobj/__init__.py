#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2017-2026 Eduard Permyakov 
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

bl_info = {
    "name": "Permafrost Engine Object (.pfobj)",
    "author": "Eduard Permyakov",
    "version": (1, 1, 0),
    "blender": (2, 80, 0),
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
    StringProperty,
    BoolProperty
)


class ImportPFOBJ(bpy.types.Operator, ImportHelper):
    """Load a Permafrost Engine OBJ File"""

    bl_idname    = "import_scene.pfobj"
    bl_label     = "Import PFOBJ"
    filename_ext = ".pfobj"
    filter_glob: StringProperty(
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

    filter_glob: StringProperty(
        default="*.pfobj",
        options={'HIDDEN'}
    )
    check_extension = True
    filename_ext    = ".pfobj"

    export_bbox: BoolProperty(
        name="Export Object Bounding Box",
        description="Export Bounding Box (used for entities which have collision). For animated " \
            "entities, this is the bind pose bounding box.",
        default=True
    )

    local_origin: BoolProperty(
        name="Use Local Object Origin",
        description="Worldspace transform is not applied to vertices, they are exported in object space instead.",
        default=False
    )

    def execute(self, context):
        from . import export_pfobj
        from mathutils import Matrix, Vector

        keywords = self.as_keywords(ignore=("check_existing",
                                            "filter_glob"))

        # Convert to the engine coordinate system. axis_conversion only ever yields a
        # rotation, but the engine's view matrix is left-handed (determinant -1), so we
        # compose in an X reflection or models export mirrored left-to-right.
        global_matrix = Matrix.Diagonal(Vector((-1.0, 1.0, 1.0, 1.0))) \
            @ axis_conversion(to_forward='-Z', to_up='Y').to_4x4()
        keywords["global_matrix"] = global_matrix

        return export_pfobj.save(self, context, **keywords)


def menu_func_import(self, context):
    self.layout.operator(ImportPFOBJ.bl_idname,
                         text="Permafrost Engine Object (.pfobj)")


def menu_func_export(self, context):
    self.layout.operator(ExportPFOBJ.bl_idname,
                         text="Permafrost Engine Object (.pfobj)")

classes = (
    ImportPFOBJ,
    ExportPFOBJ,
)

def register():
    for cls in classes:
        bpy.utils.register_class(cls)

    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)

def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)

    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)

if __name__ == "__main__":
    register()
