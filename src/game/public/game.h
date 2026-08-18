/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2023 Eduard Permyakov 
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


#define MAX_FACTIONS     15
#define MAX_FAC_NAME_LEN 32
#define AIR_UNIT_HEIGHT  20.0f
#define BASE_CAM_HEIGHT  175.0f

struct map;
struct tile_desc;
struct tile;
struct faction;
struct render_workspace;
struct nk_context;
struct nk_style_item;
struct nk_color;
struct proj_desc;
struct kh_id_s;
struct kh_pos_s;
struct kh_range_s;

struct entity_block_desc{
    uint32_t uid;
    float    radius;
    vec2_t   pos;
};

VEC_TYPE(entity, uint32_t)
VEC_IMPL(static inline, entity, uint32_t)

KHASH_DECLARE(entity, khint32_t, uint32_t)


enum cam_mode{
    CAM_MODE_FPS,
    CAM_MODE_RTS,
    CAM_MODE_FREE,
};

enum diplomacy_state{
    DIPLOMACY_STATE_PEACE,
    DIPLOMACY_STATE_WAR,
    DIPLOMACY_STATE_MAX
};

enum simstate{
    G_RUNNING           = (1 << 0),
    G_PAUSED_FULL       = (1 << 1),
    G_PAUSED_UI_RUNNING = (1 << 2),
    G_ALL               = G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING
};

struct render_input{
    const struct camera *cam;
    const struct map    *map;
    bool                 shadows;
    /* Restrict terrain rendering to chunks at or adjacent to a water chunk.
     * Set by the water reflection/refraction passes. */
    bool                 water_only;
    vec3_t               light_pos;
    /* The visible entities to render */
    vec_rstat_t         cam_vis_stat;
    vec_ranim_t         cam_vis_anim;
    /* The entities 'visible' from the light source PoV. They are
     * used for rendering the shadow map. */
    vec_rstat_t         light_vis_stat;
    vec_ranim_t         light_vis_anim;
};

enum hb_mode{
    HB_MODE_ALWAYS,
    HB_MODE_DAMAGED,
    HB_MODE_NEVER
};

enum formation_type{
    FORMATION_NONE,
    FORMATION_RANK,
    FORMATION_COLUMN,
    FORMATION_MAX
};

/*###########################################################################*/
/* GAME GENERAL                                                              */
/*###########################################################################*/

bool            G_Init(void);
bool            G_LoadMap(SDL_RWops *stream, bool update_navgrid);
void            G_Shutdown(void);

bool            G_HasWork(void);
void            G_FlushWork(void);
void            G_ClearState(void);
void            G_ClearRenderWork(void);

void            G_Update(void);
void            G_Render(void);
void            G_SwapBuffers(void);

/* This does not have any side effects besides  making draw calls, 
 * so it is safe to invoke from the render thread. 
 */
void            G_RenderMapAndEntities(struct render_input *in);

bool            G_GetMinimapPos(float *out_x, float *out_y);
bool            G_SetMinimapPos(float x, float y);
bool            G_GetMinimapSize(int *out_size);
bool            G_SetMinimapSize(int size);
bool            G_SetMinimapResizeMask(int mask);
void            G_SetMinimapRenderAllEntities(bool on);
bool            G_MouseOverMinimap(void);
bool            G_MouseInTargetMode(void);
bool            G_MapHeightAtPoint(vec2_t xz, float *out_height);
bool            G_MapClosestPathable(vec2_t xz, vec2_t *out, enum nav_layer layer);
bool            G_MapPositionPathable(vec2_t xz, enum nav_layer layer);
bool            G_PointInsideMap(vec2_t xz);
bool            G_PointOverWater(vec2_t xz);
bool            G_PointOverLand(vec2_t xz);
bool            G_MapAddSplat(int base_mat_idx, int accent_mat_idx);
bool            G_MapRemoveSplat(int base_mat_idx, int accent_mat_idx);

void            G_BakeNavDataForScene(void);

