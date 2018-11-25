/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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

#include "shader.h"

#include <SDL.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#define SHADER_PATH_LEN 128
#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))

#define MAKE_PATH(buff, base, file) \
    do{                             \
        strcpy(buff, base);         \
        strcat(buff, file);         \
    }while(0)


struct shader_resource{
    GLint       prog_id;
    const char *name;
    const char *vertex_path;
    const char *geo_path;
    const char *frag_path;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* Shader 'prog_id' will be initialized by R_Shader_InitAll */
static struct shader_resource s_shaders[] = {
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.static.colored",
        .vertex_path = "shaders/vertex_basic.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_colored.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.static.textured",
        .vertex_path = "shaders/vertex_static.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_textured.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.static.textured-phong",
        .vertex_path = "shaders/vertex_static.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_textured-phong.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.static.tile-outline",
        .vertex_path = "shaders/vertex_static.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_tile-outline.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.animated.textured-phong",
        .vertex_path = "shaders/vertex_skinned.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_textured-phong.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.static.normals.colored",
        .vertex_path = "shaders/vertex_static.glsl",
        .geo_path    = "shaders/geometry_normals.glsl",
        .frag_path   = "shaders/fragment_colored.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.animated.normals.colored",
        .vertex_path = "shaders/vertex_skinned.glsl",
        .geo_path    = "shaders/geometry_normals.glsl",
        .frag_path   = "shaders/fragment_colored.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "terrain",
        .vertex_path = "shaders/vertex_terrain.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_terrain.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "terrain-baked",
        .vertex_path = "shaders/vertex_static.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_terrain-baked.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.static.colored-per-vert",
        .vertex_path = "shaders/vertex_colored.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_colored-per-vert.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.static.depth",
        .vertex_path = "shaders/vertex_depth.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_passthrough.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.animated.depth",
        .vertex_path = "shaders/vertex_skinned-depth.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_passthrough.glsl"
    }
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

const char *shader_text_load(const char *path)
{
    SDL_RWops *stream = SDL_RWFromFile(path, "r");
    if(!stream){
        return NULL;
    }

    const Sint64 fsize = SDL_RWsize(stream);    
    char *ret = malloc(fsize + 1);
    if(!ret) {
        SDL_RWclose(stream);
        return NULL; 
    }
    char *out = ret;

    Sint64 read, read_total = 0;
    while(read_total < fsize) {
        read = SDL_RWread(stream, out, 1, fsize - read_total); 

        if(read == 0)
            break;

        read_total += read;
        out        += read;
    }
    SDL_RWclose(stream);

    if(read_total != fsize){
        return NULL;
    }

    out[0] = '\0';
    return ret;
}

static bool shader_init(const char *text, GLuint *out, GLint type)
{
    char info[512];
    GLint success;

    *out = glCreateShader(type);
    glShaderSource(*out, 1, &text, NULL);
    glCompileShader(*out);

    glGetShaderiv(*out, GL_COMPILE_STATUS, &success);
    if(!success) {

        glGetShaderInfoLog(*out, sizeof(info), NULL, info);
        fprintf(stderr, "%s\n", info);
        return false;
    }

    return true;
}

static bool shader_load_and_init(const char *path, GLint *out, GLint type)
{
    const char *text = shader_text_load(path);
    if(!text) {
        fprintf(stderr, "Could not load shader at: %s\n", path);
        goto fail;
    }
    
    if(!shader_init(text, out, type)){
        fprintf(stderr, "Could not compile shader at: %s\n", path);
        goto fail;
    }

    free((char*)text);
    return true;

fail:
    free((char*)text);
    return false;
}

static bool shader_make_prog(const GLuint vertex_shader, const GLuint geo_shader, const GLuint frag_shader, GLint *out)
{
    char info[512];
    GLint success;

    *out = glCreateProgram();
    glAttachShader(*out, vertex_shader);

    if(geo_shader) {
        glAttachShader(*out, geo_shader); 
    }

    glAttachShader(*out, frag_shader);
    glLinkProgram(*out);

    glGetProgramiv(*out, GL_LINK_STATUS, &success);
    if(!success) {

        glGetProgramInfoLog(*out, sizeof(info), NULL, info);
        fprintf(stderr, "%s\n", info);
        return false;
    }

    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_Shader_InitAll(const char *base_path)
{
    for(int i = 0; i < ARR_SIZE(s_shaders); i++){

        struct shader_resource *res = &s_shaders[i];
        GLuint vertex, geometry = 0, fragment;
        char path[512];
    
        MAKE_PATH(path, base_path, res->vertex_path);
        if(!shader_load_and_init(path, &vertex, GL_VERTEX_SHADER)) {
            fprintf(stderr, "Failed to load and init vertex shader.\n");
            return false;
        }

        if(res->geo_path)
            MAKE_PATH(path, base_path, res->geo_path);
        if(res->geo_path && !shader_load_and_init(path, &geometry, GL_GEOMETRY_SHADER)) {
            fprintf(stderr, "Failed to load and init geometry shader.\n");
            return false;
        }
        assert(!res->geo_path || geometry > 0);

        MAKE_PATH(path, base_path, res->frag_path);
        if(!shader_load_and_init(path, &fragment, GL_FRAGMENT_SHADER)) {
            fprintf(stderr, "Failed to load and init fragment shader.\n");
            return false;
        }

        if(!shader_make_prog(vertex, geometry, fragment, &res->prog_id)) {

            glDeleteShader(vertex);
            if(geometry)
                glDeleteShader(geometry);
            glDeleteShader(fragment);
            fprintf(stderr, "Failed to make shader program %d of %d.\n",
                i + 1, (int)ARR_SIZE(s_shaders));
            return false;
        }

        glDeleteShader(vertex);
        if(geometry)
            glDeleteShader(geometry);
        glDeleteShader(fragment);
    }

    return true;
}


GLint R_Shader_GetProgForName(const char *name)
{
    for(int i = 0; i < ARR_SIZE(s_shaders); i++) {

        const struct shader_resource *curr = &s_shaders[i];

        if(!strcmp(curr->name, name))
            return curr->prog_id;
    }
    
    return -1;
}
    
