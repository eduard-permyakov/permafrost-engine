#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

struct pfobj_hdr;

/* In PF Object format */
void   R_DumpPrivate(FILE *stream, void *priv_data);

size_t R_AL_PrivBuffSizeFromHeader(const struct pfobj_hdr *header);
bool   R_AL_InitPrivFromStream(const struct pfobj_hdr *header, FILE *stream, void *priv_buff);

#endif
