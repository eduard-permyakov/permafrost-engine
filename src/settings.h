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

#ifndef SETTINGS_H
#define SETTINGS_H

#include "pf_math.h"

#include <stdbool.h>
#include <stdint.h>

#define SETT_NAME_LEN 128
#define SETT_MAX_PRIO 2

struct sval{
    enum{
        ST_TYPE_STRING,
        ST_TYPE_FLOAT,
        ST_TYPE_INT,
        ST_TYPE_BOOL,
        ST_TYPE_VEC2,
    }type;
    union{
        char   as_string[SETT_NAME_LEN];
        float  as_float;
        int    as_int;
        bool   as_bool;
        vec2_t as_vec2;
    };
};

struct setting{
    char        name[SETT_NAME_LEN];
    struct sval val;
    /* When reading the settings file, all settings with a lower priority number 
     * will be read before settings with a higher priority number. This allows 
     * creating some dependencies between settings. */
    int         prio;

    /* Called before a new setting value is committed - if 'validate'
     * returns false, the update is aborted. Can be NULL. */
    bool (*validate)(const struct sval *new_val);
    /* Called when the value of a setting updated. This can be used 
     * to actually apply engine settings (ex. changing the resolution). 
     * Can be NULL. */
    void (*commit)(const struct sval *new_val);
};

typedef enum settings_status{
    SS_OKAY = 0,
    SS_NO_SETTING,
    SS_INVALID_VAL,
    SS_FILE_ACCESS,
    SS_FILE_PARSING,
    SS_BADALLOC,
}ss_e;

ss_e Settings_Init(void);
void Settings_Shutdown(void);

/* If a setting with this name already exists, its' value is preserved and 
 * it is used to replace the provided default. */
ss_e Settings_Create(struct setting sett);
ss_e Settings_Delete(const char *name);

ss_e Settings_Get(const char *name, struct sval *out);
ss_e Settings_Set(const char *name, const struct sval *new_val);
ss_e Settings_SetNoValidate(const char *name, const struct sval *new_val);
/* The new value is not written to the settings file. Until it is overwritten 
 * with a persistent value, the old value will be written. */
ss_e Settings_SetNoPersist(const char *name, const struct sval *new_val);

ss_e Settings_SaveToFile(void);
ss_e Settings_LoadFromFile(void);
const char *Settings_GetFile(void);

#endif

