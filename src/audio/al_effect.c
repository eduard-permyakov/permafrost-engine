/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2021-2023 Eduard Permyakov 
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

#include "al_effect.h"
#include "al_private.h"
#include "al_assert.h"
#include "../sched.h"
#include "../pf_math.h"
#include "../event.h"
#include "../entity.h"
#include "../perf.h"
#include "../settings.h"
#include "../game/public/game.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/vec.h"
#include "../lib/public/quadtree.h"
#include "../lib/public/pf_string.h"

#include <stdint.h>
#include <assert.h>
#include <stdlib.h>

#include <AL/al.h>
#include <AL/alc.h>


#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

struct al_effect{
    uint32_t uid;
    vec3_t   pos;
    uint32_t start_tick;
    uint32_t end_tick;
    ALuint   source;
};

VEC_TYPE(effect, struct al_effect)
VEC_IMPL(static inline, effect, struct al_effect)

QUADTREE_TYPE(effect, struct al_effect)
QUADTREE_PROTOTYPES(static, effect, struct al_effect)
QUADTREE_IMPL(static, effect, struct al_effect)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static vec_effect_t     s_effects;
static qt_effect_t      s_effect_tree;
static vec_effect_t     s_active;
static ALfloat          s_effect_volume = 5.0f;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool effects_equal(const struct al_effect *a, const struct al_effect *b)
{
    return (a->uid == b->uid);
}

static int compare_effects(const void* a, const void* b)
{
    struct al_effect *effa = (struct al_effect*)a;
    struct al_effect *effb = (struct al_effect*)b;
    return (effa->uid - effb->uid);
}

static void audio_update_active_set(vec_effect_t *active)
{
    vec_effect_reset(active);

    vec2_t center = Audio_ListenerPosXZ();
    struct al_effect potential[512];
    size_t npotential = qt_effect_inrange_circle(&s_effect_tree, center.x, center.z, HEARING_RANGE,
        potential, ARR_SIZE(potential));

    for(int i = 0; i < npotential; i++) {

        const struct al_effect *curr = &potential[i];
        vec2_t xz_pos = (vec2_t){curr->pos.x, curr->pos.z};
        if(!G_Fog_PlayerVisible(xz_pos))
            continue;

        if(SDL_TICKS_PASSED(SDL_GetTicks(), curr->end_tick))
            continue;

        vec_effect_push(active, potential[i]);
    }
}

static size_t audio_nsamples(ALuint buffer)
{
    ALint nbytes, channels, bits;
    alGetBufferi(buffer, AL_SIZE, &nbytes);
    alGetBufferi(buffer, AL_CHANNELS, &channels);
    alGetBufferi(buffer, AL_BITS, &bits);
    return nbytes * 8 / (channels * bits);
}

static void audio_advance(const struct al_effect *effect)
{
    ALint state;
    alGetSourcei(effect->source, AL_SOURCE_STATE, &state);
    assert(state == AL_INITIAL);

    uint32_t curr = SDL_GetTicks();
    uint32_t elapsed = curr - effect->start_tick;
    uint32_t total = effect->end_tick - effect->start_tick;

    ALint buffer;
    alGetSourcei(effect->source, AL_BUFFER, &buffer);
    const size_t nsamples = audio_nsamples(buffer);

    ALint sample_offset = (((float)elapsed) / total) * nsamples;
    sample_offset = MIN(sample_offset, nsamples-1);
    alSourcei(effect->source, AL_SAMPLE_OFFSET, sample_offset);
    AL_ASSERT_OK();
}

static void audio_active_difference(vec_effect_t *curr, vec_effect_t *prev,
                                    vec_effect_t *added, vec_effect_t *removed)
{
    vec_effect_reset(added);
    vec_effect_reset(removed);

    size_t n = vec_size(curr);
    size_t m = vec_size(prev);

    qsort(curr->array, n, sizeof(struct al_effect), compare_effects);
    qsort(prev->array, m, sizeof(struct al_effect), compare_effects);

    /* use the algorithm for finding the symmetric difference 
     * of two sorted arrays: */

    size_t nchanged = 0;
    int i = 0, j = 0;
    while(i < n && j < m) {

        if(curr->array[i].uid < prev->array[j].uid) {
            vec_effect_push(added, curr->array[i]);
            i++;
        }else if(prev->array[j].uid < curr->array[i].uid) {
            vec_effect_push(removed, prev->array[j]);
            j++;
        }else{
            i++;
            j++;
        }
    }

    while(i < n) {
        vec_effect_push(added, curr->array[i]);
        i++;
    }

    while(j < m) {
        vec_effect_push(removed, prev->array[j]);
        j++;
    }
}

