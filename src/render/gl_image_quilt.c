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

#include "gl_image_quilt.h"
#include "../pf_math.h"
#include "../lib/public/stb_image.h"
#include "../lib/public/stb_image_resize.h"

#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#define BLOCK_DIM           (65)
#define OVERLAP_DIM         (10)
#define TILE_DIM            (130)
#define OVERLAP_TOLERANCE   (0.3)

enum constraint{
    CONSTRAIN_LEFT,
    CONSTRAIN_TOP,
    CONSTRAIN_TOP_LEFT
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

struct image_tile{
    char *pixels;
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool dump_ppm(const char* filename, const unsigned char *data, 
                     int nr_channels, int width, int height)
{
    FILE* file = fopen(filename, "wb");
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

static bool dump_mask_ppm(const char* filename, const struct image_patch_mask *mask)
{
    FILE* file = fopen(filename, "wb");
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
                     struct image_patch *template)
{
    for(int r = 0; r < OVERLAP_DIM; r++) {
        size_t row_offset = (view.y + view.height + r) * image.width * image.nr_channels;
        size_t col_offset = view.x * image.nr_channels;
        size_t bytes_per_row = image.nr_channels * view.width;
        const unsigned char *src = image.data + row_offset + col_offset;
        char *dst = template->pixels + (r * bytes_per_row);
        memcpy(dst, src, bytes_per_row);
    }
}

static bool copy_overlap(const struct image image, struct image_view view, 
                         enum constraint constraint, struct image_patch *template)
{
    size_t patch_size = image.nr_channels * view.width * view.height;
    template->pixels = malloc(patch_size);
    if(!template->pixels)
        return false;

    memset(template->pixels, 0, view.width * view.height * image.nr_channels);
    switch(constraint) {
    case CONSTRAIN_LEFT:
        copy_left(image, view, template);
        break;
    case CONSTRAIN_TOP:
        copy_top(image, view, template);
        break;
    case CONSTRAIN_TOP_LEFT: {
        copy_left(image, view, template);
        copy_top(image, view, template);
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

static int compute_ssd(struct image image)
{
    return 0;
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
                      struct image_patch *patch, struct image_patch_mask *mask)
{
}

/* choose_sample takes as input the cost image (each pixel's value is the cost of
 * selecting the patch centered at that pixel) and selects a randomly sampled patch
 * with low cost.
 */
static void choose_sample(struct image image, struct image cost_image)
{
}

static void match_block(struct image image, enum constraint constraint,
                        int x, int y, struct image_patch *out)
{
    size_t cost_width = image.width - BLOCK_DIM + 1;
    size_t cost_height = image.height - BLOCK_DIM + 1;
    struct cost_image cost_image = (struct cost_image){
        .data = malloc(sizeof(int) * cost_width * cost_height),
        .width = cost_width,
        .height = cost_height,
    };
    struct image_patch_mask mask;
    create_mask(constraint, &mask);
    ssd_patch(image, cost_image, out, &mask);
    free(cost_image.data);
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
    struct image_patch template;
    struct image_patch blocks[4];

    struct image_view view = random_block(image);
    if(!copy_view(image, view, &blocks[0]))
        goto fail_block_0;

    if(!copy_overlap(image, view, CONSTRAIN_TOP_LEFT, &template))
        return false;

    struct image_patch_mask mask;
    create_mask(CONSTRAIN_TOP_LEFT, &mask);

    ret = true;
    free(template.pixels);
    free(blocks[0].pixels);
fail_block_0:
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

