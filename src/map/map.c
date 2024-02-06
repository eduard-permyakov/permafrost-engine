/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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

#include "map_private.h"
#include "pfchunk.h"
#include "public/map.h"
#include "../sched.h"
#include "../perf.h"
#include "../camera.h"
#include "../settings.h"
#include "../phys/public/collision.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../navigation/public/nav.h"
#include "../game/public/game.h"

#include <string.h>
#include <assert.h>

#include <SDL.h>

#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define CLAMP(a, min, max)  (MIN(MAX((a), (min)), (max)))
#define ARR_SIZE(a)         (sizeof(a)/sizeof(a[0]))


/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void m_aabb_for_chunk(const struct map *map, struct chunkpos p, struct aabb *out)
{
    size_t chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    size_t chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;
    size_t chunk_max_height = MAX_HEIGHT_LEVEL * Y_COORDS_PER_TILE;

    int x_offset = -(p.c * chunk_x_dim);
    int z_offset =  (p.r * chunk_z_dim);

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

static bool m_chunk_has_water(const struct pfchunk *chunk)
{
    for(int i = 0; i < TILES_PER_CHUNK_HEIGHT * TILES_PER_CHUNK_WIDTH; i++) {
        if(chunk->tiles[i].base_height < 0)
            return true;
    }
    return false;
}

static struct tile *tile_for_desc(const struct map *map, const struct tile_desc *td)
{
    struct map_resolution res, nav_res;
    M_GetResolution(map, &res);
    M_NavGetResolution(map, &nav_res);

    struct box bounds = M_Tile_Bounds(nav_res, map->pos, *td);
    vec2_t center = (vec2_t){
        bounds.x - bounds.width / 2.0f,
        bounds.z + bounds.height / 2.0f
    };

    struct tile_desc converted_td;
    M_Tile_DescForPoint2D(res, map->pos, center, &converted_td);

    struct tile *ret = NULL;
    M_TileForDesc(map, converted_td, &ret);
    assert(ret);
    return ret;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void M_Update(const struct map *map)
{
    N_Update(map->nav_private);
}

void M_ModelMatrixForChunk(const struct map *map, struct chunkpos p, mat4x4_t *out)
{
    int x_offset = -(p.c * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE);
    int z_offset =  (p.r * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);
    vec3_t chunk_pos = (vec3_t) {map->pos.x + x_offset, map->pos.y, map->pos.z + z_offset};
   
    PFM_Mat4x4_MakeTrans(chunk_pos.x, chunk_pos.y, chunk_pos.z, out);
}

void M_RenderEntireMap(const struct map *map, bool shadows, enum render_pass pass)
{
    vec2_t pos = (vec2_t){map->pos.x, map->pos.z};
    const bool fval = false;

    R_PushCmd((struct rcmd){ 
        .func = R_GL_MapBegin,
        .nargs = 2,
        .args = { 
            R_PushArg(&shadows, sizeof(shadows)),
            R_PushArg(&pos, sizeof(pos)),
        },
    });

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {
    
        mat4x4_t chunk_model;
        const struct pfchunk *chunk = &map->chunks[r * map->width + c];
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);

        switch(pass) {
        case RENDER_PASS_DEPTH: 
            R_PushCmd((struct rcmd){
                .func = R_GL_RenderDepthMap,
                .nargs = 2,
                .args = {
                    chunk->render_private,
                    R_PushArg(&chunk_model, sizeof(chunk_model)),
                },
            });
            break;
        case RENDER_PASS_REGULAR:
            R_PushCmd((struct rcmd){
                .func = R_GL_Draw,
                .nargs = 3,
                .args = {
                    chunk->render_private,
                    R_PushArg(&chunk_model, sizeof(chunk_model)),
                    R_PushArg(&fval, sizeof(fval)),
                },
            });
            break;
        default: assert(0);
        }
    }}

    R_PushCmd((struct rcmd){ R_GL_MapEnd, 0 });
}