bool            G_AddEntity(uint32_t uid, uint32_t flags, vec3_t pos);
bool            G_RemoveEntity(uint32_t uid);
void            G_StopEntity(uint32_t uid, bool stop_move, bool stop_garrison);
void            G_UpdateBounds(uint32_t uid);
void            G_Zombiefy(uint32_t uid, bool invis);
bool            G_EntityExists(uint32_t uid);
bool            G_EntityIsZombie(uint32_t uid);
bool            G_EntityIsGarrisoned(uint32_t uid);

void            G_FreeEntity(uint32_t uid);
void            G_DeferredRemove(uint32_t uid);

bool            G_AddFaction(const char *name, vec3_t color, int *out_id);
bool            G_RemoveFaction(int faction_id);
bool            G_UpdateFaction(int faction_id, const char *name, vec3_t color, bool control);
uint16_t        G_GetFactions(char out_names[][MAX_FAC_NAME_LEN], vec3_t *out_colors, bool *out_ctrl);
uint16_t        G_GetPlayerControlledFactions(void);
uint16_t        G_GetEnemyFactions(int faction_id);
void            G_SetFactionID(uint32_t uid, int faction_id);
int             G_GetFactionID(uint32_t uid);
int             G_GetFactionIDFrom(struct kh_id_s *table, uint32_t uid);
void            G_GetFieldCacheStats(struct fc_stats *out);

void            G_SetVisionRange(uint32_t uid, float range);
float           G_GetVisionRange(uint32_t uid);

/* A ring drawn around an entity at the given radius, for showing how far an
 * ability reaches. Only one is shown at a time.
 */
void            G_SetRangeIndicator(uint32_t uid, float radius, vec3_t color, float width);
void            G_ClearRangeIndicator(void);

void            G_SetSelectionRadius(uint32_t uid, float range);
float           G_GetSelectionRadius(uint32_t uid);
float           G_GetSelectionRadiusFrom(struct kh_range_s *table, uint32_t uid);

bool            G_SetDiplomacyState(int fac_id_a, int fac_id_b, enum diplomacy_state ds);
bool            G_GetDiplomacyState(int fac_id_a, int fac_id_b, enum diplomacy_state *out);
bool            G_GetDiplomacyStateFrom(enum diplomacy_state (*table)[MAX_FACTIONS],
                                        int fac_id_a, int fac_id_b, 
                                        enum diplomacy_state *out);

void            G_SetActiveCamera(struct camera *cam, enum cam_mode mode);
struct camera  *G_GetActiveCamera(void);
enum cam_mode   G_GetCameraMode(void);
void            G_MoveActiveCamera(vec2_t xz_ground_pos);

bool            G_UpdateTile(const struct tile_desc *desc, const struct tile *tile);
bool            G_GetTile(const struct tile_desc *desc, struct tile *out);

void            G_SetSimState(enum simstate ss);
enum simstate   G_GetSimState(void);
void            G_UpdateSimStateChangeTick(void);
void            G_SetLightPos(vec3_t pos);
vec3_t          G_GetLightPos(void);
void            G_SetHideHealthbars(bool on);

void            G_SetAmbientLightColor(vec3_t color);
vec3_t          G_GetAmbientLightColor(void);
void            G_SetEmitLightColor(vec3_t color);
vec3_t          G_GetEmitLightColor(void);
void            G_SetSkybox(const char *dir, const char *extension);
void            G_GetSkybox(const char **dir, const char **extension);

bool            G_SaveGlobalState(SDL_RWops *stream);
bool            G_LoadGlobalState(SDL_RWops *stream);
bool            G_SaveEntityState(SDL_RWops *stream);
bool            G_LoadEntityState(SDL_RWops *stream);

struct render_workspace *G_GetSimWS(void);
struct render_workspace *G_GetRenderWS(void);
bool                     G_MapLoaded(void);
const struct map        *G_GetPrevTickMap(void);

uint32_t        G_GPUIDForEnt(uint32_t uid);
uint32_t        G_EntForGPUID(uint32_t gpuid);

void            G_FlagsSet(uint32_t uid, uint32_t flags);
uint32_t        G_FlagsGet(uint32_t uid);
uint32_t        G_FlagsGetFrom(struct kh_id_s *table, uint32_t uid);

