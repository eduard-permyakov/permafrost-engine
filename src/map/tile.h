#ifndef TILE_H
#define TILE_H

#include <stdbool.h>

enum tiletype{
    /* TILETYPE_FLAT:
     *                     +----------+
     *                    /          /|
     *                -  +----------+ +
     * base_height -> |  |          |/
     *                -  +----------+
     */
    TILETYPE_FLAT = 0,
    TILETYPE_RAMP_SN,
    TILETYPE_RAMP_NS,
    TILETYPE_RAMP_EW,
    TILETYPE_RAMP_WE,
    TILETYPE_CORNER_CONCAVE,
    TILETYPE_CORNER_CONVEX,
};

struct tile{
    /* ------------------------------------------------------------------------
     * 'pathable' is only valid when subtile_resolution is false. This means 
     * that all subtiles for this tile have the same pathability held in this 
     * tile's 'pathable' property. Otherwise, the subtiles have different 
     * pathability. 
     * ------------------------------------------------------------------------
     */
    bool          pathable;    
    bool          subtile_resolution;
    enum tiletype type;
    int           base_height;
    /* ------------------------------------------------------------------------
     * Only valid when 'type' is a ramp or corner tile.
     * ------------------------------------------------------------------------
     */
    int           ramp_height;
};

#endif
