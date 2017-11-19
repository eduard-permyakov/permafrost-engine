#ifndef ANIM_PRIVATE_H
#define ANIM_PRIVATE_H

struct skeleton;

struct anim_private{
    struct anim_data *data;
    struct anim_ctx  *ctx;
};

/* Computes the inverse bind matrix for each joint based on the 
 * joint's bind SQT. The inverse bind matrix will be used by the vertex
 * shader to transform a vertex to the coordinate space of a joint
 * it is bound to (i.e. give a position of the vertex relative to 
 * a joint in bind pose). The matrices will be written to the memory
 * pointed to by 'skel->inv_bind_poses' which is expected to be 
 * allocated already.
 */
void A_PrepareInvBindMatrices(const struct skeleton *skel);

#endif
