/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

#include "public/map.h"
#include "../render/public/render.h"
#include "../navigation/public/nav.h"
#include "map_private.h"
#include "pfchunk.h"
#include "../camera.h"
#include "../collision.h"

#include <unistd.h>
#include <string.h>
#include <assert.h>

#include <SDL.h>

#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))


/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void m_aabb_for_chunk(const struct map *map, struct chunkpos p, struct aabb *out)
{
    size_t chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    size_t chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;
    size_t chunk_max_height = MAX_HEIGHT_LEVEL * Y_COORDS_PER_TILE;

    ssize_t x_offset = -(p.c * chunk_x_dim);
    ssize_t z_offset =  (p.r * chunk_z_dim);

    out->x_max = map->pos.x + x_offset;
    out->x_min = out->x_max - chunk_x_dim;

    out->z_min = map->pos.z + z_offset;
    out->z_max = out->z_min + chunk_z_dim;

    out->y_min = 0.0f;
    out->y_max = chunk_max_height;

    assert(out->x_max >= out->x_min);
    assert(out->y_max >= out->y_min);
    assert(out->z_max >= out->z_min);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void M_ModelMatrixForChunk(const struct map *map, struct chunkpos p, mat4x4_t *out)
{
    ssize_t x_offset = -(p.c * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE);
    ssize_t z_offset =  (p.r * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);
    vec3_t chunk_pos = (vec3_t) {map->pos.x + x_offset, map->pos.y, map->pos.z + z_offset};
   
    PFM_Mat4x4_MakeTrans(chunk_pos.x, chunk_pos.y, chunk_pos.z, out);
}

void M_RenderEntireMap(const struct map *map, enum render_pass pass)
{
    for(int r = 0; r < map->height; r++) {
        for(int c = 0; c < map->width; c++) {
        
            mat4x4_t chunk_model;
            const struct pfchunk *chunk = &map->chunks[r * map->width + c];
            void *render_private = 
                (chunk->mode == CHUNK_RENDER_MODE_REALTIME_BLEND) ? chunk->render_private_tiles
                                                                  : chunk->render_private_prebaked;

            M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
            switch(pass) {
            case RENDER_PASS_DEPTH: 
                R_GL_RenderDepthMap(render_private, &chunk_model);
                break;
            case RENDER_PASS_REGULAR:
                R_GL_Draw(render_private, &chunk_model);
                break;
            default: assert(0);
            }
        }
    }
}

void M_RenderVisibleMap(const struct map *map, const struct camera *cam, enum render_pass pass)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
        for(int c = 0; c < map->width; c++) {

            struct aabb chunk_aabb;
            m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

            /* Due to the nature of the the map (perfect grid), the fast and greedy frustrum 
             * intersection test will yield too many false positives. As each chunk mesh has 
             * a high vertex count, this is undesirable. It is absolutely worth it to do the 
             * precise frustrum intersection test. With it, the map rendering performance
             * scales great for large maps. */
            if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
                continue;

            mat4x4_t chunk_model;
            const struct pfchunk *chunk = &map->chunks[r * map->width + c];
            void *render_private = 
                (chunk->mode == CHUNK_RENDER_MODE_REALTIME_BLEND) ? chunk->render_private_tiles
                                                                  : chunk->render_private_prebaked;

            M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
            switch(pass) {
            case RENDER_PASS_DEPTH: 
                R_GL_RenderDepthMap(render_private, &chunk_model);
                break;
            case RENDER_PASS_REGULAR:
                R_GL_Draw(render_private, &chunk_model);
                break;
            default: assert(0);
            }
        }
    }
}

void M_RenderVisiblePathableLayer(const struct map *map, const struct camera *cam)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
        for(int c = 0; c < map->width; c++) {

            struct aabb chunk_aabb;
            m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

            if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
                continue;

            mat4x4_t chunk_model;
            M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
            N_RenderPathableChunk(map->nav_private, &chunk_model, map, r, c); 
        }
    }
}