void M_RenderVisibleMap(const struct map *map, const struct camera *cam, 
                        bool shadows, enum render_pass pass)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);
    vec2_t pos = (vec2_t){map->pos.x, map->pos.z};
    const bool fval = false;

    R_PushCmd((struct rcmd){ 
        .func = R_GL_MapBegin, 
        .nargs = 2, 
        .args = {
            R_PushArg(&shadows, sizeof(shadows)),
            R_PushArg(&pos, sizeof(pos)),
        },
    });

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

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
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);

        switch(pass) {
        case RENDER_PASS_DEPTH: 
            R_PushCmd((struct rcmd){
                .func = R_GL_RenderDepthMap,
                .nargs = 2,
                .args = {
                    chunk->render_private,
                    R_PushArg(&chunk_model, sizeof(chunk_model)),
                },
            });
            break;
        case RENDER_PASS_REGULAR:
            R_PushCmd((struct rcmd){
                .func = R_GL_Draw,
                .nargs = 3,
                .args = {
                    chunk->render_private,
                    R_PushArg(&chunk_model, sizeof(chunk_model)),
                    R_PushArg(&fval, sizeof(fval)),
                },
            });
            break;
        default: assert(0);
        }
    }}
    R_PushCmd((struct rcmd){ R_GL_MapEnd, 0 });
}

void M_RenderVisiblePathableLayer(const struct map *map, const struct camera *cam, enum nav_layer layer)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        mat4x4_t chunk_model;
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
        N_RenderPathableChunk(map->nav_private, &chunk_model, map, r, c, layer); 
    }}
}

void M_RenderChunkBoundaries(const struct map *map, const struct camera *cam)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        mat4x4_t chunk_model;
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);

        vec2_t corners[4] = {
            (vec2_t){chunk_aabb.x_max, chunk_aabb.z_min},
            (vec2_t){chunk_aabb.x_min, chunk_aabb.z_min},
            (vec2_t){chunk_aabb.x_min, chunk_aabb.z_max},
            (vec2_t){chunk_aabb.x_max, chunk_aabb.z_max},
        };
        vec3_t red = (vec3_t){1.0f, 0.0f, 0.0f};
        float width = 1.0f;

        R_PushCmd((struct rcmd){
            .func = R_GL_DrawQuad,
            .nargs = 4,
            .args = {
                R_PushArg(corners, sizeof(corners)),
                R_PushArg(&width, sizeof(width)),
                R_PushArg(&red, sizeof(red)),
                (void*)G_GetPrevTickMap(),
            },
        });
    }}
}

void M_RenderChunkVisibility(const struct map *map, const struct camera *cam, int faction_id)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        mat4x4_t chunk_model;
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
        G_Fog_RenderChunkVisibility(faction_id, r, c, &chunk_model); 
    }}
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

    return (xz.x <= map->pos.x && xz.x >= map->pos.x - width)
        && (xz.z >= map->pos.z && xz.z <= map->pos.z + height);
}

vec2_t M_ClampedMapCoordinate(const struct map *map, vec2_t xz)
{
    const float EPSILON = (1.0f/1024);
    float x, z;

    x = MIN(map->pos.x - EPSILON, xz.x);
    x = MAX(map->pos.x - map->width * (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) + EPSILON, x);

    z = MAX(map->pos.z + EPSILON, xz.z);
    z = MIN(map->pos.z + (map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE) - EPSILON, z);

    return (vec2_t){x, z};
}

