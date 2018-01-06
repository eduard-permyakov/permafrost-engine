#ifndef PFCHUNK_H
#define PFCHUNK_H

#include "public/tile.h"
#include "../pf_math.h"

#include <stdbool.h>

#define TILES_PER_CHUNK_HEIGHT 32
#define TILES_PER_CHUNK_WIDTH  32

struct pfchunk{
    /* ------------------------------------------------------------------------
     * Initialized and used by the rendering subsystem. Holds the mesh data 
     * and everything the rendering subsystem needs to render this PFChunk.
     * ------------------------------------------------------------------------
     */
    void           *render_private;
    /* ------------------------------------------------------------------------
     * Pointers to adjacent chunks. NULL if there are none.
     * ------------------------------------------------------------------------
     */
    struct pfchunk *north, *south, *east, *west;
    /* ------------------------------------------------------------------------
     * Worldspace position of the top left corner. 
     * ------------------------------------------------------------------------
     */
    vec3_t          position;
    /* ------------------------------------------------------------------------
     * The 'has_path' bools are precomuted when the pfchunk is initialized or 
     * updated.
     *
     * They are used later to quickly query if we can reach onother chunk 
     * adjacent to this one. If not, there is no point looking at the tile or 
     * subtile resolution.
     *
     * Note that having a North->South path implies that there is also a 
     * South->North path and so on.
     * ------------------------------------------------------------------------
     */
    bool            has_ns_path;
    bool            has_sw_path;
    bool            has_wn_path;
    bool            has_ne_path;
    bool            has_es_path;
    bool            has_sn_path;
    /* ------------------------------------------------------------------------
     * Each tiles' attributes, stored in row-major order.
     * ------------------------------------------------------------------------
     */
    struct tile     tiles[TILES_PER_CHUNK_HEIGHT * TILES_PER_CHUNK_WIDTH];
    /* ------------------------------------------------------------------------
     * Pointers for maintaining an embedded doubly-linked list structure.
     * These are used for efficiently iterating over all pfchunks in a map.
     * Can be NULL.
     * ------------------------------------------------------------------------
     */
    struct pfchunk *next, *prev;
};


/* ------------------------------------------------------------------------
 * Will return a heap-allocated pointer with the 'render_private' pointer
 * already initialized to a buffer of the right size. This buffer cannot
 * be initialized with the render data until the tiles of this chunk are filled
 * out. All other pointers will be NULL and all other bools will be false,
 * including for the tiles.
 *
 * 'M_PFChunk_Free' must be called on the pointer to free all resources. 
 * ------------------------------------------------------------------------
 */
struct pfchunk *M_PFChunk_New(size_t num_mats);
void            M_PFChunk_Free(struct pfchunk *chunk);

#endif
