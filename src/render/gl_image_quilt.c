/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2024 Eduard Permyakov 
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

/* Implementation of the Image Quilting algorithm as described 
 * in the paper "Image Quilting for Texture Synthesis and Transfer"
 * by Alexei A. Efros and William T. Freeman.
 */

#define MEM_FILE_SYS MEM_SYS_RENDER
#define MEM_FILE_SUB MEM_SUB_RENDER_GL_IMAGE_QUILT

#include "../lib/public/windows.h"
#include "gl_image_quilt.h"
#include "gl_texture.h"
#include "gl_assert.h"
#include "../main.h"
#include "../pf_math.h"
#include "../lib/public/stb_image.h"
#include "../lib/public/stb_image_resize.h"
#include "../lib/public/vec.h"
#include "../lib/public/khash.h"
#include "../lib/public/pf_string.h"

#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <time.h>

#include "../lib/public/mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_IMAGE_QUILT)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_IMAGE_QUILT)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_IMAGE_QUILT)

#define BLOCK_DIM           (65)
#define OVERLAP_DIM         (10)
#define TILE_DIM            (130)
#define OVERLAP_TOLERANCE   (0.1)
#define CANDIDATE_MIN_SEP   (BLOCK_DIM / 2)
#define PHASE_TOL           (1)
#define PERIOD_MIN_LAG      (8)
#define PERIOD_MIN_CORR     (0.5)

#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))

enum constraint{
    CONSTRAIN_LEFT,
    CONSTRAIN_TOP,
    CONSTRAIN_TOP_LEFT
};

enum direction{
    DIRECTION_HORIZONTAL,
    DIRECTION_VERTICAL
};

enum tile_patch{
    TOP_LEFT,
    TOP_RIGHT,
    BOT_LEFT,
    BOT_RIGHT
};

struct image{
    unsigned char *data;
    size_t         width;
    size_t         height;
    size_t         nr_channels;
};

struct cost_image{
    int   *data;
    size_t width;
    size_t height;
};

struct image_view{
    int x, y;
    int width, height;
};

struct image_patch{
    char *pixels;
};

struct image_patch_mask{
    char bits[BLOCK_DIM][BLOCK_DIM];
};

struct seam_mask{
    char *bits;
};

struct image_tile{
    char *pixels;
};

struct diamond_patch{
    int width, height;
    char *pixels;
};

struct coord{
    int r, c;
};

struct phase_lock{
    int px, py;   /* source period per axis; 0 when that axis is aperiodic */
    int rx, ry;   /* phase of the first block: its position modulo the period */
};

VEC_TYPE(coord, struct coord)
VEC_IMPL(static inline, coord, struct coord)

KHASH_MAP_INIT_INT64(coord, int)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static unsigned int s_seed;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int pf_rand(unsigned *seed)
{
#ifdef _WIN32
    unsigned int ret = 0;
    rand_s(&ret);
    return (ret % RAND_MAX);
#else
    return rand_r(seed);
#endif
}

static uint64_t coord_to_key(struct coord coord)
{
    return ((uint64_t)coord.r) << 32
         | ((uint64_t)coord.c) << 0;
}

struct coord key_to_coord(uint64_t key)
{
    uint64_t r = key >> 32;
    uint64_t c = (key & 0xffffffff);
    return (struct coord){r, c};
}

static bool dump_ppm(const char *filename, const unsigned char *data, 
                     int nr_channels, int width, int height)
{
    FILE *file = fopen(filename, "wb");
    if(!file)
        return false;

    fprintf(file, "P6\n%d %d\n%d\n", width, height, 255);
    for (int i = 0; i < height; i++) {
    for (int j = 0; j < width; j++) {

        unsigned char color[3];
        color[0] = data[nr_channels * i * width + nr_channels * j + 0];
        color[1] = data[nr_channels * i * width + nr_channels * j + 1];
        color[2] = data[nr_channels * i * width + nr_channels * j + 2];
        fwrite(color, 1, 3, file);
    }}

    fclose(file);
    return true;
}

static bool dump_view_ppm(const char *filename, struct image image, 
                          struct image_view view)
{
    FILE *file = fopen(filename, "wb");
    if(!file)
        return false;

    fprintf(file, "P6\n%d %d\n%d\n", view.width, view.height, 255);
    for (int i = 0; i < view.height; i++) {
    for (int j = 0; j < view.width; j++) {

        size_t row_offset = image.nr_channels * (i + view.y) * image.width;
        size_t col_offset = image.nr_channels * (j + view.x);
        unsigned char color[3];
        color[0] = image.data[row_offset + col_offset + 0]; 
        color[1] = image.data[row_offset + col_offset + 1];
        color[2] = image.data[row_offset + col_offset + 2];
        fwrite(color, 1, 3, file);
    }}

    fclose(file);
    return true;
}

static bool dump_patch(const char *filename, const struct image image, 
                       const struct image_patch *patch)
{
    return dump_ppm(filename, (const unsigned char*)patch->pixels, 
        image.nr_channels, BLOCK_DIM, BLOCK_DIM);
}

static bool dump_tile(const char *filename, const struct image image, 
                      const struct image_tile *tile)
{
    return dump_ppm(filename, (const unsigned char*)tile->pixels, 
        image.nr_channels, TILE_DIM, TILE_DIM);
}

static bool dump_mask_ppm(const char *filename, const struct image_patch_mask *mask)
{
    FILE *file = fopen(filename, "wb");
    if(!file)
        return false;

    fprintf(file, "P6\n%d %d\n%d\n", BLOCK_DIM, BLOCK_DIM, 255);
    for (int i = 0; i < BLOCK_DIM; i++) {
    for (int j = 0; j < BLOCK_DIM; j++) {

        unsigned char color[3] = {0};
        if(mask->bits[i][j]) {
            color[0] = 255;
            color[1] = 255;
            color[2] = 255;
        }
        fwrite(color, 1, 3, file);
    }}

    fclose(file);
    return true;
}

static bool dump_seam_mask_ppm(const char *filename, const struct seam_mask *mask,
                               size_t width, size_t height)
{
    FILE *file = fopen(filename, "wb");
    if(!file)
        return false;

    fprintf(file, "P6\n%d %d\n%d\n", (int)width, (int)height, 255);
    for (int i = 0; i < height; i++) {
    for (int j = 0; j < width; j++) {

        unsigned char color[3] = {0};
        size_t offset = i * width + j;
        if(mask->bits[offset]) {
            color[0] = 255;
            color[1] = 255;
            color[2] = 255;
        }
        fwrite(color, 1, 3, file);
    }}

    fclose(file);
    return true;
}