float M_HeightAtPoint(const struct map *map, vec2_t xz)
{
    assert(M_PointInsideMap(map, xz));

    float x = xz.raw[0];
    float z = xz.raw[1];

    int chunk_r, chunk_c;
    chunk_r = CLAMP( (z - map->pos.z) / (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE), 0, map->height-1);
    chunk_c = CLAMP(-(x - map->pos.x) / (TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE), 0, map->width-1);

    float chunk_off_r = fmod(z - map->pos.z, TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);
    float chunk_off_c = fmod(-(x - map->pos.x), TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE);
    assert(chunk_off_r >= 0 && chunk_off_c >= 0);

    int tile_r, tile_c;    
    tile_r = CLAMP(chunk_off_r / Z_COORDS_PER_TILE, 0, TILES_PER_CHUNK_HEIGHT-1);
    tile_c = CLAMP(chunk_off_c / X_COORDS_PER_TILE, 0, TILES_PER_CHUNK_WIDTH-1);
    
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
    struct map_resolution res;
    M_GetResolution(map, &res);
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

void M_NavUpdateIslandsField(const struct map *map)
{
    N_UpdateIslandsField(map->nav_private);
}

bool M_NavRequestPath(const struct map *map, vec2_t xz_src, vec2_t xz_dest, 
                      enum nav_layer layer, dest_id_t *out_dest_id)
{
    return N_RequestPath(map->nav_private, xz_src, xz_dest, map->pos, layer, out_dest_id);
}

void M_NavRenderVisiblePathFlowField(const struct map *map, const struct camera *cam, 
                                     dest_id_t id)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        mat4x4_t chunk_model;
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
        N_RenderPathFlowField(map->nav_private, map, &chunk_model, r, c, id);
        N_RenderLOSField(map->nav_private, map, &chunk_model, r, c, id);
    }}
}

void M_NavRenderVisibleEnemySeekField(const struct map *map, const struct camera *cam, 
                                     enum nav_layer layer, int faction_id)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        mat4x4_t chunk_model;
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
        N_RenderEnemySeekField(map->nav_private, map, &chunk_model, r, c, layer, faction_id);
    }}
}

void M_NavRenderVisibleSurroundField(const struct map *map, const struct camera *cam, 
                                     enum nav_layer layer, const uint32_t uid)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        mat4x4_t chunk_model;
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
        N_RenderSurroundField(map->nav_private, map, &chunk_model, r, c, layer, uid);
    }}
}

void M_NavRenderNavigationBlockers(const struct map *map, const struct camera *cam, enum nav_layer layer)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        mat4x4_t chunk_model;
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
        N_RenderNavigationBlockers(map->nav_private, map, &chunk_model, r, c, layer);
    }}
}

void M_NavRenderBuildableTiles(const struct map *map, const struct camera *cam, 
                               const struct obb *obb, enum nav_layer layer, bool blocked, 
                               bool allow_shore)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        mat4x4_t chunk_model;
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
        N_RenderBuildableTiles(map->nav_private, map, &chunk_model, r, c, obb, layer, blocked, allow_shore);
    }}
}

void M_NavRenderNavigationPortals(const struct map *map, const struct camera *cam, enum nav_layer layer)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        mat4x4_t chunk_model;
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
        N_RenderNavigationPortals(map->nav_private, map, &chunk_model, r, c, layer);
    }}
}

void M_NavRenderNavigationIslandIDs(const struct map *map, const struct camera *cam,
                                    enum nav_layer layer)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        mat4x4_t chunk_model;
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
        N_RenderIslandIDs(map->nav_private, map, cam, &chunk_model, r, c, layer);
    }}
}

void M_NavRenderNavigationLocalIslandIDs(const struct map *map, const struct camera *cam,
                                         enum nav_layer layer)
{
    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        mat4x4_t chunk_model;
        M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
        N_RenderLocalIslandIDs(map->nav_private, map, cam, &chunk_model, r, c, layer);
    }}
}

vec2_t M_NavDesiredPointSeekVelocity(const struct map *map, dest_id_t id, vec2_t curr_pos, vec2_t xz_dest)
{
    return N_DesiredPointSeekVelocity(id, curr_pos, xz_dest, map->nav_private, map->pos);
}

