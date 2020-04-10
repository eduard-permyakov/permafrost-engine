/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#include "public/attr.h"
#include "public/pf_string.h"
#include "../asset_load.h"

#include <string.h>
#include <assert.h>


#define CHK_TRUE(_pred, _label) do{ if(!(_pred)) goto _label; }while(0)

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Attr_Parse(SDL_RWops *stream, struct attr *out, bool named)
{
    char line[MAX_LINE_LEN];
    READ_LINE(stream, line, fail);
    char *saveptr;
    char *token;

    if(named) {
        token = pf_strtok_r(line, " \t", &saveptr);
        CHK_TRUE(token, fail);

        strncpy(out->key, token, sizeof(out->key)); 
        out->key[sizeof(out->key)-1] = '\0';

        token = pf_strtok_r(NULL, " \t", &saveptr);
        CHK_TRUE(token, fail);
    }else{
        token = pf_strtok_r(line, " \t", &saveptr);
        CHK_TRUE(token, fail);
    }

    if(!strcmp(token, "string")) {

        out->type = TYPE_STRING;
        token = pf_strtok_r(NULL, "\n", &saveptr);
        CHK_TRUE(token, fail);
        pf_snprintf(out->val.as_string, sizeof(out->val.as_string), "%s", token);

    }else if(!strcmp(token, "quat")) {

        out->type = TYPE_QUAT;
        token = token + strlen(token) + 1;
        if(!sscanf(token, "%f %f %f %f", 
            &out->val.as_quat.x, &out->val.as_quat.y, &out->val.as_quat.z, &out->val.as_quat.w))
            goto fail;

    }else if(!strcmp(token, "vec2")) {

        out->type = TYPE_VEC2;
        token = token + strlen(token) + 1;
        if(!sscanf(token, "%f %f", 
            &out->val.as_vec2.x, &out->val.as_vec2.y))
            goto fail;

    }else if(!strcmp(token, "vec3")) {

        out->type = TYPE_VEC3;
        token = token + strlen(token) + 1;
        if(!sscanf(token, "%f %f %f", 
            &out->val.as_vec3.x, &out->val.as_vec3.y, &out->val.as_vec3.z))
            goto fail;

    }else if(!strcmp(token, "bool")) {

        out->type = TYPE_BOOL;
        token = pf_strtok_r(NULL, " \t", &saveptr);
        CHK_TRUE(token, fail);
        int tmp;
        if(!sscanf(token, "%d", &tmp))
            goto fail;
        if(tmp != 0 && tmp != 1)
            goto fail;
        out->val.as_bool = tmp;

    }else if(!strcmp(token, "float")) {

        out->type = TYPE_FLOAT;
        token = pf_strtok_r(NULL, " \t", &saveptr);
        CHK_TRUE(token, fail);
        if(!sscanf(token, "%f", &out->val.as_float))
            goto fail;

    }else if(!strcmp(token, "int")) {

        out->type = TYPE_INT;
        token = pf_strtok_r(NULL, " \t", &saveptr);
        CHK_TRUE(token, fail);
        if(!sscanf(token, "%d", &out->val.as_int))
            goto fail;

    }else {
        goto fail;
    }

    return true;

fail:
    return false;
}

bool Attr_Write(SDL_RWops *stream, const struct attr *in, const char name[static 0])
{
    if(name) {
        CHK_TRUE(SDL_RWwrite(stream, name, strlen(name), 1), fail);
        CHK_TRUE(SDL_RWwrite(stream, " ", 1, 1), fail);
    }

    switch(in->type) {
    case TYPE_STRING:
        CHK_TRUE(SDL_RWwrite(stream, "string ", strlen("string "), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, in->val.as_string, strlen(in->val.as_string), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, "\n", 1, 1), fail); 
        break;
    case TYPE_FLOAT: {
        char buff[64];
        pf_snprintf(buff, sizeof(buff), "%.6f", in->val.as_float);

        CHK_TRUE(SDL_RWwrite(stream, "float ", strlen("float "), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, buff, strlen(buff), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, "\n", 1, 1), fail); 
        break;
    }
    case TYPE_INT: {
        char buff[64];
        pf_snprintf(buff, sizeof(buff), "%d", in->val.as_int);

        CHK_TRUE(SDL_RWwrite(stream, "int ", strlen("int "), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, buff, strlen(buff), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, "\n", 1, 1), fail); 
        break;
    }
    case TYPE_VEC2: {
        char buff[64];
        pf_snprintf(buff, sizeof(buff), "%.6f %.6f", 
            in->val.as_vec2.x, in->val.as_vec2.y);

        CHK_TRUE(SDL_RWwrite(stream, "vec2 ", strlen("vec2 "), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, buff, strlen(buff), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, "\n", 1, 1), fail); 
        break;
    }
    case TYPE_VEC3: {
        char buff[64];
        pf_snprintf(buff, sizeof(buff), "%.6f %.6f %.6f", 
            in->val.as_vec3.x, in->val.as_vec3.y, in->val.as_vec3.z);

        CHK_TRUE(SDL_RWwrite(stream, "vec3 ", strlen("vec3 "), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, buff, strlen(buff), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, "\n", 1, 1), fail); 
        break;
    }
    case TYPE_QUAT: {
        char buff[64];
        pf_snprintf(buff, sizeof(buff), "%.6f %.6f %.6f %.6f", 
            in->val.as_quat.x, in->val.as_quat.y, in->val.as_quat.z, in->val.as_quat.w);

        CHK_TRUE(SDL_RWwrite(stream, "quat ", strlen("quat "), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, buff, strlen(buff), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, "\n", 1, 1), fail); 
        break;
    }
    case TYPE_BOOL: {
        char buff[64];
        pf_snprintf(buff, sizeof(buff), "%d", (int)in->val.as_bool);

        CHK_TRUE(SDL_RWwrite(stream, "bool ", strlen("bool "), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, buff, strlen(buff), 1), fail); 
        CHK_TRUE(SDL_RWwrite(stream, "\n", 1, 1), fail); 
        break;
    }
    default: assert(0);
    }

    return true;

fail:
    return false;
}