void            G_SetShowUnitIcons(bool show);
bool            G_GetShowUnitIcons(void);

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
void                  G_Sel_Add(uint32_t uid);
void                  G_Sel_Remove(uint32_t uid);
void                  G_Sel_RemoveMany(const uint32_t *uids, size_t nuids);
const vec_entity_t   *G_Sel_Get(enum selection_type *out_type);
uint32_t              G_Sel_GetHovered(void);
void                  G_Sel_Set(uint32_t *ents, size_t nents);

/*###########################################################################*/
/* GAME GROUPS                                                               */
/*###########################################################################*/

int                   G_Group_Lock(const uint32_t *uids, size_t nuids);
void                  G_Group_Unlock(int group_id);
int                   G_Group_ForEnt(uint32_t uid);
int                   G_Group_ForSet(const uint32_t *uids, size_t nuids);
const vec_entity_t   *G_Group_Members(int group_id);
void                  G_Group_SetBackgroundStyle(const struct nk_style_item *style);
void                  G_Group_SetFontColor(const struct nk_color *clr);
void                  G_Group_SetBonusColor(const struct nk_color *clr);
bool                  G_Group_MouseOverUI(int mouse_x, int mouse_y);


/*###########################################################################*/
/* GAME MOVEMENT                                                             */
/*###########################################################################*/

void G_Move_SetMoveOnLeftClick(void);
void G_Move_SetAttackOnLeftClick(void);
void G_Move_SetDest(uint32_t uid, vec2_t dest_xz, bool attack);
void G_Move_UpdateSelectionRadius(uint32_t uid, float sel_radius);
bool G_Move_Still(uint32_t uid);
void G_Move_SetClickEnabled(bool on);
bool G_Move_GetClickEnabled(void);
bool G_Move_GetMaxSpeed(uint32_t uid, float *out);
bool G_Move_SetMaxSpeed(uint32_t uid, float speed);
/* The speed the entity actually moves at is the base speed plus the bonus the
 * combat modifiers hand down, floored at zero.
 */
bool G_Move_SetSpeedBonus(uint32_t uid, float bonus);
bool G_Move_GetEffectiveSpeed(uint32_t uid, float *out);

void G_Move_ArrangeInFormation(vec_entity_t *ents, vec2_t target, 
                               vec2_t orientation, enum formation_type type);
void G_Move_AttackInFormation(vec_entity_t *ents, vec2_t target,
                              vec2_t orientation, enum formation_type type);

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

/* Opaque IDs in [0, *_TYPE_MAX); the scripting layer defines what they mean.
 * Type 0 is the default, so scripts should keep [0][0] at 1.0.
 */
#define DAMAGE_TYPE_MAX  8
#define ARMOUR_TYPE_MAX  8

/* Incoming damage is scaled by ARMOUR_K / (ARMOUR_K + armour), so ARMOUR_K
 * points halve it. Every point adds the same amount of effective HP, while the
 * blocked fraction it buys shrinks: the typical band is [0, 100], scaling well
 * to 200, with the returns falling away past that. The cap is the 99% mark;
 * total immunity is not reachable through armour and is a separate flag.
 */
#define ARMOUR_K           100
#define ARMOUR_MIN_POINTS  0
#define ARMOUR_MAX_POINTS  9900

/* Timed stat modifiers. A flat amount is in the same units as the stat it
 * moves; COMBAT_MOD_INVULNERABLE is a latch, where any positive amount grants
 * immunity. The kind is serialized as a raw integer, so new kinds go before
 * COMBAT_MOD_MAX and never in the middle.
 */
enum combat_mod_kind{
    COMBAT_MOD_ARMOUR = 0,
    COMBAT_MOD_DAMAGE,
    COMBAT_MOD_SPEED,
    COMBAT_MOD_RANGE,
    COMBAT_MOD_INVULNERABLE,
    COMBAT_MOD_MAX
};

/* A tag is the identity of a named bonus: same tag means the same bonus, so
 * re-applying refreshes rather than stacks. Like the damage and armour types,
 * the scripting layer defines what the tags mean, including the icon the group
 * banner draws beside the amount.
 */
