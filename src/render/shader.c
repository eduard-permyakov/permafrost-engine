#include "shader.h"

#include <SDL2/SDL.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define SHADER_PATH_LEN 128
#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))

#define MAKE_PATH(buff, base, file)	\
	do{								\
		strcpy(buff, base);			\
		strcat(buff, file);			\
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

/* Shader 'prog_id' will be initialized by Shader_InitAll */
static struct shader_resource s_shaders[] = {
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.static.colored",
        .vertex_path = "shaders/vertex_static.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_colored.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.animated.textured",
        .vertex_path = "shaders/vertex_skinned.glsl",
        .geo_path    = NULL,
        .frag_path   = "shaders/fragment_textured.glsl"
    },
    {
        .prog_id     = (intptr_t)NULL,
        .name        = "mesh.animated.normals.colored",
        .vertex_path = "shaders/vertex_skinned.glsl",
        .geo_path    = "shaders/geometry_normals.glsl",
        .frag_path   = "shaders/fragment_colored.glsl"
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
    if(!text)
        goto fail;
    
    if(!shader_init(text, out, type))
        goto fail;

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

bool Shader_InitAll(const char *base_path)
{
    for(int i = 0; i < ARR_SIZE(s_shaders); i++){

        struct shader_resource *res = &s_shaders[i];
        GLuint vertex, geometry = 0, fragment;
		char path[512];
    
		MAKE_PATH(path, base_path, res->vertex_path);
        if(!shader_load_and_init(path, &vertex, GL_VERTEX_SHADER))
            return false;

		if(res->geo_path)
			MAKE_PATH(path, base_path, res->geo_path);
        if(res->geo_path && !shader_load_and_init(path, &geometry, GL_GEOMETRY_SHADER))
            return false;
        assert(!res->geo_path || geometry > 0);

		MAKE_PATH(path, base_path, res->frag_path);
        if(!shader_load_and_init(path, &fragment, GL_FRAGMENT_SHADER))
            return false;

        if(!shader_make_prog(vertex, geometry, fragment, &res->prog_id)) {

            glDeleteShader(vertex);
            if(geometry)
                glDeleteShader(geometry);
            glDeleteShader(fragment);
            return false;
        }

        glDeleteShader(vertex);
        if(geometry)
            glDeleteShader(geometry);
        glDeleteShader(fragment);
    }

    return true;
}


GLint Shader_GetProgForName(const char *name)
{
    for(int i = 0; i < ARR_SIZE(s_shaders); i++) {

        const struct shader_resource *curr = &s_shaders[i];

        if(!strcmp(curr->name, name))
            return curr->prog_id;
    }
    
    return -1;
}
    