static bool dump_cost_image_ppm(const char *filename, const struct cost_image *cost_image)
{
    FILE *file = fopen(filename, "wb");
    if(!file)
        return false;

    int min = INT_MAX;
    int max = INT_MIN;

    for(int r = 0; r < cost_image->height; r++) {
    for(int c = 0; c < cost_image->width; c++) {
        size_t offset = r * cost_image->width + c;
        min = MIN(min, cost_image->data[offset]);
        max = MAX(max, cost_image->data[offset]);
    }}
    int range = max - min;

    fprintf(file, "P6\n%d %d\n%d\n", (int)cost_image->width, (int)cost_image->height, 255);
    for(int r = 0; r < cost_image->height; r++) {
    for(int c = 0; c < cost_image->width; c++) {

        size_t offset = r * cost_image->width + c;
        int value = cost_image->data[offset];
        float percent = ((float)(value - min)) / range;
        assert(percent >= 0.0f && percent <= 1.0f);
        unsigned char color[3] = {
            percent * 255,
            percent * 255,
            percent * 255
        };
        fwrite(color, 1, 3, file);
    }}

    fclose(file);
    return true;
}

static bool load_image(const char *source, struct image *out)
{
    int width, height, nr_channels;
    unsigned char *data = stbi_load(source, &width, &height, &nr_channels, 0);
    if(!data)
        return false;

    *out = (struct image){
        .data = data,
        .width = width,
        .height = height,
        .nr_channels = nr_channels
    };
    return true;
}

static struct image_view random_block(struct image image)
{
    int minx = OVERLAP_DIM, maxx = image.width - (BLOCK_DIM + OVERLAP_DIM);
    int miny = OVERLAP_DIM, maxy = image.height - (BLOCK_DIM + OVERLAP_DIM);
    int x = pf_rand(&s_seed) % (maxx + 1 - minx) + minx;
    int y = pf_rand(&s_seed) % (maxy + 1 - miny) + miny;
    return (struct image_view){x, y, BLOCK_DIM, BLOCK_DIM};
}

static bool copy_view(const struct image image, struct image_view view, struct image_patch *patch)
{
    size_t patch_size = image.nr_channels * view.width * view.height;
    patch->pixels = PF_MALLOC(patch_size);
    if(!patch->pixels)
        return false;

    for(int r = 0; r < view.height; r++) {
        size_t row_offset = (view.y + r) * image.width * image.nr_channels;
        size_t col_offset = view.x * image.nr_channels;
        size_t bytes_per_row = image.nr_channels * view.width;
        const unsigned char *src = image.data + row_offset + col_offset;
        char *dst = patch->pixels + (r * bytes_per_row);
        memcpy(dst, src, bytes_per_row);
    }
    return true;
}

static void copy_left(const struct image image, struct image_view view,
                      struct image_patch *template)
{
    for(int r = 0; r < view.height; r++) {
        size_t row_offset = (view.y + r) * image.width * image.nr_channels;
        size_t col_offset = (view.x + view.width) * image.nr_channels;
        size_t bytes_per_row = image.nr_channels * view.width;
        const unsigned char *src = image.data + row_offset + col_offset;
        char *dst = template->pixels + (r * bytes_per_row);
        memcpy(dst, src, OVERLAP_DIM * image.nr_channels);
    }
}

static void copy_top(const struct image image, struct image_view view,
                     struct image_patch *template, bool diagonal)
{
    for(int r = 0; r < OVERLAP_DIM; r++) {
        size_t row_offset = (view.y + view.height + r) * image.width * image.nr_channels;
        size_t col_offset = view.x * image.nr_channels;
        size_t bytes_per_row = image.nr_channels * view.width;
        const unsigned char *src = image.data + row_offset + col_offset;
        char *dst = template->pixels + (r * bytes_per_row);
        size_t bytes_copied = bytes_per_row;
        if(diagonal) {
            dst += r * image.nr_channels;
            src += r * image.nr_channels;
            bytes_copied -= r * image.nr_channels;
        }
        memcpy(dst, src, bytes_copied);
    }
}

static bool copy_overlap(const struct image image, struct image_view *views, 
                         enum constraint constraint, struct image_patch *template)
{
    size_t patch_size = image.nr_channels * views[0].width * views[0].height;
    template->pixels = PF_MALLOC(patch_size);
    if(!template->pixels)
        return false;

    memset(template->pixels, 0, views[0].width * views[0].height * image.nr_channels);
    switch(constraint) {
    case CONSTRAIN_LEFT:
        copy_left(image, views[0], template);
        break;
    case CONSTRAIN_TOP:
        copy_top(image, views[0], template, false);
        break;
    case CONSTRAIN_TOP_LEFT: {
        copy_left(image, views[0], template);
        copy_top(image, views[1], template, true);
        break;
    }
    default: assert(0);
    }
    return true;
}

static void create_mask(enum constraint constraint, struct image_patch_mask *mask)
{
    memset(mask->bits, 0, sizeof(mask->bits));
    switch(constraint) {
    case CONSTRAIN_LEFT: {
        for(int r = 0; r < BLOCK_DIM; r++) {
        for(int c = 0; c < OVERLAP_DIM; c++) {
            mask->bits[r][c] = 1;
        }}
        break;
    }
    case CONSTRAIN_TOP:
        for(int r = 0; r < OVERLAP_DIM; r++) {
        for(int c = 0; c < BLOCK_DIM; c++) {
            mask->bits[r][c] = 1;
        }}
        break;
    case CONSTRAIN_TOP_LEFT: {
        for(int r = 0; r < BLOCK_DIM; r++) {
        for(int c = 0; c < OVERLAP_DIM; c++) {
            mask->bits[r][c] = 1;
        }}
        for(int r = 0; r < OVERLAP_DIM; r++) {
        for(int c = 0; c < BLOCK_DIM; c++) {
            mask->bits[r][c] = 1;
        }}
        break;
    }
    default: assert(0);
    }
}

