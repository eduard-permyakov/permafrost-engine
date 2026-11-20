/* 
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2026 Eduard Permyakov 
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

#define MEM_FILE_SYS MEM_SYS_RENDER
#define MEM_FILE_SUB MEM_SUB_RENDER_GL_PIPELINE

#include "gl_pipeline.h"
#include "gl_postprocess.h"
#include "gl_render.h"
#include "gl_assert.h"
#include "gl_perf.h"
#include "render_private.h"
#include "public/render.h"
#include "public/render_ctrl.h"
#include "../main.h"
#include "../camera/public/camera.h"
#include "../entity.h"
#include "../game/public/game.h"
#include "../map/public/map.h"

#include <assert.h>

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void terrain_begin(const struct render_scene *scene, bool shadows)
{
    const struct scene_terrain *terrain = &scene->terrain;
    size_t num_splats = terrain->num_splats;
    R_GL_MapBegin(&shadows, &terrain->pos, &num_splats, terrain->splatmap);
}

static void terrain_depth(const struct render_scene *scene)
{
    const struct scene_terrain *terrain = &scene->terrain;
    terrain_begin(scene, true);
    for(int i = 0; i < terrain->nchunks; i++) {
        struct scene_chunk *chunk = &terrain->chunks[i];
        R_GL_RenderDepthMap(chunk->render_private, &chunk->model);
    }
    R_GL_MapEnd();
}

/* The baked top faces of the resident chunks go first, then the walls and
 * the chunks drawn whole, so each program is installed once per pass.
 */
static void terrain_colour(const struct render_scene *scene, bool shadows, bool near_water_only)
{
    const struct scene_terrain *terrain = &scene->terrain;
    const bool opaque = false;

    terrain_begin(scene, shadows);
    for(int i = 0; i < terrain->nchunks; i++) {

        struct scene_chunk *chunk = &terrain->chunks[i];
        if(near_water_only && !chunk->near_water)
            continue;
        if(chunk->bake_layer < 0)
            continue;
        R_GL_DrawChunkBaked(chunk->render_private, &chunk->model, 
            &chunk->bake_layer, &chunk->bake_xform);
    }
    for(int i = 0; i < terrain->nchunks; i++) {

        struct scene_chunk *chunk = &terrain->chunks[i];
        if(near_water_only && !chunk->near_water)
            continue;
        if(chunk->bake_layer >= 0) {
            R_GL_DrawChunkWalls(chunk->render_private, &chunk->model);
        }else{
            R_GL_Draw(chunk->render_private, &chunk->model, &opaque);
        }
    }
    R_GL_MapEnd();
}

static void anim_set_uniforms(struct ent_anim_rstate *ent)
{
    mat4x4_t inverse, normal;
    PFM_Mat4x4_Inverse(&ent->model, &inverse);
    PFM_Mat4x4_Transpose(&inverse, &normal);
    R_GL_AnimSetUniforms(&normal, &ent->desc);
}

static void entities_depth(const struct render_scene *scene, struct render_input *in)
{
    if(scene->batching) {
        R_GL_Batch_RenderDepthMap(in);
        return;
    }
    for(int i = 0; i < vec_size(&in->light_vis_anim); i++) {
        struct ent_anim_rstate *curr = &vec_AT(&in->light_vis_anim, i);
        anim_set_uniforms(curr);
        R_GL_RenderDepthMap(curr->render_private, &curr->model);
    }
    for(int i = 0; i < vec_size(&in->light_vis_stat); i++) {
        struct ent_stat_rstate *curr = &vec_AT(&in->light_vis_stat, i);
        R_GL_RenderDepthMap(curr->render_private, &curr->model);
    }
}

static void entities_colour(const struct render_scene *scene, struct render_input *in)
{
    if(scene->batching) {
        R_GL_Batch_Draw(in);
        return;
    }
    for(int i = 0; i < vec_size(&in->cam_vis_anim); i++) {
        struct ent_anim_rstate *curr = &vec_AT(&in->cam_vis_anim, i);
        anim_set_uniforms(curr);
        R_GL_Draw(curr->render_private, &curr->model, &curr->translucent);
    }
    for(int i = 0; i < vec_size(&in->cam_vis_stat); i++) {
        struct ent_stat_rstate *curr = &vec_AT(&in->cam_vis_stat, i);
        R_GL_Draw(curr->render_private, &curr->model, &curr->translucent);
    }
}

static void stage_shadow_map(const struct render_scene *scene)
{
    GL_PERF_ENTER();
    struct render_input *in = scene->world;

    R_GL_DepthPassBegin(&in->light_pos, scene->cam);
    if(scene->map) {
        terrain_depth(scene);
    }
    entities_depth(scene, in);
    if(vec_size(&in->light_vis_bill)) {
        R_GL_Billboard_DrawDepth(in);
    }
    R_GL_DepthPassEnd();

    GL_PERF_RETURN_VOID();
}

