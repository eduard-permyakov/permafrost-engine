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

#include "gl_image_quilt.h"
#include "../pf_math.h"
#include "../lib/public/stb_image.h"
#include "../lib/public/stb_image_resize.h"
#include "../lib/public/vec.h"
#include "../lib/public/khash.h"

#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

#define BLOCK_DIM           (65)
#define OVERLAP_DIM         (10)
#define TILE_DIM            (130)
#define OVERLAP_TOLERANCE   (0.1)

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

struct coord{
    int r, c;
};

VEC_TYPE(coord, struct coord)
VEC_IMPL(static inline, coord, struct coord)

KHASH_MAP_INIT_INT64(coord, int)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

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

static bool dump_patch(const char *filename, const struct image image, 
                       const struct image_patch *patch)
{
    return dump_ppm(filename, (const unsigned char*)patch->pixels, 
        image.nr_channels, BLOCK_DIM, BLOCK_DIM);
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
    int minx = 0, maxx = image.width - (BLOCK_DIM + OVERLAP_DIM);
    int miny = 0, maxy = image.height - (BLOCK_DIM + OVERLAP_DIM);
    int x = rand() % (maxx + 1 - minx) + minx;
    int y = rand() % (maxy + 1 - miny) + miny;
    return (struct image_view){x, y, BLOCK_DIM, BLOCK_DIM};
}

static bool copy_view(const struct image image, struct image_view view, struct image_patch *patch)
{
    size_t patch_size = image.nr_channels * view.width * view.height;
    patch->pixels = malloc(patch_size);
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
            dst += r;
            bytes_copied -= r;
        }
        memcpy(dst, src, bytes_copied);
    }
}

static bool copy_overlap(const struct image image, struct image_view *views, 
                         enum constraint constraint, struct image_patch *template)
{
    size_t patch_size = image.nr_channels * views[0].width * views[0].height;
    template->pixels = malloc(patch_size);
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

        char vector_a[32];
        memcpy(vector_a, &image.data[img_y * image.width + img_x], image.nr_channels);

        char vector_b[32];
        memcpy(vector_b, &template->pixels[r * view.width + c], image.nr_channels);

        int diff_magnitude_squared = 0;
        for(int i = 0; i < image.nr_channels; i++) {
            diff_magnitude_squared += vector_a[i] - vector_b[i];
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
    int offx = BLOCK_DIM / 2;
    int offy = BLOCK_DIM / 2;

    for(int i = 0; i < out_cost_image.width; i++) {
    for(int j = 0; j < out_cost_image.height; j++) {
        struct image_view view = (struct image_view){
            .x = offx + i,
            .y = offy + j,
            .width = BLOCK_DIM,
            .height = BLOCK_DIM
        };
        out_cost_image.data[out_cost_image.width * i + j] = 
            compute_ssd(image, view, template, mask);
    }}
}

/* choose_sample takes as input the cost image (each pixel's value is the cost of
 * selecting the patch centered at that pixel) and selects a randomly sampled patch
 * with low cost.
 */
static struct coord choose_sample(struct cost_image cost_image)
{
    vec_coord_t candidates;
    vec_coord_init(&candidates);

    int min = INT_MAX;
    int max = INT_MIN;

    for(int r = 0; r < cost_image.height; r++) {
    for(int c = 0; c < cost_image.width; c++) {
        size_t offset = r * cost_image.width + c;
        min = MIN(min, cost_image.data[offset]);
        max = MAX(max, cost_image.data[offset]);
    }}
    int range = max - min;

    for(int r = 0; r < cost_image.height; r++) {
    for(int c = 0; c < cost_image.width; c++) {
        size_t offset = r * cost_image.width + c;
        int value = cost_image.data[offset];
        float percent = ((float)(value - min)) / range;
        if(percent <= OVERLAP_TOLERANCE) {
            vec_coord_push(&candidates, (struct coord){r, c});
        }
    }}
    assert(vec_size(&candidates) > 0);

    int min_coord = 0;
    int max_coord = vec_size(&candidates) - 1;
    int idx = rand() % (max_coord + 1 - min_coord) + min_coord;

    struct coord ret = vec_AT(&candidates, idx);
    vec_coord_destroy(&candidates);
    return ret;
}

static bool match_next_block(struct image image, struct image_view *views, 
                             enum constraint constraint, struct image_view *out_view, 
                             struct image_patch *out_block)
{
    bool ret = false;
    size_t cost_width = image.width - BLOCK_DIM + 1;
    size_t cost_height = image.height - BLOCK_DIM + 1;
    struct cost_image cost_image = (struct cost_image){
        .data = malloc(sizeof(int) * cost_width * cost_height),
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
    struct coord sample = choose_sample(cost_image);
    *out_view = (struct image_view){
        .x = sample.c - (BLOCK_DIM / 2),
        .y = sample.r - (BLOCK_DIM / 2),
        .width = BLOCK_DIM,
        .height = BLOCK_DIM
    };
    copy_view(image, *out_view, out_block);
    ret = true;

