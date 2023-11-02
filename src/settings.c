/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2023 Eduard Permyakov 
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
#include "main.h"
#include "lib/public/mem.h"
#include "lib/public/khash.h"
#include "lib/public/pf_string.h"

#include <SDL.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#define STR2(x) #x
#define STR(x)  STR2(x)

#define SETT_FLAGS_NOPERSIST (1 << 0)

struct named_val{
    char name[SETT_NAME_LEN];
    struct sval val;
};

/* Additional private metadata associated 
 * with every setting */
struct setting_priv{
    uint32_t    flags;
    struct sval prev;
};

KHASH_MAP_INIT_STR(setting, struct setting)
KHASH_MAP_INIT_STR(settpriv, struct setting_priv)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(setting)  *s_settings_table;
static khash_t(settpriv) *s_priv_table;
static char               s_settings_filepath[512];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool sett_parse_line(char *line, int *out_prio, struct named_val *out_val)
{
    char *saveptr;
    char *token = pf_strtok_r(line, " \t", &saveptr);

    if(!sscanf(token, "%d", out_prio))
        goto fail;

    token = pf_strtok_r(NULL, " \t", &saveptr);
    if(strlen(token) > SETT_NAME_LEN-1)
        goto fail;
    strcpy(out_val->name, token);

    token = pf_strtok_r(NULL, " \t", &saveptr);
    if(!strcmp(token, "string")) {

        out_val->val.type = ST_TYPE_STRING;
        token = pf_strtok_r(NULL, " \t", &saveptr);
        if(!sscanf(token, "%" STR(SETT_NAME_LEN) "s\n", out_val->val.as_string))
            goto fail;

    }else if(!strcmp(token, "vec2")) {

        out_val->val.type = ST_TYPE_VEC2;
        token = token + strlen(token) + 1;
        if(!sscanf(token, "%f %f\n", 
            &out_val->val.as_vec2.x, &out_val->val.as_vec2.y))
            goto fail;

    }else if(!strcmp(token, "bool")) {

        out_val->val.type = ST_TYPE_BOOL;
        token = pf_strtok_r(NULL, " \t", &saveptr);
        int tmp;
        if(!sscanf(token, "%d\n", &tmp))
            goto fail;
        if(tmp != 0 && tmp != 1)
            goto fail;
        out_val->val.as_bool = tmp;

    }else if(!strcmp(token, "int")) {

        out_val->val.type = ST_TYPE_INT;
        token = pf_strtok_r(NULL, " \t", &saveptr);
        if(!sscanf(token, "%d\n", &out_val->val.as_int))
            goto fail;

    }else if(!strcmp(token, "float")) {

        out_val->val.type = ST_TYPE_FLOAT;
        token = pf_strtok_r(NULL, " \t", &saveptr);
        if(!sscanf(token, "%f\n", &out_val->val.as_float))
            goto fail;

    }else {
        goto fail;
    }

    return true; 
fail: 
    return false;
}

static bool sett_add_priv(const char *key, struct setting_priv priv)
{
    khiter_t k = kh_get(settpriv, s_priv_table, key);
    assert(k == kh_end(s_priv_table));

    int status;
    k = kh_put(settpriv, s_priv_table, key, &status);
    if(status == -1)
        return false;

    kh_value(s_priv_table, k) = priv;
    return true;
}

static void sett_set_priv(const char *name, struct setting_priv priv)
{
    khiter_t k = kh_get(settpriv, s_priv_table, name);
    assert(k != kh_end(s_priv_table));
    kh_value(s_priv_table, k) = priv;
}

static void sett_priv_clear(const char *name)
{
    khiter_t k = kh_get(settpriv, s_priv_table, name);
    assert(k != kh_end(s_priv_table));
    kh_value(s_priv_table, k).flags = 0;
}

static struct setting_priv sett_get_priv(const char *name)
{
    khiter_t k = kh_get(settpriv, s_priv_table, name);
    assert(k != kh_end(s_priv_table));
    return kh_value(s_priv_table, k);
}