static int compute_ssd(struct image image, struct image_view view,
                       const struct image_patch *template, 
                       const struct image_patch_mask *mask)
{
    int ssd = 0;
    for(int r = 0; r < view.height; r++) {
    for(int c = 0; c < view.width; c++) {

        if(!mask->bits[r][c])
            continue;

        int img_x = view.x + c;
        int img_y = view.y + r;

        size_t img_row_width = image.width * image.nr_channels;
        size_t template_row_width = view.width * image.nr_channels;

        size_t img_x_offset = img_x * image.nr_channels;
        size_t template_x_offset = c * image.nr_channels;

        unsigned char vector_a[32];
        memcpy(vector_a, &image.data[img_y * img_row_width + img_x_offset], image.nr_channels);

        unsigned char vector_b[32];
        memcpy(vector_b, &template->pixels[r * template_row_width + template_x_offset], 
            image.nr_channels);

        int diff_magnitude_squared = 0;
        for(int i = 0; i < image.nr_channels; i++) {
            diff_magnitude_squared += pow(vector_a[i] - vector_b[i], 2);
        }
        ssd += abs(diff_magnitude_squared);
    }}
    return ssd;
}

/* ssd_patch performs template matching with the overlapping region, computing the 
 * cost of sampling each patch, based on the sum of squared differences (SSD) of the 
 * overlapping regions of the existing and sampled patch. 
 *
 * The template is the patch in the current output image that is to be filled in 
 * (many pixel values will be 0 because they are not filled in yet). The mask has the 
 * same size as the patch template and has values of 1 in the overlapping region and 
 * values of 0 elsewhere. The output is an image in which the output is the overlap 
 * cost (SSD) of choosing a sample centered at each pixel.
 */
static void ssd_patch(struct image image, struct cost_image out_cost_image, 
                      struct image_patch *template, struct image_patch_mask *mask)
{
    int offx = OVERLAP_DIM;
    int offy = OVERLAP_DIM;

    for(int i = 0; i < out_cost_image.width; i++) {
    for(int j = 0; j < out_cost_image.height; j++) {
        struct image_view view = (struct image_view){
            .x = offx + i,
            .y = offy + j,
            .width = BLOCK_DIM,
            .height = BLOCK_DIM
        };
        out_cost_image.data[out_cost_image.width * j + i] = 
            compute_ssd(image, view, template, mask);
    }}
}

static int dominant_period(const double *profile, int n)
{
    double mean = 0;
    for(int i = 0; i < n; i++)
        mean += profile[i];
    mean /= n;

    double energy = 0;
    for(int i = 0; i < n; i++) {
        double d = profile[i] - mean;
        energy += d * d;
    }
    if(energy == 0)
        return 0;

    /* Return the lag of the strongest autocorrelation peak above the threshold,
     * or 0 when the profile has no clear period.
     */
    int best = 0;
    double best_corr = PERIOD_MIN_CORR;
    for(int k = PERIOD_MIN_LAG; k < n/2; k++) {
        double sum = 0;
        for(int i = 0; i + k < n; i++)
            sum += (profile[i] - mean) * (profile[i+k] - mean);
        sum /= energy;
        if(sum > best_corr) {
            best_corr = sum;
            best = k;
        }
    }
    return best;
}

static void estimate_period(struct image image, int *out_px, int *out_py)
{
    *out_px = 0;
    *out_py = 0;

    double *rows = PF_MALLOC(image.height * sizeof(double));
    double *cols = PF_MALLOC(image.width * sizeof(double));
    if(!rows || !cols) {
        PF_FREE(rows);
        PF_FREE(cols);
        return;
    }

    /* Average brightness profiles along each axis; the autocorrelation of each
     * reveals the dominant repeat, such as the brick course height.
     */
    for(int y = 0; y < image.height; y++) {
        double sum = 0;
        for(int x = 0; x < image.width; x++)
        for(int k = 0; k < image.nr_channels; k++)
            sum += image.data[(y * image.width + x) * image.nr_channels + k];
        rows[y] = sum / (image.width * image.nr_channels);
    }
    for(int x = 0; x < image.width; x++) {
        double sum = 0;
        for(int y = 0; y < image.height; y++)
        for(int k = 0; k < image.nr_channels; k++)
            sum += image.data[(y * image.width + x) * image.nr_channels + k];
        cols[x] = sum / (image.height * image.nr_channels);
    }

    *out_py = dominant_period(rows, image.height);
    *out_px = dominant_period(cols, image.width);

    PF_FREE(rows);
    PF_FREE(cols);
}

static struct phase_lock make_phase_lock(struct image image, struct image_view first)
{
    int px = 0, py = 0;
    estimate_period(image, &px, &py);
    return (struct phase_lock){
        .px = px,
        .py = py,
        .rx = px ? (first.x % px) : 0,
        .ry = py ? (first.y % py) : 0
    };
}

static bool phase_compatible(const struct phase_lock *lock, int r, int c)
{
    if(!lock)
        return true;

    /* The candidate block is sampled OVERLAP_DIM past (r, c) in the source; keep
     * it only if it shares the first block's phase along each periodic axis.
     */
    int sx = c + OVERLAP_DIM;
    int sy = r + OVERLAP_DIM;
    if(lock->px > 0) {
        int d = ((sx - lock->rx) % lock->px + lock->px) % lock->px;
        d = MIN(d, lock->px - d);
        if(d > PHASE_TOL)
            return false;
    }
    if(lock->py > 0) {
        int d = ((sy - lock->ry) % lock->py + lock->py) % lock->py;
        d = MIN(d, lock->py - d);
        if(d > PHASE_TOL)
            return false;
    }
    return true;
}

/* choose_sample takes as input the cost image (each pixel's value is the cost of
 * selecting the patch centered at that pixel) and selects a randomly sampled patch
 * with low cost. When the texture is periodic, only positions sharing the first
 * block's phase are eligible, so the base blocks tile cleanly in every arrangement.
 */
static struct coord choose_sample(struct cost_image cost_image, const struct phase_lock *lock)
{
    vec_coord_t candidates;
    vec_coord_init(&candidates);

    int min = INT_MAX;
    for(int r = 0; r < cost_image.height; r++) {
    for(int c = 0; c < cost_image.width; c++) {
        if(!phase_compatible(lock, r, c))
            continue;
        min = MIN(min, cost_image.data[r * cost_image.width + c]);
    }}
    if(min == INT_MAX) {
        /* No phase-compatible position; drop the constraint rather than fail. */
        lock = NULL;
        for(int r = 0; r < cost_image.height; r++) {
        for(int c = 0; c < cost_image.width; c++) {
            min = MIN(min, cost_image.data[r * cost_image.width + c]);
        }}
    }

    /* Accept any patch within OVERLAP_TOLERANCE of the best match, relative to
     * the minimum rather than the min-max range, so the candidate pool stays
     * stable across textures and always admits at least the best match.
     */
    double threshold = (double)min * (1.0 + OVERLAP_TOLERANCE);
    for(int r = 0; r < cost_image.height; r++) {
    for(int c = 0; c < cost_image.width; c++) {
        if(!phase_compatible(lock, r, c))
            continue;
        if((double)cost_image.data[r * cost_image.width + c] <= threshold) {
            vec_coord_push(&candidates, (struct coord){r, c});
        }
    }}
    assert(vec_size(&candidates) > 0);