    free(template.pixels);
fail_template:
    free(cost_image.data);
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

struct coord row_min(struct cost_image err_surface, int row)
{
    int min = INT_MAX;
    int min_col = 0;
    for(int c = 0; c < err_surface.width; c++) {
        size_t offset = row * err_surface.width + c;
        int value = err_surface.data[offset];
        if(value < min) {
            min = value;
            min_col = c;
        }
    }
    return (struct coord){row, min_col};
}

struct coord col_min(struct cost_image err_surface, int col)
{
    int min = INT_MAX;
    int min_row = 0;
    for(int r = 0; r < err_surface.height; r++) {
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
        for(int c = 0; c < err_surface.width; c++) {
            out_path[c] = col_min(err_surface, c);
        }
        break;
    case DIRECTION_VERTICAL:
        for(int r = 0; r < err_surface.height; r++) {
            out_path[r] = row_min(err_surface, r);
        }
        break;
    default: assert(0);
    }
}

static bool seam_mask_from_err_surface(struct cost_image err_surface, struct seam_mask *out,
                                       enum direction dir)
{
    size_t mask_size = err_surface.width * err_surface.height;
    out->bits = malloc(mask_size);
    if(!out->bits)
        return false;

    /* Find the minimum path accross the surface.
     */
    size_t pathlen = MAX(err_surface.width, err_surface.height);
    struct coord *path = malloc(pathlen * sizeof(struct coord));
    if(!path) {
        free(out->bits);
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

    free(path);
    return true;
}

static bool find_error_surface(struct image image, struct image_view a, struct image_view b,
                               enum direction dir)
{
    bool ret = false;
    size_t width  = (dir == DIRECTION_HORIZONTAL) ? a.width : OVERLAP_DIM * 2;
    size_t height = (dir == DIRECTION_HORIZONTAL) ? OVERLAP_DIM * 2 : a.height;
    size_t patch_size = width * height * sizeof(int);
    struct cost_image patch = (struct cost_image){
        .data = malloc(patch_size),
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
            .width = a.width,
            .height = OVERLAP_DIM * 2
        };
        overlap_b = (struct image_view){
            .x = b.x,
            .y = b.y - OVERLAP_DIM,
            .width = b.width,
            .height = OVERLAP_DIM * 2
        };
        break;
    case DIRECTION_VERTICAL:
        overlap_a = (struct image_view){
            .x = a.x + a.width - OVERLAP_DIM,
            .y = a.y,
            .width = OVERLAP_DIM * 2,
            .height = a.height
        };
        overlap_b = (struct image_view){
            .x = b.x - OVERLAP_DIM,
            .y = b.y,
            .width = OVERLAP_DIM * 2,
            .height = b.height
        };
        break;
    default: assert(0);
    }

    for(int r = 0; r < height; r++) {
    for(int c = 0; c < width; c++) {

        size_t offset_dst = (r * width + c);
        size_t offset_a = ((overlap_a.y + r) * image.width) + (overlap_a.x + c);
        size_t offset_b = ((overlap_b.y + r) * image.width) + (overlap_b.x + c);

        int diff_magnitude_squared = 0;
        for(int i = 0; i < image.nr_channels; i++) {
            diff_magnitude_squared += image.data[offset_a + i] - image.data[offset_b + i];
        }
        patch.data[offset_dst] = abs(diff_magnitude_squared);
    }}

    struct cost_image min_err_surface = (struct cost_image){
        .data = malloc(patch_size),
        .width = width,
        .height = height
    };
    if(!min_err_surface.data)
        goto fail_err_surface;
    if(!compute_min_err_surface(patch, min_err_surface, dir))
        goto fail_seam;

    struct seam_mask mask;
    if(!seam_mask_from_err_surface(min_err_surface, &mask, dir))
        goto fail_seam;

    ret = true;
    free(mask.bits);
fail_seam:
    free(min_err_surface.data);
fail_err_surface:
    free(patch.data);
fail_patch:
    return ret;
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
    struct image_patch blocks[4] = {0};

    views[0] = random_block(image);
    if(!copy_view(image, views[0], &blocks[0]))
        goto fail_block;

    struct image_view b1_views[] = {views[0]};
    if(!match_next_block(image, b1_views, CONSTRAIN_LEFT, &views[1], &blocks[1]))
        goto fail_block;

    struct image_view b2_views[] = {views[0]};
    if(!match_next_block(image, b2_views, CONSTRAIN_TOP, &views[2], &blocks[2]))
        goto fail_block;

    struct image_view b3_views[] = {views[1], views[2]};
    if(!match_next_block(image, b3_views, CONSTRAIN_TOP_LEFT, &views[3], &blocks[3]))
        goto fail_block;

    /* Now that we have selected the blocks, find the minimum error boundary
     * but between adjacent blocks and paste them onto the final tile texture.
     */
    find_error_surface(image, views[0], views[2], DIRECTION_HORIZONTAL);

    ret = true;
fail_block:
    for(int i = 0; i < 4; i++) {
        free(blocks[i].pixels);
    }
    return ret;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_ImageQuilt_MakeTileset(const char *source, struct texture_arr *out, GLuint tunit)
{
    bool ret = false;
    struct image image;
    if(!load_image(source, &image))
        goto fail_load;

    struct image_tile tile;
    if(!quilt_tile(image, &tile))
        goto fail_quilt;

    ret = true;
fail_quilt:
    free(image.data);
fail_load:
    return ret;
}

