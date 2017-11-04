#include "shader.h"

#include <SDL2/SDL.h>

#include <stdlib.h>
#include <string.h>

#define SHADER_PATH_LEN 128
#define ARR_SIZE(a)     (sizeof(a[0])/sizeof(a))

struct shader_resource{
    GLint               prog_id;
    const char         *name;
    const char         *vertex_path;
    const char         *frag_path;
};

//TODO: non hard coded path
/* Shader program ids will be initialized by Shader_InitAll */
static struct shader_resource s_shaders[] = {
    { 
        .prog_id     = (intptr_t)NULL,
        .name        = "generic",
        .vertex_path = "/home/eduard/engine/shaders/vertex.glsl",
        .frag_path   = "/home/eduard/engine/shaders/fragment.glsl"
    },
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
        return false;
    
    if(!shader_init(text, out, type)) {
        return false;
    }

    free((char*)text);

    return true;
}

static bool shader_make_prog(const GLuint vertex_shader, const GLuint frag_shader, GLint *out)
{
    char info[512];
    GLint success;

    *out = glCreateProgram();
    glAttachShader(*out, vertex_shader);
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

bool Shader_InitAll(void)
{
    for(int i = 0; i < ARR_SIZE(s_shaders); i++){

        struct shader_resource *res = &s_shaders[i];
        GLuint vertex, fragment;
    
        if(!shader_load_and_init(res->vertex_path, &vertex, GL_VERTEX_SHADER))
            return false;

        if(!shader_load_and_init(res->frag_path, &fragment, GL_FRAGMENT_SHADER))
            return false;

        if(!shader_make_prog(vertex, fragment, &res->prog_id)) {

            glDeleteShader(vertex);
            glDeleteShader(fragment);
            return false;
        }

        glDeleteShader(vertex);
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
    
