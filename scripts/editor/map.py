#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2018-2023 Eduard Permyakov 
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
import traceback
import copy


EDITOR_PFMAP_VERSION = 1.0


def tile_to_string(tile):
    ret = ""
    assert tile.type >= 0 and tile.type < 16
    ret += "{0:1X}".format(tile.type)
    assert tile.base_height >= -99 and tile.base_height <= 99
    ret += "+" if tile.base_height >= 0 else "-"
    ret += "{0:02d}".format(abs(tile.base_height))
    assert tile.ramp_height >= 0 and tile.ramp_height <= 99
    ret += "{0:02d}".format(tile.ramp_height)
    assert tile.top_mat_idx >= 0 and tile.top_mat_idx <= 999
    ret += "{0:03d}".format(tile.top_mat_idx)
    assert tile.sides_mat_idx >= 0 and tile.sides_mat_idx <= 999
    ret += "{0:03d}".format(tile.sides_mat_idx)
    assert tile.pathable >= 0 and tile.pathable <= 1
    ret += str(tile.pathable)
    assert tile.blend_mode in [pf.BLEND_MODE_BLUR, pf.BLEND_MODE_NOBLEND]
    ret += str(tile.blend_mode)
    assert tile.blend_normals >= 0 and tile.blend_normals <= 1
    ret += str(tile.blend_normals)
    # The rest of the 24 characters are reserved for future expansion
    ret += "000000000"
    assert len(ret) == 24
    return ret

def tile_from_string(string):
    assert len(string) == 24
    ret = pf.Tile()
    ret.type = int(string[0], 16)
    ret.base_height = int(string[2:4])
    if string[1] == '-':
        ret.base_height *= -1
    ret.ramp_height= int(string[4:6])
    ret.top_mat_idx = int(string[6:9])
    ret.sides_mat_idx = int(string[9:12])
    ret.pathable = int(string[12])
    ret.blend_mode = int(string[13])
    ret.blend_normals = int(string[14])
    return ret


class Material(object):

    def __init__(self, name, texname):
        self.name = name
        self.texname = texname

    def pfmap_str(self):
        return "material " + self.name + " {0}\n".format(self.texname)

    @classmethod
    def from_string(cls, string):
        name = string.split()[1]
        texname = string.split()[2]
        return Material(name, texname)

class Chunk(object):

    def __init__(self):
        """
        A default chunk starts with all tiles set to flat type and 0 height. 
        All tiles will use index 0 for the top material and index 1 for the side material.
        """
        self.rows = pf.TILES_PER_CHUNK_HEIGHT
        self.cols = pf.TILES_PER_CHUNK_WIDTH

        self.tiles = []
        for r in range(0, self.rows):
            row = [pf.Tile() for c in range(0, self.cols)]
            self.tiles.append(row)
        assert(len(self.tiles) == self.rows)

    def pfmap_str(self):
        ret = ""
        num_tiles_written = 0
        for r in range(0, self.rows):
            for c in range(0, self.cols):
                ret += tile_to_string(self.tiles[r][c])
                num_tiles_written += 1
                if num_tiles_written % 4 == 0:
                    ret += "\n"
                else:
                    ret += " "
        return ret

    @classmethod
    def from_lines(cls, lines):
        ret = Chunk()
        ret.tiles = []
        line_idx = 0

        rows_read = 0
        while rows_read < pf.TILES_PER_CHUNK_HEIGHT:
            tiles_read = 0
            tiles_row = []
            while tiles_read < pf.TILES_PER_CHUNK_WIDTH:
                tile_strings = lines[line_idx].split()
                tiles_read += len(tile_strings)
                for c in range(0, len(tile_strings)):
                    tile = tile_from_string(tile_strings[c])
                    tiles_row.append(tile)
                line_idx += 1
            ret.tiles.append(tiles_row)
            rows_read += 1

        return ret, line_idx
             