vec2_t M_NavDesiredEnemySeekVelocity(const struct map *map, enum nav_layer layer, 
                                     vec2_t curr_pos, int faction_id)
{
    return N_DesiredEnemySeekVelocity(curr_pos, map->nav_private, layer, map->pos, faction_id);
}

vec2_t M_NavDesiredSurroundVelocity(const struct map *map, enum nav_layer layer, 
                                    vec2_t curr_pos, const uint32_t uid, int faction_id)
{
    return N_DesiredSurroundVelocity(curr_pos, map->nav_private, layer, map->pos, uid, faction_id);
}

bool M_NavHasDestLOS(const struct map *map, dest_id_t id, vec2_t curr_pos)
{
    return N_HasDestLOS(id, curr_pos, map->nav_private, map->pos);
}

bool M_NavPositionPathable(const struct map *map, enum nav_layer layer, vec2_t xz_pos)
{
    struct box map_box = (struct  box){
        map->pos.x,
        map->pos.z,
        map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE,
        map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE,
    };

    if(!C_BoxPointIntersection(xz_pos.x, xz_pos.z, map_box))
        return false; 

    return N_PositionPathable(xz_pos, layer, map->nav_private, map->pos);
}

bool M_NavPositionBlocked(const struct map *map, enum nav_layer layer, vec2_t xz_pos)
{
    struct box map_box = (struct  box){
        map->pos.x,
        map->pos.z,
        map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE,
        map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE,
    };

    if(!C_BoxPointIntersection(xz_pos.x, xz_pos.z, map_box))
        return false; 

    return N_PositionBlocked(xz_pos, layer, map->nav_private, map->pos);
}

vec2_t M_NavClosestReachableDest(const struct map *map, enum nav_layer layer, 
                                 vec2_t xz_src, vec2_t xz_dst)
{
    return N_ClosestReachableDest(map->nav_private, layer, map->pos, xz_src, xz_dst);
}

bool M_NavClosestReachableAdjacentPos(const struct map *map, enum nav_layer layer, 
                                      vec2_t xz_src, const uint32_t target_uid, vec2_t *out)
{
    uint32_t flags = G_FlagsGet(target_uid);
    if(flags & ENTITY_FLAG_MOVABLE) {

        return N_ClosestReachableAdjacentPosDynamic(map->nav_private, layer, map->pos, 
            xz_src, G_Pos_GetXZ(target_uid), G_GetSelectionRadius(target_uid), out);
    }else{

        struct obb obb;
        Entity_CurrentOBB(target_uid, &obb, false);
        return N_ClosestReachableAdjacentPosStatic(map->nav_private, layer,
            map->pos, xz_src, &obb, out);
    }
}

bool M_NavClosestPathable(const struct map *map, enum nav_layer layer, vec2_t xz_src, vec2_t *out)
{
    return N_ClosestPathable(map->nav_private, layer, map->pos, xz_src, out);
}

bool M_NavLocationsReachable(const struct map *map, enum nav_layer layer, 
                             vec2_t a, vec2_t b)
{
    return N_LocationsReachable(map->nav_private, layer, map->pos, a, b);
}

void M_NavBlockersIncref(vec2_t xz_pos, float range, int faction_id, 
                         uint32_t flags, const struct map *map)
{
    N_BlockersIncref(xz_pos, range, faction_id, flags, map->pos, map->nav_private);
}

void M_NavBlockersDecref(vec2_t xz_pos, float range, int faction_id, 
                         uint32_t flags, const struct map *map)
{
    N_BlockersDecref(xz_pos, range, faction_id, flags, map->pos, map->nav_private);
}

void M_NavBlockersIncrefOBB(const struct map *map, int faction_id, 
                            uint32_t flags, const struct obb *obb)
{
    N_BlockersIncrefOBB(map->nav_private, faction_id, flags, map->pos, obb);
}

