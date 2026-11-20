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
         vec2  uv;
         float card_depth;
    flat float depth_slope;
    flat int   layer;
}to_fragment;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform mat4  light_space_transform;
uniform int   bb_nazimuths;
/* From the silhouette's light clip space back to the bake's model space */
uniform mat4  bb_sil_unproj;
/* The Y rotation that carried the world's light into the bake's frame */
uniform float bb_sil_delta;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

vec3 bake_to_world(vec3 v)
{
    float c = cos(bb_sil_delta);
    float s = sin(bb_sil_delta);
    return vec3((c * v.x - s * v.z) * in_scale.x,
                v.y * in_scale.y,
                (s * v.x + c * v.z) * in_scale.x);
}

void main()
{
    /* The bake frame is the RTS camera's, with the light fixed in it, so the
     * cell whose model stands to the light as this entity does is the one
     * the colour pass would show the RTS camera
     */
    float rel = bb_sil_delta - in_yaw;
    int bin = int(mod(floor(rel * float(bb_nazimuths) / TAU + 0.5), float(bb_nazimuths)));

    /* A card facing the light through the middle of the silhouette's depth
     * window; each texel's own depth lies along the light's rays from it
     */
    vec4 card = bb_sil_unproj * vec4(in_corner, 0.0, 1.0);
    vec3 ws_pos = in_pos + bake_to_world(card.xyz / card.w);
    vec3 ray = bake_to_world((bb_sil_unproj * vec4(0.0, 0.0, 1.0, 0.0)).xyz);

    vec4 clip = light_space_transform * vec4(ws_pos, 1.0);
    to_fragment.uv = in_corner * 0.5 + 0.5;
    to_fragment.card_depth = clip.z / clip.w;
    to_fragment.depth_slope = (light_space_transform * vec4(ray, 0.0)).z;
    to_fragment.layer = in_cell_base + bin;
    gl_Position = clip;
}