static int compare_strings(const void* a, const void* b)
{
    const char *stra = *(const char **)a;
    const char *strb = *(const char **)b;
    return strcmp(stra, strb);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

ss_e Settings_Init(void)
{
    ASSERT_IN_MAIN_THREAD();

    s_settings_table = kh_init(setting);
    if(!s_settings_table)
        return SS_BADALLOC;

    s_priv_table = kh_init(settpriv);
    if(!s_priv_table) {
        kh_destroy(setting, s_settings_table);
        return SS_BADALLOC;
    }

    extern const char *g_basepath;
    strcpy(s_settings_filepath, g_basepath);
    strcat(s_settings_filepath, "/");
    strcat(s_settings_filepath, CONFIG_SETTINGS_FILENAME);

    return SS_OKAY;
}

void Settings_Shutdown(void)
{
    ASSERT_IN_MAIN_THREAD();

    const char *key;
    struct setting curr;

    kh_foreach(s_settings_table, key, curr, {
        (void)curr;
        free((char*)key);
    });
    kh_destroy(setting, s_settings_table);
    kh_destroy(settpriv, s_priv_table);
}

ss_e Settings_Create(struct setting sett)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(setting, s_settings_table, sett.name);
    struct sval saved;

    if((k != kh_end(s_settings_table))
    && (saved = kh_value(s_settings_table, k).val, true)
    && (sett.validate && sett.validate(&saved)) ){
    
        sett.val = saved;

    }else {

        const char *key = pf_strdup(sett.name);

        int put_status;
        k = kh_put(setting, s_settings_table, key, &put_status);

        if(put_status == -1)
            return SS_BADALLOC;
        if(!sett_add_priv(key, (struct setting_priv){0}))
            return SS_BADALLOC;
    }

    kh_value(s_settings_table, k) = sett;

    if(sett.commit)
        sett.commit(&sett.val);

    sett_priv_clear(sett.name);;
    return SS_OKAY;
}

ss_e Settings_Delete(const char *name)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(setting, s_settings_table, name);
    if(k == kh_end(s_settings_table))
        return SS_NO_SETTING;

    free((char*)kh_key(s_settings_table, k));
    kh_del(setting, s_settings_table, k);
    return SS_OKAY; 
}

ss_e Settings_Get(const char *name, struct sval *out)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(setting, s_settings_table, name);
    if(k == kh_end(s_settings_table))
        return SS_NO_SETTING;

    *out = kh_value(s_settings_table, k).val;
    return SS_OKAY;
}

ss_e Settings_Set(const char *name, const struct sval *new_val)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(setting, s_settings_table, name);
    if(k == kh_end(s_settings_table))
        return SS_NO_SETTING;

    struct setting *sett = &kh_value(s_settings_table, k);
    if(sett->validate && !sett->validate(new_val))
        return SS_INVALID_VAL;

    sett->val = *new_val;

    if(sett->commit)
        sett->commit(new_val);
        
    sett_priv_clear(sett->name);
    return SS_OKAY;
}

ss_e Settings_SetNoValidate(const char *name, const struct sval *new_val)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(setting, s_settings_table, name);
    if(k == kh_end(s_settings_table))
        return SS_NO_SETTING;

    struct setting *sett = &kh_value(s_settings_table, k);
    sett->val = *new_val;

    if(sett->commit)
        sett->commit(new_val);
        
    sett_priv_clear(sett->name);
    return SS_OKAY;
}

ss_e Settings_SetNoPersist(const char *name, const struct sval *new_val)
{
    ss_e ret;
    struct sval prev;
    struct setting_priv priv = sett_get_priv(name);

    if(priv.flags & SETT_FLAGS_NOPERSIST) {
        prev = priv.prev;
    }else{
        ret = Settings_Get(name, &prev);
        if(ret != SS_OKAY)
            return ret;
    }

    ret = Settings_Set(name, new_val);
    if(ret != SS_OKAY)
        return ret;

    khiter_t k = kh_get(setting, s_settings_table, name);
    struct setting *sett = &kh_value(s_settings_table, k);

    sett_set_priv(sett->name, (struct setting_priv){
        .flags = SETT_FLAGS_NOPERSIST,
        .prev = prev
    });
    return ret;
}