void M_NavBlockersDecrefOBB(const struct map *map, int faction_id, 
                            uint32_t flags, const struct obb *obb)
{
    N_BlockersDecrefOBB(map->nav_private, faction_id, flags, map->pos, obb);
}

bool M_TileForDesc(const struct map *map, struct tile_desc desc, struct tile **out)
{
    if(desc.chunk_r < 0 || desc.chunk_r >= map->height)
        return false;
    if(desc.chunk_c < 0 || desc.chunk_c >= map->width)
        return false;
    if(desc.tile_r < 0 || desc.tile_r >= TILES_PER_CHUNK_HEIGHT)
        return false;
    if(desc.tile_c < 0 || desc.tile_c >= TILES_PER_CHUNK_WIDTH)
        return false;

    *out = (struct tile*)&map->chunks[desc.chunk_r * map->width + desc.chunk_c]
        .tiles[desc.tile_r * TILES_PER_CHUNK_WIDTH + desc.tile_c];
    return true;
}

void M_GetResolution(const struct map *map, struct map_resolution *out)
{
    out->chunk_w = map->width;
    out->chunk_h = map->height;
    out->tile_w = TILES_PER_CHUNK_WIDTH;
    out->tile_h = TILES_PER_CHUNK_HEIGHT;
    out->field_w = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    out->field_h = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;
}

void M_SetShadowsEnabled(struct map *map, bool on)
{
    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        const struct pfchunk *chunk = &map->chunks[r * map->width + c];
        R_PushCmd((struct rcmd){
            .func = R_GL_SetShadowsEnabled,
            .nargs = 2,
            .args = {
                chunk->render_private,
                R_PushArg(&on, sizeof(on)),
            },
        });
    }}
}

vec3_t M_GetCenterPos(const struct map *map)
{
    struct map_resolution res;
    M_GetResolution(map, &res);
    return (vec3_t){
        map->pos.x - (res.chunk_w * res.tile_w * X_COORDS_PER_TILE)/2.0f,
        map->pos.y,
        map->pos.z + (res.chunk_h * res.tile_h * Z_COORDS_PER_TILE)/2.0f,
    };
}

vec3_t M_GetPos(const struct map *map)
{
    return map->pos;
}

bool M_WaterMaybeVisible(const struct map *map, const struct camera *cam)
{
    PERF_ENTER();

    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        struct aabb chunk_aabb;
        m_aabb_for_chunk(map, (struct chunkpos) {r, c}, &chunk_aabb);

        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        const struct pfchunk *chunk = &map->chunks[r * map->width + c];
        if(m_chunk_has_water(chunk))
            PERF_RETURN(true);
    }}
    PERF_RETURN(false);
}

bool M_NavIsMaximallyClose(const struct map *map, enum nav_layer layer, vec2_t xz_pos, 
                           vec2_t xz_dest, float tolerance)
{
    return N_IsMaximallyClose(map->nav_private, layer, map->pos, xz_pos, xz_dest, tolerance);
}

bool M_NavIsAdjacentToImpassable(const struct map *map, enum nav_layer layer, vec2_t xz_pos)
{
    return N_IsAdjacentToImpassable(map->nav_private, layer, map->pos, xz_pos);
}

uint32_t M_NavDestIDForPos(const struct map *map, vec2_t xz_pos, enum nav_layer layer)
{
    return N_DestIDForPos(map->nav_private, map->pos, xz_pos, layer);
}

uint32_t M_NavDestIDForPosAttacking(const struct map *map, vec2_t xz_pos, 
                                    enum nav_layer layer, int faction_id)
{
    return N_DestIDForPosAttacking(map->nav_private, map->pos, xz_pos, layer, faction_id);
}