#define COMBAT_MOD_TAG_LEN    64
#define COMBAT_BONUS_ICON_LEN 128

/* A highlight outlines the set of units a bonus is reaching, for as long as the
 * player is asking. Drawn from the live membership, so nothing has to be handed
 * back to the engine as units come and go.
 */
/* The ring under each affected unit. The extent of the reach is marked by the
 * range indicator instead, so that every ability draws it the same way.
 */
struct bonus_highlight{
    bool   active;
    vec3_t color;
    float  width;
};

/* One tag moves one stat. */
struct group_bonus_desc{
    char                 tag[COMBAT_MOD_TAG_LEN];
    char                 icon[COMBAT_BONUS_ICON_LEN];
    enum combat_mod_kind kind;
    float                amount;
    bool                 percent;
};

struct proj_fire_desc{
    /* How many frames into the "fire" animation
     * do we launch it? */
    size_t frame_offset;
    int    fire_mode;
    /* The name of the bone of the firing entity 
     * from which the projectile will be spawned. 
     * A first byte of NUL (0) means the value is 
     * not used. */
    char   bone_name[64];
    /* When present, this is the offset from the 
     * position of bone_name bone. When bone_name
     * is not used, this is the offset from the local
     * origin. */
    vec3_t offset;
};

void  G_Combat_AttackUnit(uint32_t uid, uint32_t target);

void  G_Combat_SetStance(uint32_t uid, enum combat_stance stance);
void  G_Combat_SetCurrentHP(uint32_t uid, int hp);
int   G_Combat_GetCurrentHP(uint32_t uid);
void  G_Combat_UpdateRef(int oldfac, int newfac, vec2_t pos);
bool  G_Combat_IsDying(uint32_t uid);

void  G_Combat_SetBaseArmour(uint32_t uid, int armour);
int   G_Combat_GetBaseArmour(uint32_t uid);
/* Bridges between the flat points the simulation uses and the blocked fraction
 * the scripts author. A fraction at or above 1.0 saturates at the cap.
 */
int   G_Combat_ArmourPointsForFrac(float frac);
float G_Combat_FracForArmourPoints(int armour);
void  G_Combat_SetInvulnerable(uint32_t uid, bool on);
bool  G_Combat_GetInvulnerable(uint32_t uid);
/* A 'secs' of 0 keeps the modifier until it is removed by tag or cleared. A tag
 * may be NULL; adding one that matches a live tag replaces it, which is what
 * makes re-applying an aura idempotent. A 'percent' modifier scales the base
 * stat it moves, so 0.2 is +20%; it is resolved against the base every time the
 * sums are rebuilt.
 */
void  G_Combat_AddModifier(uint32_t uid, enum combat_mod_kind kind, float amount,
                           bool percent, uint32_t secs, const char *tag);
void  G_Combat_RemoveModifier(uint32_t uid, const char *tag);
void  G_Combat_ClearModifiers(uint32_t uid);
float G_Combat_GetBonus(uint32_t uid, enum combat_mod_kind kind);
int   G_Combat_GetEffectiveArmour(uint32_t uid);
int   G_Combat_GetEffectiveDamage(uint32_t uid);
float G_Combat_GetEffectiveRange(uint32_t uid);
bool  G_Combat_GetEffectiveInvulnerable(uint32_t uid);
/* A group bonus is a standing contribution an entity makes to every member of
 * whatever group it is in. It is not a modifier record: the bonus is derived
 * from live membership, so it lapses the moment the entity leaves.
 */
void  G_Combat_SetGroupBonus(uint32_t uid, const struct group_bonus_desc *desc);
void  G_Combat_ClearGroupBonus(uint32_t uid, const char *tag);
int   G_Combat_GetGroupBonuses(uint32_t uid, struct group_bonus_desc *out, size_t maxout);
void  G_Combat_RefreshBonuses(uint32_t uid);
/* The aggregate of the group members' contributions, declared here rather than
 * beside the other group calls because it is only meaningful with the modifier
 * kinds in scope. Contributions are folded by tag, so eight bannermen carry one
 * bannerman's bonus while two differently-named bonuses both apply.
 * G_Group_RefreshBonus rebuilds the aggregate and re-derives the stats of every
 * member; a group_id of 0 is a no-op, so an ungrouped entity needs no guard.
 */