    /* Greedily keep a spatially spread subset so the random pick is not drawn
     * from a cluster of near-identical neighbouring positions.
     */
    vec_coord_t spread;
    vec_coord_init(&spread);
    for(int i = 0; i < vec_size(&candidates); i++) {
        struct coord cand = vec_AT(&candidates, i);
        bool isolated = true;
        for(int j = 0; j < vec_size(&spread); j++) {
            struct coord other = vec_AT(&spread, j);
            int dr = cand.r - other.r;
            int dc = cand.c - other.c;
            if(dr * dr + dc * dc < CANDIDATE_MIN_SEP * CANDIDATE_MIN_SEP) {
                isolated = false;
                break;
            }
        }
        if(isolated)
            vec_coord_push(&spread, cand);
    }
    assert(vec_size(&spread) > 0);

    int idx = pf_rand(&s_seed) % vec_size(&spread);
    struct coord ret = vec_AT(&spread, idx);

    vec_coord_destroy(&spread);
    vec_coord_destroy(&candidates);
    return ret;
}

static bool match_next_block(struct image image, struct image_view *views,
                             enum constraint constraint, const struct phase_lock *lock,
                             struct image_view *out_view)
{
    bool ret = false;
    size_t cost_width = image.width - (BLOCK_DIM + OVERLAP_DIM * 2) + 1;
    size_t cost_height = image.height - (BLOCK_DIM + OVERLAP_DIM * 2) + 1;
    struct cost_image cost_image = (struct cost_image){
        .data = PF_MALLOC(sizeof(int) * cost_width * cost_height),
        .width = cost_width,
        .height = cost_height,
    };
    if(!cost_image.data)
        goto fail_cost_image;

    struct image_patch template;
    if(!copy_overlap(image, views, constraint, &template))
        goto fail_template;

    struct image_patch_mask mask;
    create_mask(constraint, &mask);

    ssd_patch(image, cost_image, &template, &mask);
    struct coord sample = choose_sample(cost_image, lock);
    *out_view = (struct image_view){
        .x = sample.c + OVERLAP_DIM,
        .y = sample.r + OVERLAP_DIM,
        .width = BLOCK_DIM,
        .height = BLOCK_DIM
    };
    assert(out_view->x >= 0);
    assert(out_view->y >= 0);
    ret = true;

    PF_FREE(template.pixels);
fail_template:
    PF_FREE(cost_image.data);
fail_cost_image:
    return ret;
}

static int compute_min_err_at(struct cost_image err, khash_t(coord) *memo, struct coord coord,
                              enum direction dir)
{
    uint64_t key = coord_to_key(coord);
    khiter_t k = kh_get(coord, memo, key);
    if(k != kh_end(memo)) {
        return kh_val(memo, k);
    }

    /* For horizontal patches, the path goes from left to right.
     * For vertical patches, the path goes from top to bottom.
     */
    if((dir == DIRECTION_VERTICAL && (coord.r == 0))
    || (dir == DIRECTION_HORIZONTAL && (coord.c == 0))) {
        int value = err.data[coord.r * err.width + coord.c];
        int status;
        khiter_t k = kh_put(coord, memo, key, &status);
        assert(status != -1);
        kh_val(memo, k) = value;
        assert(value >= 0);
        return value;
    }

    /* If we are not at the edge, find the minimum adjacent 
     * cumulative costs and combine it with the current cost;
     */
    int ret = 0;
    size_t nadjacent = 0;
    struct coord adjacent[3] = {0};
    int adjacent_errs[3] = {0};

    switch(dir) {
    case DIRECTION_VERTICAL: {
        assert(coord.r > 0);
        if(coord.c > 0) {
            adjacent[nadjacent++] = (struct coord){coord.r - 1, coord.c - 1};
        }
        adjacent[nadjacent++] = (struct coord){coord.r - 1, coord.c};
        if(coord.c < err.width-1) {
            adjacent[nadjacent++] = (struct coord){coord.r - 1, coord.c + 1};
        }
        break;
    }
    case DIRECTION_HORIZONTAL: {
        assert(coord.c > 0);
        if(coord.r > 0) {
            adjacent[nadjacent++] = (struct coord){coord.r - 1, coord.c - 1};
        }
        adjacent[nadjacent++] = (struct coord){coord.r, coord.c - 1};
        if(coord.r < err.height-1) {
            adjacent[nadjacent++] = (struct coord){coord.r + 1, coord.c - 1};
        }
        break;
    }
    default: assert(0);
    }
    assert(nadjacent > 0);

    int min_err_idx = -1;
    (void)min_err_idx;
    int min_err = INT_MAX;
    for(int i = 0; i < nadjacent; i++) {
        adjacent_errs[i] = compute_min_err_at(err, memo, adjacent[i], dir);
        if(adjacent_errs[i] < min_err) {
            min_err = adjacent_errs[i];
            min_err_idx = i;
        }
    }
    assert(min_err_idx != -1);

    int curr_err = err.data[coord.r * err.width + coord.c];
    ret = curr_err + min_err;

    int status;
    k = kh_put(coord, memo, key, &status);
    assert(status != -1);
    kh_val(memo, k) = ret;
    assert(ret >= 0);
    return ret;
}

/* Use dynamic programming to find the minimum error sufrace.
 */
static bool compute_min_err_surface(struct cost_image err, struct cost_image out,
                                    enum direction dir)
{
    khash_t(coord) *memo = kh_init(coord);
    if(!memo)
        return false;
    if(0 != kh_resize(coord, memo, err.width * err.height)) {
        kh_destroy(coord, memo);
        return false;
    }

    for(int r = 0; r < err.height; r++) {
    for(int c = 0; c < err.width; c++) {
        size_t offset = r * err.width + c;
        out.data[offset] = compute_min_err_at(err, memo, (struct coord){r, c}, dir);
    }}

    kh_destroy(coord, memo);
    return true;
}

struct coord row_min(struct cost_image err_surface, int row, int minc, int maxc)
{
    int min = INT_MAX;
    int min_col = 0;
    for(int c = minc; c <= maxc; c++) {
        size_t offset = row * err_surface.width + c;
        int value = err_surface.data[offset];
        if(value < min) {
            min = value;
            min_col = c;
        }
    }
    return (struct coord){row, min_col};
}

