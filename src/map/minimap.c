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

#include "public/map.h"
#include "map_private.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../phys/public/collision.h"
#include "../game/public/game.h"
#include "../event.h"
#include "../main.h"
#include "../ui.h"
#include "../camera.h"

#include <SDL.h>

#include <assert.h>


#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define DEFAULT_BORDER_CLR {65.0f/255.0f, 65.0f/255.0f, 65.0f/255.0f, 1.0f}

struct quad{
    vec2_t a, b, c, d;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static bool   s_mouse_down_in_minimap = false;
static vec4_t s_border_clr = DEFAULT_BORDER_CLR;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static vec2_t rotate_about_point_ccw(vec2_t src, vec2_t point, float radians)
{
    vec2_t point_to_src;
    PFM_Vec2_Sub(&src, &point, &point_to_src);

    vec2_t rotated = (vec2_t){
        cos(radians) * point_to_src.x + sin(radians) * point_to_src.y,
       -sin(radians) * point_to_src.x + cos(radians) * point_to_src.y
    };

    vec2_t ret;
    PFM_Vec2_Add(&rotated, &point, &ret);
    return ret;
}

static struct quad rotate_rect_ccw(vec2_t center, float width, float height, float radians)
{
    float left  = center.x - width/2.0f;
    float right = center.x + width/2.0f;
    float top   = center.y - height/2.0f;
    float bot   = center.y + height/2.0f;

    vec2_t tl = (vec2_t){left,  top};
    vec2_t tr = (vec2_t){right, top};
    vec2_t br = (vec2_t){right, bot};
    vec2_t bl = (vec2_t){left,  bot};

    /*            b
     *            + 
     *          /   \
     * (0,0) a +     + c (1,1)
     *          \   /
     *            +
     *            d
     */

    /* a, b, c, d are given in screen coordinates ((0,0) in top left corner) */
    return (struct quad) {
        rotate_about_point_ccw(tl, center, radians),
        rotate_about_point_ccw(tr, center, radians),
        rotate_about_point_ccw(br, center, radians),
        rotate_about_point_ccw(bl, center, radians)
    };
}

static struct quad m_curr_bounds(const struct map *map)
{
    /* Fixup the minimap position and size based on how the virtual resolution aspect
     * ratio differs from the screen resolution aspect ration and the resize mask.
     */
    struct rect orig_rect = (struct rect){
        map->minimap_center_pos.x, map->minimap_center_pos.y,
        map->minimap_sz, map->minimap_sz
    };
    struct rect final_rect = UI_BoundsForAspectRatio(orig_rect, map->minimap_vres, 
        UI_ArAdjustedVRes(map->minimap_vres), map->minimap_resize_mask);

    vec2_t center = (vec2_t){final_rect.x, final_rect.y};
    return rotate_rect_ccw(center, final_rect.w, final_rect.h, M_PI/4.0f);
}

static struct quad m_curr_terrain_bounds(const struct map *map)
{
    float width, height;

    if(map->width < map->height) {
        width = map->minimap_sz * (((float)map->width)/map->height);
    }else{
        width = map->minimap_sz; 
    }

    if(map->height < map->width) {
        height = map->minimap_sz * (((float)map->height)/map->width);
    }else{
        height = map->minimap_sz; 
    }

    struct rect orig_terrain_rect = (struct rect) {
        map->minimap_center_pos.x, map->minimap_center_pos.y,
        width, height
    };
    struct rect final_rect = UI_BoundsForAspectRatio(orig_terrain_rect, map->minimap_vres, 
        UI_ArAdjustedVRes(map->minimap_vres), map->minimap_resize_mask);