void  G_Group_GetBonus(int group_id, enum combat_mod_kind kind, float *out_flat,
                       float *out_percent);
void  G_Group_RefreshBonus(int group_id);
int   G_Group_GetBonuses(int group_id, struct group_bonus_desc *out, size_t maxout);
void  G_Group_SetHighlight(int group_id, const struct bonus_highlight *hl);
void  G_Combat_SetBaseDamage(uint32_t uid, int dmg);
int   G_Combat_GetBaseDamage(uint32_t uid);
void  G_Combat_SetMaxHP(uint32_t uid, int hp);
int   G_Combat_GetMaxHP(uint32_t uid);
void  G_Combat_SetRange(uint32_t uid, float range);
float G_Combat_GetRange(uint32_t uid);
void  G_Combat_SetProjDesc(uint32_t uid, const struct proj_desc *pd);
void  G_Combat_SetProjFireDesc(uint32_t uid, const struct proj_fire_desc *fd);
void  G_Combat_SetCorpseModel(uint32_t uid, const char *dir, const char *pfobj, vec3_t scale);
bool  G_Combat_GetHPDisplay(uint32_t uid, int *out_curr, int *out_max);
enum combat_stance G_Combat_GetStance(uint32_t uid);

void  G_Combat_SetDamageType(uint32_t uid, int type);
int   G_Combat_GetDamageType(uint32_t uid);
void  G_Combat_SetArmourType(uint32_t uid, int type);
int   G_Combat_GetArmourType(uint32_t uid);

/* 'mult' is an 'nrows' x 'ncols' row-major array of multipliers indexed
 * [damage_type][armour_type]; it may be smaller than the capacity, in which
 * case the remaining cells are 1.0. All 1.0 by default, which leaves the damage
 * arithmetic exactly as it is without a table. The getter always writes the
 * full DAMAGE_TYPE_MAX * ARMOUR_TYPE_MAX array.
 */
void  G_Combat_SetDamageTable(const float *mult, int nrows, int ncols);
void  G_Combat_GetDamageTable(float *out_mult);

/*###########################################################################*/
/* GAME POSITION                                                             */
/*###########################################################################*/

struct kh_id_s;
struct bg_ent_s;

bool           G_Pos_Set(uint32_t uid, vec3_t pos);
vec3_t         G_Pos_Get(uint32_t uid);
vec2_t         G_Pos_GetXZ(uint32_t uid);

vec3_t         G_Pos_GetFrom(struct kh_pos_s *table, uint32_t uid);
vec2_t         G_Pos_GetXZFrom(struct kh_pos_s *table, uint32_t uid);
bool           G_Pos_HasFrom(struct kh_pos_s *table, uint32_t uid);

int            G_Pos_EntsInRect(vec2_t xz_min, vec2_t xz_max, uint32_t *out, size_t maxout);
int            G_Pos_EntsInRectFrom(struct bg_ent_s *tree, struct kh_id_s *flags,
                                    vec2_t xz_min, vec2_t xz_max, uint32_t *out, size_t maxout);
int            G_Pos_EntsInRectWithPred(vec2_t xz_min, vec2_t xz_max, uint32_t *out, size_t maxout,
                                        bool (*predicate)(uint32_t ent, void *arg), void *arg);
int            G_Pos_EntsInCircle(vec2_t xz_point, float range, uint32_t *out, size_t maxout);
int            G_Pos_EntsInCircleWithPred(vec2_t xz_point, float range, uint32_t *out, size_t maxout,
                                  bool (*predicate)(uint32_t ent, void *arg), void *arg);

uint32_t       G_Pos_Nearest(vec2_t xz_point);
uint32_t       G_Pos_NearestWithPred(vec2_t xz_point, 
                                     bool (*predicate)(uint32_t ent, void *arg), 
                                     void *arg, float max_range);

/*###########################################################################*/
/* GAME FOG-OF-WAR                                                           */
/*###########################################################################*/

