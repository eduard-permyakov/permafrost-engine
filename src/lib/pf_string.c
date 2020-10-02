/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2020 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#include "public/pf_string.h"

#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

char *pf_strtok_r(char *str, const char *delim, char **saveptr)
{
    if(str == NULL)
        str = *saveptr;

    if(*str == '\0') {
        *saveptr = str; 
        return NULL;
    }

    str += strspn(str, delim);
    if(*str == '\0') {
        *saveptr = str;
        return NULL;
    }

    char *end = str + strcspn(str, delim);
    if(*end == '\0') {
        *saveptr = end; 
        return str;
    }

    *end = '\0';
    *saveptr = end + 1;
    return str;
}

char *pf_strdup(const char *str)
{
    char *ret = malloc(strlen(str) + 1);
    if(ret)
        strcpy(ret, str);
    return ret;
}

char *pf_strapp(char *str, const char *append)
{
    size_t len = strlen(str) + strlen(append) + 1;
    char *ret = realloc((void*)str, len);
    if(!ret)
        return NULL;
    strcat(ret, append);
    assert(ret[len-1] == '\0');
    return ret;
}

size_t pf_strlcpy(char *dest, const char *src, size_t size)
{
    if(!size)
        return 0;

    size_t srclen = strlen(src);
    size_t ret = (srclen > size-1) ? size-1 : srclen;
    memcpy(dest, src, ret);
    dest[ret] = '\0';
    return ret;
}

int pf_snprintf(char *str, size_t size, const char *format, ...)
{
    int ret;
    va_list args;

    va_start(args, format);
    ret = vsnprintf(str, size, format, args);
    va_end(args);

    str[size-1] = '\0';
    return ret;
}

int pf_endswith(const char *str, const char *end)
{
    size_t slen = strlen(str);
    size_t elen = strlen(end);

    if(elen > slen)
        return 0;

    return (0 == strcmp(str + (slen - elen), end));
}

char *pf_strlcat(char *dest, const char *src, size_t size)
{
    size_t srclen = strlen(src);
    size_t dstlen = strlen(dest);
    if(size < 2)
        return dest;
    if(dstlen >= size-1)
        return dest;
    size_t left = size - 1 - dstlen;
    size_t ncpy = left > srclen ? srclen : left;
    memcpy(dest + dstlen, src, ncpy);
    dest[dstlen + ncpy] = '\0';
    return dest;
}

