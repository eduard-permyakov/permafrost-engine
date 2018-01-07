#include "public/map.h"
#include "../render/public/render.h"
#include "map_private.h"
#include "pfchunk.h"

#include <unistd.h>

struct chunkpos{
    int r, c;
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void m_model_matrix_for_chunk(const struct map *map, struct chunkpos p,
                                     mat4x4_t *out)
{
    ssize_t x_offset = -(p.c * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE);
    ssize_t z_offset =  (p.r * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);
    vec3_t chunk_pos = (vec3_t) {map->pos.x + x_offset, map->pos.y, map->pos.z + z_offset};
   
    PFM_Mat4x4_MakeTrans(chunk_pos.x, chunk_pos.y, chunk_pos.z, out);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void M_RenderEntireMap(const struct map *map)
{
    for(int r = 0; r < map->height; r++) {
        for(int c = 0; c < map->width; c++) {
        
            mat4x4_t chunk_model;

            m_model_matrix_for_chunk(map, (struct chunkpos) {r, c}, &chunk_model);
            R_GL_Draw(map->chunks[r * map->width + c].render_private, &chunk_model);
        }
    }
}

void M_CenterAtOrigin(struct map *map)
{
    size_t width  = map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    size_t height = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    map->pos = (vec3_t) {(width / 2.0f), 0.0f, -(height / 2.0f)};
}

