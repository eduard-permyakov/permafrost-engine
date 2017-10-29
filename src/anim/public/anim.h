#ifndef ANIM_H
#define ANIM_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

struct pfobj_hdr;

/* In PF Object format */
void   A_DumpPrivate(FILE *stream, void *priv_data);

size_t A_AL_PrivBuffSizeFromHeader(const struct pfobj_hdr *header);
bool   A_AL_InitPrivFromStream(const struct pfobj_hdr *header, FILE *stream, void *priv_buff);

#endif
