#include "public/map.h"
#include "../render/public/render.h"
#include "pfchunk.h"

void M_ModelMatrixForChunk(const struct map *map, struct pos p, mat4x4_t *out)
{
    //TODO: we gonna index chunk from buffer with pos and add its' position
   
    PFM_Mat4x4_MakeTrans(map->pos.x, map->pos.y, map->pos.z, out);
}

void M_RenderChunk(const struct map *map, struct pos p)
{
    mat4x4_t chunk_model;

    M_ModelMatrixForChunk(map, (struct pos) {0, 0}, &chunk_model);
    R_GL_Draw(map->head->render_private, &chunk_model);
}

