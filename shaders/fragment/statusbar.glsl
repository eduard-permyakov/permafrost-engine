/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2020 Eduard Permyakov 
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

#version 330 core

#define FULL_HP_CLR     vec4(0.0, 1.0, 0.0, 1.0)
#define NO_HP_CLR       vec4(1.0, 0.0, 0.0, 1.0)
#define BG_CLR          vec4(0.0, 0.0, 0.0, 1.0)

#define BORDER_PX_WIDTH (1.0)

/* Must match the definition in the vertex shader */
#define CURR_HB_HEIGHT  (max(4.0/1080 * curr_res.y, 4.0))
#define CURR_HB_WIDTH   (40.0/1080 * curr_res.y)

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag {
         vec2 uv;
        float health_pc;
}from_vertex;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out vec4 o_frag_color;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform ivec2 curr_res;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

void main()
{
    vec4 bar_clr = mix(NO_HP_CLR, FULL_HP_CLR, from_vertex.health_pc);

    /* Slightly darken up the bottom part of the bar */
    if(from_vertex.uv.y > 0.5)
        bar_clr *= 0.8;

    /* We are in the border region */
    if(from_vertex.uv.y < BORDER_PX_WIDTH/CURR_HB_HEIGHT
    || from_vertex.uv.y > (1.0 - BORDER_PX_WIDTH/CURR_HB_HEIGHT)
    || from_vertex.uv.x < BORDER_PX_WIDTH/CURR_HB_WIDTH
    || from_vertex.uv.x > (1.0 - BORDER_PX_WIDTH/CURR_HB_WIDTH))
        o_frag_color = BG_CLR;
    /* We are in the region right of a partially full healthbar */
    else if(from_vertex.uv.x > from_vertex.health_pc)
        o_frag_color = BG_CLR;
    /* We are in the healthbar */
    else
        o_frag_color = bar_clr;
}