bool  G_Fog_ObjExplored(uint16_t fac_mask, uint32_t uid, const struct obb *obb);
bool  G_Fog_ObjVisible(uint16_t fac_mask, const struct obb *obb);
bool  G_Fog_Visible(int faction_id, vec2_t xz_pos);
bool  G_Fog_ObjVisibleFrom(uint32_t *state, bool enabled, 
                           uint16_t fac_mask, const struct obb *obb);
bool  G_Fog_PlayerVisible(vec2_t xz_pos);
bool  G_Fog_Explored(int faction_id, vec2_t xz_pos);
bool  G_Fog_PlayerExplored(vec2_t xz_pos);
void  G_Fog_RenderChunkVisibility(int faction_id, int chunk_r, int chunk_c, mat4x4_t *model);
void  G_Fog_ExploreMap(int faction_id);
void  G_Fog_Enable(void);
void  G_Fog_Disable(void);

/*###########################################################################*/
/* GAME BUILDING                                                             */
/*###########################################################################*/

bool   G_Building_Mark(uint32_t uid);
bool   G_Building_Found(uint32_t uid, bool blocking);
bool   G_Building_Supply(uint32_t uid);
bool   G_Building_Complete(uint32_t uid);
bool   G_Building_Unobstructed(uint32_t uid);
bool   G_Building_IsFounded(uint32_t uid);
bool   G_Building_IsSupplied(uint32_t uid);
bool   G_Building_IsCompleted(uint32_t uid);
void   G_Building_SetVisionRange(uint32_t uid, float vision_range);
float  G_Building_GetVisionRange(uint32_t uid);
int    G_Building_GetRequired(uint32_t uid, const char *rname);
bool   G_Building_SetRequired(uint32_t uid, const char *rname, int req);
size_t G_Building_GetAllRequired(uint32_t uid, size_t maxout, 
                                 const char *names[], int amounts[]);
void   G_Building_SetPathable(uint32_t uid, bool pathable);
bool   G_Building_GetPathable(uint32_t uid);
void   G_Building_SetRallyPoint(uint32_t uid, vec2_t pos);
vec2_t G_Building_GetRallyPoint(uint32_t uid);
void   G_Building_SetPositionRallyPointOnLeftClick(void);
void   G_Building_SetGroundTexture(uint32_t uid, const char *texture);
const char *G_BuildingGetGroundTexture(uint32_t uid);

/*###########################################################################*/
/* GAME BUILDER                                                              */
/*###########################################################################*/

bool G_Builder_Build(uint32_t uid, uint32_t building);
void G_Builder_SetBuildSpeed(uint32_t uid, int speed);
int  G_Builder_GetBuildSpeed(uint32_t uid);
void G_Builder_SetBuildOnLeftClick(void);

/*###########################################################################*/
/* GAME RESOURCE                                                             */
/*###########################################################################*/

int         G_Resource_GetAmount(uint32_t uid);
void        G_Resource_SetAmount(uint32_t uid, int amount);
bool        G_Resource_SetName(uint32_t uid, const char *name);
const char *G_Resource_GetName(uint32_t uid);
const char *G_Resource_GetCursor(uint32_t uid);
bool        G_Resource_SetCursor(uint32_t uid, const char *cursor);
void        G_Resource_SetIcon(const char *name, const char *path);
const char *G_Resource_GetIcon(const char *name);
int         G_Resource_GetAllNames(size_t maxout, const char* out[]);
void        G_Resource_UpdateSelectionRadius(uint32_t uid, float radius);
bool        G_Resource_GetReplenishable(uint32_t uid);
void        G_Resource_SetReplenishable(uint32_t uid, bool set);
bool        G_Resource_SetReplenishAmount(uint32_t uid, const char *rname, int amount);
int         G_Resource_GetReplenishAmount(uint32_t uid, const char *rname);
int         G_Resource_GetRestoredAmount(uint32_t uid);
void        G_Resource_SetRestoredAmount(uint32_t uid, int amount);

/*###########################################################################*/
/* GAME HARVESTER                                                            */
/*###########################################################################*/

