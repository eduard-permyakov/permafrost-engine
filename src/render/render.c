#include "public/render.h"
#include "render_private.h"
#include "vertex.h"

void R_DumpPrivate(FILE *file, void *priv_data)
{
    struct render_private *priv = priv_data;

    for(int i = 0; i < 1098; i++) {
        fprintf(file, "v %.6f %.6f %.6f\n", 
            priv->mesh.vbuff[i].pos.x, 
            priv->mesh.vbuff[i].pos.y, 
            priv->mesh.vbuff[i].pos.z); 
    }
}