static void on_update_start(void *user, void *event)
{
    PERF_PUSH("audio_effect::on_update_start");

    vec_effect_t prev, added, removed;
    vec_effect_init(&prev);
    vec_effect_init(&added);
    vec_effect_init(&removed);

    vec_effect_copy(&prev, &s_active);

    audio_update_active_set(&s_active);
    audio_active_difference(&s_active, &prev, &added, &removed);

    for(int i = 0; i < vec_size(&added); i++) {
        struct al_effect *curr = &added.array[i];
        audio_advance(curr);
        alSourcef(curr->source, AL_GAIN, s_effect_volume);
        alSourcePlay(curr->source);
        /* We couldn't play the source - possibly due to hitting 
         * the maximum source limit on our hardware. Keep huffing 
         * and puffing along. There unfortunaly doesn't seem to be
         * a foolproof, portable way to query this limit in OpenAL.
         */
        if(alGetError() != AL_NO_ERROR) {
            alSourceStop(curr->source);
            int idx = vec_effect_indexof(&s_active, *curr, effects_equal);
            assert(idx >= 0);
            vec_effect_del(&s_active, idx);
        }
    }

    for(int i = 0; i < vec_size(&removed); i++) {
        struct al_effect *curr = &removed.array[i];
        alSourceStop(curr->source);
        alSourceRewind(curr->source);
    }

    vec_effect_destroy(&prev);
    vec_effect_destroy(&added);
    vec_effect_destroy(&removed);

    AL_ASSERT_OK();
    PERF_POP();
}

static void on_new_map(void *user, void *event)
{
    const struct map *map = G_GetPrevTickMap();
    if(!map)
        return;

    Audio_EffectClearState();
    assert(vec_size(&s_active) == 0);
    assert(vec_size(&s_effects) == 0);
    assert(s_effect_tree.nrecs == 0);

    struct map_resolution res;
    M_GetResolution(map, &res);

    vec3_t center = M_GetCenterPos(map);

    float xmin = center.x - (res.tile_w * res.chunk_w * X_COORDS_PER_TILE) / 2.0f;
    float xmax = center.x + (res.tile_w * res.chunk_w * X_COORDS_PER_TILE) / 2.0f;
    float zmin = center.z - (res.tile_h * res.chunk_h * Z_COORDS_PER_TILE) / 2.0f;
    float zmax = center.z + (res.tile_h * res.chunk_h * Z_COORDS_PER_TILE) / 2.0f;

    qt_effect_destroy(&s_effect_tree);
    qt_effect_init(&s_effect_tree, xmin, xmax, zmin, zmax, effects_equal);
    qt_effect_reserve(&s_effect_tree, 4096);
}

static void on_1hz_tick(void *user, void *event)
{
    PERF_PUSH("audio_effect::on_1hz_tick");
    uint32_t now = SDL_GetTicks();

    for(int i = vec_size(&s_effects)-1; i >= 0; i--) {
        struct al_effect curr = s_effects.array[i];
        if(!SDL_TICKS_PASSED(now, curr.end_tick))
            continue;

        int idx = vec_effect_indexof(&s_active, curr, effects_equal);
        if(idx != -1)
            continue;

        alDeleteSources(1, &curr.source);
        vec_effect_del(&s_effects, i);
        qt_effect_delete(&s_effect_tree, curr.pos.x, curr.pos.z, curr);
    }

    assert(s_effect_tree.nrecs == vec_size(&s_effects));
    AL_ASSERT_OK();
    PERF_POP();
}

static bool audio_volume_validate(const struct sval *val)
{
    if(val->type != ST_TYPE_FLOAT)
        return false;
    if(val->as_float < 0.0f || val->as_float > 10.0f)
        return false;
    return true;
}

static void audio_effect_volume_commit(const struct sval *val)
{
    s_effect_volume = val->as_float;

    for(int i = 0; i < vec_size(&s_active); i++) {
        const struct al_effect *curr = &vec_AT(&s_active, i);
        alSourcef(curr->source, AL_GAIN, s_effect_volume);
    }
    Audio_SetForegroundEffectVolume(s_effect_volume);
}

static void audio_create_settings(void)
{
    ss_e status;
    (void)status;

    status = Settings_Create((struct setting){
        .name = "pf.audio.effect_volume",
        .val = (struct sval) {
            .type = ST_TYPE_FLOAT,
            .as_float = s_effect_volume
        },
        .prio = 0,
        .validate = audio_volume_validate,
        .commit = audio_effect_volume_commit,
    });
    assert(status == SS_OKAY);
}

