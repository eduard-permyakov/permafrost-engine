#include "public/render.h"
#include "shader.h"
#include "texture.h"

bool R_Init(void)
{
    R_Texture_Init();

    return Shader_InitAll();    
}