bool M_NavObjAdjacent(const struct map *map, uint32_t uid, uint32_t target_uid)
{
    if(G_FlagsGet(target_uid) & ENTITY_FLAG_MOVABLE) {

        return N_ObjAdjacentToDynamic(map->nav_private, map->pos, uid, 
            G_Pos_GetXZ(target_uid), G_GetSelectionRadius(target_uid));
    }else{

        struct obb obb;
        Entity_CurrentOBB(target_uid, &obb, false);
        return N_ObjAdjacentToStatic(map->nav_private, map->pos, uid, &obb);
    }
}

bool M_NavObjAdjacentToStaticWith(const struct map *map, vec2_t xz_pos, float radius, 
                                  const struct obb *stat)
{
    return N_ObjAdjacentToStaticWith(map->nav_private, map->pos, xz_pos, radius, stat);
}

bool M_NavObjAdjacentToDynamicWith(const struct map *map, vec2_t xz_pos_a, float radius_a,
                                   vec2_t xz_pos_b, float radius_b)
{
    return N_ObjAdjacentToDynamicWith(map->nav_private, map->pos, xz_pos_a, radius_a,
                                      xz_pos_b, radius_b);
}

void M_NavGetResolution(const struct map *map, struct map_resolution *out)
{
    return N_GetResolution(map->nav_private, out);
}

bool M_NavObjectBuildable(const struct map *map, enum nav_layer layer, 
                          bool allow_shore, const struct obb *obb)
{
    return N_ObjectBuildable(map->nav_private, map, layer, allow_shore, map->pos, obb);
}

bool M_NavHasEntityLOS(const struct map *map, enum nav_layer layer, 
                       vec2_t xz_pos, uint32_t uid)
{
    return N_HasEntityLOS(xz_pos, uid, map->nav_private, layer, map->pos);
}

vec2_t M_NavClosestReachableInRange(const struct map *map, enum nav_layer layer,
                                    vec2_t xz_src, vec2_t xz_target, float range)
{
    return N_ClosestReachableInRange(map->nav_private, map->pos, xz_src, xz_target, range, layer);
}

void M_NavRequestAsyncEnemySeekField(const struct map *map, enum nav_layer layer, 
                                     vec2_t curr_pos, int faction_id)
{
    N_RequestAsyncEnemySeekField(curr_pos, map->nav_private, layer, map->pos, faction_id);
}

void M_NavRequestAsyncSurroundField(const struct map *map, enum nav_layer layer, 
                                    vec2_t curr_pos, uint32_t ent, int faction_id)
{
    N_RequestAsyncSurroundField(curr_pos, map->nav_private, layer, map->pos, ent, faction_id);
}

void M_NavCopyIslandsFieldView(const struct map *map, vec2_t center,
                               int nrows, int ncols, enum nav_layer layer, uint16_t *out_field)
{
    N_CopyIslandsFieldView(map->nav_private, center, map->pos, nrows, ncols, layer, out_field);
}

void M_NavCellArrivalFieldCreate(const struct map *map, size_t rdim, size_t cdim, 
                                 enum nav_layer layer, uint16_t enemies,
                                 struct tile_desc target, struct tile_desc center,
                                 uint8_t *out, void *workspace, size_t workspace_size)
{
    assert(map->nav_private);
    N_CellArrivalFieldCreate(map->nav_private, rdim, cdim, layer, enemies, 
        target, center, out, workspace, workspace_size);
}

void M_NavCellArrivalFieldUpdateToNearestPathable(const struct map *map, 
                              size_t rdim, size_t cdim, enum nav_layer layer, uint16_t enemies,
                              struct tile_desc start, struct tile_desc center, 
                              uint8_t *inout, void *workspace, size_t workspace_size)
{
    assert(map->nav_private);
    N_CellArrivalFieldUpdateToNearestPathable(map->nav_private, rdim, cdim, layer, enemies,
        start, center, inout, workspace, workspace_size);
}

