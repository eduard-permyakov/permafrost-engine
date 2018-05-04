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
import traceback
import copy


EDITOR_PFMAP_VERSION = 1.0

TILETYPE_FLAT              = 0x0
TILETYPE_RAMP_SN           = 0x1
TILETYPE_RAMP_NS           = 0x2
TILETYPE_RAMP_EW           = 0x3
TILETYPE_RAMP_WE           = 0x4
TILETYPE_CORNER_CONCAVE_SW = 0x5
TILETYPE_CORNER_CONVEX_SW  = 0x6
TILETYPE_CORNER_CONCAVE_SE = 0x7
TILETYPE_CORNER_CONVEX_SE  = 0x8
TILETYPE_CORNER_CONCAVE_NW = 0x9
TILETYPE_CORNER_CONVEX_NW  = 0xa
TILETYPE_CORNER_CONCAVE_NE = 0xb
TILETYPE_CORNER_CONVEX_NE  = 0xc


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

def tile_from_string(string):
    assert len(string) == 6
    ret = pf.Tile()
    ret.type = int(string[0], 16)
    ret.pathable = int(string[1])
    ret.base_height = int(string[2])
    ret.top_mat_idx = int(string[3])
    ret.sides_mat_idx = int(string[4])
    ret.ramp_height = int(string[5])
    return ret


class Material(object):

    def __init__(self, name, texname, ambient, diffuse, specular):
        self.refcount = 0
        self.name = name
        self.texname = texname
        self.ambient = ambient
        self.diffuse = diffuse
        self.specular = specular

    def __eq__(self, other): 
        if isinstance(other, Material):
            # Ignore 'refcount' attribute when comparing equality
            return  self.name == other.name \
                and self.texname == other.texname \
                and self.ambient == other.ambient \
                and self.diffuse == other.diffuse \
                and self.specular == other.specular
        else:
            return False

    def __ne__(self, other):
        return not self.__eq__(other)

    def pfmap_str(self):
        ret = ""
        ret += "material " + self.name + "\n"
        ret += "\tambient {0:.6f}\n".format(self.ambient)
        ret += "\tdiffuse {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n".format(v=self.diffuse)
        ret += "\tspecular {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n".format(v=self.specular)
        ret += "\ttexture {0}\n".format(self.texname)
        return ret

    @classmethod
    def from_lines(cls, lines):
        name = lines[0].split()[1]
        texname = lines[4].split()[1]
        ambient = float(lines[1].split()[1])
        diffuse = tuple([float(s) for s in lines[2].split()[1:]])
        specular = tuple([float(s) for s in lines[3].split()[1:]])
        return Material(name, texname, ambient, diffuse, specular)


class Chunk(object):
    DEFAULT_TOP_MATERIAL  = Material("Grass",  "grass.png",  1.0, (0.3, 0.3, 0.3), (0.1, 0.1, 0.1))
    DEFAULT_SIDE_MATERIAL = Material("Cliffs", "cliffs.png", 1.0, (0.4, 0.4, 0.4), (0.2, 0.2, 0.2))

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
        self.materials[0] = copy.deepcopy(Chunk.DEFAULT_TOP_MATERIAL)
        self.materials[0].refcount = self.rows * self.cols
        self.materials[1] = copy.deepcopy(Chunk.DEFAULT_SIDE_MATERIAL)
        self.materials[1].refcount = self.rows * self.cols

    def pfmap_str(self):
        ret = ""
        for r in range(0, self.rows):
            for c in range(0, self.cols):
                ret += tile_to_string(self.tiles[r][c]) + " "
            ret += "\n"
        ret += self.materials_str()
        return ret

    def materials_str(self):
        ret = ""
        for m in self.materials:
            if m is not None:
                ret += m.pfmap_str()
            else:
                ret += "material __none__\n"
        return ret

    def index_for_mat(self, material):
        try:
            return self.materials.index(material)
        except: 
            return None

    def free_material_slots(self):
        return len([m for m in self.materials if m is None])

    def free_material_slot_idx(self):
        return self.materials.index(None)

    @classmethod
    def from_lines(cls, lines):
        ret = Chunk()
        ret.tiles = []
        ret.materials = [None]*pf.MATERIALS_PER_CHUNK
        line_idx = 0

        for r in range(0, pf.TILES_PER_CHUNK_HEIGHT):
            tile_strings = lines[line_idx].split()
            tiles_row = []
            for c in range(0, pf.TILES_PER_CHUNK_WIDTH):
                tile = tile_from_string(tile_strings[c])
                tiles_row.append(tile)
            ret.tiles.append(tiles_row)
            line_idx += 1

        for i in range(0, pf.MATERIALS_PER_CHUNK):
            if lines[line_idx].split()[1] == "__none__":
                line_idx += 1
            else:
                ret.materials[i] = Material.from_lines(lines[line_idx:line_idx+5])
                line_idx += 5

        for r in range(0, pf.TILES_PER_CHUNK_HEIGHT):
            for c in range(0, pf.TILES_PER_CHUNK_WIDTH):
                ret.materials[ret.tiles[r][c].top_mat_idx].refcount += 1
                ret.materials[ret.tiles[r][c].sides_mat_idx].refcount += 1
        return ret
             

