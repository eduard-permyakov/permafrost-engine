#include "texture.h"
#include "../lib/public/stb_image.h"

#include <string.h>
#include <assert.h>

#define MAX_NUM_TEXTURE  128
#define MAX_TEX_NAME_LEN 32


struct texture_resource{
    char                     name[MAX_TEX_NAME_LEN];
    GLint                    texture_id;
    struct texture_resource *next_free;
    struct texture_resource *prev_free;
    bool                     free;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct texture_resource s_tex_resources[MAX_NUM_TEXTURE];
struct texture_resource       *s_free_head = &s_tex_resources[0];


/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool r_texture_gl_init(const char *path, GLuint *out)
{
    GLuint ret;
    int width, height, nr_channels;
    unsigned char *data;
    
    data = stbi_load(path, &width, &height, &nr_channels, 0);
    if(!data)
        goto fail_load;

    glGenTextures(1, &ret);
    glBindTexture(GL_TEXTURE_2D, ret);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);   
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    stbi_image_free(data);
    return true;

fail_load:
    return false;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_Texture_Init(void)
{
    for(int i = 0; i < MAX_NUM_TEXTURE - 1; i++) {

        struct texture_resource *res  = &s_tex_resources[i];
        struct texture_resource *next = &s_tex_resources[i+1];

        res->next_free = next;
        next->prev_free = res;

        res->free = true;
    }
}

bool R_Texture_GetForName(const char *name, GLuint *out)
{
    for(int i = 0; i < MAX_NUM_TEXTURE - 1; i++) {

        struct texture_resource *curr = &s_tex_resources[i];

        if(!strcmp(name, curr->name) && !curr->free) {
            *out = curr->texture_id;
            return true;
        }
    }

    return false;
}

bool R_Texture_Load(const char *basedir, const char *name, GLuint *out)
{
    if(!s_free_head)
        return false;

    struct texture_resource *alloc = s_free_head;
    alloc->free = false;

    s_free_head = s_free_head->next_free;
    if(alloc->next_free)
        alloc->next_free->prev_free = s_free_head;

    assert( strlen(name) < MAX_TEX_NAME_LEN );
    strcpy(alloc->name, name);

    char texture_path[128];
    assert( strlen(basedir) + strlen(name) < sizeof(texture_path) );
    strcat(texture_path, basedir);
    strcat(texture_path, "/");
    strcat(texture_path, name);

    if(!r_texture_gl_init(texture_path, out))
        goto fail;

    return true;

fail:
    return false;
}

void R_Texture_Free(const char *name)
{
    for(int i = 0; i < MAX_NUM_TEXTURE - 1; i++) {

        struct texture_resource *curr = &s_tex_resources[i];

        if(!strcmp(name, curr->name) && !curr->free) {

            glDeleteTextures(1, &curr->texture_id);
            curr->free = true;

            struct texture_resource *tmp = s_free_head;
            s_free_head = curr;
            s_free_head->next_free = tmp;
            if(tmp)
                tmp->prev_free = s_free_head;
            break;
        }
    }
}

void R_Texture_GL_Activate(const struct texture *text)
{

}