void M_CenterAtOrigin(struct map *map)
{
    size_t width  = map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    size_t height = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    map->pos = (vec3_t) {(width / 2.0f), 0.0f, -(height / 2.0f)};
}

void M_RestrictRTSCamToMap(const struct map *map, struct camera *cam)
{
    struct bound_box bounds;

    /* 'Camera_RestrictPosWithBox' restricts the position of the camera to an XZ box.
     * However, if we just let this box be the map position and dimensions, the corners will 
     * not appear equal due to the camera tilt. For example, with yaw = 135 and pitch = -70, 
     * less of the top center corner will be visible than the bottom center corner due to 
     * the camera being tilted up. What we actually want is the camera ray position at ground 
     * level to be bounded within the map box. To achieve this, we offset the camera 
     * position bounding box by the XZ components of the difference between the camera position 
     * and where the camera ray intersects the ground. 
     *
     * This assumes the camera pitch, yaw, and height will not change.
     */
    
    float offset_mag = cos(DEG_TO_RAD(Camera_GetPitch(cam))) * Camera_GetHeight(cam);

    bounds.x = map->pos.x - cos(DEG_TO_RAD(Camera_GetYaw(cam))) * offset_mag;
    bounds.z = map->pos.z + sin(DEG_TO_RAD(Camera_GetYaw(cam))) * offset_mag;
    bounds.w = map->width  * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE;
    bounds.h = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    Camera_RestrictPosWithBox(cam, bounds);
}

void M_SetChunkRenderMode(struct map *map, int chunk_r, int chunk_c, enum chunk_render_mode mode)
{
    assert(chunk_r >= 0 && chunk_r < map->height);
    assert(chunk_c >= 0 && chunk_r < map->width);

    struct pfchunk *chunk = &map->chunks[chunk_r * map->width + chunk_c];
    chunk->mode = mode;

    if(mode == CHUNK_RENDER_MODE_PREBAKED) {
        
        ssize_t x_offset = -(chunk_c * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE);
        ssize_t z_offset =  (chunk_r * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);
        vec3_t chunk_pos = (vec3_t) {map->pos.x + x_offset, map->pos.y, map->pos.z + z_offset};

        mat4x4_t chunk_model;
        PFM_Mat4x4_MakeTrans(chunk_pos.x, chunk_pos.y, chunk_pos.z, &chunk_model);

        vec3_t chunk_center = (vec3_t) {
            chunk_pos.x - (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE)/2.0f,
            chunk_pos.y,
            chunk_pos.z + (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE)/2.0f
        };

        chunk->render_private_prebaked = R_GL_TileBakeChunk(chunk->render_private_tiles, chunk_center, &chunk_model,
            TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT, chunk->tiles, chunk_r, chunk_c);
    }
}

void M_SetMapRenderMode(struct map *map, enum chunk_render_mode mode)
{
    assert(map);

    for(int r = 0; r < map->height; r++) {
        for(int c = 0; c < map->width; c++) {
           M_SetChunkRenderMode(map, r, c, mode); 
        }
    }
}

vec2_t M_WorldCoordsToNormMapCoords(const struct map *map, vec2_t xz)
{
    float width  = map->width  * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE;
    float height = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;
    float dim = MAX(width, height);

    return (vec2_t) {
        -xz.raw[0] / (dim / 2.0f),
         xz.raw[1] / (dim / 2.0f)
    };
}

bool M_PointInsideMap(const struct map *map, vec2_t xz)
{
    float width  = map->width  * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE;
    float height = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    return (xz.raw[0] <= map->pos.x && xz.raw[0] >= map->pos.x - width)
        && (xz.raw[1] >= map->pos.z && xz.raw[1] <= map->pos.z + height);
}

vec2_t M_ClampedMapCoordinate(const struct map *map, vec2_t xz)
{
    const float EPSILON = (1.0f/1024);
    float x, z;

    x = MIN(map->pos.x - EPSILON, xz.raw[0]);
    x = MAX(map->pos.x - map->width * (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) + EPSILON, x);

    z = MAX(map->pos.z + EPSILON, xz.raw[1]);
    z = MIN(map->pos.z + (map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE) - EPSILON, z);

    return (vec2_t){x, z};
}