struct coord col_min(struct cost_image err_surface, int col, int minr, int maxr)
{
    int min = INT_MAX;
    int min_row = 0;
    for(int r = minr; r <= maxr; r++) {
        size_t offset = r * err_surface.width + col;
        int value = err_surface.data[offset];
        if(value < min) {
            min = value;
            min_row = r;
        }
    }
    return (struct coord){min_row, col};
}

static void seam_path(struct cost_image err_surface, enum direction dir, struct coord *out_path)
{
    switch(dir) {
    case DIRECTION_HORIZONTAL:
        for(int c = err_surface.width-1; c >= 0; c--) {
            int minr, maxr;
            if(c == err_surface.width-1) {
                minr = 0;
                maxr = err_surface.height-1;
            }else{
                minr = MAX(out_path[c+1].r-1, 0);
                maxr = MIN(out_path[c+1].r+1, err_surface.height-1);
            }
            out_path[c] = col_min(err_surface, c, minr, maxr);
        }
        break;
    case DIRECTION_VERTICAL:
        for(int r = err_surface.height-1; r >= 0; r--) {
            int minc, maxc;
            if(r == err_surface.height-1) {
                minc = 0;
                maxc = err_surface.width-1;
            }else{
                minc = MAX(out_path[r+1].c-1, 0);
                maxc = MIN(out_path[r+1].c+1, err_surface.width-1);
            }
            out_path[r] = row_min(err_surface, r, minc, maxc);
        }
        break;
    default: assert(0);
    }
}

static bool seam_mask_from_err_surface(struct cost_image err_surface, struct seam_mask *out,
                                       enum direction dir)
{
    size_t mask_size = err_surface.width * err_surface.height;
    out->bits = PF_MALLOC(mask_size);
    if(!out->bits)
        return false;

    /* Find the minimum path accross the surface.
     */
    size_t pathlen = MAX(err_surface.width, err_surface.height);
    struct coord *path = PF_MALLOC(pathlen * sizeof(struct coord));
    if(!path) {
        PF_FREE(out->bits);
        return false;
    }
    seam_path(err_surface, dir, path);

    memset(out->bits, 0, mask_size);
    for(int i = 0; i < pathlen; i++) {

        struct coord curr = path[i];
        switch(dir) {
        case DIRECTION_HORIZONTAL:
            for(int r = 0; r < curr.r; r++) {
                size_t offset = r * err_surface.width + i;
                out->bits[offset] = 1;
            }
            break;
        case DIRECTION_VERTICAL:
            for(int c = 0; c < curr.c; c++) {
                size_t offset = i * err_surface.width + c;
                out->bits[offset] = 1;
            }
            break;
        default: assert(0);
        }
    }

    PF_FREE(path);
    return true;
}

static bool find_seam(struct image image, struct image_view a, struct image_view b,
                      enum direction dir, struct seam_mask *out)
{
    bool ret = false;
    size_t width  = (dir == DIRECTION_HORIZONTAL) ? a.width + OVERLAP_DIM: OVERLAP_DIM * 2;
    size_t height = (dir == DIRECTION_HORIZONTAL) ? OVERLAP_DIM * 2 : a.height + OVERLAP_DIM;
    size_t patch_size = width * height * sizeof(int);
    struct cost_image patch = (struct cost_image){
        .data = PF_MALLOC(patch_size),
        .width = width,
        .height = height
    };
    if(!patch.data)
        goto fail_patch;

    struct image_view overlap_a, overlap_b;
    switch(dir) {
    case DIRECTION_HORIZONTAL:
        overlap_a = (struct image_view){
            .x = a.x,
            .y = a.y + a.height - OVERLAP_DIM,
            .width = a.width + OVERLAP_DIM,
            .height = OVERLAP_DIM * 2
        };
        overlap_b = (struct image_view){
            .x = b.x,
            .y = b.y - OVERLAP_DIM,
            .width = b.width + OVERLAP_DIM,
            .height = OVERLAP_DIM * 2
        };
        break;
    case DIRECTION_VERTICAL:
        overlap_a = (struct image_view){
            .x = a.x + a.width - OVERLAP_DIM,
            .y = a.y,
            .width = OVERLAP_DIM * 2,
            .height = a.height + OVERLAP_DIM
        };
        overlap_b = (struct image_view){
            .x = b.x - OVERLAP_DIM,
            .y = b.y,
            .width = OVERLAP_DIM * 2,
            .height = b.height + OVERLAP_DIM
        };
        break;
    default: assert(0);
    }

    for(int r = 0; r < height; r++) {
    for(int c = 0; c < width; c++) {

        size_t offset_dst = (r * width + c);
        size_t offset_a = (((overlap_a.y + r) * image.width) + (overlap_a.x + c)) * image.nr_channels;
        size_t offset_b = (((overlap_b.y + r) * image.width) + (overlap_b.x + c)) * image.nr_channels;

        int diff_magnitude_squared = 0;
        for(int i = 0; i < image.nr_channels; i++) {
            diff_magnitude_squared += pow(image.data[offset_a + i] - image.data[offset_b + i], 2);
        }
        patch.data[offset_dst] = abs(diff_magnitude_squared);
    }}

    struct cost_image min_err_surface = (struct cost_image){
        .data = PF_MALLOC(patch_size),
        .width = width,
        .height = height
    };
    if(!min_err_surface.data)
        goto fail_err_surface;
    if(!compute_min_err_surface(patch, min_err_surface, dir))
        goto fail_seam;

    if(!seam_mask_from_err_surface(min_err_surface, out, dir))
        goto fail_seam;

    ret = true;
fail_seam:
    PF_FREE(min_err_surface.data);
fail_err_surface:
    PF_FREE(patch.data);
fail_patch:
    return ret;
}

static void blit_patch_mask(enum tile_patch patch, struct seam_mask *patch_mask,
                            struct seam_mask *vertical, struct seam_mask *horizontal)
{
    /* Blit the non-overlapping region */
    int offx, offy;
    switch(patch) {
    case TOP_LEFT:
        offx = 0;
        offy = 0;
        break;
    case TOP_RIGHT:
        offx = OVERLAP_DIM * 2;
        offy = 0;
        break;
    case BOT_LEFT:
        offx = 0;
        offy = OVERLAP_DIM * 2;
        break;
    case BOT_RIGHT:
        offx = OVERLAP_DIM * 2;
        offy = OVERLAP_DIM * 2;
        break;
    default: assert(0);
    }

