#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

struct pfobj_hdr;

size_t R_AL_PrivBuffSizeFromHeader(const struct pfobj_hdr *header);
bool   R_AL_InitPrivFromStream(const struct pfobj_hdr *header, FILE *stream, void *priv_buff);
/* In PF Object format */
void   R_AL_DumpPrivate(FILE *stream, void *priv_data);

#endif