float M_HeightAtPoint(const struct map *map, vec2_t xz)
{
    assert(M_PointInsideMap(map, xz));

    float x = xz.raw[0];
    float z = xz.raw[1];

    int chunk_r, chunk_c;
    chunk_r =  (z - map->pos.z) / (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);
    chunk_c = -(x - map->pos.x) / (TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE);
    assert(chunk_r >= 0 && chunk_r < map->height);
    assert(chunk_c >= 0 && chunk_c < map->width);

    float chunk_off_r = fmod(z - map->pos.z, TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);
    float chunk_off_c = fmod(-(x - map->pos.x), TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE);
    assert(chunk_off_r >= 0 && chunk_off_c >= 0);

    int tile_r, tile_c;    
    tile_r = chunk_off_r / Z_COORDS_PER_TILE;
    tile_c = chunk_off_c / X_COORDS_PER_TILE;
    assert(tile_r >= 0 && tile_r < TILES_PER_CHUNK_HEIGHT);
    assert(tile_c >= 0 && tile_c < TILES_PER_CHUNK_WIDTH);
    
    float tile_frac_width, tile_frac_height;
    tile_frac_width =  fmod(chunk_off_c, X_COORDS_PER_TILE) / X_COORDS_PER_TILE;
    tile_frac_height = fmod(chunk_off_r, Z_COORDS_PER_TILE) / Z_COORDS_PER_TILE;
    assert(tile_frac_width >= 0.0f && tile_frac_width <= 1.0f);
    assert(tile_frac_height >= 0.0f && tile_frac_height <= 1.0f);

    const struct tile *tile = &map->chunks[chunk_r * map->width + chunk_c].tiles[tile_r * TILES_PER_CHUNK_WIDTH + tile_c];
    return M_Tile_HeightAtPos(tile, tile_frac_width, tile_frac_height);
}

bool M_DescForPoint2D(const struct map *map, vec2_t point_xz, struct tile_desc *out)
{
    struct map_resolution res = (struct map_resolution) {
        .chunk_w = map->width,
        .chunk_h = map->height,
        .tile_w = TILES_PER_CHUNK_WIDTH,
        .tile_h = TILES_PER_CHUNK_HEIGHT,
    };
    return M_Tile_DescForPoint2D(res, map->pos, point_xz, out);
}

void M_NavCutoutStaticObject(const struct map *map, const struct obb *obb)
{
    N_CutoutStaticObject(map->nav_private, map->pos, obb);
}

void M_NavUpdatePortals(const struct map *map)
{
    N_UpdatePortals(map->nav_private);
}

bool M_NavRequestPath(const struct map *map, vec2_t xz_src, vec2_t xz_dest, 
                      dest_id_t *out_dest_id)
{
    return N_RequestPath(map->nav_private, xz_src, xz_dest, map->pos, out_dest_id);
}

void M_NavRenderVisiblePathFlowField(const struct map *map, const struct camera *cam, dest_id_t id)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
        for(int c = 0; c < map->width; c++) {

            struct aabb chunk_aabb;
            m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

            if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
                continue;

            mat4x4_t chunk_model;
            M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
            N_RenderPathFlowField(map->nav_private, map, &chunk_model, r, c, id); 
            N_RenderLOSField(map->nav_private, map, &chunk_model, r, c, id);
        }
    }
}

vec2_t M_NavDesiredVelocity(const struct map *map, dest_id_t id, vec2_t curr_pos, vec2_t xz_dest)
{
    return N_DesiredVelocity(id, curr_pos, xz_dest, map->nav_private, map->pos);
}

bool M_NavHasDestLOS(const struct map *map, dest_id_t id, vec2_t curr_pos)
{
    return N_HasDestLOS(id, curr_pos, map->nav_private, map->pos);
}

bool M_NavPositionPathable(const struct map *map, vec2_t xz_pos)
{
    return N_PositionPathable(xz_pos, map->nav_private, map->pos);
}