class Map(object):

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
        ret += "num_cols " + str(self.chunk_cols) + "\n"

        for chunk_r in range(0, self.chunk_rows):
            for chunk_c in range(0, self.chunk_cols):
                ret += self.chunks[chunk_r][chunk_c].pfmap_str()
        return ret

    def write_to_file(self):
        if self.filename is not None:
            with open(self.filename, "w") as mapfile:
                mapfile.write(self.pfmap_str())

    def update_tile_mat(self, tile_coords, top_material):

        chunk = self.chunks[tile_coords[0][0]][tile_coords[0][1]]
        tile = chunk.tiles[tile_coords[1][0]][tile_coords[1][1]]

        if chunk.materials[tile.top_mat_idx] != top_material:

            chunk.materials[tile.top_mat_idx].refcount -= 1
            mat_deleted = chunk.materials[tile.top_mat_idx].refcount == 0
            if mat_deleted:
                chunk.materials[tile.top_mat_idx] = None

            mat_idx = chunk.index_for_mat(top_material)
            if mat_idx is None and chunk.free_material_slots() == 0: 
                print("Only {0} materials allowed per chunk!".format(pf.MATERIALS_PER_CHUNK))
                return

            mat_added = chunk.free_material_slots() > 0 and mat_idx is None
            if mat_added:
                mat_idx = chunk.free_material_slot_idx() 
                chunk.materials[mat_idx] = copy.deepcopy(top_material)

            assert mat_idx is not None
            assert mat_idx >= 0 and mat_idx < pf.MATERIALS_PER_CHUNK
            chunk.materials[mat_idx].refcount += 1
            tile.top_mat_idx = mat_idx
            pf.update_tile(tile_coords[0], tile_coords[1], tile)

            if mat_deleted or mat_added:
                pf.update_chunk_materials(tile_coords[0], chunk.materials_str())

    def update_tile(self, tile_coords, newheight=None, newtype=None, new_ramp_height=None):
        chunk = self.chunks[tile_coords[0][0]][tile_coords[0][1]]
        tile = chunk.tiles[tile_coords[1][0]][tile_coords[1][1]]
        if newheight is not None:
            tile.base_height = newheight
        if newtype is not None:
            tile.type = newtype
        if new_ramp_height is not None:
            tile.ramp_height = new_ramp_height
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
            ret.chunk_rows = int(lines[1].split()[1])
            ret.chunk_cols = int(lines[2].split()[1])
            line_idx += 3 #skip past header

            for r in range(0, ret.chunk_rows):
                row = []
                for c in range(0, ret.chunk_cols):
                    new_chunk =  Chunk.from_lines(lines[line_idx:])
                    row.append(new_chunk)
                    num_mats = len([m for m in new_chunk.materials if m is not None])
                    line_idx += pf.TILES_PER_CHUNK_HEIGHT + (num_mats * 5) + (pf.MATERIALS_PER_CHUNK - num_mats)
                ret.chunks.append(row)
        except:
            traceback.print_exc()
            print("Could not parse PFMAP string.")
            return None

        return ret


