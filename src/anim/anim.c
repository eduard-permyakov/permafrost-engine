#include "public/anim.h"
#include "anim_private.h"
#include "anim_data.h"
#include "../entity.h"

const struct skeleton *A_GetSkeleton(struct entity *ent)
{
    struct anim_private *priv = ent->anim_private;
    return &priv->data->skel;
}