static void audio_make_effect_source(ALuint *out, vec3_t pos, ALint buffer)
{
    ALuint source;
    alGenSources(1, &source);

    alSourcef(source,  AL_PITCH, 1);
    alSourcef(source,  AL_GAIN, s_effect_volume);
    alSourcei(source,  AL_BUFFER, buffer);
    alSource3f(source, AL_POSITION, pos.x, pos.y, pos.z);
    alSource3f(source, AL_VELOCITY, 0, 0, 0);
    alSourcei(source,  AL_LOOPING, AL_FALSE);
    alSourcei(source,  AL_SOURCE_RELATIVE, AL_FALSE);
    alSourcef(source,  AL_MAX_DISTANCE, HEARING_RANGE * 2.0f);
    alSourcef(source,  AL_ROLLOFF_FACTOR, 0.5f);

    *out = source;
    AL_ASSERT_OK();
}

static bool audio_save_effect(SDL_RWops *stream, const struct al_effect *effect)
{
    ALint buffer;
    alGetSourcei(effect->source, AL_BUFFER, &buffer);

    const char *name = Audio_GetEffectName(buffer);
    struct attr name_attr = (struct attr){ .type = TYPE_STRING, };
    pf_strlcpy(name_attr.val.as_string, name, sizeof(name_attr.val.as_string));
    CHK_TRUE_RET(Attr_Write(stream, &name_attr, "name"));

    struct attr uid_attr = (struct attr){
        .type = TYPE_INT, 
        .val.as_int = effect->uid
    };
    CHK_TRUE_RET(Attr_Write(stream, &uid_attr, "uid"));

    struct attr pos_attr = (struct attr){
        .type = TYPE_VEC3, 
        .val.as_vec3 = effect->pos
    };
    CHK_TRUE_RET(Attr_Write(stream, &pos_attr, "pos"));

    ALint offset;
    alGetSourcei(effect->source, AL_SAMPLE_OFFSET, &offset);
    struct attr offset_attr = (struct attr){
        .type = TYPE_INT, 
        .val.as_int = offset
    };
    CHK_TRUE_RET(Attr_Write(stream, &offset_attr, "offset"));

    ALint state;
    alGetSourcei(effect->source, AL_SOURCE_STATE, &state);
    struct attr state_attr = (struct attr){
        .type = TYPE_INT, 
        .val.as_int = state
    };
    CHK_TRUE_RET(Attr_Write(stream, &state_attr, "state"));

    return true;
}

static bool audio_load_effect(SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_STRING);
    char name[sizeof(attr.val.as_string)];
    pf_strlcpy(name, attr.val.as_string, sizeof(name));

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    uint32_t uid = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    vec3_t pos = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t offset = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    ALint state = attr.val.as_int;

    ALint buffer;
    if(Audio_GetEffectBuffer(name, &buffer)) {

        ALuint source;
        audio_make_effect_source(&source, pos, buffer);
        alSourcei(source, AL_SAMPLE_OFFSET, offset);

        if(alGetError() != AL_NO_ERROR) {
            alDeleteSources(1, &source);
            return true;
        }

        switch(state){
        case AL_INITIAL:
            break;
        case AL_PLAYING:
            alSourcePlay(source);
            break;
        case AL_PAUSED:
            alSourcePlay(source);
            alSourcePause(source);
            break;
        case AL_STOPPED:
            alSourcePlay(source);
            alSourceStop(source);
            break;
        }

        if(alGetError() != AL_NO_ERROR) {
            alSourceStop(source);
            alDeleteSources(1, &source);
            return true;
        }

        const size_t nsamples = audio_nsamples(buffer);
        const float duration = Audio_BufferDuration(buffer);
        const float elapsed = (((float)offset) / nsamples) * duration;

        uint32_t start_tick = SDL_GetTicks() - elapsed * 1000;
        uint32_t end_tick = start_tick + duration * 1000;

        struct al_effect effect = (struct al_effect) {
            .uid = uid,
            .pos = pos,
            .start_tick = start_tick,
            .end_tick = end_tick,
            .source = source
        };
        vec_effect_push(&s_effects, effect);
        qt_effect_insert(&s_effect_tree, pos.x, pos.z, effect);

        if(state == AL_PLAYING || state == AL_PAUSED) {
            vec_effect_push(&s_active, effect);
        }
    }
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Audio_Effect_Init(void)
{
    vec_effect_init(&s_effects);
    if(!vec_effect_resize(&s_effects, 4096))
        goto fail_vec;

    /* When no map is loaded, pick some arbitrary bounds for the 
     * quadtree. They will be updated for the map size when it is 
     * loaded. 
     */
    const float min = -1024.0;
    const float max = 1024.0;

    qt_effect_init(&s_effect_tree, min, max, min, max, effects_equal);
    if(!qt_effect_reserve(&s_effect_tree, 4096))
        goto fail_tree;

    vec_effect_init(&s_active);
    if(!vec_effect_resize(&s_effects, 64))
        goto fail_active;

    audio_create_settings();
    E_Global_Register(EVENT_NEW_GAME, on_new_map, NULL, G_ALL);
    E_Global_Register(EVENT_SESSION_LOADED, on_new_map, NULL, G_ALL);
    E_Global_Register(EVENT_SESSION_POPPED, on_new_map, NULL, G_ALL);
    E_Global_Register(EVENT_UPDATE_START, on_update_start, NULL, G_ALL);
    E_Global_Register(EVENT_1HZ_TICK, on_1hz_tick, NULL, G_RUNNING);
    return true;

fail_active:
    qt_effect_destroy(&s_effect_tree);
fail_tree:
    vec_effect_destroy(&s_effects);
fail_vec:
    return false;
}

