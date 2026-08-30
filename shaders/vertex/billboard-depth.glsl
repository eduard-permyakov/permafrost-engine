/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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

layout (location = 0) in vec2 in_corner;
layout (location = 1) in vec3 in_pos;
layout (location = 2) in float in_yaw;
layout (location = 3) in vec2 in_scale;
layout (location = 4) in int in_cell_base;

#define PI  (3.14159265358979)
#define TAU (6.28318530717959)

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out VertexToFrag{
         vec2 uv;
    flat int  layer;
}to_fragment;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform mat4 light_space_transform;
uniform vec3 light_pos;

uniform vec2 bb_world_size;
uniform vec2 bb_anchor_off;
uniform int  bb_nazimuths;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

void main()
{
    /* The light is directional: rays travel opposite the position vector */
    vec3 light_dir = -normalize(light_pos);

    /* An upright card yawed to face the light's horizontal azimuth. The right
     * vector matches the colour path's cross(forward, world_up) convention so
     * asymmetric silhouettes are not mirrored. A near-vertical light
     * degenerates the azimuth; fall back to an arbitrary horizontal axis.
     */
    float hlen = length(light_dir.xz);
    vec3 card_right = (hlen > 1e-3)
        ? vec3(-light_dir.z, 0.0, light_dir.x) / hlen
        : vec3(1.0, 0.0, 0.0);
    vec3 card_up = vec3(0.0, 1.0, 0.0);

    /* The atlas cell as seen from the light: the colour pass's binning with
     * the light direction standing in for the view ray
     */
    float phi = atan(light_dir.x, light_dir.z);
    float rel = PI - in_yaw - phi;
    int bin = int(mod(floor(rel * float(bb_nazimuths) / TAU + 0.5), float(bb_nazimuths)));

    vec3 center = in_pos
                + card_right * (bb_anchor_off.x * in_scale.x)
                + card_up    * (bb_anchor_off.y * in_scale.y);
    vec3 ws_pos = center
                + card_right * (in_corner.x * bb_world_size.x * 0.5 * in_scale.x)
                + card_up    * (in_corner.y * bb_world_size.y * 0.5 * in_scale.y);

    to_fragment.uv = in_corner * 0.5 + 0.5;
    to_fragment.layer = in_cell_base + bin;
    gl_Position = light_space_transform * vec4(ws_pos, 1.0);
}
