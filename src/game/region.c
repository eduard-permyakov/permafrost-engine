/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#include "region.h"
#include "game_private.h"
#include "../ui.h"
#include "../main.h"
#include "../camera.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/khash.h"

#include <assert.h>

struct region{
    enum region_type type;
    union{
        float radius;
        struct{
            float xlen; 
            float zlen;
        };
    };
    vec2_t pos;
};

KHASH_MAP_INIT_STR(region, struct region)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map *s_map;
static khash_t(region)  *s_regions;
static bool              s_render = false;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool region_add(const char *name, struct region reg)
{
    if(kh_get(region, s_regions, name) != kh_end(s_regions))
        return false;

    const char *key = pf_strdup(name);
    if(!key)
        return false;

    int status;
    khiter_t k = kh_put(region, s_regions, key, &status);
    if(status == -1)
        return false;

    kh_value(s_regions, k) = reg;
    return true;
}

static vec2_t region_ss_pos(vec2_t pos)
{
    int width, height;
    Engine_WinDrawableSize(&width, &height);

    float y = M_HeightAtPoint(s_map, M_ClampedMapCoordinate(s_map, pos));
    vec4_t pos_homo = (vec4_t) { pos.x, y, pos.z, 1.0f };

    const struct camera *cam = G_GetActiveCamera();
    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view);
    Camera_MakeProjMat(cam, &proj);

    vec4_t clip, tmp;
    PFM_Mat4x4_Mult4x1(&view, &pos_homo, &tmp);
    PFM_Mat4x4_Mult4x1(&proj, &tmp, &clip);
    vec3_t ndc = (vec3_t){ clip.x / clip.w, clip.y / clip.w, clip.z / clip.w };

    float screen_x = (ndc.x + 1.0f) * width/2.0f;
    float screen_y = height - ((ndc.y + 1.0f) * height/2.0f);
    return (vec2_t){screen_x, screen_y};
}

static void on_render_3d(void *user, void *event)
{
    if(!s_render)
        return;

    const float width = 0.25f;
    const vec3_t red = (vec3_t){1.0f, 0.0f, 0.0f};

    const char *key;
    struct region reg;

    kh_foreach(s_regions, key, reg, {

        switch(reg.type) {
        case REGION_CIRCLE: {

            R_PushCmd((struct rcmd){
                .func = R_GL_DrawSelectionCircle,
                .nargs = 5,
                .args = {
                    R_PushArg(&reg.pos, sizeof(reg.pos)),
                    R_PushArg(&reg.radius, sizeof(reg.radius)),
                    R_PushArg(&width, sizeof(width)),
                    R_PushArg(&red, sizeof(red)),
                    (void*)G_GetPrevTickMap(),
                },
            });
            break;
        }
        case REGION_RECTANGLE: {

            vec2_t corners[4] = {
                (vec2_t){reg.pos.x + reg.xlen, reg.pos.z - reg.zlen},
                (vec2_t){reg.pos.x - reg.xlen, reg.pos.z - reg.zlen},
                (vec2_t){reg.pos.x - reg.xlen, reg.pos.z + reg.zlen},
                (vec2_t){reg.pos.x + reg.xlen, reg.pos.z + reg.zlen},
            };
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
            break;
        }
        default: assert(0);
        }

        vec2_t ss_pos = region_ss_pos(reg.pos);
        struct rect bounds = (struct rect){ss_pos.x - 75, ss_pos.y, 150, 16};
        struct rgba color = (struct rgba){255, 0, 0, 255};
        UI_DrawText(key, bounds, color);
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Region_Init(const struct map *map)
{
    s_regions = kh_init(region);
    if(!s_regions)
        goto fail_regions;

    s_map = map;
    return true;

fail_regions:
    return false;
}

void G_Region_Shutdown(void)
{
    kh_destroy(region, s_regions);
    s_map = NULL;
}

bool G_Region_AddCircle(const char *name, vec2_t pos, float radius)
{
    struct region newreg = (struct region) {
        .type = REGION_CIRCLE,
        .radius = radius,
        .pos = pos
    };
    return region_add(name, newreg);
}

bool G_Region_AddRectangle(const char *name, vec2_t pos, float xlen, float zlen)
{
    struct region newreg = (struct region) {
        .type = REGION_CIRCLE,
        .xlen = xlen,
        .zlen = zlen,
        .pos = pos
    };
    return region_add(name, newreg);
}

void G_Region_Remove(const char *name)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return;
    kh_del(region, s_regions, k);
}

bool G_Region_Move(const char *name, vec2_t pos)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return false;

    kh_value(s_regions, k).pos = pos;
    return true;
}

int G_Region_GetEnts(const char *name, size_t maxout, struct entity *ents[static maxout])
{
    return 0;
}

bool G_Region_ContainsEnt(const char *name, uint32_t uid)
{
    return true;
}

void G_Region_RemoveEnt(uint32_t uid)
{

}

void G_Region_SetRender(bool on)
{
    s_render = on;
}

bool G_Region_GetRender(void)
{
    return s_render;
}