    for(int r = offy; r < offy + BLOCK_DIM - OVERLAP_DIM; r++) {
    for(int c = offx; c < offx + BLOCK_DIM - OVERLAP_DIM; c++) {
        size_t width = BLOCK_DIM + OVERLAP_DIM;
        size_t offset = r * width + c;
        patch_mask->bits[offset] = 1;
    }}

    /* Blit the horizontal overlap */
    switch(patch) {
    case TOP_LEFT:
        offx = 0;
        offy = BLOCK_DIM - OVERLAP_DIM;
        break;
    case TOP_RIGHT:
        offx = OVERLAP_DIM * 2;
        offy = BLOCK_DIM - OVERLAP_DIM;
        break;
    case BOT_LEFT:
        offx = 0;
        offy = 0;
        break;
    case BOT_RIGHT:
        offx = OVERLAP_DIM * 2;
        offy = 0;
        break;
    default: assert(0);
    }

    for(int r = offy; r < offy + OVERLAP_DIM * 2; r++) {
    for(int c = offx; c < offx + BLOCK_DIM - OVERLAP_DIM; c++) {
        size_t width = BLOCK_DIM + OVERLAP_DIM;
        size_t offset = r * width + c;

        size_t horizontal_width = BLOCK_DIM + OVERLAP_DIM;
        size_t horizontal_offset = (r - offy) * horizontal_width + (c);

        if(patch == TOP_LEFT || patch == TOP_RIGHT) {
            patch_mask->bits[offset] = horizontal->bits[horizontal_offset];
        }else{
            patch_mask->bits[offset] = !horizontal->bits[horizontal_offset];
        }
    }}

    /* Blit the vertical overlap */
    switch(patch) {
    case TOP_LEFT:
        offx = BLOCK_DIM - OVERLAP_DIM;
        offy = 0;
        break;
    case TOP_RIGHT:
        offx = 0;
        offy = 0;
        break;
    case BOT_LEFT:
        offx = BLOCK_DIM - OVERLAP_DIM;
        offy = OVERLAP_DIM * 2;
        break;
    case BOT_RIGHT:
        offx = 0;
        offy = OVERLAP_DIM * 2;
        break;
    default: assert(0);
    }

    for(int r = offy; r < offy + BLOCK_DIM - OVERLAP_DIM; r++) {
    for(int c = offx; c < offx + OVERLAP_DIM * 2; c++) {
        size_t width = BLOCK_DIM + OVERLAP_DIM;
        size_t offset = r * width + c;

        size_t vertical_width = OVERLAP_DIM * 2;
        size_t vertical_offset = (r) * vertical_width + (c - offx);

        if(patch == TOP_LEFT || patch == BOT_LEFT) {
            patch_mask->bits[offset] = vertical->bits[vertical_offset];
        }else{
            patch_mask->bits[offset] = !vertical->bits[vertical_offset];
        }
    }}

    /* Blit the intersection of the vertical and horizontal overlaps */
    switch(patch) {
    case TOP_LEFT:
        offx = BLOCK_DIM - OVERLAP_DIM;
        offy = BLOCK_DIM - OVERLAP_DIM;
        break;
    case TOP_RIGHT:
        offx = 0;
        offy = BLOCK_DIM - OVERLAP_DIM;
        break;
    case BOT_LEFT:
        offx = BLOCK_DIM - OVERLAP_DIM;
        offy = 0;
        break;
    case BOT_RIGHT:
        offx = 0;
        offy = 0;
        break;
    default: assert(0);
    }

    for(int r = offy; r < offy + OVERLAP_DIM * 2; r++) {
    for(int c = offx; c < offx + OVERLAP_DIM * 2; c++) {
        size_t width = BLOCK_DIM + OVERLAP_DIM;
        size_t offset = r * width + c;

        size_t vertical_width = OVERLAP_DIM * 2;
        size_t vertical_offset = (r) * vertical_width + (c - offx);

        size_t horizontal_width = BLOCK_DIM + OVERLAP_DIM;
        size_t horizontal_offset = (r - offy) * horizontal_width + (c);

        char horizontal_bit;
        if(patch == TOP_LEFT || patch == TOP_RIGHT) {
            horizontal_bit = horizontal->bits[horizontal_offset];
        }else{
            horizontal_bit = !horizontal->bits[horizontal_offset];
        }

        char vertical_bit;
        if(patch == TOP_LEFT || patch == BOT_LEFT) {
            vertical_bit = vertical->bits[vertical_offset];
        }else{
            vertical_bit = !vertical->bits[vertical_offset];
        }

        patch_mask->bits[offset] = horizontal_bit & vertical_bit;
    }}
}

static bool paste_block(struct image image, enum tile_patch patch, struct image_view view, 
                        struct seam_mask *vertical, struct seam_mask *horizontal,
                        struct image_tile *out)
{
    struct seam_mask patch_mask;
    size_t patch_mask_size = (BLOCK_DIM + OVERLAP_DIM) * (BLOCK_DIM + OVERLAP_DIM);
    patch_mask.bits = PF_MALLOC(patch_mask_size);
    if(!patch_mask.bits)
        return false;

    memset(patch_mask.bits, 0, patch_mask_size);
    blit_patch_mask(patch, &patch_mask, vertical, horizontal);

    int offx, offy;
    struct image_view source_view;
    switch(patch) {
    case TOP_LEFT:
        offx = 0;
        offy = 0;
        source_view = (struct image_view){
            .x = view.x,
            .y = view.y,
            .width = view.width + OVERLAP_DIM,
            .height = view.height + OVERLAP_DIM
        };
        break;
    case TOP_RIGHT:
        offx = BLOCK_DIM - OVERLAP_DIM;
        offy = 0;
        source_view = (struct image_view){
            .x = view.x - OVERLAP_DIM,
            .y = view.y,
            .width = view.width + OVERLAP_DIM,
            .height = view.height + OVERLAP_DIM
        };
        break;
    case BOT_LEFT:
        offx = 0;
        offy = BLOCK_DIM - OVERLAP_DIM;
        source_view = (struct image_view){
            .x = view.x,
            .y = view.y - OVERLAP_DIM,
            .width = view.width + OVERLAP_DIM,
            .height = view.height + OVERLAP_DIM
        };
        break;
    case BOT_RIGHT:
        offx = BLOCK_DIM - OVERLAP_DIM;
        offy = BLOCK_DIM - OVERLAP_DIM;
        source_view = (struct image_view){
            .x = view.x - OVERLAP_DIM,
            .y = view.y - OVERLAP_DIM,
            .width = view.width + OVERLAP_DIM,
            .height = view.height + OVERLAP_DIM
        };
        break;
    default: assert(0);
    }

