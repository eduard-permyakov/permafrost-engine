#ifndef MAP_PRIVATE_H
#define MAP_PRIVATE_H

#include "pfchunk.h"

struct map{
    /* ------------------------------------------------------------------------
     * Map dimensions in numbers of chunks.
     * ------------------------------------------------------------------------
     */
    size_t width, height;
    /* ------------------------------------------------------------------------
     * World-space location of the top left corner of the map.
     * ------------------------------------------------------------------------
     */
    vec3_t pos;
    /* ------------------------------------------------------------------------
     * The map chunks stored in row-major order. In total, there must be 
     * (width * height) number of chunks.
     * ------------------------------------------------------------------------
     */
    struct pfchunk chunks[];
};

#endif
