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

#ifndef GL_POSTPROCESS_H
#define GL_POSTPROCESS_H

#include <stdbool.h>
#include <GL/glew.h>

struct camera;

void R_GL_PostProcess_Shutdown(void);

/* Redirects the drawing that follows from the bound framebuffer to a target
 * of its own, 'scale' times smaller along each side when 'look' is given. 
 * 'look' is a directory, relative to the base path, holding the look's 
 * grade.png and palette.png, or NULL. Returns false, leaving the bound 
 * framebuffer in place, when there is neither a loadable look nor 
 * antialiasing to redirect for.
 */
bool R_GL_PostProcess_BeginWorld(const char *look, float scale, bool antialias);

/* Whether the redirected drawing is being graded to a look */
bool R_GL_PostProcess_Graded(void);

/* Draws the redirected image back into the framebuffer it was taken from:
 * antialiased if asked, then, with a look, graded, dithered by 'dither' (in 
 * OKLab lightness) and snapped to the look's palette. Its pixels keep hard 
 * edges, softened across one output pixel where the two sizes are not a 
 * whole multiple apart.
 */
void R_GL_PostProcess_Resolve(const struct camera *cam, float dither, bool antialias);

/* Gives the framebuffer the image was drawn back into the depth of the 
 * redirected drawing, for what is drawn over it to be hidden by.
 */
void R_GL_PostProcess_CopyDepth(void);

/* Sets a program's grade uniforms, with the look's grade on the texture unit,
 * for drawing over a graded image in matching colours. Off, the program 
 * draws ungraded.
 */
void R_GL_PostProcess_GradeBind(GLenum tunit, bool on);

#endif