    vec2_t center = (vec2_t){final_rect.x, final_rect.y};
    return rotate_rect_ccw(center, final_rect.w, final_rect.h, M_PI/4.0f);
}

static vec2_t m_minimap_mouse_coords_to_world(const struct map *map, vec2_t virt_screen_coords)
{
    /*      b
     *      + 
     *    /   \
     * a +     + c
     *    \   /
     *      +
     *      d
     */
    struct quad curr_bounds = m_curr_bounds(map);

    /* Now project the mouse coordinates (relative to A) on the AB line segment to get the X dimension fraction
     * and project onto the AD line segment to get the Z dimension fraction. */
    vec2_t ap, ab, ad;
    PFM_Vec2_Sub(&virt_screen_coords, &curr_bounds.a, &ap);
    PFM_Vec2_Sub(&curr_bounds.b, &curr_bounds.a, &ab);
    PFM_Vec2_Sub(&curr_bounds.d, &curr_bounds.a, &ad);

    float x_frac = PFM_Vec2_Dot(&ap, &ab) / PFM_Vec2_Dot(&ab, &ab);
    float z_frac = PFM_Vec2_Dot(&ap, &ad) / PFM_Vec2_Dot(&ad, &ad);
    
    /* Clamp to the range of [0.0, 1.0] to account for any imprecision */
    x_frac = MAX(x_frac, 0.0f);
    x_frac = MIN(x_frac, 1.0f);
    x_frac -= 0.5f;

    z_frac = MAX(z_frac, 0.0f);
    z_frac = MIN(z_frac, 1.0f);
    z_frac -= 0.5f;

    float map_ws_width  = map->width  * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE;
    float map_ws_height = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;
    float map_ws_len = MAX(map_ws_width, map_ws_height);

    vec3_t center_pos = M_GetCenterPos(map);
    return (vec2_t) {
        center_pos.x - x_frac * map_ws_len,
        center_pos.z + z_frac * map_ws_len
    };
}

bool m_mouse_over_screen_rect(const struct map *map, struct quad quad)
{
    int x, y;
    SDL_GetMouseState(&x, &y);

    int w, h;
    Engine_WinDrawableSize(&w, &h);

    vec2_t virt_mouse_pos = (vec2_t){
        ((float)x) / w * UI_ArAdjustedVRes(map->minimap_vres).x,
        ((float)y) / h * UI_ArAdjustedVRes(map->minimap_vres).y,
    };
    return C_PointInsideRect2D(virt_mouse_pos, quad.a, quad.b, quad.c ,quad.d);
}

static void on_mouseclick(void *user, void *event)
{
    const struct map *map = (const struct map*)user; 
    SDL_Event *mouse_event = (SDL_Event*)event;
    assert(mouse_event->type == SDL_MOUSEBUTTONDOWN);

    if(map->minimap_sz == 0)
        return;

    if(!m_mouse_over_screen_rect(map, m_curr_terrain_bounds(map))){
        s_mouse_down_in_minimap = false;
        return;
    }else{
        s_mouse_down_in_minimap = true; 
    }

    if(mouse_event->button.button != SDL_BUTTON_LEFT)
        return;

    if(G_MouseInTargetMode())
        return;

    int w, h;
    Engine_WinDrawableSize(&w, &h);
    vec2_t virt_mouse_button = (vec2_t){
        (float)mouse_event->button.x / w * UI_ArAdjustedVRes(map->minimap_vres).x,
        (float)mouse_event->button.y / h * UI_ArAdjustedVRes(map->minimap_vres).y
    };

    vec2_t ws_coords = m_minimap_mouse_coords_to_world(map, virt_mouse_button);
    G_MoveActiveCamera(ws_coords);
}

static void on_mousemove(void *user, void *event)
{
    const struct map *map = (const struct map*)user; 
    SDL_Event *mouse_event = (SDL_Event*)event;
    assert(mouse_event->type == SDL_MOUSEMOTION);

    if(!s_mouse_down_in_minimap)
        return;

    if(!(mouse_event->motion.state & SDL_BUTTON_LMASK))
        return;

    int w, h;
    Engine_WinDrawableSize(&w, &h);
    vec2_t virt_mouse_motion = (vec2_t){
        (float)mouse_event->motion.x / w * UI_ArAdjustedVRes(map->minimap_vres).x,
        (float)mouse_event->motion.y / h * UI_ArAdjustedVRes(map->minimap_vres).y
    };

    vec2_t ws_coords = m_minimap_mouse_coords_to_world(map, virt_mouse_motion);
    G_MoveActiveCamera(ws_coords);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool M_InitMinimap(struct map *map, vec2_t center_pos)
{
    assert(map);
    map->minimap_center_pos = center_pos;

    STALLOC(void*, chunk_rprivates, map->width * map->height);
    STALLOC(mat4x4_t, chunk_model_mats, map->width * map->height);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {
        
        const struct pfchunk *curr = &map->chunks[r * map->width + c];
        chunk_rprivates[r * map->width + c] = curr->render_private;
        M_ModelMatrixForChunk(map, (struct chunkpos){r, c}, chunk_model_mats + (r * map->width + c));
    }}
    
    R_PushCmd((struct rcmd){
        .func = R_GL_MinimapBake,
        .nargs = 3,
        .args = {
            (void*)G_GetPrevTickMap(),
            R_PushArg(chunk_rprivates, map->width * map->height * sizeof(void*)),
            R_PushArg(chunk_model_mats, map->width * map->height * sizeof(mat4x4_t)),
        },
    });

    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mouseclick, map, G_RUNNING);
    E_Global_Register(SDL_MOUSEMOTION,     on_mousemove,  map, G_RUNNING);

    STFREE(chunk_rprivates);
    STFREE(chunk_model_mats);

    return true;
}

bool M_UpdateMinimapChunk(const struct map *map, int chunk_r, int chunk_c)
{
    if(chunk_r >= map->height || chunk_c >= map->width)
        return false;

    mat4x4_t model;
    M_ModelMatrixForChunk(map, (struct chunkpos){chunk_r, chunk_c}, &model);

    R_PushCmd((struct rcmd){
        .func = R_GL_MinimapUpdateChunk,
        .nargs = 5,
        .args = {
            (void*)G_GetPrevTickMap(),
            map->chunks[chunk_r * map->width + chunk_c].render_private,
            R_PushArg(&model, sizeof(model)),
            R_PushArg(&chunk_r, sizeof(chunk_r)),
            R_PushArg(&chunk_c, sizeof(chunk_c)),
        }
    });
    return true;
}

