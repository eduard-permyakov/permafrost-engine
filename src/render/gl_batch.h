/* 
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2026 Eduard Permyakov 
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

#ifndef GL_BATCH_H
#define GL_BATCH_H

#include <GL/glew.h>
#include <stdbool.h>

struct render_input;
struct render_private;

bool R_GL_Batch_Init(void);
void R_GL_Batch_Shutdown(void);

/* Upload a static or animated mesh's vertices and textures into the shared
 * batch storage, recording the location in 'priv->mesh.batch'. */
bool R_GL_Batch_AppendMesh(struct render_private *priv, const void *vbuff);

/* Map/unmap a batched mesh's vertex data for CPU read-back. The returned
 * pointer is offset to the start of the mesh's vertices. */
const void *R_GL_Batch_MeshVertsMap(const struct render_private *priv);
void        R_GL_Batch_MeshVertsUnmap(const struct render_private *priv);

/* Bind the VAO holding a batched mesh and return the index of its first vertex. */
GLint R_GL_Batch_MeshBindVAO(const struct render_private *priv);

/* Return the buffer object and the byte offset at which a mesh's vertex data
 * begins, for callers that build their own VAOs. The offset is 0 for
 * standalone meshes. */
void R_GL_Batch_MeshGetStorage(const struct render_private *priv, GLuint *out_vbo, size_t *out_offset);

/* Bind the texture arrays holding a batched mesh's textures. The per-material
 * array/slot indices are carried in the material uniforms. */
void R_GL_Batch_MeshBindTextures(const struct render_private *priv, GLuint shader_prog);

#endif

