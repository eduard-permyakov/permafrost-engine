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

#include "public/noise.h"

#include <math.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* Hash lookup table as defined by Ken Perlin. This is a randomly arranged
 * array of all numbers 0-255, inclusive.
 */
static const int permutation[256] = { 151,160,137,91,90,15,
    131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
    190, 6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,
    88,237,149,56,87,174,20,125,136,171,168, 68,175,74,165,71,134,139,48,27,166,
    77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,
    102,143,54, 65,25,63,161, 1,216,80,73,209,76,132,187,208, 89,18,169,200,196,
    135,130,116,188,159,86,164,100,109,198,173,186, 3,64,52,217,226,250,124,123,
    5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
    223,183,170,213,119,248,152, 2,44,154,163, 70,221,153,101,155,167, 43,172,9,
    129,22,39,253, 19,98,108,110,79,113,224,232,178,185, 112,104,218,246,97,228,
    251,34,242,193,238,210,144,12,191,179,162,241, 81,51,145,235,249,14,239,107,
    49,192,214, 31,181,199,106,157,184, 84,204,176,115,121,50,45,127, 4,150,254,
    138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
};

/* Doubled permutation table to avoid overflow 
 */
static int p[512];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static double fade(double t)
{
    return t * t * t * (t * (t * 6 - 15) + 10);
}

static float lerp(float t, float a, float b)
{
    return a + t * (b - a);
}

/* Convert low 4 bits of hash code into 12 gradient directions.
 */