    for(int r = 0; r < BLOCK_DIM + OVERLAP_DIM; r++) {
    for(int c = 0; c < BLOCK_DIM + OVERLAP_DIM; c++) {

        int patchx = offx + c;
        int patchy = offy + r;

        size_t tile_width = TILE_DIM * image.nr_channels;
        size_t patch_offset = patchy * tile_width + (patchx * image.nr_channels);
        size_t mask_width = BLOCK_DIM + OVERLAP_DIM;
        size_t mask_offset = r * mask_width + c;

        size_t row_offset = (source_view.y + r) * image.width * image.nr_channels;
        size_t col_offset = (source_view.x + c) * image.nr_channels;
        const unsigned char *src = image.data + row_offset + col_offset;

        if(patch == TOP_LEFT || patch_mask.bits[mask_offset]) {
            memcpy(out->pixels + patch_offset, src, image.nr_channels);
        }
    }}

    PF_FREE(patch_mask.bits);
    return true;
}

static bool quilt_tile(struct image image, struct image_tile *tile)
{
    /* Go through the image to be synthesized in raster scan order in steps of one 
     * block (minus the overlap) 
     *
     * For every location, search the input texture for a set of blocks that satisfy
     * the overlap constraints (above and left) within the error tolerance. Randomly
     * pick one such block.
     *
     * Compute the error surface between the newly chosen block and the old blocks at 
     * the overlap region. Find the minimum cost path along this surface and make that 
     * the boundary of the new block. Paste the block onto the texture. Repeat.
     */

    bool ret = false;
    struct image_view views[4] = {0};
    views[0] = random_block(image);
    struct phase_lock lock = make_phase_lock(image, views[0]);

    struct image_view b1_views[] = {views[0]};
    if(!match_next_block(image, b1_views, CONSTRAIN_LEFT, &lock, &views[1]))
        goto fail_block;

    struct image_view b2_views[] = {views[0]};
    if(!match_next_block(image, b2_views, CONSTRAIN_TOP, &lock, &views[2]))
        goto fail_block;

    struct image_view b3_views[] = {views[2], views[1]};
    if(!match_next_block(image, b3_views, CONSTRAIN_TOP_LEFT, &lock, &views[3]))
        goto fail_block;

    struct seam_mask seams[4] = {0};
    if(!find_seam(image, views[0], views[1], DIRECTION_VERTICAL, &seams[0]))
        goto fail_seams;
    if(!find_seam(image, views[0], views[2], DIRECTION_HORIZONTAL, &seams[1]))
        goto fail_seams;
    if(!find_seam(image, views[1], views[3], DIRECTION_HORIZONTAL, &seams[2]))
        goto fail_seams;
    if(!find_seam(image, views[2], views[3], DIRECTION_VERTICAL, &seams[3]))
        goto fail_seams;

    tile->pixels = PF_MALLOC(image.nr_channels * TILE_DIM * TILE_DIM);
    if(!tile->pixels)
        goto fail_seams;

    if(!paste_block(image, TOP_LEFT, views[0], &seams[0], &seams[1], tile))
        goto fail_paste;
    if(!paste_block(image, TOP_RIGHT, views[1], &seams[0], &seams[2], tile))
        goto fail_paste;
    if(!paste_block(image, BOT_LEFT, views[2], &seams[3], &seams[1], tile))
        goto fail_paste;
    if(!paste_block(image, BOT_RIGHT, views[3], &seams[3], &seams[2], tile))
        goto fail_paste;

    ret = true;
fail_paste:
    if(!ret) {
        PF_FREE(tile->pixels);
    }
fail_seams:
    for(int i = 0; i < 4; i++) {
        PF_FREE(seams[i].bits);
    }
fail_block:
    return ret;
}

static bool stitch_samples(struct image image, struct image_view *views, struct image_tile *tile)
{
    bool ret = false;
    struct seam_mask seams[4] = {0};
    if(!find_seam(image, views[0], views[1], DIRECTION_VERTICAL, &seams[0]))
        goto fail_seams;
    if(!find_seam(image, views[0], views[2], DIRECTION_HORIZONTAL, &seams[1]))
        goto fail_seams;
    if(!find_seam(image, views[1], views[3], DIRECTION_HORIZONTAL, &seams[2]))
        goto fail_seams;
    if(!find_seam(image, views[2], views[3], DIRECTION_VERTICAL, &seams[3]))
        goto fail_seams;

    tile->pixels = PF_MALLOC(image.nr_channels * TILE_DIM * TILE_DIM);
    if(!tile->pixels)
        goto fail_seams;

    if(!paste_block(image, TOP_LEFT, views[0], &seams[0], &seams[1], tile))
        goto fail_paste;
    if(!paste_block(image, TOP_RIGHT, views[1], &seams[0], &seams[2], tile))
        goto fail_paste;
    if(!paste_block(image, BOT_LEFT, views[2], &seams[3], &seams[1], tile))
        goto fail_paste;
    if(!paste_block(image, BOT_RIGHT, views[3], &seams[3], &seams[2], tile))
        goto fail_paste;

    ret = true;
fail_paste:
    if(!ret) {
        PF_FREE(tile->pixels);
    }
fail_seams:
    for(int i = 0; i < 4; i++) {
        PF_FREE(seams[i].bits);
    }
    return ret;
}

static bool sample_diamond(size_t nr_channels, const struct image_tile *tile, 
                           struct diamond_patch *out)
{
    size_t rotated_size = ceil(TILE_DIM * cos(M_PI/4)) * 2;
    size_t diamond_size = round(TILE_DIM / 2.0 / cos(M_PI/4));
    out->pixels = PF_MALLOC(nr_channels * diamond_size * diamond_size);
    if(!out->pixels)
        return false;
    out->width = diamond_size;
    out->height = diamond_size;

    /* The diamond is the central square of the tile rotated by 45 degrees. For
     * each destination pixel, inverse-map it back into the tile and bilinearly
     * sample, which avoids the holes left by a forward-mapped rotation.
     */
    const double c45 = cos(M_PI/4), s45 = sin(M_PI/4);
    const double tile_center = TILE_DIM / 2.0;
    const double rot_center = rotated_size / 2.0;
    const int padding = (rotated_size - diamond_size) / 2;

