/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2022-2025 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#version 430 core

/*****************************************************************************/
/* DEFINES                                                                   */
/*****************************************************************************/

#define ATTR(_gpuid, _name) \
    moveattrs[_gpuid - 1]._name

#define ATTR_VEC2(_gpuid, _name) \
    vec2(ATTR(_gpuid, _name##_x), ATTR(_gpuid, _name##_z))

#define VEC2_PACKED(_name) \
    float _name##_x, _name##_z

#define FLOCK_ATTR(_flockid, _name) \
    flocks[_flockid - 1]._name

#define FLOCK_ATTR_VEC2(_flockid, _name) \
    vec2(FLOCK_ATTR(_flockid, _name##_x), FLOCK_ATTR(_flockid, _name##_z))

/* Must match movement.c */

#define ENTITY_MASS                 (1.0f)
#define EPSILON                     (1.0f/1024)

#define X_COORDS_PER_TILE           (8)
#define Z_COORDS_PER_TILE           (8)

#define STATE_MOVING                0
#define STATE_MOVING_IN_FORMATION   1
#define STATE_ARRIVED               2
#define STATE_SEEK_ENEMIES          3
#define STATE_WAITING               4
#define STATE_SURROUND_ENTITY       5
#define STATE_ENTER_ENTITY_RANGE    6
#define STATE_TURNING               7
#define STATE_ARRIVING_TO_CELL      8

#define ENTITY_FLAG_ANIMATED        (1 << 0)
#define ENTITY_FLAG_COLLISION       (1 << 1)
#define ENTITY_FLAG_SELECTABLE      (1 << 2)
#define ENTITY_FLAG_MOVABLE         (1 << 3)
#define ENTITY_FLAG_COMBATABLE      (1 << 4)
#define ENTITY_FLAG_INVISIBLE       (1 << 5)
#define ENTITY_FLAG_ZOMBIE          (1 << 6)
#define ENTITY_FLAG_MARKER          (1 << 7)
#define ENTITY_FLAG_BUILDING        (1 << 8)
#define ENTITY_FLAG_BUILDER         (1 << 9)
#define ENTITY_FLAG_TRANSLUCENT     (1 << 10)
#define ENTITY_FLAG_RESOURCE        (1 << 11)
#define ENTITY_FLAG_HARVESTER       (1 << 12)
#define ENTITY_FLAG_STORAGE_SITE    (1 << 13)
#define ENTITY_FLAG_WATER           (1 << 14)
#define ENTITY_FLAG_AIR             (1 << 15)
#define ENTITY_FLAG_GARRISON        (1 << 16)
#define ENTITY_FLAG_GARRISONABLE    (1 << 17)
#define ENTITY_FLAG_GARRISONED      (1 << 18)

#define MAX_FLOCK_MEMBERS           (1024) 
#define MAX_FORCE                   (0.75f)
#define MAX_NEAR_ENTS               (128)

#define SEPARATION_FORCE_SCALE      (0.6f)
#define MOVE_ARRIVE_FORCE_SCALE     (0.5f)
#define MOVE_COHESION_FORCE_SCALE   (0.15f)
#define ALIGNMENT_FORCE_SCALE       (0.15f)

#define SEPARATION_BUFFER_DIST      (0.0f)
#define COHESION_NEIGHBOUR_RADIUS   (50.0f)
#define ARRIVE_SLOWING_RADIUS       (10.0f)
#define ADJACENCY_SEP_DIST          (5.0f)
#define ALIGN_NEIGHBOUR_RADIUS      (10.0f)
#define SEPARATION_NEIGHB_RADIUS    (30.0f)
#define CELL_ARRIVAL_RADIUS         (30.0f)

#define COLLISION_MAX_SEE_AHEAD     (10.0f)
#define WAIT_TICKS                  (60)
#define MAX_TURN_RATE               (15.0f)
#define MAX_NEIGHBOURS              (32)

#define CLEARPATH_NEIGHBOUR_RADIUS  (10.0f)
#define CLEARPATH_BUFFER_RADIUS     (0.0f)
#define CLEARPATH_MAX_XPOINTS       (32)

#define NUM_LAYERS                  (12)

#define TILES_PER_CHUNK_HEIGHT      (32)
#define TILES_PER_CHUNK_WIDTH       (32)
#define FIELD_RES_R                 (64)
#define FIELD_RES_C                 (64)
#define X_COORDS_PER_TILE           (8)
#define Z_COORDS_PER_TILE           (8)

/* Must match gl_position.c */
#define POSTEX_RES_SCALE            (2)

/*****************************************************************************/
/* INPUT/OUTPUT                                                              */
/*****************************************************************************/

/* Careful with using verex and matrix types because they have
 * greater alignment requirements, and may introduce padding
 * into this struct.
 */

/* Must match movement.c */
struct move_input{
    VEC2_PACKED(dest);
    VEC2_PACKED(vdes);
    VEC2_PACKED(cell_pos);
    VEC2_PACKED(formation_cohesion_force);
    VEC2_PACKED(formation_align_force);
    VEC2_PACKED(formation_drag_force);
    VEC2_PACKED(pos);
    VEC2_PACKED(velocity);
    uint  movestate;
    uint  flock_id;
    uint  flags;
    float speed;
    float max_speed;
    float radius;
    uint  layer;
    uint  has_dest_los;
    uint  formation_assignment_ready;
    uint  _pad0; /* Keep aligned to vec2 size */
};

/* Must match movement.c */
struct flock{
    uint  ents[MAX_FLOCK_MEMBERS];
    uint  nmembers;
    float target_x, target_z;
};

/* Velocity Obstacle */
struct VO{
    vec2 xz_apex;
    vec2 xz_left_side;
    vec2 xz_right_side;
};

/* Reciprocal Velocity Obstacle */
struct RVO{
    vec2 xz_apex;
    vec2 xz_left_side;
    vec2 xz_right_side;
};

/* Hybrid Reciprocal Velocity Obstacle */
struct HRVO{
    vec2 xz_apex;
    vec2 xz_left_side;
    vec2 xz_right_side;
};

struct ray{
    vec2 point;
    vec2 dir;
};

layout(local_size_x = 1) in;

/* A flat array of GPU IDs that should have new velocities
 * calculated. The GPU IDs can be used to index the auxiliary
 * moveattrs buffer. While we require the attributes for all
 * moveable entities, we only need to perform the velocity
 * calculations for those that are currently not stationary.
 */
layout(std430, binding = 0) readonly buffer in_dispatch_ids
{
    uint gpuids[];
};

/* Per-entitty attributes.
 */
layout(std430, binding = 1) readonly buffer in_movedata
{
    move_input moveattrs[];
};

/* Collections of entities. The entity set is a flat 
 * array containting GPU ID's of entities.
 */
layout(std430, binding = 2) readonly buffer in_flocks
{
    flock flocks[];
};

/* A 2D texture, covering the entire map surface, which
 * stores entities' GPU IDs at the location (pixel) where they 
 * are present. Essentially, a spacial hash of all the entities'
 * positions.
 */
layout(r32ui,  binding = 3) uniform readonly uimage2D in_pos_id_map;

/* The cost base field of the map. Stores layers together,
 * with chunks for each layer stored in row-major order.
 * Each element is a single bytes.
 */
layout(std430, binding = 4) readonly buffer in_cost_base
{
    uint cost_base[];
};

/* The blockers field of the map. Stores layers together,
 * with chunks for each layer stored in row-major order.
 * Each element is 2 bytes.
 */
layout(std430, binding = 5) readonly buffer in_blockers
{
    uint blockers[];
};

layout(std430, binding = 6) writeonly buffer o_data
{
    vec2 velocities[];
};

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform ivec4 map_resolution;
uniform vec2  map_pos;
uniform int   ticks_hz;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

vec2 tile_dims()
{
    float x_ratio = float(TILES_PER_CHUNK_WIDTH) / FIELD_RES_C;
    float z_ratio = float(TILES_PER_CHUNK_HEIGHT) / FIELD_RES_R;
    return vec2(
        x_ratio * X_COORDS_PER_TILE,
        z_ratio * Z_COORDS_PER_TILE
    );
}

/*
 * x = chunk_r
 * y = chunk_c
 * z = tile_r
 * a = tile_c
 */
ivec4 nav_tile_desc_at(vec3 ws_pos)
{
    int chunk_w = map_resolution[0];
    int chunk_h = map_resolution[1];
    int tile_w = FIELD_RES_R;
    int tile_h = FIELD_RES_C;
    float xtile = tile_dims().x;
    float ztile = tile_dims().y;

    float chunk_x_dist = tile_w * xtile;
    float chunk_z_dist = tile_h * ztile;

    int chunk_r = int(abs(map_pos.y - ws_pos.z) / chunk_z_dist);
    int chunk_c = int(abs(map_pos.x - ws_pos.x) / chunk_x_dist);

    int chunk_base_x = int(map_pos.x - (chunk_c * chunk_x_dist));
    int chunk_base_z = int(map_pos.y + (chunk_r * chunk_z_dist));

    int tile_c = int(abs(chunk_base_x - ws_pos.x) / xtile);
    int tile_r = int(abs(chunk_base_z - ws_pos.z) / ztile);

    return ivec4(chunk_r, chunk_c, tile_r, tile_c);
}

uint extract_byte_from_dword(uint word, uint byte_idx)
{
    if(byte_idx == 0) {
        return (word & 0xff);
    }else if(byte_idx == 1) {
        return ((word >> 8) & 0xff);
    }else if(byte_idx == 2) {
        return ((word >> 16) & 0xff);
    }else if(byte_idx == 3) {
        return ((word >> 24) & 0xff);
    }
    return 0;
}

uint extract_half_from_dword(uint word, uint idx)
{
    if(idx == 0) {
        return (word & 0xffff);
    }else if(idx == 1) {
        return ((word >> 16) & 0xffff);
    }
    return 0;
}

bool position_pathable(uint layer, vec2 pos)
{
    uint chunk_bytes = (FIELD_RES_R * FIELD_RES_C);
    uint layer_bytes = chunk_bytes * map_resolution[0] * map_resolution[1];
    ivec4 desc = nav_tile_desc_at(vec3(pos.x, 0, pos.y));

    uint layer_offset_bytes = layer_bytes * layer;
    uint chunk_offset_bytes = (desc.x * map_resolution[0] + desc.y) * chunk_bytes;

    uint layer_offset_words = layer_offset_bytes / 4;
    uint chunk_offset_words = chunk_offset_bytes / 4;
    uint buffer_words = (layer_bytes * NUM_LAYERS) / 4;

    uint row_offset_bytes = FIELD_RES_C * desc.z;
    uint row_offset_words = row_offset_bytes / 4;

    /* Each 'cost base' tile is one byte, so we have 4 tiles packed per word.
     * This way, 4 indices are used up per word, and the overflow can be used
     * to extract the byte from the word.
     */
    uint col_idx = desc.a;
    uint col_word_idx = (col_idx >> 2);
    uint col_byte_idx = (col_idx & 0x3);

    /* Handle cases where we are outside the map bounds */
    uint dword_idx = layer_offset_words + chunk_offset_words + row_offset_words + col_word_idx;
    if(dword_idx > buffer_words)
        return false;

    uint dword = cost_base[dword_idx];
    uint byte = extract_byte_from_dword(dword, col_byte_idx);
    return (byte != 0xff);
}

bool position_blocked(uint layer, vec2 pos)
{
    uint chunk_bytes = 2 * (FIELD_RES_R * FIELD_RES_C);
    uint layer_bytes = chunk_bytes * map_resolution[0] * map_resolution[1];
    ivec4 desc = nav_tile_desc_at(vec3(pos.x, 0, pos.y));

    uint layer_offset_bytes = layer_bytes * layer;
    uint chunk_offset_bytes = (desc.x * map_resolution[0] + desc.y) * chunk_bytes;

    uint layer_offset_words = layer_offset_bytes / 4;
    uint chunk_offset_words = chunk_offset_bytes / 4;
    uint buffer_words = (layer_bytes * NUM_LAYERS) / 4;

    uint row_offset_bytes = (2 * FIELD_RES_C) * desc.z;
    uint row_offset_words = row_offset_bytes / 4;

    /* Each 'cost base' tile is two bytes, so we have 2 tiles packed per word.
     * This way, 2 indices are used up per word, and the overflow can be used
     * to extract the halfword from the word.
     */

    uint col_idx = desc.a;
    uint col_word_idx = (col_idx >> 1);
    uint col_half_idx = (col_idx & 0x1);

    /* Handle cases where we are outside the map bounds */
    uint dword_idx = layer_offset_words + chunk_offset_words + row_offset_words + col_word_idx;
    if(dword_idx > buffer_words)
        return true;

    uint dword = blockers[dword_idx];
    uint halfword = extract_half_from_dword(dword, col_half_idx);
    return (halfword > 0);
}

vec2 nullify_impass_components(uint gpuid, vec2 force)
{
    vec2 ret = force;
    vec2 nt_dims = tile_dims();
    float radius = ATTR(gpuid, radius);
    uint layer = ATTR(gpuid, layer);

    vec2 pos = ATTR_VEC2(gpuid, pos);
    vec2 left  = vec2(pos.x + nt_dims.x, pos.y);
    vec2 right = vec2(pos.x - nt_dims.x, pos.y);
    vec2 top   = vec2(pos.x, pos.y + nt_dims.y);
    vec2 bot   = vec2(pos.x, pos.y - nt_dims.y);

    if(ret.x > 0 && (!position_pathable(layer, left) || position_blocked(layer, left))) {
        ret.x = 0.0;
    }
    if(ret.x < 0 && (!position_pathable(layer, right) || position_blocked(layer, right))) {
        ret.x = 0.0;
    }
    if(ret.y > 0 && (!position_pathable(layer, top) || position_blocked(layer, top))) {
        ret.y = 0.0;
    }
    if(ret.y < 0 && (!position_pathable(layer, bot) || position_blocked(layer, bot))) {
        ret.y = 0.0;
    }
    return ret;
}

float scaled_max_force()
{
    return ((MAX_FORCE / ticks_hz) * 20.0);
}

vec2 truncate(vec2 vec, float max)
{
    if(length(vec) > max) {
        return (normalize(vec) * max);
    }
    return vec;
}

/* Cohesion is a behaviour that causes agents to steer towards the center of mass of nearby agents.
 */
vec2 cohesion_force(uint gpuid, uint flockid)
{
    vec2 COM = vec2(0.0, 0.0);
    uint neighbour_count = 0;
    vec2 ent_xz_pos = ATTR_VEC2(gpuid, pos);
    uint nents = FLOCK_ATTR(flockid, nmembers);

    for(uint i = 0; i < nents; i++) {

        uint curr_gpuid = flocks[flockid - 1].ents[i];
        if(curr_gpuid == gpuid)
            continue;

        vec2 curr_xz_pos = ATTR_VEC2(curr_gpuid, pos);
        vec2 diff = curr_xz_pos - ent_xz_pos;

        float t = (length(diff) - COHESION_NEIGHBOUR_RADIUS*0.75) 
                / COHESION_NEIGHBOUR_RADIUS;
        float scale = exp(-6.0f * t);

        vec2 added = curr_xz_pos * scale;
        COM += added;
        neighbour_count++;
    }

    if(0 == neighbour_count)
        return vec2(0.0, 0.0);

    COM *= (1.0 / neighbour_count);
    vec2 ret = COM - ent_xz_pos;
    ret = truncate(ret, scaled_max_force());
    return ret;
}

uint ents_in_circle(vec2 origin, float radius, out uint near_ents[MAX_NEAR_ENTS])
{
    uint ret = 0;
    /* Find the x,y image coordinate of the origin 
     */
    int resx = map_resolution.x * map_resolution.z * X_COORDS_PER_TILE;
    int resz = map_resolution.y * map_resolution.w * Z_COORDS_PER_TILE;

    /* The image has (0, 0) in the bot left corner, which corresponds
     * to the (+x, -z) direction. The coordinate of the top right corner
     * is (resx-1, resy-1), which correspons to the (-x, +z) direction.
     */
    float origin_normal_x = ((origin.x - map_pos.x) / (resx) * -1); /* [0 -> 1] */
    float origin_normal_z = ((origin.y - map_pos.y) / (resz) * +1); /* [0 -> 1] */

    int relative_origin_x = int(round(origin_normal_x * imageSize(in_pos_id_map).x));
    int relative_origin_z = int(round(origin_normal_z * imageSize(in_pos_id_map).y));

    /* Find the distance corresponding to a single pixel in 
     * the posbuff texture.
     */
    int x_pixels_radius = int(ceil(radius)) * POSTEX_RES_SCALE + 1;
    int z_pixels_radius = int(ceil(radius)) * POSTEX_RES_SCALE + 1;

    /* Do a grid scan.
     * Increasing x-coord -> (-z -> +z)
     * Increasing z-coord -> (+x -> -x)
     */
    for(int dx = -x_pixels_radius; dx <= x_pixels_radius; dx++) {
    for(int dz = -z_pixels_radius; dz <= z_pixels_radius; dz++) {

        int x = relative_origin_x + dx;
        int z = relative_origin_z + dz;

        if(x < 0 || x >= imageSize(in_pos_id_map).x)
            continue;
        if(z < 0 || z >= imageSize(in_pos_id_map).y)
            continue;

        uvec4 bin_contents = imageLoad(in_pos_id_map, ivec2(x, z));
        if(bin_contents.r > 0) {

            vec2 ent_pos = ATTR_VEC2(bin_contents.r, pos);
            if(length(ent_pos - origin) > radius)
                continue;

            near_ents[ret++] = bin_contents.r;
            if(ret == MAX_NEAR_ENTS)
                break;
        }
    }}
    return ret;
}

/* Separation is a behaviour that causes agents to steer away from nearby agents.
 */
vec2 separation_force(uint gpuid, float buffer_dist)
{
    vec2 ret = vec2(0.0, 0.0);
    uint near_ents[MAX_NEAR_ENTS];
    vec2 pos_xz = ATTR_VEC2(gpuid, pos);
    uint num_near = ents_in_circle(pos_xz, SEPARATION_NEIGHB_RADIUS, near_ents);

    for(int i = 0; i < num_near; i++) {

        uint curr = near_ents[i];
        if(curr == gpuid)
            continue;

        uint curr_flags = ATTR(curr, flags);
        uint ent_flags = ATTR(gpuid, flags);
        if((curr_flags & ENTITY_FLAG_AIR) != (ent_flags & ENTITY_FLAG_AIR))
            continue;

        vec2 curr_pos = ATTR_VEC2(curr, pos);
        vec2 ent_pos = ATTR_VEC2(gpuid, pos);
        vec2 diff = curr_pos - ent_pos;

        if(length(diff) < EPSILON)
            continue;

        float radius = ATTR(curr, radius) + ATTR(gpuid, radius) + buffer_dist;
        float t = (length(diff) - radius*0.85) / length(diff);
        float scale = exp(-20.0 * t);

        diff *= scale;
        ret += diff;
    }

    if(0 == num_near)
        return vec2(0.0, 0.0);

    ret *= -1.0;
    ret = truncate(ret, scaled_max_force());
    return ret;
}

vec2 arrive_force_point(uint gpuid, vec2 target_xz, vec2 vdes, uint has_dest_los)
{
    vec2 desired_velocity = vec2(0.0, 0.0);
    vec2 pos_xz = ATTR_VEC2(gpuid, pos);
    float max_speed = ATTR(gpuid, max_speed);
    float scale = max_speed / float(ticks_hz);

    if(bool(has_dest_los)) {

        vec2 delta = target_xz - pos_xz;
        float distance = length(delta);

        desired_velocity = normalize(delta);
        desired_velocity *= scale;

        if(distance < ARRIVE_SLOWING_RADIUS) {
            desired_velocity *= (distance / ARRIVE_SLOWING_RADIUS);
        }
    }else{
        desired_velocity = vdes * scale;
    }

    vec2 ret = desired_velocity - ATTR_VEC2(gpuid, velocity);
    ret = truncate(ret, scaled_max_force());
    return ret;
}

vec2 arrive_force_enemies(uint gpuid, vec2 vdes)
{
    vec2 pos_xz = ATTR_VEC2(gpuid, pos);
    float max_speed = ATTR(gpuid, max_speed);
    vec2 desired_velocity = vdes * (max_speed / ticks_hz);
    vec2 ret = desired_velocity - ATTR_VEC2(gpuid, velocity);
    ret = truncate(ret, scaled_max_force());
    return ret;
}

vec2 enemy_seek_total_force(uint gpuid, vec2 vdes)
{
    vec2 arrive = arrive_force_enemies(gpuid, vdes);
    vec2 separation = separation_force(gpuid, SEPARATION_BUFFER_DIST);

    arrive *= MOVE_ARRIVE_FORCE_SCALE;
    separation *= SEPARATION_FORCE_SCALE;

    vec2 ret = vec2(0.0, 0.0);
    ret += arrive;
    ret += separation;

    ret = truncate(ret, scaled_max_force());
    return ret;
}

vec2 enemy_seek_vpref(uint gpuid, float speed, vec2 vdes)
{
    vec2 steer_force = enemy_seek_total_force(gpuid, vdes);
    vec2 accel = steer_force * (1.0 / ENTITY_MASS);
    vec2 new_vel = ATTR_VEC2(gpuid, velocity) + accel;
    new_vel = truncate(new_vel, speed / ticks_hz);
    return new_vel;
}

vec2 arrive_force_cell(uint gpuid, vec2 cell_pos, vec2 vdes)
{
    vec2 pos_xz = ATTR_VEC2(gpuid, pos);
    vec2 desired_velocity = cell_pos - pos_xz;
    float distance = length(desired_velocity);

    if(distance < ARRIVE_SLOWING_RADIUS) {
        desired_velocity *= (distance / ARRIVE_SLOWING_RADIUS);
    }else{
        float max_speed = ATTR(gpuid, max_speed);
        desired_velocity = vdes * (max_speed / ticks_hz);
    }
    return desired_velocity;
}

vec2 cell_seek_total_force(uint gpuid, vec2 cell_pos, vec2 vdes,
                           vec2 cohesion, vec2 alignment)
{
    vec2 pos_xz = ATTR_VEC2(gpuid, pos);
    vec2 delta = cell_pos - pos_xz;

    vec2 arrive = arrive_force_cell(gpuid, cell_pos, vdes);
    vec2 separation = separation_force(gpuid, SEPARATION_BUFFER_DIST);

    arrive *= MOVE_ARRIVE_FORCE_SCALE;
    separation *= SEPARATION_FORCE_SCALE;
    cohesion *= MOVE_COHESION_FORCE_SCALE;
    alignment *= ALIGNMENT_FORCE_SCALE;

    vec2 ret = vec2(0.0, 0.0);
    ret += arrive;
    ret += separation;

    if(length(delta) > CELL_ARRIVAL_RADIUS) {
        ret += cohesion;
        ret += alignment;
    }

    ret = truncate(ret, scaled_max_force());
    return ret;
}

vec2 cell_arrival_seek_vpref(uint gpuid, vec2 cell_pos, float speed, vec2 vdes,
                             vec2 cohesion, vec2 alignment, vec2 drag)
{
    vec2 steer_force = vec2(0.0, 0.0);
    for(int prio = 0; prio < 3; prio++) {

        if(prio == 0) {
            steer_force = cell_seek_total_force(gpuid, cell_pos, vdes, cohesion, alignment);
        }else if(prio == 1) {
            steer_force = separation_force(gpuid, SEPARATION_BUFFER_DIST);
        }else if(prio == 2) {
            steer_force = arrive_force_cell(gpuid, cell_pos, vdes);
        }
        steer_force = nullify_impass_components(gpuid, steer_force);
        if(length(steer_force) > scaled_max_force() * 0.01)
            break;
    }

    vec2 accel = steer_force * (1.0 / ENTITY_MASS);
    vec2 new_vel = ATTR_VEC2(gpuid, velocity) + accel;
    new_vel = truncate(new_vel, speed / ticks_hz);
    if(length(drag) > EPSILON) {
        new_vel = truncate(new_vel, speed * 0.75 / ticks_hz);
    }
    return new_vel;
}

vec2 formation_point_seek_total_force(uint gpuid, uint flockid, vec2 vdes, vec2 cohesion,
                                      vec2 alignment, uint has_dest_los)
{
    vec2 arrive = arrive_force_point(gpuid, FLOCK_ATTR_VEC2(flockid, target), vdes, has_dest_los);
    vec2 separation = separation_force(gpuid, SEPARATION_BUFFER_DIST);

    arrive *= MOVE_ARRIVE_FORCE_SCALE;
    cohesion *= MOVE_COHESION_FORCE_SCALE;
    separation *= SEPARATION_FORCE_SCALE;
    alignment *= ALIGNMENT_FORCE_SCALE;

    vec2 ret = vec2(0.0, 0.0);
    ret += arrive;
    ret += separation;
    ret += cohesion;

    ret = truncate(ret, scaled_max_force());
    return ret;
}

vec2 formation_seek_vpref(uint gpuid, uint flockid, float speed, vec2 vdes, vec2 cohesion, 
                          vec2 alignment, vec2 drag, uint has_dest_los)
{
    vec2 steer_force = vec2(0.0, 0.0);
    for(int prio = 0; prio < 3; prio++) {

        if(prio == 0) {
            steer_force = formation_point_seek_total_force(gpuid, flockid, vdes, cohesion,
                alignment, has_dest_los);
        }else if(prio == 1) {
            steer_force = separation_force(gpuid, SEPARATION_BUFFER_DIST);
        }else if(prio == 2) {
            steer_force = arrive_force_point(gpuid, FLOCK_ATTR_VEC2(flockid, target), vdes,
                has_dest_los);
        }
        steer_force = nullify_impass_components(gpuid, steer_force);
        if(length(steer_force) > scaled_max_force() * 0.01)
            break;
    }

    vec2 accel = steer_force * (1.0 / ENTITY_MASS);
    vec2 new_vel = ATTR_VEC2(gpuid, velocity) + accel;
    new_vel = truncate(new_vel, speed / ticks_hz);
    if(length(drag) > EPSILON) {
        new_vel = truncate(new_vel, (speed * 0.75) / ticks_hz);
    }
    return new_vel;
}

vec2 point_seek_total_force(uint gpuid, uint flockid, vec2 vdes, uint has_dest_los)
{
    vec2 arrive = arrive_force_point(gpuid, FLOCK_ATTR_VEC2(flockid, target), vdes, has_dest_los);
    vec2 cohesion = cohesion_force(gpuid, flockid);
    vec2 separation = separation_force(gpuid, SEPARATION_BUFFER_DIST);

    arrive *= MOVE_ARRIVE_FORCE_SCALE;
    cohesion *= MOVE_COHESION_FORCE_SCALE;
    separation *= SEPARATION_FORCE_SCALE;

    vec2 ret = vec2(0.0, 0.0);
    ret += arrive;
    ret += separation;
    ret += cohesion;

    ret = truncate(ret, scaled_max_force());
    return ret;
}

vec2 point_seek_vpref(uint gpuid, uint flockid, vec2 vdes, uint has_dest_los, float speed)
{
    vec2 steer_force = vec2(0.0, 0.0);
    for(int prio = 0; prio < 3; prio++) {

        if(prio == 0) {
            steer_force = point_seek_total_force(gpuid, flockid, vdes, has_dest_los);
        }else if(prio == 1) {
            steer_force = separation_force(gpuid, SEPARATION_BUFFER_DIST);
        }else if(prio == 2) {
            steer_force = arrive_force_point(gpuid, FLOCK_ATTR_VEC2(flockid, target), 
                vdes, has_dest_los);
        }
        steer_force = nullify_impass_components(gpuid, steer_force);
        if(length(steer_force) > scaled_max_force() * 0.01)
            break;
    }

    vec2 accel = steer_force * (1.0 / ENTITY_MASS);
    vec2 new_vel = ATTR_VEC2(gpuid, velocity) + accel;
    new_vel = truncate(new_vel, speed / ticks_hz);

    return new_vel;
}

bool ent_still(uint gpuid)
{
    uint state = ATTR(gpuid, movestate);
    return (state == STATE_ARRIVED || state == STATE_WAITING);
}

ivec2 find_neighbours(in uint gpuid, 
                     inout uint static_neighbours[MAX_NEIGHBOURS], 
                     inout uint dynamic_neighbours[MAX_NEIGHBOURS])
{
    ivec2 ret = ivec2(0, 0);
    vec2 pos_xz = ATTR_VEC2(gpuid, pos);
    uint ent_flags = ATTR(gpuid, flags);

    uint near_ents[MAX_NEAR_ENTS];
    uint num_near = ents_in_circle(pos_xz, CLEARPATH_NEIGHBOUR_RADIUS, near_ents);

    for(int i = 0; i < num_near; i++) {

        uint curr_gpuid = near_ents[i];
        uint curr_flags = ATTR(curr_gpuid, flags);

        if(curr_gpuid == gpuid)
            continue;

        float radius = ATTR(curr_gpuid, radius);
        if(radius == 0.0)
            continue;

        if((ent_flags & ENTITY_FLAG_AIR) != (curr_flags & ENTITY_FLAG_AIR))
            continue;

        if(ent_still(curr_gpuid)) {
            if(ret.x < MAX_NEIGHBOURS)
                static_neighbours[ret.x++] = curr_gpuid;
        }else{
            if(ret.y < MAX_NEIGHBOURS)
                dynamic_neighbours[ret.y++] = curr_gpuid;
        }
    }
    return ret;
}

bool same_position(vec2 a, vec2 b)
{
    vec2 delta = b - a;
    return (length(delta) < EPSILON);
}

bool infinite_rays_intersection(in struct ray l1, 
                                in struct ray l2, 
                                out vec2 out_intersec_point)
{
    bool l1_zero = abs(l1.dir.x) < EPSILON;
    bool l2_zero = abs(l2.dir.x) < EPSILON;
    float l1_slope = l1_zero ? 1e30 : (l1.dir.y / l1.dir.x);
    float l2_slope = l2_zero ? 1e30 : (l2.dir.y / l2.dir.x);

    if(l1_zero && l2_zero)
        return false;

    if(abs(l1_slope - l2_slope) < EPSILON)
        return false;

    if(l1_zero && !l2_zero) {

        out_intersec_point = vec2(
            l1.point.x,
            (l1.point.x - l2.point.x) * l2_slope + l2.point.y
        );

    }else if(!l1_zero && l2_zero) {

        out_intersec_point = vec2(
            l2.point.x,
            (l2.point.x - l1.point.x) * l1_slope + l2.point.y
        );

    }else{

        float x = (l1_slope * l1.point.x - l2_slope * l2.point.x + l2.point.y - l1.point.y) 
                / (l1_slope - l2_slope);
        float y = l2_slope * (x - l2.point.x) + l2.point.y;
        out_intersec_point = vec2(x, y);
    }

    return true;
}

void compute_vo_edges(uint gpuid, uint neighb_gpuid, 
                      out vec2 xz_right, out vec2 xz_left)
{
    vec2 ent_to_nb = normalize(ATTR_VEC2(neighb_gpuid, pos) - ATTR_VEC2(gpuid, pos));
    vec2 right = vec2(-ent_to_nb.y, ent_to_nb.x);
    right *= ATTR(neighb_gpuid, radius) + ATTR(gpuid, radius) + CLEARPATH_BUFFER_RADIUS;

    vec2 right_tangent = ATTR_VEC2(neighb_gpuid, pos) + right;
    vec2 left_tangent = ATTR_VEC2(neighb_gpuid, pos) - right;

    xz_right = normalize(right_tangent - ATTR_VEC2(gpuid, pos));
    xz_left = normalize(left_tangent - ATTR_VEC2(gpuid, pos));
}

struct VO compute_vo(uint gpuid, uint neighb_gpuid)
{
    struct VO ret;
    compute_vo_edges(gpuid, neighb_gpuid, ret.xz_right_side, ret.xz_left_side);
    ret.xz_apex = ATTR_VEC2(gpuid, pos) + ATTR_VEC2(neighb_gpuid, velocity);
    return ret;
}

struct RVO compute_rvo(uint gpuid, uint neighb_gpuid)
{
    struct RVO ret;
    compute_vo_edges(gpuid, neighb_gpuid, ret.xz_right_side, ret.xz_left_side);

    vec2 apex_off = ATTR_VEC2(gpuid, velocity) + ATTR_VEC2(neighb_gpuid, velocity);
    apex_off *= 0.5;
    ret.xz_apex = ATTR_VEC2(gpuid, pos) + apex_off;

    return ret;
}

struct HRVO compute_hrvo(uint gpuid, uint neighb_gpuid)
{
    struct HRVO ret;
    struct RVO rvo = compute_rvo(gpuid, neighb_gpuid);

    vec2 centerline = rvo.xz_left_side + rvo.xz_right_side;
    vec2 intersec_point = vec2(0.0, 0.0);

    vec2 vo_apex = ATTR_VEC2(gpuid, pos) + ATTR_VEC2(neighb_gpuid, velocity);
    float det = (centerline.x * ATTR_VEC2(gpuid, velocity).y) 
              - (centerline.y * ATTR_VEC2(gpuid, velocity).x);

    if(det > EPSILON) {

        struct ray l1 = ray(rvo.xz_apex, rvo.xz_left_side);
        struct ray l2 = ray(vo_apex, rvo.xz_right_side);

        bool collide = infinite_rays_intersection(l1, l2, intersec_point);
        ret.xz_apex = intersec_point;

    }else if(det < -EPSILON) {

        struct ray l1 = ray(rvo.xz_apex, rvo.xz_right_side);
        struct ray l2 = ray(vo_apex, rvo.xz_left_side);

        bool collide = infinite_rays_intersection(l1, l2, intersec_point);
        ret.xz_apex = intersec_point;

    }else{
        ret.xz_apex = rvo.xz_apex;
    }

    ret.xz_right_side = rvo.xz_right_side;
    ret.xz_left_side = rvo.xz_left_side;
    return ret;
}

uint compute_all_hrvos(in uint gpuid,
                       in uint ndynamic,
                       in uint dynamic_neighbours[MAX_NEIGHBOURS],
                       out struct HRVO out_hrvos[MAX_NEIGHBOURS])
{
    uint ret = 0;
    vec2 ent_pos = ATTR_VEC2(gpuid, pos);

    for(int i = 0; i < ndynamic; i++) {

        uint curr_gpuid = dynamic_neighbours[i];
        vec2 curr_pos = ATTR_VEC2(curr_gpuid, pos);
        if(same_position(curr_pos, ent_pos))
            continue;
        out_hrvos[ret++] = compute_hrvo(gpuid, curr_gpuid);
    }
    return ret;
}

uint compute_all_vos(in uint gpuid,
                     in uint nstatic,
                     in uint static_neighbours[MAX_NEIGHBOURS],
                     out struct VO out_vos[MAX_NEIGHBOURS])
{
    uint ret = 0;
    vec2 ent_pos = ATTR_VEC2(gpuid, pos);

    for(int i = 0; i < nstatic; i++) {

        uint curr_gpuid = static_neighbours[i];
        vec2 curr_pos = ATTR_VEC2(curr_gpuid, pos);
        if(same_position(curr_pos, ent_pos))
            continue;
        out_vos[ret++] = compute_vo(gpuid, curr_gpuid);
    }
    return ret;
}

void rays_repr(in uint nstatic,
               in uint ndynamic,
               in struct VO vos[MAX_NEIGHBOURS],
               in struct HRVO hrvos[MAX_NEIGHBOURS],
               out struct ray out_rays[(MAX_NEIGHBOURS + MAX_NEIGHBOURS) * 2])
{
    uint rays_idx = 0;

    for(int i = 0; i < ndynamic; i++) {

        out_rays[rays_idx + 0].point = hrvos[i].xz_apex;
        out_rays[rays_idx + 0].dir = hrvos[i].xz_left_side;

        out_rays[rays_idx + 1].point = hrvos[i].xz_apex;
        out_rays[rays_idx + 1].dir = hrvos[i].xz_right_side;

        rays_idx += 2;
    }

    for(int i = 0; i < nstatic; i++) {

        out_rays[rays_idx + 0].point = vos[i].xz_apex;
        out_rays[rays_idx + 0].dir = vos[i].xz_left_side;

        out_rays[rays_idx + 1].point = vos[i].xz_apex;
        out_rays[rays_idx + 1].dir = vos[i].xz_right_side;

        rays_idx += 2;
    }
}

bool inside_pcr(in vec2 test,
                in uint nrays,
                in struct ray rays[(MAX_NEIGHBOURS + MAX_NEIGHBOURS) * 2])
{
    uint idx = gl_GlobalInvocationID.x;
    uint npoints = 0;
    for(int i = 0; i < nrays; i+= 2) {

        const float left_dir_x = rays[i + 0].dir.x;
        const float left_dir_z = rays[i + 0].dir.y;

        vec2 point_to_test = test - rays[i + 0].point;
        if(length(point_to_test) < EPSILON)
            continue;

        point_to_test = normalize(point_to_test);
        float left_det = (point_to_test.y * left_dir_x) - (point_to_test.x * left_dir_z);
        bool left_of_vo = (left_det < EPSILON);

        if(left_of_vo)
            continue;

        const float right_dir_x = rays[i + 1].dir.x;
        const float right_dir_z = rays[i + 1].dir.y;

        point_to_test = test - rays[i + 1].point;
        if(length(point_to_test) < EPSILON)
            continue;

        point_to_test = normalize(point_to_test);
        float right_det = (point_to_test.y * right_dir_x) - (point_to_test.x * right_dir_z);
        bool right_of_vo = (right_det > -EPSILON);

        if(right_of_vo)
            continue;

        return true;
    }

    return false;
}

bool finite_rays_intersection(struct ray l1, struct ray l2, out vec2 out_xz)
{
    vec2 isec_point;
    if(!infinite_rays_intersection(l1, l2, isec_point))
        return false;

    if((isec_point.x - l1.point.x) / l1.dir.x < 0.0)
        return false;

    if((isec_point.y - l1.point.y) / l1.dir.y < 0.0)
        return false;

    if((isec_point.x - l2.point.x) / l2.dir.x < 0.0)
        return false;

    if((isec_point.y - l2.point.y) / l2.dir.y < 0.0)
        return false;

    out_xz = isec_point;
    return true;
}

uint compute_vo_xpoints(in uint nrays,
                        in struct ray rays[(MAX_NEIGHBOURS + MAX_NEIGHBOURS) * 2],
                        out vec2 xpoints[CLEARPATH_MAX_XPOINTS])
{
    uint ret = 0;
    for(int i = 0; i < nrays; i++) {
    for(int j = 0; j < nrays; j++) {

        if(ret == CLEARPATH_MAX_XPOINTS)
            return ret;

        if(i == j)
            continue;

        vec2 isec_point;
        if(!finite_rays_intersection(rays[i], rays[j], isec_point))
            continue;

        if(inside_pcr(isec_point, nrays, rays))
            continue;

        xpoints[ret++] = isec_point;
    }}
    return ret;
}

void compute_vdes_proj_points(in uint nrays,
                              in struct ray rays[(MAX_NEIGHBOURS + MAX_NEIGHBOURS) * 2],
                              in vec2 vdes,
                              inout uint nxpoints,
                              inout vec2 xpoints[CLEARPATH_MAX_XPOINTS])
{
    for(int i = 0; i < nrays; i++) {

        if(nxpoints == CLEARPATH_MAX_XPOINTS)
            return;

        float len = dot(rays[i].dir, vdes);
        vec2 proj = rays[i].point + (rays[i].dir * len);

        if(!inside_pcr(proj, nrays, rays)) {
            xpoints[nxpoints++] = proj;
        }
    }
}

vec2 compute_vnew(in uint nxpoints,
                  in vec2 xpoints[CLEARPATH_MAX_XPOINTS],
                  in vec2 vdes,
                  in vec2 pos_xz)
{
    float min_dist = 1e30;
    vec2 ret = vec2(0.0, 0.0);

    for(int i = 0; i < nxpoints; i++) {

        vec2 curr = xpoints[i];
        vec2 delta = curr - pos_xz;
        vec2 diff = vdes - delta;
        float len = length(diff);

        if(len < min_dist) {

            min_dist = len;
            ret = delta;
        }
    }
    return ret;
}

bool clearpath_try(uint gpuid, vec2 vdes, 
                   uint nstatic, uint ndynamic,
                   in uint static_neighbours[MAX_NEIGHBOURS],
                   in uint dynamic_neighbours[MAX_NEIGHBOURS],
                   out vec2 out_velocity)
{
    struct HRVO dyn_hrvos[MAX_NEIGHBOURS];
    struct VO stat_vos[MAX_NEIGHBOURS];

    uint num_hrvos = compute_all_hrvos(gpuid, ndynamic, dynamic_neighbours, dyn_hrvos);
    uint num_vos = compute_all_vos(gpuid, nstatic, static_neighbours, stat_vos);

    const uint nrays = (num_hrvos + num_vos) * 2;
    struct ray rays[(MAX_NEIGHBOURS + MAX_NEIGHBOURS) * 2];
    rays_repr(nstatic, ndynamic, stat_vos, dyn_hrvos, rays);

    vec2 des_v_ws = ATTR_VEC2(gpuid, pos) + vdes;

    if(!inside_pcr(des_v_ws, nrays, rays)) {
        out_velocity = vdes;
        return true;
    }

    /* There are a maximum of (nrays)^2 intersection points 
     * However, practically speaking, we can limit the number
     * to a constant and truncate anything after that. 
     */
    vec2 xpoints[CLEARPATH_MAX_XPOINTS];
    uint nxpoints = compute_vo_xpoints(nrays, rays, xpoints);
    compute_vdes_proj_points(nrays, rays, vdes, nxpoints, xpoints);

    if(nxpoints == 0)
        return false;

    out_velocity = compute_vnew(nxpoints, xpoints, vdes, ATTR_VEC2(gpuid, pos));
    return true;
}

bool find_furthest_maybe(in vec2 pos, 
                         inout float max_dist,
                         inout int max_idx,
                         in uint nents,
                         in uint ents[MAX_NEIGHBOURS])
{
    bool ret = false;
    for(int i = 0; i < nents; i++) {

        uint curr_gpuid = ents[i];
        vec2 curr_pos = ATTR_VEC2(curr_gpuid, pos);
        vec2 diff = pos - curr_pos;
        float len = length(diff);

        if(len > max_dist) {
            max_dist = len;
            max_idx = i;
            ret = true;
        }
    }
    return ret;
}

void vec_remove(in uint idx,
                inout uint size,
                inout uint ents[MAX_NEIGHBOURS])
{
    for(uint i = idx; i < size-1; i++) {
        uint next = ents[i + 1];
        ents[i] = next;
    }
    size--;
}

void remove_furthest(in vec2 pos, 
                     inout uint nstatic, 
                     inout uint ndynamic,
                     inout uint static_neighbours[MAX_NEIGHBOURS],
                     inout uint dynamic_neighbours[MAX_NEIGHBOURS])
{
    float max_dist = -1e30;
    int max_idx = -1;

    bool stat_found = find_furthest_maybe(pos, max_dist, max_idx, nstatic, static_neighbours);
    bool dyn_found = find_furthest_maybe(pos, max_dist, max_idx, ndynamic, dynamic_neighbours);

    if(dyn_found) {
        vec_remove(max_idx, ndynamic, dynamic_neighbours);
    }else if(stat_found) {
        vec_remove(max_idx, nstatic, static_neighbours);
    }
}

vec2 clearpath_new_velocity(in uint gpuid, 
                            in vec2 vpref,
                            in uint nstatic, 
                            in uint ndynamic,
                            in uint static_neighbours[MAX_NEIGHBOURS],
                            in uint dynamic_neighbours[MAX_NEIGHBOURS])
{
    vec2 pos = ATTR_VEC2(gpuid, pos);
    do{
        vec2 ret = vec2(0.0, 0.0);
        bool found = clearpath_try(gpuid, vpref, nstatic, ndynamic,
            static_neighbours, dynamic_neighbours, ret);
        if(found)
            return ret;

        remove_furthest(pos, nstatic, ndynamic, static_neighbours, dynamic_neighbours);

    }while(nstatic > 0 && ndynamic > 0);

    return vec2(0, 0);
}

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    uint gpuid = gpuids[idx];

    vec2 vpref = vec2(0.0, 0.0);
    uint state = ATTR(gpuid, movestate);

    if(state == STATE_TURNING) {
        vpref = vec2(0.0, 0.0);
    }else if(state == STATE_SEEK_ENEMIES) {
        vpref = enemy_seek_vpref(
            gpuid, 
            ATTR(gpuid, speed), 
            ATTR_VEC2(gpuid, vdes)
        );
    }else if(state == STATE_ARRIVING_TO_CELL) {
        if(!bool(ATTR(gpuid, formation_assignment_ready))) {
            vpref = vec2(0.0, 0.0);
        }else{
            vpref = cell_arrival_seek_vpref(
                gpuid,
                ATTR_VEC2(gpuid, cell_pos),
                ATTR(gpuid, speed),
                ATTR_VEC2(gpuid, vdes),
                ATTR_VEC2(gpuid, formation_cohesion_force),
                ATTR_VEC2(gpuid, formation_align_force),
                ATTR_VEC2(gpuid, formation_drag_force)
            );
        }
    }else if(state == STATE_MOVING_IN_FORMATION) {
        if(!bool(ATTR(gpuid, formation_assignment_ready))) {
            vpref = vec2(0.0, 0.0);
        }else{
            vpref = formation_seek_vpref(
                gpuid,
                ATTR(gpuid, flock_id),
                ATTR(gpuid, speed),
                ATTR_VEC2(gpuid, vdes),
                ATTR_VEC2(gpuid, formation_cohesion_force),
                ATTR_VEC2(gpuid, formation_align_force),
                ATTR_VEC2(gpuid, formation_drag_force),
                ATTR(gpuid, has_dest_los)
            );
        }
    }else{
        vpref = point_seek_vpref(
            gpuid,
            ATTR(gpuid, flock_id),
            ATTR_VEC2(gpuid, vdes),
            ATTR(gpuid, has_dest_los),
            ATTR(gpuid, speed)
        );
    }

    uint static_neighbours[MAX_NEIGHBOURS];
    uint dynamic_neighbours[MAX_NEIGHBOURS];
    ivec2 num_neighbs = find_neighbours(gpuid, static_neighbours, dynamic_neighbours);

    vec2 new_vel = clearpath_new_velocity(gpuid, vpref, num_neighbs.x, num_neighbs.y,
        static_neighbours, dynamic_neighbours);
    new_vel = truncate(new_vel, ATTR(gpuid, max_speed) / ticks_hz);

    velocities[idx] = new_vel;
}

