#include "public/render.h"
#include "render_private.h"
#include "vertex.h"

#include <string.h>

void R_DumpPrivate(FILE *stream, void *priv_data)
{
    struct render_private *priv = priv_data;

    for(int i = 0; i < priv->mesh.num_verts; i++) {

        struct vertex *v = &priv->mesh.vbuff[i];

        fprintf(stream, "v %.6f %.6f %.6f\n", 
            v->pos.x, v->pos.y, v->pos.z); 

        fprintf(stream, "vt %.6f %.6f \n", 
            v->uv.x, v->uv.y); 

        struct joint_weight empty = {0}; 

        fprintf(stream, "vw");
        for(int j = 0; j < 4; j++) {
            struct joint_weight *jw = &priv->mesh.vbuff[i].weights[j];

            if(memcmp(jw, &empty, sizeof(struct joint_weight)) != 0) {
                fprintf(stream, " %d/%.6f", jw->joint_idx, jw->weight); 
            }
        }
        fprintf(stream, "\n");
    }

    for(int i = 0; i < priv->mesh.num_faces; i++) {

        struct face *f = &priv->mesh.ebuff[i];

        fprintf(stream, "f %d %d %d\n",
            f->vertex_indeces[0], f->vertex_indeces[1], f->vertex_indeces[2]);
    }
}

