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
layout (location = 5) in uint in_uid;

#define PI  (3.14159265358979)
#define TAU (6.28318530717959)

/* The width, in bins, of the strip over which neighbouring entities change
 * cell at different view azimuths
 */
#define BIN_DITHER_WIDTH (0.5)

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out VertexToFrag{
         vec2  uv;
         vec3  ws_pos;
    flat int   layer;
    flat float depth_scale;
    flat float shadow_offset;
    flat vec3  forward;
    flat float bake_turn;
}to_fragment;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform mat4 view;
uniform mat4 projection;
uniform vec3 view_pos;

uniform vec2 bb_world_size;
uniform vec2 bb_anchor_off;
uniform int  bb_nazimuths;
uniform float bb_shadow_offset;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

float uid_hash01(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x >> 8) / 16777216.0;
}

void main()
{
    /* World-space camera basis from the view matrix rows */
    vec3 cam_right   = vec3(view[0][0], view[1][0], view[2][0]);
    vec3 cam_up      = vec3(view[0][1], view[1][1], view[2][1]);
    vec3 cam_forward = -vec3(view[0][2], view[1][2], view[2][2]);

    /* The direction the entity is seen from: the constant view direction under
     * an orthographic projection, the per-entity ray under a perspective one
     */
    bool ortho = (projection[3][3] == 1.0);
    vec3 vdir = ortho ? cam_forward : (in_pos - view_pos);
    float phi = atan(vdir.x, vdir.z);

    /* An entity with yaw theta faces (-sin, cos) and a cell baked at azimuth
     * theta faces (sin, cos), so the entity yaw enters negated
     */
    float rel = PI - in_yaw - phi;
    /* Each entity changes cell at its own azimuth near the bin boundary, so a
     * formation facing one way breaks up over a strip rather than along a line
     */
    float dither = (uid_hash01(in_uid) - 0.5) * BIN_DITHER_WIDTH;
    int bin = int(mod(floor(rel * float(bb_nazimuths) / TAU + 0.5 + dither), float(bb_nazimuths)));

    /* The card faces the ray its cell was chosen for, keeping the camera
     * basis's handedness
     */
    vec3 forward = cam_forward;
    vec3 right = cam_right;
    vec3 up = cam_up;
    if(!ortho) {
        forward = normalize(vdir);
        vec3 horz = vec3(-forward.z, 0.0, forward.x);
        if(length(horz) > 1e-4) {
            right = normalize(horz) * sign(dot(horz, cam_right) + 1e-6);
        }
        up = normalize(cross(right, forward));
        up *= sign(dot(up, cam_up) + 1e-6);
    }

    vec3 center = in_pos
                + right * (bb_anchor_off.x * in_scale.x)
                + up    * (bb_anchor_off.y * in_scale.y);
    vec3 ws_pos = center
                + right * (in_corner.x * bb_world_size.x * 0.5 * in_scale.x)
                + up    * (in_corner.y * bb_world_size.y * 0.5 * in_scale.y);

    to_fragment.uv = in_corner * 0.5 + 0.5;
    to_fragment.ws_pos = ws_pos;
    to_fragment.layer = in_cell_base + bin;
    to_fragment.depth_scale = in_scale.x;
    to_fragment.shadow_offset = bb_shadow_offset * in_scale.x;
    to_fragment.forward = forward;
    /* The cell was baked looking along azimuth pi; turning it by this carries
     * its surface to the world's. The cell's model stands up to half a bin off
     * the entity's facing, which the turn makes up for, so that formations
     * are lit as they face rather than banded where the bin changes.
     */
    float bin_error = rel - float(bin) * TAU / float(bb_nazimuths);
    bin_error -= TAU * floor(bin_error / TAU + 0.5);
    to_fragment.bake_turn = phi - PI + bin_error;
    gl_Position = projection * view * vec4(ws_pos, 1.0);
}
