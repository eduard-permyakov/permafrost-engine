#include "public/game.h"
#include "gamestate.h"
#include "../render/public/render.h"
#include "../anim/public/anim.h"
#include "../map/public/map.h"
#include "../entity.h"

#include <assert.h> 

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct gamestate s_gs;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Init(void)
{
    s_gs.active = kl_init(entity);

    if(!s_gs.active)
        return false;

    return true;
}

void G_Shutdown(void)
{
    assert(s_gs.active);
    kl_destroy(entity, s_gs.active);
}

void G_Render(void)
{
    assert(s_gs.active);

    if(s_gs.map) M_RenderEntireMap(s_gs.map);

    kliter_t(entity) *p;
    for (p = kl_begin(s_gs.active); p != kl_end(s_gs.active); p = kl_next(p)) {
    
        struct entity *curr = kl_val(p);

        /* TODO: Currently, we perform animation right before rendering due to 'A_Update' setting
         * some uniforms for the shader. Investigate if it's better to perform the animation for all
         * entities at once and just set the uniform right before rendering. 
         *
         * Also, add a proper distinction between static and animated entities - currently we 
         * assume all are animated.*/
        A_Update(curr);

        mat4x4_t model;
        Entity_ModelMatrix(curr, &model);
        R_GL_Draw(curr->render_private, &model);
    }
}

bool G_AddEntity(struct entity *ent)
{
    *kl_pushp(entity, s_gs.active) = ent;
}

bool G_RemoveEntity(struct entity *ent)
{
    return (0 == kl_remove_first(entity, s_gs.active, &ent));
}

bool G_SetMap(struct map *map)
{
    assert(map);
    s_gs.map = map;
}

const struct map *G_GetMap(void)
{
    return s_gs.map;
}

