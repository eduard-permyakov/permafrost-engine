/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2021 Eduard Permyakov 
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

#include "audio.h"
#include "main.h"
#include "event.h"
#include "settings.h"
#include "lib/public/khash.h"
#include "lib/public/pf_string.h"
#include "lib/public/nk_file_browser.h"
#include "game/public/game.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <SDL_mixer.h>


#define MAX_CHANNELS    (16)
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))

struct audio_chan_ctx{
    bool music_finished;
    bool chan_finished[MAX_CHANNELS];
};

KHASH_MAP_INIT_STR(music, Mix_Music*)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static Mix_Music             *s_playing = NULL;
static khash_t(music)        *s_music;
static bool                   s_mute_on_focus_loss = false;
static int                    s_volume = MIX_MAX_VOLUME / 2;
static enum playback_mode     s_music_mode = MUSIC_MODE_PLAYLIST;
static struct audio_chan_ctx  s_chan_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void audio_index_directory(const char *dir)
{
    char absdir[NK_MAX_PATH_LEN];
    pf_snprintf(absdir, sizeof(absdir), "%s/%s", g_basepath, dir);

    size_t nfiles = 0;
    struct file *files = nk_file_list(absdir, &nfiles);

    for(int i = 0; i < nfiles; i++) {

        if(files[i].is_dir)
            continue;
        if(!pf_endswith(files[i].name, ".wav"))
            continue;

        char path[NK_MAX_PATH_LEN];
        pf_snprintf(path, sizeof(path), "%s/%s", absdir, files[i].name);

        Mix_Music *audio = Mix_LoadMUS(path);
        if(!audio)
            continue;

        char name[128];
        pf_strlcpy(name, files[i].name, sizeof(name));
        name[strlen(name) - strlen(".wav")] = '\0';

        const char *key = pf_strdup(name);
        if(!key) {
            Mix_FreeMusic(audio);
            continue;
        }

        int status;
        khiter_t k = kh_put(music, s_music, key, &status);
        if(status == -1) {
            free((void*)key);
            Mix_FreeMusic(audio);
            continue;
        }
        kh_value(s_music, k) = audio;
    }

    free(files);
}

static bool audio_volume_validate(const struct sval *val)
{
    if(val->type != ST_TYPE_FLOAT)
        return false;
    if(val->as_float < 0.0f || val->as_float > 1.0f)
        return false;
    return true;
}

static void audio_volume_commit(const struct sval *val)
{
    s_volume = MIX_MAX_VOLUME * val->as_float;
    Mix_VolumeMusic(s_volume);
}

static bool audio_bool_validate(const struct sval *val)
{
    return (val->type == ST_TYPE_BOOL);
}

static void audio_mute_focus_commit(const struct sval *val)
{
    s_mute_on_focus_loss = val->as_bool;
}

static bool audio_music_mode_validate(const struct sval *val)
{
    if(val->type != ST_TYPE_INT)
        return false;
    if(val->as_int < 0 || val->as_int > MUSIC_MODE_SHUFFLE)
        return false;
    return true;
}

static void audio_music_mode_commit(const struct sval *val)
{
    s_music_mode = val->as_int;
}

static void audio_create_settings(void)
{
    ss_e status;
    (void)status;

    status = Settings_Create((struct setting){
        .name = "pf.audio.music_volume",
        .val = (struct sval) {
            .type = ST_TYPE_FLOAT,
            .as_float = 0.5f
        },
        .prio = 0,
        .validate = audio_volume_validate,
        .commit = audio_volume_commit,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.audio.mute_on_focus_loss",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = s_mute_on_focus_loss
        },
        .prio = 0,
        .validate = audio_bool_validate,
        .commit = audio_mute_focus_commit,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.audio.music_playback_mode",
        .val = (struct sval) {
            .type = ST_TYPE_INT,
            .as_bool = s_music_mode
        },
        .prio = 0,
        .validate = audio_music_mode_validate,
        .commit = audio_music_mode_commit,
    });
    assert(status == SS_OKAY);
}

static void audio_window_event(void *user, void *arg)
{
    if(!s_mute_on_focus_loss)
        return;

    SDL_WindowEvent *event = arg;
    switch(event->event) {
    case SDL_WINDOWEVENT_FOCUS_LOST:
        Mix_VolumeMusic(0);
        break;
    case SDL_WINDOWEVENT_FOCUS_GAINED:
        Mix_VolumeMusic(s_volume);
        break;
    }
}