void M_FreeMinimap(struct map *map)
{
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mouseclick);
    E_Global_Unregister(SDL_MOUSEMOTION,     on_mousemove);

    R_PushCmd((struct rcmd){ R_GL_MinimapFree, 0 });
    s_mouse_down_in_minimap = false;
}

void M_GetMinimapAdjVres(const struct map *map, vec2_t *out_vres)
{
    *out_vres = UI_ArAdjustedVRes(map->minimap_vres);
}

void M_SetMinimapVres(struct map *map, vec2_t vres)
{
    map->minimap_vres = vres;
}

void M_SetMinimapResizeMask(struct map *map, int resize_mask)
{
    map->minimap_resize_mask = resize_mask;
}

void M_GetMinimapPos(const struct map *map, vec2_t *out_center_pos)
{
    *out_center_pos = map->minimap_center_pos;
}

void M_SetMinimapPos(struct map *map, vec2_t center_pos)
{
    assert(map);
    map->minimap_center_pos = center_pos;
}

int M_GetMinimapSize(const struct map *map)
{
    return map->minimap_sz;
}

void M_SetMinimapSize(struct map *map, int side_len)
{
    map->minimap_sz = side_len;
}

void M_RenderMinimap(const struct map *map, const struct camera *cam)
{
    assert(map);
    if(map->minimap_sz == 0)
        return;

    struct quad curr_bounds = m_curr_bounds(map);

    vec2_t center;
    PFM_Vec2_Add(&curr_bounds.a, &curr_bounds.b, &center);
    PFM_Vec2_Add(&center, &curr_bounds.c, &center);
    PFM_Vec2_Add(&center, &curr_bounds.d, &center);
    PFM_Vec2_Scale(&center, 0.25f, &center);

    vec2_t ab;
    PFM_Vec2_Sub(&curr_bounds.b, &curr_bounds.a, &ab);
    int len = PFM_Vec2_Len(&ab);

    R_PushCmd((struct rcmd){
        .func = R_GL_MinimapRender,
        .nargs = 5,
        .args = {
            (void*)G_GetPrevTickMap(),
            R_PushArg(cam, g_sizeof_camera),
            R_PushArg(&center, sizeof(center)),
            R_PushArg(&len, sizeof(len)),
            R_PushArg(&s_border_clr, sizeof(s_border_clr)),
        },
    });
}

void M_RenderMinimapUnits(const struct map *map, size_t nunits, vec2_t *posbuff, vec3_t *colorbuff)
{
    assert(map);
    if(map->minimap_sz == 0)
        return;

    struct quad curr_bounds = m_curr_bounds(map);

    vec2_t center;
    PFM_Vec2_Add(&curr_bounds.a, &curr_bounds.b, &center);
    PFM_Vec2_Add(&center, &curr_bounds.c, &center);
    PFM_Vec2_Add(&center, &curr_bounds.d, &center);
    PFM_Vec2_Scale(&center, 0.25f, &center);

    vec2_t ab;
    PFM_Vec2_Sub(&curr_bounds.b, &curr_bounds.a, &ab);
    int len = PFM_Vec2_Len(&ab);

    void *pbuff = stalloc(&G_GetSimWS()->args, nunits * sizeof(vec2_t));
    void *cbuff = stalloc(&G_GetSimWS()->args, nunits * sizeof(vec3_t));

    memcpy(pbuff, posbuff, nunits * sizeof(vec2_t));
    memcpy(cbuff, colorbuff, nunits * sizeof(vec3_t));

    R_PushCmd((struct rcmd){
        .func = R_GL_MinimapRenderUnits,
        .nargs = 6,
        .args = {
            (void*)G_GetPrevTickMap(),
            R_PushArg(&center, sizeof(center)),
            R_PushArg(&len, sizeof(len)),
            R_PushArg(&nunits, sizeof(nunits)),
            pbuff,
            cbuff
        },
    });
}

bool M_MouseOverMinimap(const struct map *map)
{
    if(map->minimap_sz == 0)
        return false;

    return m_mouse_over_screen_rect(map, m_curr_bounds(map));
}

bool M_MinimapMouseMapCoords(const struct map *map, vec3_t *out)
{
    if(!M_MouseOverMinimap(map))
        return false;

    int x, y;
    SDL_GetMouseState(&x, &y);

    int w, h;
    Engine_WinDrawableSize(&w, &h);

    vec2_t virt_mouse_button = (vec2_t){
        (float)x / w * UI_ArAdjustedVRes(map->minimap_vres).x,
        (float)y / h * UI_ArAdjustedVRes(map->minimap_vres).y
    };

    vec2_t ws_coords = m_minimap_mouse_coords_to_world(map, virt_mouse_button);
    *out = (vec3_t) {
        ws_coords.x, 
        M_HeightAtPoint(map, ws_coords),
        ws_coords.z
    };
    return true;
}

void M_MinimapSetBorderClr(vec4_t clr)
{
    s_border_clr = clr;
}

vec4_t M_MinimapGetBorderClr(void)
{
    return s_border_clr;
}

void M_MinimapClearBorderClr(void)
{
    s_border_clr = (vec4_t)DEFAULT_BORDER_CLR;
}

