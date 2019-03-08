/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019 Eduard Permyakov 
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


#include "settings.h"
#include "config.h"
#include "asset_load.h"
#ifndef __USE_POSIX
    #define __USE_POSIX /* strtok_r */
#endif
#include "lib/public/khash.h"
#include "lib/public/kvec.h"

#include <SDL.h>
#include <string.h>
#include <assert.h>

#define STR2(x) #x
#define STR(x)  STR2(x)


struct named_val{
    char name[SETT_NAME_LEN];
    struct sval val;
};

KHASH_MAP_INIT_STR(setting, struct setting)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(setting) *s_settings_table;
static char              s_settings_filepath[512];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool named_val_from_line(char *line, struct named_val *out)
{
    char *saveptr;
    char *token = strtok_r(line, " \t", &saveptr);

    if(strlen(token) > SETT_NAME_LEN-1)
        goto fail;
    strcpy(out->name, token);

    token = strtok_r(NULL, " \t", &saveptr);
    if(!strcmp(token, "string")) {

        out->val.type = ST_TYPE_STRING;
        token = strtok_r(NULL, " \t", &saveptr);
        if(!sscanf(token, "%" STR(SETT_NAME_LEN) "s\n", out->val.as_string))
            goto fail;

    }else if(!strcmp(token, "vec2")) {

        out->val.type = ST_TYPE_VEC2;
        token = token + strlen(token) + 1;
        if(!sscanf(token, "%f %f\n", 
            &out->val.as_vec2.x, &out->val.as_vec2.y))
            goto fail;

    }else if(!strcmp(token, "bool")) {

        out->val.type = ST_TYPE_BOOL;
        token = strtok_r(NULL, " \t", &saveptr);
        int tmp;
        if(!sscanf(token, "%d\n", &tmp))
            goto fail;
        if(tmp != 0 && tmp != 1)
            goto fail;
        out->val.as_bool = tmp;

    }else if(!strcmp(token, "float")) {

        out->val.type = ST_TYPE_FLOAT;
        token = strtok_r(NULL, " \t", &saveptr);
        if(!sscanf(token, "%f\n", &out->val.as_float))
            goto fail;

    }else if(!strcmp(token, "int")) {

        out->val.type = ST_TYPE_INT;
        token = strtok_r(NULL, " \t", &saveptr);
        if(!sscanf(token, "%d\n", &out->val.as_int))
            goto fail;

    }else {
        goto fail;
    }

    return true; 
fail: 
    return false;
}

/* A copy of the key string is stored in the 'struct setting' itself. Make the key 
 * (string pointer) be a pointer to that buffer in order to avoid allocating/storing the key 
 * strings separately. All keys must be patched in case rehashing took place. */
static void kh_update_str_keys(khash_t(setting) *table)
{
    for(khiter_t k = kh_begin(table); k != kh_end(table); k++) {
        if(!kh_exist(table, k)) continue;
        kh_key(table, k) = kh_value(table, k).name;
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

ss_e Settings_Init(void)
{
    s_settings_table = kh_init(setting);
    if(!s_settings_table)
        return SS_BADALLOC;

    extern const char *g_basepath;
    strcpy(s_settings_filepath, g_basepath);
    strcat(s_settings_filepath, "/");
    strcat(s_settings_filepath, CONFIG_SETTINGS_FILENAME);

    return SS_OKAY;
}

void Settings_Shutdown(void)
{
    kh_destroy(setting, s_settings_table);
}

ss_e Settings_Create(struct setting sett)
{
    khiter_t k;
    struct sval saved;

    if((k = kh_get(setting, s_settings_table, sett.name)) != kh_end(s_settings_table)
    && (saved = kh_value(s_settings_table, k).val, true)
    && (sett.validate && sett.validate(&saved)) ){
    
        sett.val = saved;

    }else {

        int put_status;
        k = kh_put(setting, s_settings_table, sett.name, &put_status);

        if(put_status == -1)
            return SS_BADALLOC;

        kh_update_str_keys(s_settings_table);
    }

    kh_value(s_settings_table, k) = sett;
    return SS_OKAY;
}

ss_e Settings_Get(const char *name, struct sval *out)
{
    khiter_t k = kh_get(setting, s_settings_table, name);
    if(k == kh_end(s_settings_table))
        return SS_NO_SETTING;

    *out = kh_value(s_settings_table, k).val;
    return SS_OKAY;
}

ss_e Settings_Set(const char *name, const struct sval *new_val)
{
    khiter_t k = kh_get(setting, s_settings_table, name);
    if(k == kh_end(s_settings_table))
        return SS_NO_SETTING;

    struct setting *sett = &kh_value(s_settings_table, k);
    if(sett->validate && !sett->validate(new_val))
        return SS_INVALID_VAL;

    sett->val = *new_val;

    if(sett->commit)
        sett->commit(new_val);
        
    return SS_OKAY;
}

ss_e Settings_SaveToFile(void)
{
    ss_e ret = SS_OKAY;

    SDL_RWops *stream = SDL_RWFromFile(s_settings_filepath, "w");
    if(!stream) {
        ret = SS_FILE_ACCESS;
        goto fail_stream;
    }

    const char *name;
    struct setting curr;
    kh_foreach(s_settings_table, name, curr, {
         
        char line[MAX_LINE_LEN];
        switch(curr.val.type) {
        case ST_TYPE_STRING: 
            snprintf(line, sizeof(line), "%s %s %s\n",
                name, "string", curr.val.as_string);
            break;
        case ST_TYPE_FLOAT:
            snprintf(line, sizeof(line), "%s %s %.6f\n",
                name, "float", curr.val.as_float);
            break;
        case ST_TYPE_VEC2:
            snprintf(line, sizeof(line), "%s %s %.6f %.6f\n",
                name, "vec2", curr.val.as_vec2.x, curr.val.as_vec2.y);
            break;
        case ST_TYPE_BOOL:
            snprintf(line, sizeof(line), "%s %s %d\n", 
                name, "bool", curr.val.as_bool);
            break;
        case ST_TYPE_INT:
            snprintf(line, sizeof(line), "%s %s %d\n", 
                name, "int", curr.val.as_int);
            break;
        default: assert(0);
        }

        if(!stream->write(stream, line, strlen(line), 1)) {
            ret = SS_FILE_ACCESS;
            goto fail_write;
        }
    });

    stream->close(stream);
    return SS_OKAY;

fail_write:
    stream->close(stream);
fail_stream:
    return ret;
}

ss_e Settings_LoadFromFile(void)
{
    ss_e ret = SS_OKAY;

    SDL_RWops *stream = SDL_RWFromFile(s_settings_filepath, "r");
    if(!stream) {
        ret = SS_FILE_ACCESS;
        goto fail_stream;
    }

    char line[MAX_LINE_LEN];
    while(AL_ReadLine(stream, line)) {

        struct named_val nv;
        if(!named_val_from_line(line, &nv)) {
            ret = SS_FILE_PARSING;
            goto fail_line;
        }

        khiter_t k;
        if((k = kh_get(setting, s_settings_table, nv.name)) != kh_end(s_settings_table)) {
        
            Settings_Set(nv.name, &nv.val);
        }else{
            struct setting sett = (struct setting){
                .val = nv.val,
                .validate = NULL,
                .commit = NULL,
            };
            strcpy(sett.name, nv.name);
            Settings_Create(sett);
        }
    }

    stream->close(stream);
    return ret;

fail_line:
    stream->close(stream);
fail_stream:
    return ret; 
}

const char *Settings_GetFile(void)
{
    return s_settings_filepath;
}