class Map(object):

    DEFAULT_MATERIALS_LIST = [
        Material("Grass",           "grass.png"), 
        Material("Cliffs",          "cliffs.png"),
        Material("Grass2",          "grass2.jpg"), 
        Material("Cobblestone",     "cobblestone.jpg"),
        Material("Dirty-Grass",     "dirty_grass.jpg"),
        Material("Dirt-Road",       "dirt_road.jpg"),
        Material("Cracked-Dirt",    "cracked_dirt.jpg"),
        Material("Metal-Platform",  "metal_platform.jpg"),
        Material("Snowy-Grass",     "snowy_grass.jpg"),
        Material("Lava-Ground",     "lava_ground.jpg"),
        Material("Sand",            "sand.jpg"),
    ]

    def __init__(self, chunk_rows, chunk_cols):
        self.filename = None
        self.chunk_rows = chunk_rows
        self.chunk_cols = chunk_cols
        self.chunks = []
        for r in range(0, self.chunk_rows):
            row = [Chunk() for c in range(0, self.chunk_cols)]
            self.chunks.append(row)
        self.materials = Map.DEFAULT_MATERIALS_LIST

    def pfmap_str(self):
        ret = ""
        ret += "version " + str(EDITOR_PFMAP_VERSION) + "\n"
        ret += "num_materials " + str(len(self.materials)) + "\n"
        ret += "num_rows " + str(self.chunk_rows) + "\n"
        ret += "num_cols " + str(self.chunk_cols) + "\n"

        for mat in self.materials:
            ret += mat.pfmap_str()

        for chunk_r in range(0, self.chunk_rows):
            for chunk_c in range(0, self.chunk_cols):
                ret += self.chunks[chunk_r][chunk_c].pfmap_str()
        return ret

    def write_to_file(self):
        if self.filename is not None:
            with open(self.filename, "w") as mapfile:
                mapfile.write(self.pfmap_str())

    def update_tile_mat(self, tile_coords, top_material, blend_mode, blend_normals):

        chunk = self.chunks[tile_coords[0][0]][tile_coords[0][1]]
        tile = chunk.tiles[tile_coords[1][0]][tile_coords[1][1]]

        tile.top_mat_idx = self.materials.index(top_material)
        tile.blend_mode = blend_mode
        tile.blend_normals = blend_normals

        pf.update_tile(tile_coords[0], tile_coords[1], tile)

    def update_tile(self, tile_coords, newheight, newtype, new_side_mat, new_ramp_height, new_blend_mode, new_blend_normals):
        chunk = self.chunks[tile_coords[0][0]][tile_coords[0][1]]
        tile = chunk.tiles[tile_coords[1][0]][tile_coords[1][1]]
        tile.base_height = newheight
        tile.type = newtype
        tile.sides_mat_idx = self.materials.index(new_side_mat)
        tile.ramp_height = new_ramp_height
        tile.blend_mode = new_blend_mode
        tile.blend_normals = new_blend_normals
        pf.update_tile(tile_coords[0], tile_coords[1], tile)

    def relative_tile_coords(self, global_r, global_c, dr, dc):

        if global_r + dr < 0 or global_r + dr >= self.chunk_rows * pf.TILES_PER_CHUNK_HEIGHT:
            return None

        if global_c + dc < 0 or global_c + dc >= self.chunk_cols * pf.TILES_PER_CHUNK_WIDTH:
            return None

        chunk_r = (global_r + dr) // pf.TILES_PER_CHUNK_HEIGHT
        chunk_c = (global_c + dc) // pf.TILES_PER_CHUNK_WIDTH
        tile_r  = (global_r + dr) %  pf.TILES_PER_CHUNK_HEIGHT
        tile_c  = (global_c + dc) %  pf.TILES_PER_CHUNK_WIDTH

        return (chunk_r, chunk_c), (tile_r, tile_c)

    def tile_at_coords(self, chunk_coords, tile_coords):
        chunk_r, chunk_c = chunk_coords
        tile_r, tile_c = tile_coords
        return self.chunks[chunk_r][chunk_c].tiles[tile_r][tile_c]

    def relative_tile(self, global_r, global_c, dr, dc):
        tc = self.relative_tile_coords(global_r, global_c, dr, dc)
        if tc is None:
            return None
        return self.tile_at_coords(*tc)

    @classmethod
    def from_filepath(cls, filepath):
        with open(filepath, "r") as mapfile:
            mapdata = mapfile.read()
            ret = Map.from_string(mapdata)
            ret.filename = filepath
            return ret

    @classmethod
    def from_string(cls, string):
        ret = Map(0, 0)
        lines = string.split("\n")
        line_idx = 0

        try:
            assert lines[0].split()[1] == str(EDITOR_PFMAP_VERSION)
            num_mats = int(lines[1].split()[1])
            ret.chunk_rows = int(lines[2].split()[1])
            ret.chunk_cols = int(lines[3].split()[1])
            line_idx += 4 #skip past header

            ret.materials = []
            for _ in range(0, num_mats):
                ret.materials += [Material.from_string(lines[line_idx])]
                line_idx += 1
            for mat in Map.DEFAULT_MATERIALS_LIST:
                if mat.texname not in [m.texname for m in ret.materials]:
                    ret.materials += [mat]

            for r in range(0, ret.chunk_rows):
                row = []
                for c in range(0, ret.chunk_cols):
                    new_chunk, lines_read = Chunk.from_lines(lines[line_idx:])
                    row.append(new_chunk)
                    line_idx += lines_read
                ret.chunks.append(row)
        except:
            traceback.print_exc()
            print("Could not parse PFMAP string.")
            return None

        return ret