enum tstrategy{
    /* The harvester will take resources from the closest eligible
     * storage site, ragardless of whether or not that will cause 
     * the stored amount to dip under the desired stockpile amount. */
    TRANSPORT_STRATEGY_NEAREST,
    /* The harvester will respect the desired stockpile settings of 
     * all storage sites and take resources only from those sites 
     * that have 'excess' resources. */
    TRANSPORT_STRATEGY_EXCESS,
    /* The harvester will attempt to gather resources to keep the 
     * target storage site saturated. The harvester will fall back 
     * to the 'NEAREST' strategy. */
    TRANSPORT_STRATEGY_GATHERING,
};

void  G_Harvester_SetGatherOnLeftClick(void);
void  G_Harvester_SetPickUpOnLeftClick(void);
void  G_Harvester_SetDropOffOnLeftClick(void);
void  G_Harvester_SetTransportOnLeftClick(void);

bool  G_Harvester_Gather(uint32_t uid, uint32_t storage);
bool  G_Harvester_PickUp(uint32_t uid, uint32_t storage);
bool  G_Harvester_DropOff(uint32_t uid, uint32_t storage);
bool  G_Harvester_Transport(uint32_t uid, uint32_t storage);

bool  G_Harvester_SetGatherSpeed(uint32_t uid, const char *rname, float speed);
float G_Harvester_GetGatherSpeed(uint32_t uid, const char *rname);
bool  G_Harvester_SetMaxCarry(uint32_t uid, const char *rname, int max);
int   G_Harvester_GetMaxCarry(uint32_t uid, const char *rname);
bool  G_Harvester_SetCurrCarry(uint32_t uid, const char *rname, int curr);
int   G_Harvester_GetCurrCarry(uint32_t uid, const char *rname);
void  G_Harvester_ClearCurrCarry(uint32_t uid);
void  G_Harvester_SetStrategy(uint32_t uid, enum tstrategy strat);
int   G_Harvester_GetStrategy(uint32_t uid);
bool  G_Harvester_IncreaseTransportPrio(uint32_t uid, const char *rname);
bool  G_Harvester_DecreaseTransportPrio(uint32_t uid, const char *rname);
int   G_Harvester_GetTransportPrio(uint32_t uid, size_t maxout, const char* out[]);
int   G_Harvester_GetCurrTotalCarry(uint32_t uid);
bool  G_Harvester_SetDoNotTransport(uint32_t uid, const char *rname, bool set);
bool  G_Harvester_GetDoNotTransport(uint32_t uid, const char *rname);

/*###########################################################################*/
/* GAME STORAGE SITE                                                         */
/*###########################################################################*/

struct ss_delta_event{
    const char *name;
    int delta;
};

enum ss_ui_mode{
    SS_UI_SHOW_ALWAYS,
    SS_UI_SHOW_SELECTED,
    SS_UI_SHOW_NEVER,
};

bool G_StorageSite_SetCapacity(uint32_t uid, const char *rname, int max);
int  G_StorageSite_GetCapacity(uint32_t uid, const char *rname);
bool G_StorageSite_SetCurr(uint32_t uid, const char *rname, int curr);
int  G_StorageSite_GetCurr(uint32_t uid, const char *rname);
bool G_StorageSite_SetCurr(uint32_t uid, const char *rname, int curr);
int  G_StorageSite_GetDesired(uint32_t uid, const char *rname);
bool G_StorageSite_SetDesired(uint32_t uid, const char *rname, int des);
int  G_StorageSite_GetStorableResources(uint32_t uid, size_t maxout, const char* out[]);
int  G_StorageSite_GetPlayerStored(const char *rname);
int  G_StorageSite_GetPlayerCapacity(const char *rname);
void G_StorageSite_SetFontColor(const struct nk_color *clr);
void G_StorageSite_SetBorderColor(const struct nk_color *clr);
void G_StorageSite_SetBackgroundStyle(const struct nk_style_item *style);
void G_StorageSite_SetShowUI(bool show);
bool G_StorageSite_GetDoNotTakeLand(uint32_t uid);
void G_StorageSite_SetDoNotTakeLand(uint32_t uid, bool on);
bool G_StorageSite_GetDoNotTakeWater(uint32_t uid);
void G_StorageSite_SetDoNotTakeWater(uint32_t uid, bool on);