void Audio_Effect_Shutdown(void)
{
    for(int i = 0; i < vec_size(&s_active); i++) {
        struct al_effect *curr = &s_active.array[i];
        alSourceStop(curr->source);
    }

    for(int i = 0; i < vec_size(&s_effects); i++) {
        struct al_effect *curr = &s_effects.array[i];
        alDeleteSources(1, &curr->source);
    }

    E_Global_Unregister(EVENT_NEW_GAME, on_new_map);
    E_Global_Unregister(EVENT_SESSION_LOADED, on_new_map);
    E_Global_Unregister(EVENT_SESSION_POPPED, on_new_map);
    E_Global_Unregister(EVENT_UPDATE_START, on_update_start);
    E_Global_Unregister(EVENT_1HZ_TICK, on_1hz_tick);

    vec_effect_destroy(&s_active);
    vec_effect_destroy(&s_effects);
    qt_effect_destroy(&s_effect_tree);
}

bool Audio_Effect_Add(vec3_t pos, const char *track)
{
    ALint buffer;
    if(!Audio_GetEffectBuffer(track, &buffer))
        return false;

    ALuint source;
    audio_make_effect_source(&source, pos, buffer);

    uint32_t start_tick = SDL_GetTicks();
    float duration = Audio_BufferDuration(buffer);
    uint32_t end_tick = start_tick + duration * 1000;

    struct al_effect effect = (struct al_effect) {
        .uid = Entity_NewUID(),
        .pos = pos,
        .start_tick = start_tick,
        .end_tick = end_tick,
        .source = source
    };

    vec_effect_push(&s_effects, effect);
    qt_effect_insert(&s_effect_tree, pos.x, pos.z, effect);
    return true;
}

float Audio_EffectVolume(void)
{
    return s_effect_volume;
}

void Audio_EffectPause(void)
{
    for(int i = 0; i < vec_size(&s_active); i++) {
        struct al_effect *curr = &s_active.array[i];
        alSourcePause(curr->source);
    }
}

void Audio_EffectResume(uint32_t dt)
{
    for(int i = 0; i < vec_size(&s_active); i++) {
        struct al_effect *curr = &s_active.array[i];
        alSourcePlay(curr->source);
        curr->start_tick += dt;
        curr->end_tick += dt;
    }
}

void Audio_EffectClearState(void)
{
    for(int i = 0; i < vec_size(&s_active); i++) {
        struct al_effect *curr = &s_active.array[i];
        alSourceStop(curr->source);
    }
    for(int i = 0; i < vec_size(&s_effects); i++) {
        struct al_effect *curr = &s_effects.array[i];
        alDeleteSources(1, &curr->source);
    }
    vec_effect_reset(&s_active);
    vec_effect_reset(&s_effects);
    qt_effect_clear(&s_effect_tree);
}

bool Audio_EffectSaveState(struct SDL_RWops *stream)
{
    struct attr num_effects_attr = (struct attr){
        .type = TYPE_INT, 
        .val.as_int = vec_size(&s_effects)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_effects_attr, "num_effects"));
    Sched_TryYield();

    for(int i = 0; i < vec_size(&s_effects); i++) {
        const struct al_effect *curr = &vec_AT(&s_effects, i);
        if(!audio_save_effect(stream, curr))
            return false;
        Sched_TryYield();
    }
    return true;
}

bool Audio_EffectLoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t num_effects = attr.val.as_int;

    for(int i = 0; i < num_effects; i++) {
        if(!audio_load_effect(stream))
            return false;
        Sched_TryYield();
    }

    return true;
}