    for(int dr = 0; dr < diamond_size; dr++) {
    for(int dc = 0; dc < diamond_size; dc++) {

        double rc = (dc + padding) - rot_center;
        double rr = (dr + padding) - rot_center;
        double sc = rc * c45 - rr * s45 + tile_center;
        double sr = rc * s45 + rr * c45 + tile_center;

        int c0 = (int)floor(sc), r0 = (int)floor(sr);
        double fc = sc - c0, fr = sr - r0;
        int c1 = MAX(0, MIN(c0 + 1, TILE_DIM - 1));
        int r1 = MAX(0, MIN(r0 + 1, TILE_DIM - 1));
        c0 = MAX(0, MIN(c0, TILE_DIM - 1));
        r0 = MAX(0, MIN(r0, TILE_DIM - 1));

        const unsigned char *px = (const unsigned char*)tile->pixels;
        char *dst = out->pixels + (dr * diamond_size + dc) * nr_channels;
        for(int i = 0; i < nr_channels; i++) {
            double top = px[(r0 * TILE_DIM + c0) * nr_channels + i] * (1.0 - fc)
                       + px[(r0 * TILE_DIM + c1) * nr_channels + i] * fc;
            double bot = px[(r1 * TILE_DIM + c0) * nr_channels + i] * (1.0 - fc)
                       + px[(r1 * TILE_DIM + c1) * nr_channels + i] * fc;
            dst[i] = (unsigned char)(top * (1.0 - fr) + bot * fr + 0.5);
        }
    }}
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_ImageQuilt_MakeTile(const char *source, struct texture *out, GLuint tunit)
{
    ASSERT_IN_RENDER_THREAD();

    bool ret = false;
    struct image image;
    if(!load_image(source, &image))
        goto fail_load;

    if(image.nr_channels != 3 && image.nr_channels != 4)
        goto fail_quilt;

    struct image_tile tile;
    if(!quilt_tile(image, &tile))
        goto fail_quilt;

    GLuint texture;
    glActiveTexture(tunit);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    GLint format = (image.nr_channels == 3) ? GL_RGB : GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, format, image.width, image.height, 0, 
        format, GL_UNSIGNED_BYTE, tile.pixels);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, LOD_BIAS);

    glBindTexture(GL_TEXTURE_2D, 0);

    out->id = texture;
    out->tunit = tunit;
    ret = true;

fail_quilt:
    free(image.data);
fail_load:
    GL_ASSERT_OK();
    return ret;
}

bool R_GL_ImageQuilt_MakeTileset(const char *source, struct texture_arr *out, GLuint tunit)
{
    ASSERT_IN_RENDER_THREAD();
    if(!s_seed) {
        s_seed = time(NULL);
    }

    bool ret = false;
    struct image image;
    if(!load_image(source, &image))
        goto fail_load;

    if(image.nr_channels != 3 && image.nr_channels != 4)
        goto fail_block;

    /* For Wang tileset generation, first pick 4 sample images */
    struct image_view views[4] = {0};
    views[0] = random_block(image);
    struct phase_lock lock = make_phase_lock(image, views[0]);

    struct image_view b1_views[] = {views[0]};
    if(!match_next_block(image, b1_views, CONSTRAIN_LEFT, &lock, &views[1]))
        goto fail_block;

    struct image_view b2_views[] = {views[0]};
    if(!match_next_block(image, b2_views, CONSTRAIN_TOP, &lock, &views[2]))
        goto fail_block;

    struct image_view b3_views[] = {views[2], views[1]};
    if(!match_next_block(image, b3_views, CONSTRAIN_TOP_LEFT, &lock, &views[3]))
        goto fail_block;

    /* Next, generate an 8-tile Wang tileset by stitching the sample images
     * together in different combinations */
    const int BLUE = 0, RED = 1, YELLOW = 2, GREEN = 3;
    struct image_tile tiles[8] = {0};

    if(!stitch_samples(image, (struct image_view[4]){
        views[BLUE], views[RED], views[GREEN], views[YELLOW]}, &tiles[0]))
        goto fail_stitch;
    if(!stitch_samples(image, (struct image_view[4]){
        views[BLUE], views[GREEN], views[GREEN], views[BLUE]}, &tiles[1]))
        goto fail_stitch;
    if(!stitch_samples(image, (struct image_view[4]){
        views[YELLOW], views[RED], views[RED], views[YELLOW]}, &tiles[2]))
        goto fail_stitch;
    if(!stitch_samples(image, (struct image_view[4]){
        views[YELLOW], views[GREEN], views[RED], views[BLUE]}, &tiles[3]))
        goto fail_stitch;
    if(!stitch_samples(image, (struct image_view[4]){
        views[YELLOW], views[RED], views[GREEN], views[BLUE]}, &tiles[4]))
        goto fail_stitch;
    if(!stitch_samples(image, (struct image_view[4]){
        views[YELLOW], views[GREEN], views[GREEN], views[YELLOW]}, &tiles[5]))
        goto fail_stitch;
    if(!stitch_samples(image, (struct image_view[4]){
        views[BLUE], views[RED], views[RED], views[BLUE]}, &tiles[6]))
        goto fail_stitch;
    if(!stitch_samples(image, (struct image_view[4]){
        views[BLUE], views[GREEN], views[RED], views[YELLOW]}, &tiles[7]))
        goto fail_stitch;

    /* Sample the middle diamond from each of the generated tiles */
    struct diamond_patch diamonds[8] = {0};
    for(int i = 0; i < 8; i++) {
        if(!sample_diamond(image.nr_channels, &tiles[i], &diamonds[i]))
            goto fail_diamond;
    }
    size_t diamond_dim = diamonds[0].width;

    /* Generate a texture array from the created tiles */
    glActiveTexture(tunit);
    out->tunit = tunit;
    glGenTextures(1, &out->id);
    glBindTexture(GL_TEXTURE_2D_ARRAY, out->id);

    GLint format = (image.nr_channels == 3) ? GL_RGB : GL_RGBA;
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, format, 
        diamond_dim, diamond_dim, 8, 0, format, GL_UNSIGNED_BYTE, 0);

    for(int i = 0; i < 8; i++) {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, diamond_dim, 
            diamond_dim, 1, format, GL_UNSIGNED_BYTE, diamonds[i].pixels);
    }

    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_LOD_BIAS, LOD_BIAS);

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    ret = true;

fail_diamond:
    for(int i = 0; i < 8; i++) {
        PF_FREE(diamonds[i].pixels);
    }
fail_stitch:
    for(int i = 0; i < 8; i++) {
        PF_FREE(tiles[i].pixels);
    }
fail_block:
    free(image.data);
fail_load:
    GL_ASSERT_OK();
    return ret;
}

size_t R_GL_ImageQuilt_TilesetDim(void)
{
    return round(TILE_DIM / 2.0 / cos(M_PI/4));
}