ss_e Settings_SaveToFile(void)
{
    ASSERT_IN_MAIN_THREAD();

    ss_e ret = SS_OKAY;

    SDL_RWops *stream = SDL_RWFromFile(s_settings_filepath, "w");
    if(!stream) {
        ret = SS_FILE_ACCESS;
        return false;
    }

    STALLOC(const char*, settings, kh_size(s_settings_table));
    size_t nsetts = 0;

    const char *name;
    kh_foreach(s_settings_table, name, (struct setting){0}, {
        settings[nsetts++] = name;
    });
    qsort(settings, nsetts, sizeof(const char*), compare_strings);

    for(int i = 0; i < nsetts; i++) {

        const char *name = settings[i];
        khiter_t k = kh_get(setting, s_settings_table, name);
        assert(k != kh_end(s_settings_table));
        struct setting curr = kh_value(s_settings_table, k);;

        char line[MAX_LINE_LEN];
        struct sval saveval = curr.val;

        struct setting_priv priv = sett_get_priv(name);
        if(priv.flags & SETT_FLAGS_NOPERSIST)
            saveval = priv.prev;

        switch(curr.val.type) {
        case ST_TYPE_STRING: 
            snprintf(line, sizeof(line), "%d %s %s %s\n",
                curr.prio, name, "string", saveval.as_string);
            break;
        case ST_TYPE_FLOAT:
            snprintf(line, sizeof(line), "%d %s %s %.6f\n",
                curr.prio, name, "float", saveval.as_float);
            break;
        case ST_TYPE_VEC2:
            snprintf(line, sizeof(line), "%d %s %s %.6f %.6f\n",
                curr.prio, name, "vec2", saveval.as_vec2.x, saveval.as_vec2.y);
            break;
        case ST_TYPE_BOOL:
            snprintf(line, sizeof(line), "%d %s %s %d\n", 
                curr.prio, name, "bool", saveval.as_bool);
            break;
        case ST_TYPE_INT:
            snprintf(line, sizeof(line), "%d %s %s %d\n", 
                curr.prio, name, "int", saveval.as_int);
            break;
        default: assert(0);
        }

        if(!stream->write(stream, line, strlen(line), 1)) {
            ret = SS_FILE_ACCESS;
            goto fail_write;
        }
    }

    ret = SS_OKAY;

fail_write:
    stream->close(stream);
    STFREE(settings);
    return ret;
}

ss_e Settings_LoadFromFile(void)
{
    ASSERT_IN_MAIN_THREAD();

    ss_e ret = SS_OKAY;

    SDL_RWops *stream = SDL_RWFromFile(s_settings_filepath, "r");
    if(!stream) {
        ret = SS_FILE_ACCESS;
        goto fail_stream;
    }

    for(int i = 0; i < SETT_MAX_PRIO; i++) {
    
        char line[MAX_LINE_LEN];
        while(AL_ReadLine(stream, line)) {

            int prio;
            struct named_val nv;

            if(!sett_parse_line(line, &prio, &nv)) {
                ret = SS_FILE_PARSING;
                goto fail_line;
            }

            if(prio < i)
                continue;

            khiter_t k;
            if((k = kh_get(setting, s_settings_table, nv.name)) != kh_end(s_settings_table)) {
            
                Settings_Set(nv.name, &nv.val);
            }else{
                struct setting sett = (struct setting){
                    .val = nv.val,
                    .prio = prio,
                    .validate = NULL,
                    .commit = NULL,
                };
                strcpy(sett.name, nv.name);
                Settings_Create(sett);
            }
        }

        SDL_RWseek(stream, 0, RW_SEEK_SET);
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
    ASSERT_IN_MAIN_THREAD();

    return s_settings_filepath;
}