static float grad(int hash, double x, double y, double z) 
{
    int h = hash & 15;
    double u = (h < 8) ? x : y,
           v = (h < 4) ? y : (h == 12) || (h ==14) ? x : z;
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

static int inc(int num, int repeat) 
{
    num++;
    if(repeat > 0) {
        num %= repeat;
    }
    return num;
}

static float noise(float x, float y, float z,
                   int repeatx, int repeaty, int repeatz)
{
    if(repeatx > 0) {
        x = fmod(x, repeatx);
    }
    if(repeaty > 0) {
        y = fmod(y, repeaty);
    }
    if(repeatz > 0) {
        z = fmod(z, repeatz);
    }
    /* Find unit cube that contains point */
    int xi = (int)floor(x) & 255;
    int yi = (int)floor(y) & 255;
    int zi = (int)floor(z) & 255;

    /* Find relative x, y, z of point in cube */
    x -= floor(x);
    y -= floor(y);
    z -= floor(z);

    /* Compute fade curves for each of x, y, z */
    float u = fade(x);
    float v = fade(y);
    float w = fade(z);

    /* Compute hash coordinates of the 8 cube corners */
    int aaa, aba, aab, abb, baa, bba, bab, bbb;
    aaa = p[p[p[    xi          ]+    yi          ]+    zi          ];
    aba = p[p[p[    xi          ]+inc(yi, repeaty)]+    zi          ];
    aab = p[p[p[    xi          ]+    yi          ]+inc(zi, repeatz)];
    abb = p[p[p[    xi          ]+inc(yi, repeaty)]+inc(zi, repeatz)];
    baa = p[p[p[inc(xi, repeatx)]+    yi          ]+    zi          ];
    bba = p[p[p[inc(xi, repeatx)]+inc(yi, repeaty)]+    zi          ];
    bab = p[p[p[inc(xi, repeatx)]+    yi          ]+inc(zi, repeatz)];
    bbb = p[p[p[inc(xi, repeatx)]+inc(yi, repeaty)]+inc(zi, repeatz)];

    /* Add blended results from 8 corners of the cube */
    return lerp(w, lerp(v, lerp(u, grad(aaa, x  , y  , z   ),
                                   grad(baa, x-1, y  , z   )),
                           lerp(u, grad(aba, x  , y-1, z   ),
                                   grad(bba, x-1, y-1, z   ))),
                   lerp(v, lerp(u, grad(aab, x  , y  , z-1 ),
                                   grad(bab, x-1, y  , z-1 )),
                           lerp(u, grad(abb, x  , y-1, z-1 ),
                                   grad(bbb, x-1, y-1, z-1 ))));
}

static float octave_noise(float x, float y, float z, int repeatx, int repeaty, int repeatz, 
                          float frequency, int octaves, float persistence)
{
    float total = 0.0f;
    float max_value = 0.0f;
    float amplitude = 1.0f;

    for(int i = 0; i < octaves; i++) {
        total += noise(x * frequency, y * frequency, z * frequency, 
            repeatx * pow(2, i), repeaty * pow(2, i), repeatz * pow(2, i)) * amplitude;
        max_value += amplitude;
        amplitude *= persistence;
        frequency *= 2;
    }
    return total / max_value;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void Noise_Init(void)
{
    memcpy(p, permutation, sizeof(permutation));
    memcpy((char*)p + sizeof(permutation), permutation, sizeof(permutation));
}

void Noise_GeneratePerlin1D(size_t x, float frequency, float *outbuff)
{
    for(size_t ix = 0; ix < x; ix++) {
        outbuff[ix] = noise(ix * frequency, 0, 0, 0, 0, 0);
    }
}

void Noise_GeneratePerlin2D(size_t x, size_t y, float frequency, float *outbuff)
{
    for(size_t iy = 0; iy < y; iy++) {
    for(size_t ix = 0; ix < x; ix++) {
        size_t out_idx = (iy * y) + ix;
        assert(out_idx < x * y);
        outbuff[out_idx] = noise(ix * frequency, iy * frequency, 0, 0, 0, 0);
    }}
}

void Noise_GenerateOctavePerlin2D(size_t x, size_t y, float frequency, int octaves,
                                  float persistence, float *outbuff)
{
    for(size_t iy = 0; iy < y; iy++) {
    for(size_t ix = 0; ix < x; ix++) {
        size_t out_idx = (iy * x) + ix;
        assert(out_idx < x * y);
        outbuff[out_idx] = octave_noise(ix, iy, 0, 0, 0, 0, frequency, octaves, persistence);
    }}
}

void Noise_GenerateOctavePerlinTile2D(size_t x, size_t y, float frequency, int octaves,
                                      float persistence, float *outbuff)
{
    for(size_t iy = 0; iy < y; iy++) {
    for(size_t ix = 0; ix < x; ix++) {
        size_t out_idx = (iy * x) + ix;
        assert(out_idx < x * y);
        outbuff[out_idx] = octave_noise(ix, iy, 0, x * frequency, y * frequency, 0, 
            frequency, octaves, persistence);
    }}
}

void Noise_Normalize2D(size_t x, size_t y, float *inout)
{
    for(size_t iy = 0; iy < y; iy++) {
    for(size_t ix = 0; ix < x; ix++) {
        size_t idx = (iy * x) + ix;
        float value = inout[idx];
        value += 1.0f;
        value /= 2.0f;
        inout[idx] = value;
    }}
}

void Noise_GeneratePerlin3D(size_t x, size_t y, size_t z, float frequency, float *outbuff)
{
    for(size_t iy = 0; iy < y; iy++) {
    for(size_t ix = 0; ix < x; ix++) {
    for(size_t iz = 0; iz < z; iz++) {
        size_t out_idx = (iz * x * y) + (iy * x) + ix;
        assert(out_idx < x * y);
        outbuff[out_idx] = noise(ix * frequency, iy * frequency, iz * frequency, 0, 0, 0);
    }}}
}

void Noise_DumpPPM(const char *path, size_t width, size_t height, float *buffer)
{
    FILE *file = fopen(path, "wb");
    if(!file)
        return;

    fprintf(file, "P6\n%lu %lu\n%d\n", (unsigned long)width, (unsigned long)height, 255);

    for(int r = 0; r < height; r++) {
    for(int c = 0; c < width; c++) {

        size_t index = r * width + c;
        float value = buffer[index];
        unsigned char color[3];
        color[0] = value * 255;
        color[1] = value * 255;
        color[2] = value * 255;
        fwrite(color, 1, 3, file);
    }}

    fclose(file);
}

