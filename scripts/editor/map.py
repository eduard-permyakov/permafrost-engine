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

EDITOR_PFMAP_VERSION = 1.0

class Material():

    def __init__(self, name, texname):
        self.refcount = 0
        self.name = name
        self.texname = texname
        self.ambient = 1.0
        self.diffuse = (0.5, 0.5, 0.5)
        self.specular = (0.2, 0.2, 0.2)

    def pfmap_str(self):
        ret = ""
        ret += "material " + self.name + "\n"
        ret += "\tambient {0:.6f}\n".format(self.ambient)
        ret += "\tdiffuse {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n".format(v=self.diffuse)
        ret += "\tspecular {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n".format(v=self.specular)
        ret += "\ttexture {0}\n".format(self.texname)
        return ret


class Chunk():
    DEFAULT_TOP_MATERIAL = Material("Grass", "grass.png")
    DEFAULT_SIDE_MATERIAL = Material("Cliffs", "cliffs.png")

    def __init__(self):
        """
        A default chunk starts with all tiles set to flat type and 0 height. 
        All tiles will use index 0 for the top material and index 1 for the side material.
        """
        self.rows = pf.TILES_PER_CHUNK_WIDTH
        self.cols = pf.TILES_PER_CHUNK_HEIGHT

        self.tiles = []
        for r in range(0, self.rows):
            row = [pf.Tile() for c in range(0, self.cols)]
            self.tiles.append(row)

        self.materials = [None]*pf.MATERIALS_PER_CHUNK
        self.materials[0] = Chunk.DEFAULT_TOP_MATERIAL
        self.materials[0].refcount = self.rows * self.cols
        self.materials[1] = Chunk.DEFAULT_SIDE_MATERIAL
        self.materials[1].refcount = self.rows * self.cols

    def pfmap_str(self):
        ret = ""
        for r in range(0, self.rows):
            for c in range(0, self.cols):
                ret += Chunk.tile_to_string(self.tiles[r][c]) + " "
            ret += "\n"
        used_mats = [m for m in self.materials if m is not None and m.refcount > 0]
        ret += "chunk_materials {0}\n".format(len(used_mats))
        for m in used_mats:
            ret += m.pfmap_str()
        return ret

    @staticmethod
    def tile_to_string(tile):
        ret = ""
        assert tile.type >= 0 and tile.type < 16
        ret += "{0:X}".format(tile.type)
        assert tile.pathable >= 0 and tile.pathable <= 1
        ret += str(tile.pathable)
        assert tile.base_height >= 0 and tile.base_height <= 9
        ret += str(tile.base_height)
        assert tile.top_mat_idx >= 0 and tile.top_mat_idx <= pf.MATERIALS_PER_CHUNK
        ret += str(tile.top_mat_idx)
        assert tile.sides_mat_idx >= 0 and tile.sides_mat_idx <= pf.MATERIALS_PER_CHUNK
        ret += str(tile.sides_mat_idx)
        assert tile.ramp_height >= 0 and tile.ramp_height <= 9
        ret += str(tile.ramp_height)
        return ret


class Map():

    def __init__(self, chunk_rows, chunk_cols):
        self.filename = None
        self.chunk_rows = chunk_rows
        self.chunk_cols = chunk_cols
        self.chunks = []
        for r in range(0, self.chunk_rows):
            row = [Chunk() for c in range(0, self.chunk_cols)]
            self.chunks.append(row)
    
    def pfmap_str(self):
        ret = ""
        ret += "version " + str(EDITOR_PFMAP_VERSION) + "\n"
        ret += "num_rows " + str(self.chunk_rows) + "\n"
        ret += "num_cols" + str(self.chunk_cols) + "\n"
        ret += "num_materials " + str(self.chunk_cols * self.chunk_rows * pf.MATERIALS_PER_CHUNK) + "\n"

        for chunk_r in range(0, self.chunk_rows):
            for chunk_c in range(0, self.chunk_cols):
                ret += self.chunks[chunk_r][chunk_c].pfmap_str()

        return ret

