/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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

#ifndef GAME_H
#define GAME_H

#include "../../entity.h"
#include "../../map/public/map.h"
#include "../../lib/public/vec.h"
#include "../../lib/public/khash.h"

#include <stdbool.h>
#include <SDL.h>


#define MAX_FACTIONS     16
#define MAX_FAC_NAME_LEN 32

struct entity;
struct map;
struct tile_desc;
struct tile;
struct faction;
struct render_workspace;
struct nk_context;


VEC_TYPE(pentity, struct entity *)
VEC_IMPL(static inline, pentity, struct entity *)

KHASH_DECLARE(entity, khint32_t, struct entity*)


enum cam_mode{
    CAM_MODE_FPS,
    CAM_MODE_RTS
};

enum diplomacy_state{
    DIPLOMACY_STATE_PEACE,
    DIPLOMACY_STATE_WAR
};

enum simstate{
    G_RUNNING           = (1 << 0),
    G_PAUSED_FULL       = (1 << 1),
    G_PAUSED_UI_RUNNING = (1 << 2),
};

struct render_input{
    const struct camera *cam;
    const struct map    *map;
    bool                 shadows;
    /* The visible entities to render */
    vec_rstat_t         cam_vis_stat;
    vec_ranim_t         cam_vis_anim;
    /* The entities 'visible' from the light source PoV. They are 
     * used for rendering the shadow map. */
    vec_rstat_t         light_vis_stat;
    vec_ranim_t         light_vis_anim;
};


/*###########################################################################*/
/* GAME GENERAL                                                              */
/*###########################################################################*/

bool   G_Init(void);
bool   G_NewGameWithMap(const char *dir, const char *pfmap);
bool   G_NewGameWithMapString(const char *mapstr);
void   G_Shutdown(void);

void   G_Update(void);
void   G_Render(void);
void   G_SwapBuffers(void);

/* This does not have any side effects besides  making draw calls, 
 * so it is safe to invoke from the render thread. 
 */
void   G_RenderMapAndEntities(struct render_input in);

void   G_GetMinimapPos(float *out_x, float *out_y);
void   G_SetMinimapPos(float x, float y);
int    G_GetMinimapSize(void);
void   G_SetMinimapSize(int size);
void   G_SetMinimapResizeMask(int mask);
bool   G_MouseOverMinimap(void);
bool   G_MapHeightAtPoint(vec2_t xz, float *out_height);
bool   G_PointInsideMap(vec2_t xz);

void   G_BakeNavDataForScene(void);

bool   G_AddEntity(struct entity *ent, vec3_t pos);
bool   G_RemoveEntity(struct entity *ent);
void   G_StopEntity(const struct entity *ent);

/* Wrapper around AL_EntityFree to defer the call until the render thread 
 * (which owns some part of entity resources) finishes its' work. */
void   G_SafeFree(struct entity *ent);

bool   G_AddFaction(const char *name, vec3_t color);
bool   G_RemoveFaction(int faction_id);
bool   G_UpdateFaction(int faction_id, const char *name, vec3_t color, bool control);
int    G_GetFactions(char out_names[][MAX_FAC_NAME_LEN], vec3_t *out_colors, bool *out_ctrl);
bool   G_SetDiplomacyState(int fac_id_a, int fac_id_b, enum diplomacy_state ds);
bool   G_GetDiplomacyState(int fac_id_a, int fac_id_b, enum diplomacy_state *out);

bool   G_ActivateCamera(int idx, enum cam_mode mode);
void   G_MoveActiveCamera(vec2_t xz_ground_pos);
vec3_t G_ActiveCamPos(void);
vec3_t G_ActiveCamDir(void);

bool   G_UpdateMinimapChunk(int chunk_r, int chunk_c);
bool   G_UpdateTile(const struct tile_desc *desc, const struct tile *tile);

void          G_SetSimState(enum simstate ss);
enum simstate G_GetSimState(void);
void          G_SetLightPos(vec3_t pos);

struct render_workspace *G_GetSimWS(void);
struct render_workspace *G_GetRenderWS(void);
const struct map        *G_GetPrevTickMap(void);

/*###########################################################################*/
/* GAME SELECTION                                                            */
/*###########################################################################*/

enum selection_type{
    SELECTION_TYPE_PLAYER = 0,
    SELECTION_TYPE_ALLIED,
    SELECTION_TYPE_ENEMY
};

void                  G_Sel_Enable(void);
void                  G_Sel_Disable(void);

void                  G_Sel_Clear(void);
void                  G_Sel_Add(struct entity *ent);
void                  G_Sel_Remove(struct entity *ent);
const vec_pentity_t  *G_Sel_Get(enum selection_type *out_type);


/*###########################################################################*/
/* GAME MOVEMENT                                                             */
/*###########################################################################*/

void G_Move_SetMoveOnLeftClick(void);
void G_Move_SetAttackOnLeftClick(void);
void G_Move_SetDest(const struct entity *ent, vec2_t dest_xz);


/*###########################################################################*/
/* GAME COMBAT                                                               */
/*###########################################################################*/

enum combat_stance{
    /* The entity will move to attack anyone within 
     * its' target acquisition radius. */
    COMBAT_STANCE_AGGRESSIVE,
    /* The entity will attack entities within its' attack
     * range but it will not move from its' current position. */
    COMBAT_STANCE_HOLD_POSITION,
    /* The entity will not take part in combat. */
    COMBAT_STANCE_NO_ENGAGEMENT,
};

/* Can only be called with entities that have 'ENTITY_FLAG_COMBATABLE' set */
bool  G_Combat_SetStance(const struct entity *ent, enum combat_stance stance);
int   G_Combat_GetCurrentHP(const struct entity *ent);

void  G_Combat_SetBaseArmour(const struct entity *ent, float armour_pc);
float G_Combat_GetBaseArmour(const struct entity *ent);
void  G_Combat_SetBaseDamage(const struct entity *ent, int dmg);
int   G_Combat_GetBaseDamage(const struct entity *ent);


/*###########################################################################*/
/* GAME POSITION                                                             */
/*###########################################################################*/

bool   G_Pos_Set(uint32_t uid, vec3_t pos);
vec3_t G_Pos_Get(uint32_t uid);
vec2_t G_Pos_GetXZ(uint32_t uid);

int    G_Pos_EntsInRect(vec2_t xz_min, vec2_t xz_max, struct entity **out, size_t maxout);
int    G_Pos_EntsInRectWithPred(vec2_t xz_min, vec2_t xz_max, struct entity **out, size_t maxout,
                                bool (*predicate)(const struct entity *ent, void *arg), void *arg);
int    G_Pos_EntsInCircle(vec2_t xz_point, float range, struct entity **out, size_t maxout);

struct entity *G_Pos_Nearest(vec2_t xz_point);
struct entity *G_Pos_NearestWithPred(vec2_t xz_point, 
                                     bool (*predicate)(const struct entity *ent, void *arg), void *arg);

#endif