bool M_PointOverWater(const struct map *map, vec2_t pos)
{
    struct tile *tile = NULL;
    struct tile_desc td;
    if(!M_DescForPoint2D(map, pos, &td))
        return false;
    M_TileForDesc(map, td, &tile);
    assert(tile);
    return (M_Tile_BaseHeight(tile) < 0);
}

bool M_PointOverLand(const struct map *map, vec2_t pos)
{
    struct tile *tile = NULL;
    struct tile_desc td;
    if(!M_DescForPoint2D(map, pos, &td))
        return false;
    M_TileForDesc(map, td, &tile);
    assert(tile);
    return (M_Tile_BaseHeight(tile) >= 0);
}

bool M_TileAdjacentToWater(const struct map *map, const struct tile_desc *td)
{
    struct map_resolution res, nav_res;
    M_GetResolution(map, &res);
    M_NavGetResolution(map, &nav_res);

    struct tile *tile = tile_for_desc(map, td);
    if(M_Tile_BaseHeight(tile) < 0)
        return true;

    struct tile_desc adjacent[9];
    size_t nadjacent = M_Tile_Contour(1, td, nav_res, adjacent, ARR_SIZE(adjacent));

    for(int i = 0; i < nadjacent; i++) {

        struct tile *tile = tile_for_desc(map, &adjacent[i]);
        if(M_Tile_BaseHeight(tile) < 0)
            return true;
    }
    return false;
}

bool M_TileAdjacentToLand(const struct map *map, const struct tile_desc *td)
{
    struct map_resolution nav_res;
    M_NavGetResolution(map, &nav_res);

    struct tile *tile = tile_for_desc(map, td);
    if(M_Tile_BaseHeight(tile) >= -1)
        return true;

    struct tile_desc adjacent[9];
    size_t nadjacent = M_Tile_Contour(1, td, nav_res, adjacent, ARR_SIZE(adjacent));

    for(int i = 0; i < nadjacent; i++) {

        struct tile *tile = tile_for_desc(map, &adjacent[i]);
        if(M_Tile_BaseHeight(tile) >= -1)
            return true;
    }
    return false;
}

bool M_ObjectAdjacentToWater(const struct map *map, const struct obb *obb)
{
    assert(Sched_UsingBigStack());

    struct map_resolution nav_res;
    M_NavGetResolution(map, &nav_res);

    struct tile_desc tds[1024];
    size_t ntiles = M_Tile_AllUnderObj(map->pos, nav_res, obb, tds, ARR_SIZE(tds));

    for(int i = 0; i < ntiles; i++) {
        if(M_TileAdjacentToWater(map, &tds[i])) {
            return true;
        }
    }
    return false;
}

bool M_ObjectAdjacentToLand(const struct map *map, const struct obb *obb)
{
    assert(Sched_UsingBigStack());

    struct map_resolution nav_res;
    M_NavGetResolution(map, &nav_res);

    struct tile_desc tds[1024];
    size_t ntiles = M_Tile_AllUnderObj(map->pos, nav_res, obb, tds, ARR_SIZE(tds));

    for(int i = 0; i < ntiles; i++) {
        if(M_TileAdjacentToLand(map, &tds[i]))
            return true;
    }
    return false;
}

vec2_t M_NavClosestPointAdjacentToLand(const struct map *map, vec2_t pos)
{
    return N_ClosestPointAdjacentToLand(map, map->nav_private, map->pos, pos);
}

vec2_t M_NavClosestPointAdjacentToIsland(const struct map *map, vec2_t pos, 
                                         vec2_t island_pos, enum nav_layer layer)
{
    return N_ClosestPointAdjacentToIsland(map->nav_private, map->pos, pos, 
        island_pos, layer);
}

bool M_NavIsAdjacentToIsland(const struct map *map, enum nav_layer layer, 
                             vec2_t xz_pos, float radius, vec2_t island_pos)
{
    return N_IsAdjacentToIsland(map->nav_private, layer, map->pos, xz_pos, 
        radius, island_pos);
}