static void decals_draw(const struct render_scene *scene)
{
    const struct scene_decals *decals = &scene->decals;
    if(decals->nchunks == 0)
        return;

    R_GL_DecalBeginDraw(scene->cam, decals->res, &decals->map_pos);
    for(int i = 0; i < decals->nchunks; i++) {
        R_GL_DecalDrawChunk(&decals->chunks[i]);
    }
    R_GL_DecalEndDraw();
}

static void stage_world(const struct render_scene *scene, bool billboards)
{
    GL_PERF_ENTER();
    struct render_input *in = scene->world;

    if(scene->map) {
        terrain_colour(scene, scene->shadows, false);
        decals_draw(scene);
    }
    entities_colour(scene, in);
    /* The billboards are one instanced draw whatever the batching mode */
    if(billboards && vec_size(&in->cam_vis_bill)) {
        R_GL_Billboard_Draw(in);
    }

    GL_PERF_RETURN_VOID();
}

/* What a water pass sees of the world: the terrain around the water and the 
 * entities by it, unshadowed, with neither decals nor billboards.
 */
static void water_pass_world(void *user)
{
    const struct render_scene *scene = user;
    terrain_colour(scene, false, true);
    entities_colour(scene, scene->water);
}

static void stage_water(const struct render_scene *scene)
{
    GL_PERF_ENTER();
    R_GL_DrawWater(scene->water, &scene->water_refraction, &scene->water_reflection,
        water_pass_world, (void*)scene);
    GL_PERF_RETURN_VOID();
}

static void stage_markers(const struct render_scene *scene)
{
    GL_PERF_ENTER();
    for(int i = 0; i < scene->nmarkers; i++) {

        const struct scene_marker *curr = &scene->markers[i];
        if(curr->box) {
            R_GL_DrawSelectionRectangle(curr->box, &curr->width, &curr->color, scene->map);
        }else{
            R_GL_DrawSelectionCircle(&curr->xz, &curr->radius, &curr->width, 
                &curr->color, scene->map);
        }
    }
    GL_PERF_RETURN_VOID();
}

static void stage_overlay(const struct render_scene *scene)
{
    GL_PERF_ENTER();
    R_GL_SetScreenspaceDrawMode();

    const struct scene_healthbars *hbs = &scene->healthbars;
    if(hbs->shown) {
        R_GL_DrawHealthbars(&hbs->nents, hbs->health_pc, hbs->top_pos_ws, 
            hbs->yoffsets, scene->cam);
    }
    R_RunCmds(scene->ui);

    if(scene->map) {
        struct scene_minimap minimap = scene->minimap;
        if(minimap.shown) {
            R_GL_MinimapRender(scene->map, scene->cam, &minimap.center, 
                &minimap.side_len, &minimap.border_clr);
            R_GL_MinimapRenderUnits(scene->map, &minimap.center, &minimap.side_len,
                &minimap.nunits, minimap.unit_pos, minimap.unit_clr);
        }
        R_GL_MapInvalidate();
    }
    GL_PERF_RETURN_VOID();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_Pipeline_Render(struct render_scene *scene)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    R_GL_BeginFrame();
    bool redirected = (scene->look || scene->antialias)
        && R_GL_PostProcess_BeginWorld(scene->look, scene->world_scale, scene->antialias);
    /* Billboards are small pictures of models already: drawn into a reduced 
     * world they blur, so over a graded world they are drawn afterwards, at 
     * full size and in its colours.
     */
    bool late_billboards = redirected && R_GL_PostProcess_Graded();
    R_RunCmds(scene->pre_world);
    R_GL_MovePoll(false);

    if(scene->shadows) {
        stage_shadow_map(scene);
        R_GL_MovePoll(false);
    }
    stage_world(scene, !late_billboards);
    R_GL_MovePoll(false);

    if(scene->water) {
        stage_water(scene);
        R_GL_MovePoll(false);
    }
    stage_markers(scene);
    R_GL_DrawSkybox(scene->cam);
    R_RunCmds(scene->post_world);
    R_GL_MovePoll(false);

    /* The world ends here: what follows is drawn over it at full size */
    if(redirected) {
        R_GL_PostProcess_Resolve(scene->cam, scene->dither, scene->antialias);
    }
    if(late_billboards && vec_size(&scene->world->cam_vis_bill)) {
        R_GL_PostProcess_CopyDepth();
        R_GL_Billboard_DrawGraded(scene->world, true);
    }

    stage_overlay(scene);

    R_RunCmds(scene->finish);

    if(scene->dump_path) {
        int width, height;
        Engine_WinDrawableSize(&width, &height);
        R_GL_DumpFBColor_PPM(scene->dump_path, &width, &height);
    }
    R_GL_EndFrame();

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}