static const char *audio_music_name(Mix_Music *track)
{
    const char *name;
    Mix_Music *curr;

    kh_foreach(s_music, name, curr, {
        if(curr == track)
            return name;
    });
    return NULL;
}

static Mix_Music *audio_name_music(const char *name)
{
    khiter_t k = kh_get(music, s_music, name);
    if(k == kh_end(s_music))
        return NULL;
    return kh_value(s_music, k);
}

static void audio_next_music_track(void)
{
    const char *tracks[kh_size(s_music)];
    size_t ntracks = Audio_GetAllMusic(ARR_SIZE(tracks), tracks);
    const char *curr = audio_music_name(s_playing);

    int curr_idx = -1;
    for(int i = 0; i < ntracks; i++) {
        if(!strcmp(curr, tracks[i])) {
            curr_idx = i;
            break;
        }
    }
    assert(curr);
    assert(curr_idx >= 0 && curr_idx < ntracks);

    switch(s_music_mode) { 
    case MUSIC_MODE_LOOP:
        break;
    case MUSIC_MODE_PLAYLIST: {
        int next_idx = (curr_idx + 1) % ntracks;
        s_playing = audio_name_music(tracks[next_idx]);
        break;
    }
    case MUSIC_MODE_SHUFFLE: {
        if(ntracks > 0) {
            tracks[curr_idx] = tracks[--ntracks];
            int next_idx = rand() % ntracks;
            s_playing = audio_name_music(tracks[next_idx]);
        }
        break;
    }
    default: assert(0);
    }

    Mix_PlayMusic(s_playing, 1);
}

static void audio_music_finished(void)
{
    s_chan_ctx.music_finished = true;
}

static void audio_chan_finished(int channel)
{
    s_chan_ctx.chan_finished[channel] = true;
}

static void audio_on_update(void *user, void *event)
{
    if(s_chan_ctx.music_finished) {
        audio_next_music_track();
    }
    memset(&s_chan_ctx, 0, sizeof(s_chan_ctx));
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

bool Audio_Init(void)
{
    if(Mix_OpenAudio(MIX_DEFAULT_FREQUENCY, MIX_DEFAULT_FORMAT, 2, 2048) < 0)
        goto fail_open;

    if(NULL == (s_music = kh_init(music)))
        goto fail_music_table;

    audio_index_directory("assets/music");
    audio_create_settings();

    Mix_HookMusicFinished(audio_music_finished);
    Mix_ChannelFinished(audio_chan_finished);

    E_Global_Register(SDL_WINDOWEVENT, audio_window_event, NULL, G_ALL);
    E_Global_Register(EVENT_UPDATE_START, audio_on_update, NULL, G_ALL);
    return true;

fail_music_table:
    Mix_CloseAudio();
fail_open:
    return false;
}

void Audio_Shutdown(void)
{
    const char *name;
    Mix_Music *curr;

    kh_foreach(s_music, name, curr, {
        free((void*)name);
        Mix_FreeMusic(curr);
    });
    kh_destroy(music, s_music);

    E_Global_Unregister(SDL_WINDOWEVENT, audio_window_event);
    E_Global_Unregister(EVENT_UPDATE_START, audio_on_update);

    Mix_CloseAudio();
}

bool Audio_PlayMusic(const char *name)
{
    if(name == NULL) {
        if(s_playing) {
            Mix_FadeOutMusic(1000);
        }
        s_playing = NULL;
        return true;
    }

    khiter_t k = kh_get(music, s_music, name);
    if(k == kh_end(s_music))
        return false;

    Mix_HookMusicFinished(NULL);
    Mix_ChannelFinished(NULL);

    if(s_playing) {
        Mix_FadeOutMusic(1000);
    }

    s_playing = kh_value(s_music, k);
    Mix_FadeInMusic(s_playing, 1, 1000);

    Mix_HookMusicFinished(audio_music_finished);
    Mix_ChannelFinished(audio_chan_finished);

    return true;
}

size_t Audio_GetAllMusic(size_t maxout, const char *out[static maxout])
{
    const char *tracks[kh_size(s_music)];
    size_t ntracks = 0;

    const char *name;
    kh_foreach(s_music, name, (Mix_Music*){NULL}, {
        tracks[ntracks++] = name;
    });
    qsort(tracks, ntracks, sizeof(const char*), compare_strings);

    size_t ret = MIN(ntracks, maxout);
    memcpy(out, tracks, ret * sizeof(const char *));
    return ret;
}

const char *Audio_CurrMusic(void)
{
    if(!s_playing)
        return NULL;
    return audio_music_name(s_playing);
}