/*###########################################################################*/
/* GAME REGION                                                               */
/*###########################################################################*/

enum region_type{
    REGION_RECTANGLE,
    REGION_CIRCLE,
};

bool   G_Region_AddCircle(const char *name, vec2_t pos, float radius);
bool   G_Region_AddRectangle(const char *name, vec2_t pos, float xlen, float zlen);
void   G_Region_Remove(const char *name);
bool   G_Region_SetShown(const char *name, bool on);
bool   G_Region_GetShown(const char *name, bool *out);

bool   G_Region_SetPos(const char *name, vec2_t pos);
bool   G_Region_GetPos(const char *name, vec2_t *out);
void   G_Region_SetRender(bool on);
bool   G_Region_GetRender(void);

/* A region may carry an aura: a modifier applied to every entity inside it and
 * dropped as soon as one leaves. Only entities not at war with the owner are
 * affected, and the tag is a slot as everywhere else, so two overlapping auras
 * of the same tag do not stack.
 */
struct region_aura{
    bool                 active;
    uint32_t             owner;
    enum combat_mod_kind kind;
    float                amount;
    bool                 percent;
    char                 tag[COMBAT_MOD_TAG_LEN];
};

bool   G_Region_SetHighlight(const char *name, const struct bonus_highlight *hl);
bool   G_Region_SetAura(const char *name, const struct region_aura *aura);
bool   G_Region_ClearAura(const char *name);
/* Have the region track an entity's position. A uid of NULL_UID unpins it. */
bool   G_Region_SetFollow(const char *name, uint32_t uid);

bool   G_Region_GetRadius(const char *name, float *out);
bool   G_Region_GetXLen(const char *name, float *out);
bool   G_Region_GetZLen(const char *name, float *out);

int    G_Region_GetNumEnts(const char *name);
int    G_Region_GetEnts(const char *name, size_t maxout, uint32_t ents[]);
bool   G_Region_ContainsEnt(const char *name, uint32_t uid);
bool   G_Region_ExploreFog(const char *name, int faction_id);
bool   G_Region_Explored(const char *name, uint16_t player_mask, bool *out);

/*###########################################################################*/
/* GAME FORMATION                                                            */
/*###########################################################################*/

void G_Formation_Arrange(enum formation_type type, vec_entity_t *ents);
void G_Formation_SetPreferred(uint32_t uid, enum formation_type type);
enum formation_type G_Formation_GetPreferred(uint32_t uid);
enum formation_type G_Formation_PreferredForSet(const vec_entity_t *ents);

/*###########################################################################*/
/* GAME GARRISONN                                                            */
/*###########################################################################*/

void G_Garrison_SetCapacityConsumed(uint32_t uid, int capacity);
int  G_Garrison_GetCapacityConsumed(uint32_t uid);
void G_Garrison_SetGarrisonableCapacity(uint32_t uid, int capacity);
int  G_Garrison_GetGarrisonableCapacity(uint32_t uid);
int  G_Garrison_GetCurrentGarrisoned(uint32_t uid);
bool G_Garrison_Enter(uint32_t garrisonable, uint32_t unit);
bool G_Garrison_Evict(uint32_t garrisonable, uint32_t unit, vec2_t target);
bool G_Garrison_EvictAll(uint32_t garrisonable, vec2_t target);
void G_Garrison_SetFontColor(const struct nk_color *clr);
void G_Garrison_SetBackgroundStyle(const struct nk_style_item *item);
void G_Garrison_SetIcon(const char *path);
void G_Garrison_SetShowUI(bool show);
void G_Garrison_SetEvictOnLeftClick(void);

/*###########################################################################*/
/* GAME AUTOMATION                                                           */
/*###########################################################################*/

void G_Automation_GetIdle(vec_entity_t *out);
bool G_Automation_IsIdle(uint32_t uid);
void G_Automation_SetAutomaticTransport(uint32_t uid, bool on);
bool G_Automation_GetAutomaticTransport(uint32_t uid);

#endif

