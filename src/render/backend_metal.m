/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2017-2023 Eduard Permyakov
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
 */

#include <SDL.h>
#include <SDL_metal.h>
#include <dispatch/dispatch.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>

#include "backend_local.h"
#include "public/render.h"
#include "gl_material.h"
#include "render_private.h"
#include "gl_vertex.h"
#include "../anim/public/anim.h"
#include "../anim/public/skeleton.h"
#include "../phys/public/collision.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../camera.h"
#include "../main.h"
#include "../game/public/game.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/stb_image.h"
#include "../lib/public/stb_image_resize.h"
#include "../lib/public/mem.h"
#include "../lib/public/noise.h"
#include "../config.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>


extern bool g_trace_gpu;

static SDL_Window               *s_window;
static struct render_sync_state *s_rstate;
static SDL_MetalView             s_metal_view;
static id<MTLDevice>             s_device;
static id<MTLCommandQueue>       s_queue;
static CAMetalLayer             *s_layer;

static id<MTLRenderPipelineState> s_ui_pipeline;
static id<MTLRenderPipelineState> s_ui_msaa_pipeline;
static id<MTLRenderPipelineState> s_minimap_pipeline;
static id<MTLRenderPipelineState> s_minimap_msaa_pipeline;
static id<MTLRenderPipelineState> s_terrain_pipeline;
static id<MTLRenderPipelineState> s_terrain_msaa_pipeline;
static id<MTLRenderPipelineState> s_terrain_depth_pipeline;
static id<MTLRenderPipelineState> s_terrain_depth_msaa_pipeline;
static id<MTLRenderPipelineState> s_static_mesh_pipeline;
static id<MTLRenderPipelineState> s_static_mesh_msaa_pipeline;
static id<MTLRenderPipelineState> s_static_mesh_blend_pipeline;
static id<MTLRenderPipelineState> s_static_mesh_blend_msaa_pipeline;
static id<MTLRenderPipelineState> s_static_mesh_depth_pipeline;
static id<MTLRenderPipelineState> s_static_mesh_depth_msaa_pipeline;
static id<MTLRenderPipelineState> s_static_mesh_blend_depth_pipeline;
static id<MTLRenderPipelineState> s_static_mesh_blend_depth_msaa_pipeline;
static id<MTLRenderPipelineState> s_world_color_pipeline;
static id<MTLRenderPipelineState> s_world_color_msaa_pipeline;
static id<MTLRenderPipelineState> s_world_color_depth_pipeline;
static id<MTLRenderPipelineState> s_world_color_depth_msaa_pipeline;
static id<MTLRenderPipelineState> s_shadow_terrain_pipeline;
static id<MTLRenderPipelineState> s_shadow_mesh_pipeline;
static id<MTLRenderPipelineState> s_shadow_owner_terrain_pipeline;
static id<MTLRenderPipelineState> s_shadow_owner_mesh_pipeline;
static id<MTLRenderPipelineState> s_water_surface_pipeline;
static id<MTLRenderPipelineState> s_water_surface_msaa_pipeline;
static id<MTLRenderPipelineState> s_skybox_pipeline;
static id<MTLRenderPipelineState> s_skybox_msaa_pipeline;
static id<MTLRenderPipelineState> s_sprite_pipeline;
static id<MTLRenderPipelineState> s_sprite_msaa_pipeline;
static id<MTLDepthStencilState>   s_depth_write_state;
static id<MTLDepthStencilState>   s_depth_read_state;
static id<MTLSamplerState>       s_ui_sampler;
static id<MTLSamplerState>       s_minimap_sampler;
static id<MTLSamplerState>       s_scene_sampler;
static id<MTLSamplerState>       s_material_sampler;
static id<MTLSamplerState>       s_terrain_sampler;
static id<MTLSamplerState>       s_shadow_sampler;
static id<MTLSamplerState>       s_skybox_sampler;
static id<MTLSamplerState>       s_sprite_sampler;
static id<MTLTexture>            s_ui_font_texture;
static NSMutableDictionary      *s_ui_texpath_cache;
static NSMutableDictionary      *s_sprite_texpath_cache;
static id<MTLTexture>            s_pending_ui_texture;
static id<MTLTexture>            s_minimap_texture;
static id<MTLTexture>            s_terrain_texture_array;
static id<MTLTexture>            s_skybox_texture;
static id<MTLTexture>            s_water_dudv_texture;
static id<MTLTexture>            s_water_normal_texture;
static id<MTLTexture>            s_frame_msaa_texture;
static id<MTLTexture>            s_frame_depth_texture;
static id<MTLTexture>            s_shadow_depth_texture;
static id<MTLTexture>            s_shadow_owner_texture;
static id<MTLTexture>            s_shadow_owner_dummy_texture;
static id<MTLTexture>            s_water_reflection_texture;
static id<MTLTexture>            s_water_reflection_depth_texture;
static id<MTLTexture>            s_water_refraction_texture;
static id<MTLTexture>            s_water_refraction_depth_texture;
static id<MTLBuffer>             s_fog_buffer;
static id<MTLBuffer>             s_water_buffer;
static id<MTLBuffer>             s_heightmap_buffer;
static id<MTLBuffer>             s_splatmap_buffer;
static id<MTLBuffer>             s_splat_indices_buffer;
static id<MTLBuffer>             s_skybox_vertex_buffer;
static id<MTLBuffer>             s_shadow_screen_probe_buffer;
static id<MTLBuffer>             s_shadow_screen_probe_params_buffer;
static id<MTLBuffer>             s_shadow_screen_probe_disabled_params_buffer;
static id<MTLBuffer>             s_water_reflection_dump_buffer;

static id<CAMetalDrawable>       s_frame_drawable;
static id<MTLCommandBuffer>      s_frame_command_buffer;
static id<MTLRenderCommandEncoder> s_frame_encoder;
static id<MTLCommandBuffer>      s_shadow_command_buffer;
static id<MTLRenderCommandEncoder> s_shadow_encoder;
static id<MTLRenderCommandEncoder> s_water_scene_encoder;
static dispatch_semaphore_t      s_inflight_semaphore;

static char s_info_vendor[128];
static char s_info_renderer[128];
static char s_info_version[128];
static char s_info_sl_version[128];
static char s_info_msaa_samples[16];
static matrix_float4x4 s_scene_view;
static matrix_float4x4 s_scene_proj;
static matrix_float4x4 s_shadow_light_space;
static matrix_float4x4 s_shadow_view;
static matrix_float4x4 s_shadow_proj;
static vector_float3   s_scene_view_pos;
static vector_float3   s_light_ambient = {1.0f, 1.0f, 1.0f};
static vector_float3   s_light_color = {1.0f, 1.0f, 1.0f};
static vector_float3   s_light_pos = {1.0f, 1.0f, 1.0f};
static bool            s_have_scene_view;
static bool            s_have_scene_proj;
static bool            s_shadow_pass_active;
static bool            s_shadow_owner_pass_active;
static bool            s_water_scene_pass_active;
static bool            s_minimap_bake_pass_active;
static bool            s_shadow_map_valid;
static bool            s_shadows_enabled;
static struct {
    unsigned terrain_draws;
    unsigned static_draws;
    unsigned anim_draws;
    size_t terrain_verts;
    size_t static_verts;
    size_t anim_verts;
} s_shadow_caster_stats;
static int             s_water_scene_clip_mode;
static NSUInteger      s_frame_sample_count;
static bool            s_frame_inflight_reserved;
static bool            s_capture_env_checked;
static bool            s_capture_requested;
static bool            s_capture_active;
static bool            s_capture_done;
static unsigned        s_capture_present_count;
static unsigned        s_capture_start_present;
static unsigned        s_capture_present_span;
static unsigned        s_capture_stop_present;
static char            s_capture_output_path[PATH_MAX];
static bool            s_raw_material_debug_env_checked;
static bool            s_raw_material_debug_enabled;
static bool            s_msaa_env_checked;
static NSUInteger      s_requested_sample_count;
static bool            s_shadow_screen_probe_env_checked;
static bool            s_shadow_screen_probe_enabled;
static uint32_t        s_shadow_screen_probe_x;
static uint32_t        s_shadow_screen_probe_y;
static uint32_t        s_shadow_screen_probe_radius;
static char            s_shadow_screen_probe_output_path[PATH_MAX];
static bool            s_water_reflection_dump_pending;
static NSUInteger      s_water_reflection_dump_width;
static NSUInteger      s_water_reflection_dump_height;
static NSUInteger      s_water_reflection_dump_bpr;
static char            s_water_reflection_dump_path[PATH_MAX];
static uint32_t        s_curr_anim_uid;
static bool            s_have_anim_uid;
static matrix_float4x4 s_curr_anim_normal_transform;
static bool            s_have_anim_normal_transform;
static vector_float2   s_map_pos;
static vector_float2   s_map_tile_world_size;
static vector_uint2    s_map_chunk_size;
static vector_uint2    s_map_tiles_per_chunk;
static uint32_t        s_terrain_texture_count;
static float           s_water_move_factor;
static uint32_t        s_water_prev_frame_tick;

#define METAL_MAX_JOINTS 256
#define METAL_MINIMAP_RES 1024
#define METAL_DEFAULT_MSAA_SAMPLES 4
#define METAL_MAX_MSAA_SAMPLES 8
#define METAL_LIGHT_EXTRA_HEIGHT 300.0f
#define METAL_SHADOW_OWNER_TERRAIN_UID 0xffffffffu
#define METAL_SHADOW_OWNER_UNKNOWN_UID 0xfffffffeu
#define METAL_WATER_LEVEL (-1.0f * Y_COORDS_PER_TILE + 2.0f)
#define METAL_WATER_WAVE_SPEED 0.015f
#define METAL_WATER_DUDV_PATH "assets/water_textures/dudvmap.png"
#define METAL_WATER_NORMAL_PATH "assets/water_textures/normalmap.png"
#define METAL_HEIGHT_MAP_RES 2048
#define METAL_SPLAT_MAP_RES 1024
#define METAL_WATER_CLIP_NONE 0
#define METAL_WATER_CLIP_KEEP_BELOW 1
#define METAL_WATER_CLIP_KEEP_ABOVE 2
#define METAL_MAX_SPRITES 1024
#define METAL_MAX_SPRITE_DRAWS 512

static const char *s_ui_shader_source =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct UIUniforms {\n"
"    float2 view_size;\n"
"};\n"
"struct VertexIn {\n"
"    float2 position [[attribute(0)]];\n"
"    float2 uv [[attribute(1)]];\n"
"    float4 color [[attribute(2)]];\n"
"};\n"
"struct VertexOut {\n"
"    float4 position [[position]];\n"
"    float2 uv;\n"
"    float4 color;\n"
"};\n"
"vertex VertexOut ui_vertex(VertexIn in [[stage_in]], constant UIUniforms &uniforms [[buffer(1)]]) {\n"
"    VertexOut out;\n"
"    float2 ndc;\n"
"    ndc.x = (in.position.x / uniforms.view_size.x) * 2.0 - 1.0;\n"
"    ndc.y = 1.0 - (in.position.y / uniforms.view_size.y) * 2.0;\n"
"    out.position = float4(ndc, 0.0, 1.0);\n"
"    out.uv = in.uv;\n"
"    out.color = in.color;\n"
"    return out;\n"
"}\n"
"fragment float4 ui_fragment(VertexOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler tex_sampler [[sampler(0)]]) {\n"
"    return in.color * tex.sample(tex_sampler, in.uv);\n"
"}\n";

static const char *s_minimap_shader_source =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct UIUniforms {\n"
"    float2 view_size;\n"
"};\n"
"struct MinimapUniforms {\n"
"    uint2 chunk_size;\n"
"    uint2 tiles_per_chunk;\n"
"    uint fog_enabled;\n"
"};\n"
"struct VertexIn {\n"
"    float2 position [[attribute(0)]];\n"
"    float2 uv [[attribute(1)]];\n"
"    float4 color [[attribute(2)]];\n"
"};\n"
"struct VertexOut {\n"
"    float4 position [[position]];\n"
"    float2 uv;\n"
"    float4 color;\n"
"};\n"
"vertex VertexOut minimap_vertex(VertexIn in [[stage_in]], constant UIUniforms &uniforms [[buffer(1)]]) {\n"
"    VertexOut out;\n"
"    float2 ndc;\n"
"    ndc.x = (in.position.x / uniforms.view_size.x) * 2.0 - 1.0;\n"
"    ndc.y = 1.0 - (in.position.y / uniforms.view_size.y) * 2.0;\n"
"    out.position = float4(ndc, 0.0, 1.0);\n"
"    out.uv = in.uv;\n"
"    out.color = in.color;\n"
"    return out;\n"
"}\n"
"static uint minimap_visbuff_idx(float2 uv, uint2 chunk_size, uint2 tiles_per_chunk) {\n"
"    uint tile_w = max(tiles_per_chunk.x, 1u);\n"
"    uint tile_h = max(tiles_per_chunk.y, 1u);\n"
"    uint total_w = max(chunk_size.x, 1u) * tile_w;\n"
"    uint total_h = max(chunk_size.y, 1u) * tile_h;\n"
"    float2 clamped = clamp(uv, float2(0.0), float2(0.999999));\n"
"    uint abs_c = min(uint(clamped.x * float(total_w)), total_w - 1u);\n"
"    uint abs_r = min(uint(clamped.y * float(total_h)), total_h - 1u);\n"
"    uint chunk_c = abs_c / tile_w;\n"
"    uint chunk_r = abs_r / tile_h;\n"
"    uint tile_c = abs_c - chunk_c * tile_w;\n"
"    uint tile_r = abs_r - chunk_r * tile_h;\n"
"    uint tiles_per_chunk_count = tile_w * tile_h;\n"
"    return chunk_r * (chunk_size.x * tiles_per_chunk_count)\n"
"         + chunk_c * tiles_per_chunk_count\n"
"         + tile_r * tile_w\n"
"         + tile_c;\n"
"}\n"
"fragment float4 minimap_fragment(VertexOut in [[stage_in]], constant MinimapUniforms &uniforms [[buffer(1)]], device const uchar *fog_buff [[buffer(2)]], texture2d<float> tex [[texture(0)]], sampler tex_sampler [[sampler(0)]]) {\n"
"    float4 color = in.color * tex.sample(tex_sampler, in.uv);\n"
"    if(uniforms.fog_enabled == 0u || uniforms.chunk_size.x == 0u || uniforms.chunk_size.y == 0u)\n"
"        return color;\n"
"    uint state = fog_buff[minimap_visbuff_idx(in.uv, uniforms.chunk_size, uniforms.tiles_per_chunk)];\n"
"    if(state == 0u)\n"
"        return float4(0.0, 0.0, 0.0, color.a);\n"
"    if(state == 1u)\n"
"        color.rgb *= 0.5;\n"
"    return color;\n"
"}\n";

static const char *s_sprite_shader_source =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct SpriteUniforms {\n"
"    float4x4 view;\n"
"    float4x4 projection;\n"
"    float3 view_dir;\n"
"    uint sprite_nrows;\n"
"    uint sprite_ncols;\n"
"};\n"
"struct SpriteDesc {\n"
"    packed_float3 ws_pos;\n"
"    float pad0;\n"
"    packed_float2 ws_size;\n"
"    uint frame_idx;\n"
"    float pad1;\n"
"};\n"
"struct SpriteOut {\n"
"    float4 position [[position]];\n"
"    float2 uv;\n"
"    uint frame [[flat]];\n"
"};\n"
"vertex SpriteOut sprite_vertex(uint vid [[vertex_id]], uint iid [[instance_id]], constant SpriteUniforms &uniforms [[buffer(0)]], device const SpriteDesc *sprites [[buffer(1)]]) {\n"
"    constexpr float3 quad_pos[6] = {\n"
"        float3(-1.0, -1.0, 0.0), float3(-1.0,  1.0, 0.0), float3( 1.0,  1.0, 0.0),\n"
"        float3( 1.0,  1.0, 0.0), float3( 1.0, -1.0, 0.0), float3(-1.0, -1.0, 0.0)\n"
"    };\n"
"    constexpr float2 quad_uv[6] = {\n"
"        float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),\n"
"        float2(0.0, 1.0), float2(0.0, 0.0), float2(1.0, 0.0)\n"
"    };\n"
"    SpriteDesc desc = sprites[iid];\n"
"    float3 view_dir = normalize(uniforms.view_dir);\n"
"    float3 xz = float3(view_dir.z, 0.0, -view_dir.x);\n"
"    if(dot(xz, xz) < 0.00001)\n"
"        xz = float3(1.0, 0.0, 0.0);\n"
"    float3 cam_up = normalize(cross(view_dir, xz));\n"
"    float3 cam_right = normalize(cross(view_dir, cam_up));\n"
"    float3 center = float3(desc.ws_pos);\n"
"    float2 size = float2(desc.ws_size);\n"
"    float3 pos = center\n"
"        + cam_right * quad_pos[vid].x * (size.x * 0.5)\n"
"        + cam_up * quad_pos[vid].y * (size.y * 0.5);\n"
"    SpriteOut out;\n"
"    out.position = uniforms.projection * uniforms.view * float4(pos, 1.0);\n"
"    out.uv = quad_uv[vid];\n"
"    out.frame = desc.frame_idx;\n"
"    return out;\n"
"}\n"
"static float4 sprite_texture(texture2d<float> sprite_sheet, sampler sprite_sampler, uint frame_idx, uint nrows, uint ncols, float2 uv) {\n"
"    nrows = max(nrows, 1u);\n"
"    ncols = max(ncols, 1u);\n"
"    uint row = frame_idx / ncols;\n"
"    row = (nrows - 1u) - min(row, nrows - 1u);\n"
"    uint col = frame_idx % ncols;\n"
"    float width = float(sprite_sheet.get_width());\n"
"    float height = float(sprite_sheet.get_height());\n"
"    float row_height = floor(height / float(nrows));\n"
"    float col_width = floor(width / float(ncols));\n"
"    float u_offset = float(col) * col_width;\n"
"    float v_offset = float(row) * row_height;\n"
"    float2 sample_uv = float2((u_offset + uv.x * col_width) / width,\n"
"                              (v_offset + uv.y * row_height) / height);\n"
"    return sprite_sheet.sample(sprite_sampler, sample_uv);\n"
"}\n"
"fragment float4 sprite_fragment(SpriteOut in [[stage_in]], constant SpriteUniforms &uniforms [[buffer(0)]], texture2d<float> sprite_sheet [[texture(0)]], sampler sprite_sampler [[sampler(0)]]) {\n"
"    float4 color = sprite_texture(sprite_sheet, sprite_sampler, in.frame, uniforms.sprite_nrows, uniforms.sprite_ncols, in.uv);\n"
"    if(color.a <= 0.5)\n"
"        discard_fragment();\n"
"    return color;\n"
"}\n";

static const char *s_terrain_shader_source =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"static uint float_bits(float v) { return as_type<uint>(v); }\n"
"static void shadow_screen_probe_write(float4 frag_pos, float4 light_space_pos, float3 world_pos, uint material_idx, uint surface_kind, float bias, texture2d<uint> owner_map, depth2d<float> shadow_map, sampler shadow_sampler, device atomic_uint *probe_out, constant uint4 &probe_params) {\n"
"    if(probe_params.x == 0u) return;\n"
"    int dx = int(frag_pos.x) - int(probe_params.y);\n"
"    int dy = int(frag_pos.y) - int(probe_params.z);\n"
"    int radius = int(probe_params.w);\n"
"    if(abs(dx) > radius || abs(dy) > radius) return;\n"
"    float w = max(abs(light_space_pos.w), 0.0001);\n"
"    float2 proj_ndc = light_space_pos.xy / w;\n"
"    float2 proj_xy = float2(proj_ndc.x * 0.5 + 0.5, 0.5 - proj_ndc.y * 0.5);\n"
"    float proj_z = light_space_pos.z / w;\n"
"    uint valid = (proj_xy.x >= 0.0 && proj_xy.y >= 0.0 && proj_xy.x <= 1.0 && proj_xy.y <= 1.0 && proj_z >= 0.0 && proj_z <= 0.95) ? 1u : 0u;\n"
"    uint2 dims = uint2(owner_map.get_width(), owner_map.get_height());\n"
"    uint2 texel = uint2(clamp(proj_xy, float2(0.0), float2(0.999999)) * float2(dims));\n"
"    uint owner = valid ? owner_map.read(texel).r : 0u;\n"
"    float closest_depth = valid ? shadow_map.sample(shadow_sampler, proj_xy) : 1.0;\n"
"    atomic_store_explicit(&probe_out[0], 1u, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[1], uint(frag_pos.x), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[2], uint(frag_pos.y), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[3], texel.x, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[4], texel.y, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[5], owner, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[6], surface_kind, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[7], valid, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[8], float_bits(proj_z), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[9], float_bits(closest_depth), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[10], float_bits(world_pos.x), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[11], float_bits(world_pos.y), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[12], float_bits(world_pos.z), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[13], material_idx, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[14], float_bits(proj_z - bias), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[15], (valid && (proj_z - bias > closest_depth)) ? 1u : 0u, memory_order_relaxed);\n"
"}\n"
"struct TerrainVertexIn {\n"
"    float3 position [[attribute(0)]];\n"
"    float2 uv [[attribute(1)]];\n"
"    float3 normal [[attribute(2)]];\n"
"    int material_idx [[attribute(3)]];\n"
"    uchar blend_mode [[attribute(4)]];\n"
"    uchar no_bump_map [[attribute(5)]];\n"
"    ushort mid_indices [[attribute(6)]];\n"
"    uint2 c1_indices [[attribute(7)]];\n"
"    uint2 c2_indices [[attribute(8)]];\n"
"    uint tb_indices [[attribute(9)]];\n"
"    uint lr_indices [[attribute(10)]];\n"
"    uint wang_index [[attribute(11)]];\n"
"};\n"
"struct TerrainUniforms {\n"
"    float4x4 model;\n"
"    float4x4 view;\n"
"    float4x4 proj;\n"
"    float4 view_pos;\n"
"    float2 map_pos;\n"
"    float2 tile_world_size;\n"
"    uint2 chunk_size;\n"
"    uint2 tiles_per_chunk;\n"
"    float4 terrain_params;\n"
"    float4 water_params;\n"
"    float4 ambient_color;\n"
"    float4 light_color;\n"
"    float4 light_pos;\n"
"    float4x4 light_space_transform;\n"
"    float4 shadow_params;\n"
"    float4 clip_params;\n"
"};\n"
"struct TerrainVertexOut {\n"
"    float4 position [[position]];\n"
"    float3 normal;\n"
"    uint material_idx [[flat]];\n"
"    uint blend_mode [[flat]];\n"
"    uint no_bump_map [[flat]];\n"
"    uint mid_indices [[flat]];\n"
"    uint2 c1_indices [[flat]];\n"
"    uint2 c2_indices [[flat]];\n"
"    uint tb_indices [[flat]];\n"
"    uint lr_indices [[flat]];\n"
"    uint wang_index [[flat]];\n"
"    float2 uv;\n"
"    float3 world_pos;\n"
"    float2 world_xz;\n"
"    float4 light_space_pos;\n"
"};\n"
"static float3 terrain_material_color(uint idx) {\n"
"    switch (idx % 8u) {\n"
"    case 0u: return float3(0.36, 0.55, 0.26);\n"
"    case 1u: return float3(0.44, 0.62, 0.30);\n"
"    case 2u: return float3(0.58, 0.54, 0.32);\n"
"    case 3u: return float3(0.41, 0.45, 0.27);\n"
"    case 4u: return float3(0.64, 0.60, 0.37);\n"
"    case 5u: return float3(0.29, 0.43, 0.24);\n"
"    case 6u: return float3(0.50, 0.49, 0.31);\n"
"    default: return float3(0.48, 0.57, 0.34);\n"
"    }\n"
"}\n"
"vertex TerrainVertexOut terrain_vertex(TerrainVertexIn in [[stage_in]], constant TerrainUniforms &uniforms [[buffer(1)]]) {\n"
"    TerrainVertexOut out;\n"
"    float4 world_pos = uniforms.model * float4(in.position, 1.0);\n"
"    out.position = uniforms.proj * uniforms.view * world_pos;\n"
"    out.normal = normalize(in.normal);\n"
"    out.material_idx = (uint)max(in.material_idx, 0);\n"
"    out.blend_mode = (uint)in.blend_mode;\n"
"    out.no_bump_map = (uint)in.no_bump_map;\n"
"    out.mid_indices = (uint)in.mid_indices;\n"
"    out.c1_indices = in.c1_indices;\n"
"    out.c2_indices = in.c2_indices;\n"
"    out.tb_indices = in.tb_indices;\n"
"    out.lr_indices = in.lr_indices;\n"
"    out.wang_index = min(in.wang_index, 7u);\n"
"    out.uv = in.uv;\n"
"    out.world_pos = world_pos.xyz;\n"
"    out.world_xz = world_pos.xz;\n"
"    out.light_space_pos = uniforms.light_space_transform * float4(out.world_pos, 1.0);\n"
"    return out;\n"
"}\n"
"static float terrain_fog_factor(uint state) {\n"
"    if(state == 0u) return 0.0;\n"
"    if(state == 1u) return 0.5;\n"
"    return 1.0;\n"
"}\n"
"static uint terrain_tile_buffer_idx(uint2 chunk_size, uint2 tiles_per_chunk, int chunk_r, int chunk_c, int tile_r, int tile_c) {\n"
"    int chunk_w = int(chunk_size.x);\n"
"    int chunk_h = int(chunk_size.y);\n"
"    int tile_w = int(tiles_per_chunk.x);\n"
"    int tile_h = int(tiles_per_chunk.y);\n"
"    chunk_r = clamp(chunk_r, 0, chunk_h - 1);\n"
"    chunk_c = clamp(chunk_c, 0, chunk_w - 1);\n"
"    tile_r = clamp(tile_r, 0, tile_h - 1);\n"
"    tile_c = clamp(tile_c, 0, tile_w - 1);\n"
"    uint tiles_per_chunk_count = tiles_per_chunk.x * tiles_per_chunk.y;\n"
"    return uint(chunk_r) * (chunk_size.x * tiles_per_chunk_count)\n"
"         + uint(chunk_c) * tiles_per_chunk_count\n"
"         + uint(tile_r) * tiles_per_chunk.x\n"
"         + uint(tile_c);\n"
"}\n"
"static uint terrain_relative_tile_buffer_idx(uint2 chunk_size, uint2 tiles_per_chunk, int chunk_r, int chunk_c, int tile_r, int tile_c, int dr, int dc) {\n"
"    int tile_w = int(tiles_per_chunk.x);\n"
"    int tile_h = int(tiles_per_chunk.y);\n"
"    int abs_r = (chunk_r * tile_h) + tile_r + dr;\n"
"    int abs_c = (chunk_c * tile_w) + tile_c + dc;\n"
"    abs_r = clamp(abs_r, 0, int(chunk_size.y) * tile_h - 1);\n"
"    abs_c = clamp(abs_c, 0, int(chunk_size.x) * tile_w - 1);\n"
"    return terrain_tile_buffer_idx(chunk_size, tiles_per_chunk,\n"
"        abs_r / tile_h, abs_c / tile_w, abs_r % tile_h, abs_c % tile_w);\n"
"}\n"
"static float terrain_bilinear_unit(float tl, float tr, float bl, float br, float2 coord) {\n"
"    return bl * (1.0 - coord.x) * (1.0 - coord.y)\n"
"         + br * coord.x * (1.0 - coord.y)\n"
"         + tl * (1.0 - coord.x) * coord.y\n"
"         + tr * coord.x * coord.y;\n"
"}\n"
"static float terrain_fog_tint(device const uchar *fog_buff, uint2 chunk_size, uint2 tiles_per_chunk, int chunk_r, int chunk_c, int tile_r, int tile_c, float2 uv) {\n"
"    float c = terrain_fog_factor(fog_buff[terrain_tile_buffer_idx(chunk_size, tiles_per_chunk, chunk_r, chunk_c, tile_r, tile_c)]);\n"
"    float tl = terrain_fog_factor(fog_buff[terrain_relative_tile_buffer_idx(chunk_size, tiles_per_chunk, chunk_r, chunk_c, tile_r, tile_c, -1, -1)]);\n"
"    float tr = terrain_fog_factor(fog_buff[terrain_relative_tile_buffer_idx(chunk_size, tiles_per_chunk, chunk_r, chunk_c, tile_r, tile_c, -1,  1)]);\n"
"    float l = terrain_fog_factor(fog_buff[terrain_relative_tile_buffer_idx(chunk_size, tiles_per_chunk, chunk_r, chunk_c, tile_r, tile_c,  0, -1)]);\n"
"    float r = terrain_fog_factor(fog_buff[terrain_relative_tile_buffer_idx(chunk_size, tiles_per_chunk, chunk_r, chunk_c, tile_r, tile_c,  0,  1)]);\n"
"    float bl = terrain_fog_factor(fog_buff[terrain_relative_tile_buffer_idx(chunk_size, tiles_per_chunk, chunk_r, chunk_c, tile_r, tile_c,  1, -1)]);\n"
"    float br = terrain_fog_factor(fog_buff[terrain_relative_tile_buffer_idx(chunk_size, tiles_per_chunk, chunk_r, chunk_c, tile_r, tile_c,  1,  1)]);\n"
"    float t = terrain_fog_factor(fog_buff[terrain_relative_tile_buffer_idx(chunk_size, tiles_per_chunk, chunk_r, chunk_c, tile_r, tile_c, -1,  0)]);\n"
"    float b = terrain_fog_factor(fog_buff[terrain_relative_tile_buffer_idx(chunk_size, tiles_per_chunk, chunk_r, chunk_c, tile_r, tile_c,  1,  0)]);\n"
"    float tl_corner = (c + t + l + tl) * 0.25;\n"
"    float tr_corner = (c + t + r + tr) * 0.25;\n"
"    float bl_corner = (c + l + b + bl) * 0.25;\n"
"    float br_corner = (c + r + b + br) * 0.25;\n"
"    return terrain_bilinear_unit(tl_corner, tr_corner, bl_corner, br_corner, uv);\n"
"}\n"
"static bool clip_water_world_y(float world_y, float4 clip_params) {\n"
"    uint mode = uint(clip_params.x + 0.5);\n"
"    if(mode == 1u) return world_y > clip_params.z;\n"
"    if(mode == 2u) return world_y < clip_params.z;\n"
"    return false;\n"
"}\n"
"static float shadow_factor(float4 light_space_pos, float3 normal, float3 light_dir, depth2d<float> shadow_map, sampler shadow_sampler, float base_bias, float texel_radius) {\n"
"    float w = max(abs(light_space_pos.w), 0.0001);\n"
"    float2 proj_ndc = light_space_pos.xy / w;\n"
"    float2 proj_xy = float2(proj_ndc.x * 0.5 + 0.5, 0.5 - proj_ndc.y * 0.5);\n"
"    float  proj_z  = light_space_pos.z / w;\n"
"    if(proj_xy.x < 0.0 || proj_xy.y < 0.0\n"
"       || proj_xy.x > 1.0 || proj_xy.y > 1.0\n"
"       || proj_z < 0.0 || proj_z > 0.95) return 0.0;\n"
"    float current_depth = proj_z;\n"
"    float closest_depth = shadow_map.sample(shadow_sampler, proj_xy);\n"
"    float shadow = (current_depth - base_bias > closest_depth) ? 1.0 : 0.0;\n"
"    float visibility = 1.0;\n"
"    float2 poisson0 = float2(-0.94201624,  0.39906216);\n"
"    float2 poisson1 = float2( 0.94558609,  0.76890725);\n"
"    float2 poisson2 = float2(-0.094184101,  0.92938870);\n"
"    float2 poisson3 = float2( 0.34495938, -0.29387760);\n"
"    float depth0 = shadow_map.sample(shadow_sampler, proj_xy + poisson0 / 256.0);\n"
"    float depth1 = shadow_map.sample(shadow_sampler, proj_xy + poisson1 / 256.0);\n"
"    float depth2 = shadow_map.sample(shadow_sampler, proj_xy + poisson2 / 256.0);\n"
"    float depth3 = shadow_map.sample(shadow_sampler, proj_xy + poisson3 / 256.0);\n"
"    if(current_depth - base_bias <= depth0) visibility -= 0.25;\n"
"    if(current_depth - base_bias <= depth1) visibility -= 0.25;\n"
"    if(current_depth - base_bias <= depth2) visibility -= 0.25;\n"
"    if(current_depth - base_bias <= depth3) visibility -= 0.25;\n"
"    return shadow * visibility;\n"
"}\n"
"static float terrain_heightmap_fetch(device const float *height_map, int idx, int count) {\n"
"    return height_map[clamp(idx, 0, count - 1)];\n"
"}\n"
"static float3 terrain_normal_at_pos(float3 ws_pos, constant TerrainUniforms &uniforms, device const float *height_map) {\n"
"    float2 tile_world = max(uniforms.tile_world_size, float2(1.0, 1.0));\n"
"    float chunk_x_dist = tile_world.x * float(max(uniforms.tiles_per_chunk.x, 1u));\n"
"    float chunk_z_dist = tile_world.y * float(max(uniforms.tiles_per_chunk.y, 1u));\n"
"    int chunk_c = int(fabs(uniforms.map_pos.x - ws_pos.x) / chunk_x_dist);\n"
"    int chunk_r = int(fabs(uniforms.map_pos.y - ws_pos.z) / chunk_z_dist);\n"
"    float chunk_base_x = uniforms.map_pos.x - (float(chunk_c) * chunk_x_dist);\n"
"    float chunk_base_z = uniforms.map_pos.y + (float(chunk_r) * chunk_z_dist);\n"
"    float percentu = clamp((chunk_base_x - ws_pos.x) / chunk_x_dist, 0.0, 1.0);\n"
"    float percentv = clamp((ws_pos.z - chunk_base_z) / chunk_z_dist, 0.0, 1.0);\n"
"    int res = max(int(uniforms.terrain_params.y + 0.5), 2);\n"
"    int count = res * res;\n"
"    int buffx = int(percentu * float(res - 1));\n"
"    int buffy = int(percentv * float(res - 1));\n"
"    int backx = (buffx - 1) % res;\n"
"    int frontx = (buffx + 1) % res;\n"
"    int backz = (buffy - 1) % res;\n"
"    int back_idx = backx * res + buffy;\n"
"    int front_idx = frontx * res + buffy;\n"
"    int backz_idx = buffx * res + backz;\n"
"    float x0 = terrain_heightmap_fetch(height_map, back_idx, count);\n"
"    float x1 = terrain_heightmap_fetch(height_map, front_idx, count);\n"
"    float z0 = terrain_heightmap_fetch(height_map, backz_idx, count);\n"
"    float z1 = terrain_heightmap_fetch(height_map, front_idx, count);\n"
"    float2 slope = float2((x1 - x0) * 0.5, (z1 - z0) * 0.5);\n"
"    float len = length(slope);\n"
"    float2 xz_normal = (len > 0.00001) ? (slope / len) : float2(0.0, 0.0);\n"
"    return float3(xz_normal.x, 1.0, xz_normal.y);\n"
"}\n"
"static uint terrain_packed_idx(uint packed, uint shift) {\n"
"    return (packed >> shift) & 0xffu;\n"
"}\n"
"static float4 terrain_texture_val_raw(uint mat_idx, uint wang_idx, float2 uv, uint texture_count, texture2d_array<float> terrain_textures, sampler terrain_sampler) {\n"
"    uint layer = (mat_idx * 8u) + min(wang_idx, 7u);\n"
"    if(layer < texture_count) {\n"
"        return terrain_textures.sample(terrain_sampler, float2(uv.x, 1.0 - uv.y), layer, bias(-0.5));\n"
"    }\n"
"    if(mat_idx < texture_count) {\n"
"        return terrain_textures.sample(terrain_sampler, float2(uv.x, 1.0 - uv.y), mat_idx, bias(-0.5));\n"
"    }\n"
"    return float4(terrain_material_color(mat_idx), 1.0);\n"
"}\n"
"static float terrain_splat_alpha_at_pos(float3 ws_pos, constant TerrainUniforms &uniforms, device const float *splat_map) {\n"
"    float chunk_x_dist = uniforms.tile_world_size.x * float(max(uniforms.tiles_per_chunk.x, 1u));\n"
"    float chunk_z_dist = uniforms.tile_world_size.y * float(max(uniforms.tiles_per_chunk.y, 1u));\n"
"    int chunk_c = int(fabs(uniforms.map_pos.x - ws_pos.x) / chunk_x_dist);\n"
"    int chunk_r = int(fabs(uniforms.map_pos.y - ws_pos.z) / chunk_z_dist);\n"
"    float chunk_base_x = uniforms.map_pos.x - (float(chunk_c) * chunk_x_dist);\n"
"    float chunk_base_z = uniforms.map_pos.y + (float(chunk_r) * chunk_z_dist);\n"
"    float percentu = clamp((chunk_base_x - ws_pos.x) / chunk_x_dist, 0.0, 1.0);\n"
"    float percentv = clamp((ws_pos.z - chunk_base_z) / chunk_z_dist, 0.0, 1.0);\n"
"    int res = max(int(uniforms.terrain_params.w + 0.5), 2);\n"
"    int count = res * res;\n"
"    int buffx = int(percentu * float(res - 1));\n"
"    int buffy = int(percentv * float(res - 1));\n"
"    int idx = clamp(buffx * res + buffy, 0, count - 1);\n"
"    return clamp(splat_map[idx] + 0.2, 0.0, 1.0);\n"
"}\n"
"static float4 terrain_texture_val(uint mat_idx, uint wang_idx, float2 uv, float3 world_pos, constant TerrainUniforms &uniforms, device const float *splat_map, device const int *splats, uint texture_count, texture2d_array<float> terrain_textures, sampler terrain_sampler) {\n"
"    float4 tex_color = terrain_texture_val_raw(mat_idx, wang_idx, uv, texture_count, terrain_textures, terrain_sampler);\n"
"    if(uniforms.terrain_params.w <= 0.5 || mat_idx >= 256u) return tex_color;\n"
"    int splat_idx = splats[mat_idx];\n"
"    if(splat_idx < 0) return tex_color;\n"
"    float splat_alpha = terrain_splat_alpha_at_pos(world_pos, uniforms, splat_map);\n"
"    float4 splat_color = terrain_texture_val_raw(uint(splat_idx), wang_idx, uv, texture_count, terrain_textures, terrain_sampler);\n"
"    return mix(splat_color, tex_color, 1.0 - splat_alpha);\n"
"}\n"
"static float4 terrain_mixed_texture_val(uint2 adjacency_mats, uint wang_idx, float2 uv, float3 world_pos, constant TerrainUniforms &uniforms, device const float *splat_map, device const int *splats, uint texture_count, texture2d_array<float> terrain_textures, sampler terrain_sampler) {\n"
"    float4 ret = float4(0.0);\n"
"    for(uint i = 0u; i < 2u; i++) {\n"
"        uint packed = (i == 0u) ? adjacency_mats.x : adjacency_mats.y;\n"
"        for(uint j = 0u; j < 4u; j++) {\n"
"            ret += terrain_texture_val(terrain_packed_idx(packed, j * 8u), wang_idx, uv, world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler) * 0.125;\n"
"        }\n"
"    }\n"
"    return ret;\n"
"}\n"
"static float4 terrain_bilinear(float4 q11, float4 q12, float4 q21, float4 q22, float x1, float x2, float y1, float y2, float x, float y) {\n"
"    float x2x1 = x2 - x1;\n"
"    float y2y1 = y2 - y1;\n"
"    return (q11 * (x2 - x) * (y2 - y) +\n"
"            q21 * (x - x1) * (y2 - y) +\n"
"            q12 * (x2 - x) * (y - y1) +\n"
"            q22 * (x - x1) * (y - y1)) / (x2x1 * y2y1);\n"
"}\n"
"static float4 terrain_blended_texture_val(TerrainVertexOut in, constant TerrainUniforms &uniforms, device const float *splat_map, device const int *splats, uint texture_count, texture2d_array<float> terrain_textures, sampler terrain_sampler) {\n"
"    bool bot = (in.uv.x > in.uv.y) && (1.0 - in.uv.x > in.uv.y);\n"
"    bool top = (in.uv.x < in.uv.y) && (1.0 - in.uv.x < in.uv.y);\n"
"    bool left = (in.uv.x < in.uv.y) && (1.0 - in.uv.x > in.uv.y);\n"
"    bool right = (in.uv.x > in.uv.y) && (1.0 - in.uv.x < in.uv.y);\n"
"    bool left_half = in.uv.x < 0.5;\n"
"    bool bot_half = in.uv.y < 0.5;\n"
"    float4 color1 = terrain_mixed_texture_val(in.c1_indices, in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler);\n"
"    float4 color2 = terrain_mixed_texture_val(in.c2_indices, in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler);\n"
"    float4 tile_color = mix(\n"
"        terrain_texture_val(terrain_packed_idx(in.mid_indices, 0u), in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler),\n"
"        terrain_texture_val(terrain_packed_idx(in.mid_indices, 8u), in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler),\n"
"        0.5);\n"
"    float4 left_center_color = mix(\n"
"        terrain_texture_val(terrain_packed_idx(in.lr_indices, 16u), in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler),\n"
"        terrain_texture_val(terrain_packed_idx(in.lr_indices, 24u), in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler),\n"
"        0.5);\n"
"    float4 bot_center_color = mix(\n"
"        terrain_texture_val(terrain_packed_idx(in.tb_indices, 0u), in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler),\n"
"        terrain_texture_val(terrain_packed_idx(in.tb_indices, 8u), in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler),\n"
"        0.5);\n"
"    float4 right_center_color = mix(\n"
"        terrain_texture_val(terrain_packed_idx(in.lr_indices, 0u), in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler),\n"
"        terrain_texture_val(terrain_packed_idx(in.lr_indices, 8u), in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler),\n"
"        0.5);\n"
"    float4 top_center_color = mix(\n"
"        terrain_texture_val(terrain_packed_idx(in.tb_indices, 16u), in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler),\n"
"        terrain_texture_val(terrain_packed_idx(in.tb_indices, 24u), in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler),\n"
"        0.5);\n"
"    if(top) {\n"
"        if(left_half) return terrain_bilinear(left_center_color, color1, tile_color, top_center_color, 0.0, 0.5, 0.5, 1.0, in.uv.x, in.uv.y);\n"
"        return terrain_bilinear(tile_color, top_center_color, right_center_color, color2, 0.5, 1.0, 0.5, 1.0, in.uv.x, in.uv.y);\n"
"    }\n"
"    if(bot) {\n"
"        if(left_half) return terrain_bilinear(color1, left_center_color, bot_center_color, tile_color, 0.0, 0.5, 0.0, 0.5, in.uv.x, in.uv.y);\n"
"        return terrain_bilinear(bot_center_color, tile_color, color2, right_center_color, 0.5, 1.0, 0.0, 0.5, in.uv.x, in.uv.y);\n"
"    }\n"
"    if(left) {\n"
"        if(bot_half) return terrain_bilinear(color1, left_center_color, bot_center_color, tile_color, 0.0, 0.5, 0.0, 0.5, in.uv.x, in.uv.y);\n"
"        return terrain_bilinear(left_center_color, color2, tile_color, top_center_color, 0.0, 0.5, 0.5, 1.0, in.uv.x, in.uv.y);\n"
"    }\n"
"    if(right) {\n"
"        if(bot_half) return terrain_bilinear(bot_center_color, tile_color, color1, right_center_color, 0.5, 1.0, 0.0, 0.5, in.uv.x, in.uv.y);\n"
"        return terrain_bilinear(tile_color, top_center_color, right_center_color, color2, 0.5, 1.0, 0.5, 1.0, in.uv.x, in.uv.y);\n"
"    }\n"
"    return tile_color;\n"
"}\n"
"fragment float4 terrain_fragment(TerrainVertexOut in [[stage_in]], constant TerrainUniforms &uniforms [[buffer(1)]], device const uchar *fog_buff [[buffer(2)]], device const uchar *water_buff [[buffer(3)]], device const float *height_map [[buffer(4)]], device atomic_uint *shadow_probe [[buffer(5)]], constant uint4 &shadow_probe_params [[buffer(6)]], device const float *splat_map [[buffer(7)]], device const int *splats [[buffer(8)]], texture2d_array<float> terrain_textures [[texture(0)]], texture2d<float> water_dudv_map [[texture(1)]], texture2d<float> water_normal_map [[texture(2)]], depth2d<float> shadow_map [[texture(3)]], texture2d<uint> shadow_owner_map [[texture(4)]], sampler terrain_sampler [[sampler(0)]], sampler shadow_sampler [[sampler(1)]], sampler water_sampler [[sampler(2)]]) {\n"
"    if(clip_water_world_y(in.world_pos.y, uniforms.clip_params)) discard_fragment();\n"
"    float3 normal = normalize(in.normal);\n"
"    if(in.no_bump_map == 0u && uniforms.terrain_params.z > 0.5) {\n"
"        float3 heightmap_normal = terrain_normal_at_pos(in.world_pos, uniforms, height_map);\n"
"        normal = normalize(in.normal + heightmap_normal * 0.075);\n"
"    }\n"
"    float3 light_dir = normalize(uniforms.light_pos.xyz);\n"
"    float diffuse = max(dot(normal, light_dir), 0.0);\n"
"    float height = in.world_pos.y / 4.0;\n"
"    float3 ambient = (0.70 + height * 0.03) * uniforms.ambient_color.xyz;\n"
"    float3 color = terrain_material_color(in.material_idx);\n"
"    float fog_factor = 1.0;\n"
"    float minimap_water = 0.0;\n"
"    if(uniforms.chunk_size.x > 0u && uniforms.chunk_size.y > 0u) {\n"
"        float field_w = uniforms.tile_world_size.x * uniforms.tiles_per_chunk.x;\n"
"        float field_h = uniforms.tile_world_size.y * uniforms.tiles_per_chunk.y;\n"
"        int chunk_c = int(fabs(uniforms.map_pos.x - in.world_xz.x) / field_w);\n"
"        int chunk_r = int(fabs(uniforms.map_pos.y - in.world_xz.y) / field_h);\n"
"        chunk_c = clamp(chunk_c, 0, int(uniforms.chunk_size.x) - 1);\n"
"        chunk_r = clamp(chunk_r, 0, int(uniforms.chunk_size.y) - 1);\n"
"        float chunk_base_x = uniforms.map_pos.x - (chunk_c * field_w);\n"
"        float chunk_base_z = uniforms.map_pos.y + (chunk_r * field_h);\n"
"        int tile_c = int(fabs(chunk_base_x - in.world_xz.x) / uniforms.tile_world_size.x);\n"
"        int tile_r = int(fabs(chunk_base_z - in.world_xz.y) / uniforms.tile_world_size.y);\n"
"        tile_c = clamp(tile_c, 0, int(uniforms.tiles_per_chunk.x) - 1);\n"
"        tile_r = clamp(tile_r, 0, int(uniforms.tiles_per_chunk.y) - 1);\n"
"        uint tiles_per_chunk = uniforms.tiles_per_chunk.x * uniforms.tiles_per_chunk.y;\n"
"        uint fog_idx = uint(chunk_r) * (uniforms.chunk_size.x * tiles_per_chunk)\n"
"                     + uint(chunk_c) * tiles_per_chunk\n"
"                     + uint(tile_r) * uniforms.tiles_per_chunk.x\n"
"                     + uint(tile_c);\n"
"        if(uniforms.water_params.z > 0.5) {\n"
"            fog_factor = terrain_fog_tint(fog_buff, uniforms.chunk_size, uniforms.tiles_per_chunk, chunk_r, chunk_c, tile_r, tile_c, in.uv);\n"
"        }\n"
"        if(uniforms.water_params.y > 1.5 && water_buff[fog_idx] != 0u) {\n"
"            minimap_water = 1.0;\n"
"        }\n"
"    }\n"
"    if(uniforms.terrain_params.x > 0.5) {\n"
"        uint texture_count = uint(uniforms.terrain_params.x + 0.5);\n"
"        float4 texel = (in.blend_mode == 1u)\n"
"            ? terrain_blended_texture_val(in, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler)\n"
"            : terrain_texture_val(in.material_idx, in.wang_index, in.uv, in.world_pos, uniforms, splat_map, splats, texture_count, terrain_textures, terrain_sampler);\n"
"        if(texel.a <= 0.5) discard_fragment();\n"
"        color = texel.xyz * texel.a;\n"
"    }\n"
"    float3 view_dir = normalize(uniforms.view_pos.xyz - in.world_pos);\n"
"    float3 reflect_dir = reflect(-light_dir, normal);\n"
"    float specular = pow(max(dot(view_dir, reflect_dir), 0.0), 2.0);\n"
"    float3 diffuse_lit = uniforms.light_color.xyz * (diffuse * float3(0.90));\n"
"    float3 specular_lit = 0.5 * uniforms.light_color.xyz * (specular * float3(0.10));\n"
"    float3 lit = (ambient + diffuse_lit + specular_lit) * color;\n"
"    /* Water tiles render as terrain in the GL reference (terrain-shadowed.glsl).\n"
"     * The translucent water surface mesh handles deep/shallow tinting on top. */\n"
"    if(uniforms.shadow_params.x > 0.5) {\n"
"        float shadow = shadow_factor(in.light_space_pos, in.normal, light_dir, shadow_map, shadow_sampler, uniforms.shadow_params.y, uniforms.shadow_params.w);\n"
"        lit *= mix(1.0, uniforms.shadow_params.z, shadow);\n"
"    }\n"
"    if(minimap_water > 0.5) {\n"
"        lit = float3(0.01, 0.03, 0.035);\n"
"    }\n"
"    shadow_screen_probe_write(in.position, in.light_space_pos, in.world_pos, in.material_idx, 1u, uniforms.shadow_params.y, shadow_owner_map, shadow_map, shadow_sampler, shadow_probe, shadow_probe_params);\n"
"    return float4(lit * fog_factor, 1.0);\n"
"}\n";

static const char *s_static_mesh_shader_source =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"#define METAL_MAX_MATERIALS 16\n"
"static uint float_bits(float v) { return as_type<uint>(v); }\n"
"static void shadow_screen_probe_write(float4 frag_pos, float4 light_space_pos, float3 world_pos, uint material_idx, uint surface_kind, float bias, texture2d<uint> owner_map, depth2d<float> shadow_map, sampler shadow_sampler, device atomic_uint *probe_out, constant uint4 &probe_params) {\n"
"    if(probe_params.x == 0u) return;\n"
"    int dx = int(frag_pos.x) - int(probe_params.y);\n"
"    int dy = int(frag_pos.y) - int(probe_params.z);\n"
"    int radius = int(probe_params.w);\n"
"    if(abs(dx) > radius || abs(dy) > radius) return;\n"
"    float w = max(abs(light_space_pos.w), 0.0001);\n"
"    float2 proj_ndc = light_space_pos.xy / w;\n"
"    float2 proj_xy = float2(proj_ndc.x * 0.5 + 0.5, 0.5 - proj_ndc.y * 0.5);\n"
"    float proj_z = light_space_pos.z / w;\n"
"    uint valid = (proj_xy.x >= 0.0 && proj_xy.y >= 0.0 && proj_xy.x <= 1.0 && proj_xy.y <= 1.0 && proj_z >= 0.0 && proj_z <= 0.95) ? 1u : 0u;\n"
"    uint2 dims = uint2(owner_map.get_width(), owner_map.get_height());\n"
"    uint2 texel = uint2(clamp(proj_xy, float2(0.0), float2(0.999999)) * float2(dims));\n"
"    uint owner = valid ? owner_map.read(texel).r : 0u;\n"
"    float closest_depth = valid ? shadow_map.sample(shadow_sampler, proj_xy) : 1.0;\n"
"    atomic_store_explicit(&probe_out[0], 1u, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[1], uint(frag_pos.x), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[2], uint(frag_pos.y), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[3], texel.x, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[4], texel.y, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[5], owner, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[6], surface_kind, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[7], valid, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[8], float_bits(proj_z), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[9], float_bits(closest_depth), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[10], float_bits(world_pos.x), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[11], float_bits(world_pos.y), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[12], float_bits(world_pos.z), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[13], material_idx, memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[14], float_bits(proj_z - bias), memory_order_relaxed);\n"
"    atomic_store_explicit(&probe_out[15], (valid && (proj_z - bias > closest_depth)) ? 1u : 0u, memory_order_relaxed);\n"
"}\n"
"struct StaticMeshVertexIn {\n"
"    float3 position [[attribute(0)]];\n"
"    float2 uv [[attribute(1)]];\n"
"    float3 normal [[attribute(2)]];\n"
"    int material_idx [[attribute(3)]];\n"
"};\n"
"struct StaticMeshUniforms {\n"
"    float4x4 model;\n"
"    float4x4 normal_transform;\n"
"    float4x4 view;\n"
"    float4x4 proj;\n"
"    float4x4 light_space_transform;\n"
"    float4 material_diffuse[METAL_MAX_MATERIALS];\n"
"    float4 material_ambient[METAL_MAX_MATERIALS];\n"
"    float4 material_specular[METAL_MAX_MATERIALS];\n"
"    float4 view_pos;\n"
"    float4 ambient_color;\n"
"    float4 light_color;\n"
"    float4 light_pos;\n"
"    float4 effect_params;\n"
"    float4 shadow_params;\n"
"    float4 clip_params;\n"
"};\n"
"struct StaticMeshVertexOut {\n"
"    float4 position [[position]];\n"
"    float3 normal;\n"
"    float2 uv;\n"
"    uint material_idx;\n"
"    float3 world_pos;\n"
"    float4 light_space_pos;\n"
"};\n"
"vertex StaticMeshVertexOut static_mesh_vertex(StaticMeshVertexIn in [[stage_in]], constant StaticMeshUniforms &uniforms [[buffer(1)]]) {\n"
"    StaticMeshVertexOut out;\n"
"    float4 world_pos = uniforms.model * float4(in.position, 1.0);\n"
"    out.position = uniforms.proj * uniforms.view * world_pos;\n"
"    out.normal = normalize((uniforms.normal_transform * float4(in.normal, 0.0)).xyz);\n"
"    out.uv = in.uv;\n"
"    out.material_idx = (uint)max(in.material_idx, 0);\n"
"    out.world_pos = world_pos.xyz;\n"
"    out.light_space_pos = uniforms.light_space_transform * float4(world_pos.xyz, 1.0);\n"
"    return out;\n"
"}\n"
"static float mesh_shadow_factor(float4 light_space_pos, depth2d<float> shadow_map, sampler shadow_sampler, float bias) {\n"
"    float w = max(abs(light_space_pos.w), 0.0001);\n"
"    float2 proj_ndc = light_space_pos.xy / w;\n"
"    float2 proj_xy = float2(proj_ndc.x * 0.5 + 0.5, 0.5 - proj_ndc.y * 0.5);\n"
"    float  proj_z  = light_space_pos.z / w;\n"
"    if(proj_xy.x < 0.0 || proj_xy.y < 0.0\n"
"       || proj_xy.x > 1.0 || proj_xy.y > 1.0\n"
"       || proj_z < 0.0 || proj_z > 0.95) return 0.0;\n"
"    float closest_depth = shadow_map.sample(shadow_sampler, proj_xy);\n"
"    return (proj_z - bias > closest_depth) ? 1.0 : 0.0;\n"
"}\n"
"static bool clip_water_world_y(float world_y, float4 clip_params) {\n"
"    uint mode = uint(clip_params.x + 0.5);\n"
"    if(mode == 1u) return world_y > clip_params.z;\n"
"    if(mode == 2u) return world_y < clip_params.z;\n"
"    return false;\n"
"}\n"
"fragment float4 static_mesh_fragment(StaticMeshVertexOut in [[stage_in]], constant StaticMeshUniforms &uniforms [[buffer(1)]], device atomic_uint *shadow_probe [[buffer(2)]], constant uint4 &shadow_probe_params [[buffer(3)]], texture2d_array<float> material_textures [[texture(0)]], depth2d<float> shadow_map [[texture(1)]], texture2d<uint> shadow_owner_map [[texture(2)]], sampler material_sampler [[sampler(0)]], sampler shadow_sampler [[sampler(1)]]) {\n"
"    if(clip_water_world_y(in.world_pos.y, uniforms.clip_params)) discard_fragment();\n"
"    float3 normal = normalize(in.normal);\n"
"    float3 light_vec = uniforms.light_pos.xyz - in.world_pos;\n"
"    float3 light_dir = light_vec / max(length(light_vec), 0.0001);\n"
"    float diffuse = max(dot(normal, light_dir), 0.0);\n"
"    uint diffuse_idx = min(in.material_idx, uint(METAL_MAX_MATERIALS - 1));\n"
"    float specular_strength = max(max(uniforms.material_specular[diffuse_idx].x, uniforms.material_specular[diffuse_idx].y), uniforms.material_specular[diffuse_idx].z);\n"
"    float3 specular_normal = (uniforms.effect_params.z > 0.5 || specular_strength < 0.75) ? normal : in.normal;\n"
"    float4 tex_rgba = float4(1.0, 1.0, 1.0, 1.0);\n"
"    if(uniforms.effect_params.x > 0.5 && in.material_idx < uint(uniforms.effect_params.x + 0.5)) {\n"
"        tex_rgba = material_textures.sample(material_sampler, in.uv, in.material_idx, bias(-0.5));\n"
"        if(tex_rgba.a <= 0.5) discard_fragment();\n"
"        if(uniforms.effect_params.y > 0.5) return float4(tex_rgba.xyz, tex_rgba.w);\n"
"        tex_rgba.xyz *= tex_rgba.w;\n"
"    }\n"
"    float3 view_dir = normalize(uniforms.view_pos.xyz - in.world_pos);\n"
"    float3 reflect_dir = reflect(-light_dir, specular_normal);\n"
"    float specular = pow(max(dot(view_dir, reflect_dir), 0.0), 2.0);\n"
"    float3 ambient = uniforms.material_ambient[diffuse_idx].xxx * uniforms.ambient_color.xyz;\n"
"    float3 diffuse_lit = uniforms.light_color.xyz * (diffuse * uniforms.material_diffuse[diffuse_idx].xyz);\n"
"    float3 specular_lit = 0.5 * uniforms.light_color.xyz * (specular * uniforms.material_specular[diffuse_idx].xyz);\n"
"    float3 lit = (ambient * 0.55 + diffuse_lit * 1.5 + specular_lit * 1.5) * tex_rgba.xyz;\n"
"    if(uniforms.shadow_params.x > 0.5) {\n"
"        float shadow = mesh_shadow_factor(in.light_space_pos, shadow_map, shadow_sampler, uniforms.shadow_params.y);\n"
"        lit *= mix(1.0, uniforms.shadow_params.z, shadow);\n"
"    }\n"
"    shadow_screen_probe_write(in.position, in.light_space_pos, in.world_pos, in.material_idx, 2u, uniforms.shadow_params.y, shadow_owner_map, shadow_map, shadow_sampler, shadow_probe, shadow_probe_params);\n"
"    return float4(lit, tex_rgba.w);\n"
"}\n";

static const char *s_shadow_depth_shader_source =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct ShadowVertexIn {\n"
"    float3 position [[attribute(0)]];\n"
"};\n"
"struct ShadowUniforms {\n"
"    float4x4 model;\n"
"    float4x4 view;\n"
"    float4x4 proj;\n"
"    uint4 owner_params;\n"
"};\n"
"struct ShadowVertexOut {\n"
"    float4 position [[position]];\n"
"};\n"
"vertex ShadowVertexOut shadow_depth_vertex(ShadowVertexIn in [[stage_in]], constant ShadowUniforms &uniforms [[buffer(1)]]) {\n"
"    ShadowVertexOut out;\n"
"    out.position = uniforms.proj * uniforms.view * uniforms.model * float4(in.position, 1.0);\n"
"    return out;\n"
"}\n"
"fragment uint shadow_owner_fragment(ShadowVertexOut in [[stage_in]], constant ShadowUniforms &uniforms [[buffer(1)]]) {\n"
"    return uniforms.owner_params.x;\n"
"}\n";

static const char *s_water_surface_shader_source =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct WaterVertexIn {\n"
"    float3 position [[attribute(0)]];\n"
"};\n"
"struct WaterUniforms {\n"
"    float4x4 model;\n"
"    float4x4 view;\n"
"    float4x4 proj;\n"
"    float4 view_pos;\n"
"    float2 map_pos;\n"
"    float2 tile_world_size;\n"
"    uint2 chunk_size;\n"
"    uint2 tiles_per_chunk;\n"
"    float4 water_params;\n"
"    float4 water_texture_params;\n"
"    float4 water_depth_params;\n"
"    float4 light_color;\n"
"    float4 light_pos;\n"
"};\n"
"struct WaterVertexOut {\n"
"    float4 position [[position]];\n"
"    float3 world_pos;\n"
"    float2 world_xz;\n"
"    float2 uv;\n"
"    float3 view_dir;\n"
"    float3 light_dir;\n"
"};\n"
"vertex WaterVertexOut water_surface_vertex(WaterVertexIn in [[stage_in]], constant WaterUniforms &uniforms [[buffer(1)]]) {\n"
"    WaterVertexOut out;\n"
"    float4 world_pos = uniforms.model * float4(in.position, 1.0);\n"
"    out.position = uniforms.proj * uniforms.view * world_pos;\n"
"    out.world_pos = world_pos.xyz;\n"
"    out.world_xz = world_pos.xz;\n"
"    out.uv = float2(in.position.x * 0.5 + 0.5, in.position.z * 0.5 + 0.5) * float2(float(uniforms.chunk_size.x) * 1.5, float(uniforms.chunk_size.y) * 1.5);\n"
"    out.view_dir = normalize(uniforms.view_pos.xyz - world_pos.xyz);\n"
"    out.light_dir = normalize(uniforms.light_pos.xyz - world_pos.xyz);\n"
"    return out;\n"
"}\n"
"static float terrain_fog_factor(uint state) {\n"
"    if(state == 0u) return 0.0;\n"
"    if(state == 1u) return 0.5;\n"
"    return 1.0;\n"
"}\n"
"static int4 map_tile_desc(float2 world_xz, constant WaterUniforms &uniforms) {\n"
"    float field_w = uniforms.tile_world_size.x * uniforms.tiles_per_chunk.x;\n"
"    float field_h = uniforms.tile_world_size.y * uniforms.tiles_per_chunk.y;\n"
"    int chunk_c = int(fabs(uniforms.map_pos.x - world_xz.x) / field_w);\n"
"    int chunk_r = int(fabs(uniforms.map_pos.y - world_xz.y) / field_h);\n"
"    chunk_c = clamp(chunk_c, 0, int(uniforms.chunk_size.x) - 1);\n"
"    chunk_r = clamp(chunk_r, 0, int(uniforms.chunk_size.y) - 1);\n"
"    float chunk_base_x = uniforms.map_pos.x - (chunk_c * field_w);\n"
"    float chunk_base_z = uniforms.map_pos.y + (chunk_r * field_h);\n"
"    int tile_c = int(fabs(chunk_base_x - world_xz.x) / uniforms.tile_world_size.x);\n"
"    int tile_r = int(fabs(chunk_base_z - world_xz.y) / uniforms.tile_world_size.y);\n"
"    tile_c = clamp(tile_c, 0, int(uniforms.tiles_per_chunk.x) - 1);\n"
"    tile_r = clamp(tile_r, 0, int(uniforms.tiles_per_chunk.y) - 1);\n"
"    return int4(chunk_r, chunk_c, tile_r, tile_c);\n"
"}\n"
"static int4 map_tile_relative_desc(int4 desc, int dr, int dc, constant WaterUniforms &uniforms) {\n"
"    int4 ret = desc;\n"
"    if(desc.z + dr >= int(uniforms.tiles_per_chunk.y)) ret.x += 1;\n"
"    else if(desc.z + dr < 0) ret.x -= 1;\n"
"    if(desc.w + dc >= int(uniforms.tiles_per_chunk.x)) ret.y += 1;\n"
"    else if(desc.w + dc < 0) ret.y -= 1;\n"
"    ret.x = clamp(ret.x, 0, int(uniforms.chunk_size.y) - 1);\n"
"    ret.y = clamp(ret.y, 0, int(uniforms.chunk_size.x) - 1);\n"
"    ret.z = int(float(desc.z + dr) - float(uniforms.tiles_per_chunk.y) * floor(float(desc.z + dr) / float(uniforms.tiles_per_chunk.y)));\n"
"    ret.w = int(float(desc.w + dc) - float(uniforms.tiles_per_chunk.x) * floor(float(desc.w + dc) / float(uniforms.tiles_per_chunk.x)));\n"
"    return ret;\n"
"}\n"
"static uint map_tile_index(int4 td, constant WaterUniforms &uniforms) {\n"
"    uint tiles_per_chunk = uniforms.tiles_per_chunk.x * uniforms.tiles_per_chunk.y;\n"
"    return uint(td.x) * (uniforms.chunk_size.x * tiles_per_chunk)\n"
"         + uint(td.y) * tiles_per_chunk\n"
"         + uint(td.z) * uniforms.tiles_per_chunk.x\n"
"         + uint(td.w);\n"
"}\n"
"static float water_bilinear_unit(float tl, float tr, float bl, float br, float2 coord) {\n"
"    return bl * (1.0 - coord.x) * (1.0 - coord.y)\n"
"         + br * coord.x * (1.0 - coord.y)\n"
"         + tl * (1.0 - coord.x) * coord.y\n"
"         + tr * coord.x * coord.y;\n"
"}\n"
"static float glsl_mod(float x, float y) {\n"
"    return x - y * floor(x / y);\n"
"}\n"
"static float2 water_local_uv(float2 global_uv, constant WaterUniforms &uniforms) {\n"
"    float2 water_tiling = float2(float(uniforms.chunk_size.x) * 1.5, float(uniforms.chunk_size.y) * 1.5);\n"
"    float x_per_chunk = 1.0 / float(max(uniforms.chunk_size.x, 1u));\n"
"    float z_per_chunk = 1.0 / float(max(uniforms.chunk_size.y, 1u));\n"
"    float x_per_tile = x_per_chunk / float(max(uniforms.tiles_per_chunk.x, 1u));\n"
"    float z_per_tile = z_per_chunk / float(max(uniforms.tiles_per_chunk.y, 1u));\n"
"    global_uv /= -water_tiling;\n"
"    global_uv.x = glsl_mod(global_uv.x, x_per_tile) / x_per_tile;\n"
"    global_uv.y = glsl_mod(global_uv.y, z_per_tile) / z_per_tile;\n"
"    return global_uv;\n"
"}\n"
"static float water_fog_tint(device const uchar *fog_buff, int4 td, float2 uv, constant WaterUniforms &uniforms) {\n"
"    float c = terrain_fog_factor(fog_buff[map_tile_index(td, uniforms)]);\n"
"    float tl = terrain_fog_factor(fog_buff[map_tile_index(map_tile_relative_desc(td, -1, -1, uniforms), uniforms)]);\n"
"    float tr = terrain_fog_factor(fog_buff[map_tile_index(map_tile_relative_desc(td, -1,  1, uniforms), uniforms)]);\n"
"    float l = terrain_fog_factor(fog_buff[map_tile_index(map_tile_relative_desc(td,  0, -1, uniforms), uniforms)]);\n"
"    float r = terrain_fog_factor(fog_buff[map_tile_index(map_tile_relative_desc(td,  0,  1, uniforms), uniforms)]);\n"
"    float bl = terrain_fog_factor(fog_buff[map_tile_index(map_tile_relative_desc(td,  1, -1, uniforms), uniforms)]);\n"
"    float br = terrain_fog_factor(fog_buff[map_tile_index(map_tile_relative_desc(td,  1,  1, uniforms), uniforms)]);\n"
"    float t = terrain_fog_factor(fog_buff[map_tile_index(map_tile_relative_desc(td, -1,  0, uniforms), uniforms)]);\n"
"    float b = terrain_fog_factor(fog_buff[map_tile_index(map_tile_relative_desc(td,  1,  0, uniforms), uniforms)]);\n"
"    float tl_corner = (c + t + l + tl) * 0.25;\n"
"    float tr_corner = (c + t + r + tr) * 0.25;\n"
"    float bl_corner = (c + l + b + bl) * 0.25;\n"
"    float br_corner = (c + r + b + br) * 0.25;\n"
"    return water_bilinear_unit(tl_corner, tr_corner, bl_corner, br_corner, water_local_uv(uv, uniforms));\n"
"}\n"
"static float linearize_depth(float z, float near_z, float far_z) {\n"
"    return 2.0 * near_z * far_z / (far_z + near_z - (2.0 * z - 1.0) * (far_z - near_z));\n"
"}\n"
"static bool should_discard_water_edge(depth2d<float> refract_depth, sampler water_sampler, float2 uv, float near_z, float far_z, float boundary_thresh) {\n"
"    float2 lo = float2(0.001, 0.001);\n"
"    float2 hi = float2(0.999, 0.999);\n"
"    float depth = linearize_depth(refract_depth.sample(water_sampler, uv), near_z, far_z);\n"
"    float top = linearize_depth(refract_depth.sample(water_sampler, clamp(float2(uv.x, uv.y + boundary_thresh), lo, hi)), near_z, far_z);\n"
"    float bot = linearize_depth(refract_depth.sample(water_sampler, clamp(float2(uv.x, uv.y - boundary_thresh), lo, hi)), near_z, far_z);\n"
"    float left = linearize_depth(refract_depth.sample(water_sampler, clamp(float2(uv.x - boundary_thresh, uv.y), lo, hi)), near_z, far_z);\n"
"    float right = linearize_depth(refract_depth.sample(water_sampler, clamp(float2(uv.x + boundary_thresh, uv.y - boundary_thresh), lo, hi)), near_z, far_z);\n"
"    return fabs(depth - top) > 5.0 || fabs(depth - bot) > 5.0 || fabs(depth - left) > 5.0 || fabs(depth - right) > 5.0;\n"
"}\n"
"fragment float4 water_surface_fragment(WaterVertexOut in [[stage_in]], constant WaterUniforms &uniforms [[buffer(1)]], device const uchar *fog_buff [[buffer(2)]], device const uchar *water_buff [[buffer(3)]], texture2d<float> dudv_map [[texture(0)]], texture2d<float> normal_map [[texture(1)]], texture2d<float> reflect_tex [[texture(2)]], texture2d<float> refract_tex [[texture(3)]], depth2d<float> refract_depth [[texture(4)]], sampler water_sampler [[sampler(0)]]) {\n"
"    if(uniforms.water_params.y < 0.5 || uniforms.chunk_size.x == 0u || uniforms.chunk_size.y == 0u) discard_fragment();\n"
"    int4 td = map_tile_desc(in.world_xz, uniforms);\n"
"    uint idx = map_tile_index(td, uniforms);\n"
"    bool depth_backed_water = uniforms.water_depth_params.w > 0.5 && uniforms.water_texture_params.w > 0.5;\n"
"    if(!depth_backed_water && water_buff[idx] == 0u) discard_fragment();\n"
"    float2 screen_uv = clamp(in.position.xy / max(uniforms.water_texture_params.xy, float2(1.0, 1.0)), float2(0.001, 0.001), float2(0.999, 0.999));\n"
"    float fog_factor = 1.0;\n"
"    if(uniforms.water_params.z > 0.5) fog_factor = water_fog_tint(fog_buff, td, in.uv, uniforms);\n"
"    if(fog_factor <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);\n"
"    float2 uv = in.uv;\n"
"    float2 dudv_uv = float2(uv.x + uniforms.water_params.x, uv.y);\n"
"    float2 dudv = float2(0.0, 0.0);\n"
"    float3 water_normal = float3(0.0, 1.0, 0.0);\n"
"    if(uniforms.water_params.w > 0.5) {\n"
"        float2 coarse = dudv_map.sample(water_sampler, dudv_uv).rg * 0.1;\n"
"        float2 distorted_uv = uv + float2(coarse.x, coarse.y + uniforms.water_params.x);\n"
"        dudv = dudv_map.sample(water_sampler, distorted_uv).rg * 2.0 - 1.0;\n"
"        float2 normal_uv = distorted_uv;\n"
"        float3 sampled_normal = normal_map.sample(water_sampler, normal_uv).rgb;\n"
"        water_normal = float3(sampled_normal.r * 2.0 - 1.0, sampled_normal.b, sampled_normal.g * 2.0 - 1.0);\n"
"    }\n"
"    float2 distortion = dudv * 0.005;\n"
"    float2 refract_uv = clamp(screen_uv + distortion, float2(0.001, 0.001), float2(0.999, 0.999));\n"
"    float2 reflect_uv = float2(screen_uv.x, -screen_uv.y) + distortion;\n"
"    reflect_uv.x = clamp(reflect_uv.x, 0.001, 0.999);\n"
"    reflect_uv.y = clamp(reflect_uv.y, -0.999, -0.001);\n"
"    float scene_mix = clamp(uniforms.water_texture_params.z + uniforms.water_texture_params.w, 0.0, 1.0);\n"
"    float4 refract_color = float4(0.0, 0.3, 0.5, 1.0);\n"
"    float4 reflect_color = float4(0.0, 0.3, 0.5, 1.0);\n"
"    float water_depth = 1.0;\n"
"    float alpha = 0.0;\n"
"    if(scene_mix > 0.0) {\n"
"        refract_color = refract_tex.sample(water_sampler, refract_uv);\n"
"        reflect_color = reflect_tex.sample(water_sampler, reflect_uv);\n"
"    }\n"
"    float specular = pow(max(dot(reflect(-in.light_dir, water_normal), in.view_dir), 0.0), 0.8);\n"
"    if(uniforms.water_depth_params.w > 0.5 && uniforms.water_texture_params.w > 0.5) {\n"
"        if(should_discard_water_edge(refract_depth, water_sampler, screen_uv, uniforms.water_depth_params.x, uniforms.water_depth_params.y, uniforms.water_depth_params.z)) discard_fragment();\n"
"        float ground_dist = linearize_depth(refract_depth.sample(water_sampler, screen_uv), uniforms.water_depth_params.x, uniforms.water_depth_params.y);\n"
"        float water_dist = linearize_depth(in.position.z, uniforms.water_depth_params.x, uniforms.water_depth_params.y);\n"
"        water_depth = max(ground_dist - water_dist, 0.0);\n"
"        alpha = clamp(water_depth / 2.5, 0.0, 1.0);\n"
"    } else {\n"
"        alpha = 0.55;\n"
"    }\n"
"    float4 color = mix(refract_color, reflect_color, 0.5);\n"
"    color = mix(color, float4(0.0, 0.3, 0.5, 0.1), 0.1);\n"
"    color += float4(0.15 * specular * uniforms.light_color.xyz, 0.0);\n"
"    color.a = alpha;\n"
"    return color * fog_factor;\n"
"}\n";

static const char *s_skybox_shader_source =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct SkyboxVertexIn {\n"
"    float3 position [[attribute(0)]];\n"
"};\n"
"struct SkyboxUniforms {\n"
"    float4x4 model;\n"
"    float4x4 view_rot;\n"
"    float4x4 proj;\n"
"};\n"
"struct SkyboxVertexOut {\n"
"    float4 position [[position]];\n"
"    float3 tex_coords;\n"
"};\n"
"vertex SkyboxVertexOut skybox_vertex(SkyboxVertexIn in [[stage_in]], constant SkyboxUniforms &uniforms [[buffer(1)]]) {\n"
"    SkyboxVertexOut out;\n"
"    out.tex_coords = in.position;\n"
"    float4 pos = uniforms.proj * uniforms.view_rot * uniforms.model * float4(in.position, 1.0);\n"
"    out.position = pos.xyww;\n"
"    return out;\n"
"}\n"
"fragment float4 skybox_fragment(SkyboxVertexOut in [[stage_in]], texturecube<float> skybox [[texture(0)]], sampler skybox_sampler [[sampler(0)]]) {\n"
"    float3 coord = in.tex_coords;\n"
"    if(abs(coord.y) > abs(coord.x) && abs(coord.y) > abs(coord.z)) {\n"
"        coord = float3(-coord.z, coord.y, -coord.x);\n"
"    }\n"
"    return skybox.sample(skybox_sampler, coord);\n"
"}\n";

static const char *s_world_color_shader_source =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct WorldColorVertexIn {\n"
"    float3 position [[attribute(0)]];\n"
"    float4 color [[attribute(1)]];\n"
"};\n"
"struct WorldColorUniforms {\n"
"    float4x4 view;\n"
"    float4x4 proj;\n"
"};\n"
"struct WorldColorVertexOut {\n"
"    float4 position [[position]];\n"
"    float4 color;\n"
"};\n"
"vertex WorldColorVertexOut world_color_vertex(WorldColorVertexIn in [[stage_in]], constant WorldColorUniforms &uniforms [[buffer(1)]]) {\n"
"    WorldColorVertexOut out;\n"
"    out.position = uniforms.proj * uniforms.view * float4(in.position, 1.0);\n"
"    out.color = in.color;\n"
"    return out;\n"
"}\n"
"fragment float4 world_color_fragment(WorldColorVertexOut in [[stage_in]]) {\n"
"    return in.color;\n"
"}\n";

struct metal_ui_uniforms{
    float view_size[2];
    float _padding[2];
};

struct metal_minimap_uniforms{
    vector_uint2 chunk_size;
    vector_uint2 tiles_per_chunk;
    uint32_t     fog_enabled;
    uint32_t     _padding[3];
};

struct metal_terrain_uniforms{
    matrix_float4x4 model;
    matrix_float4x4 view;
    matrix_float4x4 proj;
    vector_float4   view_pos;
    vector_float2   map_pos;
    vector_float2   tile_world_size;
    vector_uint2    chunk_size;
    vector_uint2    tiles_per_chunk;
    vector_float4   terrain_params;
    vector_float4   water_params;
    vector_float4   ambient_color;
    vector_float4   light_color;
    vector_float4   light_pos;
    matrix_float4x4 light_space_transform;
    vector_float4   shadow_params;
    vector_float4   clip_params;
};

struct metal_static_mesh_uniforms{
    matrix_float4x4 model;
    matrix_float4x4 normal_transform;
    matrix_float4x4 view;
    matrix_float4x4 proj;
    matrix_float4x4 light_space_transform;
    vector_float4 material_diffuse[MAX_MATERIALS];
    vector_float4 material_ambient[MAX_MATERIALS];
    vector_float4 material_specular[MAX_MATERIALS];
    vector_float4 view_pos;
    vector_float4 ambient_color;
    vector_float4 light_color;
    vector_float4 light_pos;
    vector_float4 effect_params;
    vector_float4 shadow_params;
    vector_float4 clip_params;
};

struct metal_world_color_uniforms{
    matrix_float4x4 view;
    matrix_float4x4 proj;
};

struct metal_sprite_uniforms{
    matrix_float4x4 view;
    matrix_float4x4 projection;
    vector_float3 view_dir;
    uint32_t sprite_nrows;
    uint32_t sprite_ncols;
};

struct metal_sprite_desc{
    float ws_pos[3];
    float pad0;
    float ws_size[2];
    uint32_t frame_idx;
    float pad1;
};

struct metal_sprite_draw_call{
    size_t begin_idx;
    size_t end_idx;
};

struct metal_shadow_uniforms{
    matrix_float4x4 model;
    matrix_float4x4 view;
    matrix_float4x4 proj;
    vector_uint4 owner_params;
};

struct metal_water_surface_uniforms{
    matrix_float4x4 model;
    matrix_float4x4 view;
    matrix_float4x4 proj;
    vector_float4   view_pos;
    vector_float2   map_pos;
    vector_float2   tile_world_size;
    vector_uint2    chunk_size;
    vector_uint2    tiles_per_chunk;
    vector_float4   water_params;
    vector_float4   water_texture_params;
    vector_float4   water_depth_params;
    vector_float4   light_color;
    vector_float4   light_pos;
};

struct metal_skybox_uniforms{
    matrix_float4x4 model;
    matrix_float4x4 view_rot;
    matrix_float4x4 proj;
};

static const vec3_t s_skybox_cube_verts[] = {
    {-1.0f, +1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f}, {+1.0f, -1.0f, -1.0f},
    {+1.0f, -1.0f, -1.0f}, {+1.0f, +1.0f, -1.0f}, {-1.0f, +1.0f, -1.0f},

    {-1.0f, -1.0f, +1.0f}, {-1.0f, -1.0f, -1.0f}, {-1.0f, +1.0f, -1.0f},
    {-1.0f, +1.0f, -1.0f}, {-1.0f, +1.0f, +1.0f}, {-1.0f, -1.0f, +1.0f},

    {+1.0f, -1.0f, -1.0f}, {+1.0f, -1.0f, +1.0f}, {+1.0f, +1.0f, +1.0f},
    {+1.0f, +1.0f, +1.0f}, {+1.0f, +1.0f, -1.0f}, {+1.0f, -1.0f, -1.0f},

    {-1.0f, -1.0f, +1.0f}, {-1.0f, +1.0f, +1.0f}, {+1.0f, +1.0f, +1.0f},
    {+1.0f, +1.0f, +1.0f}, {+1.0f, -1.0f, +1.0f}, {-1.0f, -1.0f, +1.0f},

    {-1.0f, +1.0f, -1.0f}, {+1.0f, +1.0f, -1.0f}, {+1.0f, +1.0f, +1.0f},
    {+1.0f, +1.0f, +1.0f}, {-1.0f, +1.0f, +1.0f}, {-1.0f, +1.0f, -1.0f},

    {-1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, +1.0f}, {+1.0f, -1.0f, -1.0f},
    {+1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, +1.0f}, {+1.0f, -1.0f, +1.0f},
};

static bool append_skinned_anim_mesh(const struct render_private *priv,
                                     uint32_t uid,
                                     const mat4x4_t *model,
                                     struct vertex *dst,
                                     size_t *dst_idx);
static void render_static_mesh_draw(const struct render_private *priv, const mat4x4_t *model, bool translucent);
static void render_skinned_mesh_draw(const struct render_private *priv, const mat4x4_t *model, bool translucent);
static void render_batched_stat_entities(const vec_rstat_t *ents);
static void render_batched_anim_entities(const vec_ranim_t *ents);

static bool render_wait_cmd(struct render_sync_state *rstate)
{
    SDL_LockMutex(rstate->sq_lock);
    while(!rstate->start && !rstate->quit)
        SDL_CondWait(rstate->sq_cond, rstate->sq_lock);

    if(rstate->quit) {
        rstate->quit = false;
        SDL_UnlockMutex(rstate->sq_lock);
        return true;
    }

    assert(rstate->start == true);
    rstate->start = false;
    SDL_UnlockMutex(rstate->sq_lock);
    return false;
}

static void render_signal_done(struct render_sync_state *rstate, enum render_status status)
{
    SDL_LockMutex(rstate->done_lock);
    rstate->status = status;
    SDL_CondSignal(rstate->done_cond);
    SDL_UnlockMutex(rstate->done_lock);
}

static void render_dispatch_cmd(struct rcmd cmd)
{
    switch(cmd.nargs) {
    case 0:  ((void(*)(void))cmd.func)(); break;
    case 1:  ((void(*)(void*))cmd.func)(cmd.args[0]); break;
    case 2:  ((void(*)(void*, void*))cmd.func)(cmd.args[0], cmd.args[1]); break;
    case 3:  ((void(*)(void*, void*, void*))cmd.func)(cmd.args[0], cmd.args[1], cmd.args[2]); break;
    case 4:  ((void(*)(void*, void*, void*, void*))cmd.func)(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]); break;
    case 5:  ((void(*)(void*, void*, void*, void*, void*))cmd.func)(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]); break;
    case 6:  ((void(*)(void*, void*, void*, void*, void*, void*))cmd.func)(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4], cmd.args[5]); break;
    case 7:  ((void(*)(void*, void*, void*, void*, void*, void*, void*))cmd.func)(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4], cmd.args[5], cmd.args[6]); break;
    case 8:  ((void(*)(void*, void*, void*, void*, void*, void*, void*, void*))cmd.func)(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4], cmd.args[5], cmd.args[6], cmd.args[7]); break;
    case 9:  ((void(*)(void*, void*, void*, void*, void*, void*, void*, void*, void*))cmd.func)(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4], cmd.args[5], cmd.args[6], cmd.args[7], cmd.args[8]); break;
    case 10: ((void(*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*))cmd.func)(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4], cmd.args[5], cmd.args[6], cmd.args[7], cmd.args[8], cmd.args[9]); break;
    default: assert(0);
    }
}

static void update_drawable_size(void)
{
    int width = 0, height = 0;
    SDL_Metal_GetDrawableSize(s_window, &width, &height);
    s_layer.drawableSize = CGSizeMake(width, height);
}

static void release_ui_resources(void)
{
    s_ui_font_texture = nil;
    s_ui_sampler = nil;
    s_minimap_sampler = nil;
    s_ui_pipeline = nil;
    s_ui_msaa_pipeline = nil;
    s_minimap_pipeline = nil;
    s_minimap_msaa_pipeline = nil;
    s_ui_texpath_cache = nil;
    s_pending_ui_texture = nil;
}

static void release_scene_resources(void)
{
    s_minimap_texture = nil;
    s_frame_msaa_texture = nil;
    s_frame_depth_texture = nil;
    s_shadow_depth_texture = nil;
    s_shadow_owner_texture = nil;
    s_shadow_owner_dummy_texture = nil;
    s_water_reflection_texture = nil;
    s_water_reflection_depth_texture = nil;
    s_water_refraction_texture = nil;
    s_water_refraction_depth_texture = nil;
    s_terrain_texture_array = nil;
    s_skybox_texture = nil;
    s_water_dudv_texture = nil;
    s_water_normal_texture = nil;
    s_fog_buffer = nil;
    s_water_buffer = nil;
    s_heightmap_buffer = nil;
    s_splatmap_buffer = nil;
    s_splat_indices_buffer = nil;
    s_skybox_vertex_buffer = nil;
    s_shadow_screen_probe_buffer = nil;
    s_shadow_screen_probe_params_buffer = nil;
    s_shadow_screen_probe_disabled_params_buffer = nil;
    s_water_reflection_dump_buffer = nil;
    s_scene_sampler = nil;
    s_material_sampler = nil;
    s_terrain_sampler = nil;
    s_shadow_sampler = nil;
    s_skybox_sampler = nil;
    s_sprite_sampler = nil;
    s_depth_write_state = nil;
    s_depth_read_state = nil;
    s_terrain_pipeline = nil;
    s_terrain_msaa_pipeline = nil;
    s_terrain_depth_pipeline = nil;
    s_terrain_depth_msaa_pipeline = nil;
    s_static_mesh_pipeline = nil;
    s_static_mesh_msaa_pipeline = nil;
    s_static_mesh_blend_pipeline = nil;
    s_static_mesh_blend_msaa_pipeline = nil;
    s_static_mesh_depth_pipeline = nil;
    s_static_mesh_depth_msaa_pipeline = nil;
    s_static_mesh_blend_depth_pipeline = nil;
    s_static_mesh_blend_depth_msaa_pipeline = nil;
    s_world_color_pipeline = nil;
    s_world_color_msaa_pipeline = nil;
    s_world_color_depth_pipeline = nil;
    s_world_color_depth_msaa_pipeline = nil;
    s_shadow_terrain_pipeline = nil;
    s_shadow_mesh_pipeline = nil;
    s_shadow_owner_terrain_pipeline = nil;
    s_shadow_owner_mesh_pipeline = nil;
    s_water_surface_pipeline = nil;
    s_water_surface_msaa_pipeline = nil;
    s_skybox_pipeline = nil;
    s_skybox_msaa_pipeline = nil;
    s_sprite_pipeline = nil;
    s_sprite_msaa_pipeline = nil;
    s_sprite_texpath_cache = nil;
    s_terrain_texture_count = 0;
    s_shadow_map_valid = false;
}

static void reset_frame_state(void)
{
    s_frame_encoder = nil;
    s_frame_command_buffer = nil;
    s_frame_drawable = nil;
    s_frame_sample_count = 0;
}

static bool reserve_inflight_frame(void)
{
    if(s_frame_inflight_reserved)
        return true;
    if(!s_inflight_semaphore)
        s_inflight_semaphore = dispatch_semaphore_create(3);
    if(!s_inflight_semaphore)
        return false;
    dispatch_semaphore_wait(s_inflight_semaphore, DISPATCH_TIME_FOREVER);
    s_frame_inflight_reserved = true;
    return true;
}

static void release_inflight_frame(void)
{
    if(!s_frame_inflight_reserved || !s_inflight_semaphore)
        return;
    dispatch_semaphore_signal(s_inflight_semaphore);
    s_frame_inflight_reserved = false;
}

static unsigned capture_env_uint(const char *name, unsigned fallback)
{
    const char *value = getenv(name);
    if(!value || !*value)
        return fallback;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if(end == value)
        return fallback;
    return (unsigned)parsed;
}

static void capture_env_init_once(void)
{
    if(s_capture_env_checked)
        return;
    s_capture_env_checked = true;

    const char *path = getenv("PF_METAL_CAPTURE_PATH");
    if(!path || !*path)
        return;

    strncpy(s_capture_output_path, path, sizeof(s_capture_output_path) - 1);
    s_capture_output_path[sizeof(s_capture_output_path) - 1] = '\0';
    s_capture_start_present = capture_env_uint("PF_METAL_CAPTURE_START_PRESENT", 1);
    s_capture_present_span = capture_env_uint("PF_METAL_CAPTURE_PRESENTS", 1);
    if(s_capture_present_span == 0)
        s_capture_present_span = 1;
    s_capture_requested = true;
}

static void capture_maybe_start(void)
{
    capture_env_init_once();
    if(!s_capture_requested || s_capture_active || s_capture_done)
        return;
    if(s_capture_present_count < s_capture_start_present)
        return;

    MTLCaptureManager *manager = [MTLCaptureManager sharedCaptureManager];
    if(![manager supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
        fprintf(stderr, "Metal GPU trace capture destination is not supported.\n");
        s_capture_done = true;
        return;
    }

    NSString *path = [NSString stringWithUTF8String:s_capture_output_path];
    if(!path) {
        fprintf(stderr, "Metal GPU trace capture path is invalid.\n");
        s_capture_done = true;
        return;
    }

    MTLCaptureDescriptor *descriptor = [[MTLCaptureDescriptor alloc] init];
    if(s_queue)
        descriptor.captureObject = s_queue;
    else
        descriptor.captureObject = s_device;
    descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
    descriptor.outputURL = [NSURL fileURLWithPath:path];

    NSError *error = nil;
    if(![manager startCaptureWithDescriptor:descriptor error:&error]) {
        fprintf(stderr, "Metal GPU trace capture failed to start: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        s_capture_done = true;
        return;
    }

    s_capture_active = true;
    s_capture_stop_present = s_capture_present_count + s_capture_present_span;
    fprintf(stderr, "Metal GPU trace capture started: %s\n", s_capture_output_path);
}

static void capture_note_presented(void)
{
    s_capture_present_count++;
    if(!s_capture_active)
        return;
    if(s_capture_present_count < s_capture_stop_present)
        return;

    [[MTLCaptureManager sharedCaptureManager] stopCapture];
    s_capture_active = false;
    s_capture_done = true;
    fprintf(stderr, "Metal GPU trace capture stopped: %s\n", s_capture_output_path);
}

static bool raw_material_debug_enabled(void)
{
    if(!s_raw_material_debug_env_checked) {
        const char *value = getenv("PF_METAL_DEBUG_RAW_MATERIAL");
        s_raw_material_debug_enabled = value && *value && strcmp(value, "0");
        s_raw_material_debug_env_checked = true;
    }
    return s_raw_material_debug_enabled;
}

static bool shadow_screen_probe_enabled(void)
{
    if(!s_shadow_screen_probe_env_checked) {
        const char *target = getenv("PF_METAL_SHADOW_SCREEN_PROBE");
        const char *path = getenv("PF_METAL_SHADOW_SCREEN_PROBE_PATH");
        const char *radius = getenv("PF_METAL_SHADOW_SCREEN_PROBE_RADIUS");
        char *end = NULL;
        if(target && *target) {
            unsigned long x = strtoul(target, &end, 10);
            if(end && *end == ',') {
                char *end_y = NULL;
                unsigned long y = strtoul(end + 1, &end_y, 10);
                if(end_y && *end_y == '\0') {
                    s_shadow_screen_probe_x = (uint32_t)x;
                    s_shadow_screen_probe_y = (uint32_t)y;
                    s_shadow_screen_probe_enabled = true;
                }
            }
        }
        s_shadow_screen_probe_radius = (radius && *radius)
            ? (uint32_t)strtoul(radius, NULL, 10)
            : 0;
        if(path && *path)
            strncpy(s_shadow_screen_probe_output_path, path, sizeof(s_shadow_screen_probe_output_path) - 1);
        if(s_shadow_screen_probe_enabled) {
            fprintf(stderr, "PF_METAL_SHADOW_SCREEN_PROBE target=%u,%u radius=%u output=%s\n",
                s_shadow_screen_probe_x, s_shadow_screen_probe_y,
                s_shadow_screen_probe_radius,
                s_shadow_screen_probe_output_path[0] ? s_shadow_screen_probe_output_path : "<stderr>");
        }
        s_shadow_screen_probe_env_checked = true;
    }
    return s_shadow_screen_probe_enabled;
}

static const char *shadow_probe_kind_name(uint32_t kind)
{
    switch(kind) {
    case 1: return "terrain";
    case 2: return "static";
    default: return "unknown";
    }
}

static float uint_bits_to_float(uint32_t bits)
{
    float ret;
    memcpy(&ret, &bits, sizeof(ret));
    return ret;
}

static NSUInteger requested_frame_sample_count(void)
{
    if(!s_msaa_env_checked) {
        const char *parity_mode = getenv("PF_PARITY_MODE");
        s_requested_sample_count = (parity_mode && 0 == strcmp(parity_mode, "1"))
            ? 1
            : METAL_DEFAULT_MSAA_SAMPLES;
        const char *value = getenv("PF_METAL_MSAA_SAMPLES");
        if(value && *value) {
            char *end = NULL;
            long samples = strtol(value, &end, 10);
            if(end != value && (samples == 1 || samples == 2 || samples == 4 || samples == 8)) {
                s_requested_sample_count = (NSUInteger)samples;
            }else{
                fprintf(stderr, "PF_METAL_MSAA_SAMPLES supports 1, 2, 4, or 8; using %lu\n",
                    (unsigned long)s_requested_sample_count);
            }
        }
        if(s_requested_sample_count > METAL_MAX_MSAA_SAMPLES)
            s_requested_sample_count = METAL_MAX_MSAA_SAMPLES;
        s_msaa_env_checked = true;
    }
    return s_requested_sample_count;
}

static matrix_float4x4 matrix_from_pf_mat4(const mat4x4_t *in)
{
    return (matrix_float4x4){
        .columns[0] = {in->cols[0][0], in->cols[0][1], in->cols[0][2], in->cols[0][3]},
        .columns[1] = {in->cols[1][0], in->cols[1][1], in->cols[1][2], in->cols[1][3]},
        .columns[2] = {in->cols[2][0], in->cols[2][1], in->cols[2][2], in->cols[2][3]},
        .columns[3] = {in->cols[3][0], in->cols[3][1], in->cols[3][2], in->cols[3][3]},
    };
}

static void normal_transform_from_mat4(const mat4x4_t *in, mat4x4_t *out)
{
    mat4x4_t copy = *in;
    mat4x4_t inv;
    PFM_Mat4x4_Inverse(&copy, &inv);
    PFM_Mat4x4_Transpose(&inv, out);
}

static vec3_t transform_normal_with_mat4(const mat4x4_t *mat, vec3_t normal)
{
    vec4_t in = {normal.x, normal.y, normal.z, 0.0f};
    vec4_t out = {0};
    PFM_Mat4x4_Mult4x1((mat4x4_t *)mat, &in, &out);

    vec3_t ret = {out.x, out.y, out.z};
    if(PFM_Vec3_Len(&ret) > 0.0001f)
        PFM_Vec3_Normal(&ret, &ret);
    else
        ret = normal;
    return ret;
}

static void set_current_anim_uid(uint32_t uid)
{
    s_curr_anim_uid = uid;
    s_have_anim_uid = true;
    s_have_anim_normal_transform = false;
}

static void make_shadow_light_space(vec3_t light_pos, vec3_t cam_pos, vec3_t cam_dir,
                                    matrix_float4x4 *out_view, matrix_float4x4 *out_proj,
                                    matrix_float4x4 *out_light_space)
{
    float t = cam_pos.y / cam_dir.y;
    vec3_t cam_ray_ground_isec = {
        cam_pos.x - t * cam_dir.x,
        0.0f,
        cam_pos.z - t * cam_dir.z
    };

    vec3_t light_dir = light_pos;
    PFM_Vec3_Normal(&light_dir, &light_dir);
    PFM_Vec3_Scale(&light_dir, -1.0f, &light_dir);

    vec3_t right = {-1.0f, 0.0f, 0.0f};
    vec3_t up;
    PFM_Vec3_Cross(&light_dir, &right, &up);

    t = fabsf((cam_pos.y + METAL_LIGHT_EXTRA_HEIGHT) / light_dir.y);
    vec3_t light_origin, delta;
    PFM_Vec3_Scale(&light_dir, -t, &delta);
    PFM_Vec3_Add(&cam_ray_ground_isec, &delta, &light_origin);

    vec3_t target;
    PFM_Vec3_Add(&light_origin, &light_dir, &target);

    mat4x4_t light_view;
    mat4x4_t light_proj;
    PFM_Mat4x4_MakeLookAt(&light_origin, &target, &up, &light_view);
    PFM_Mat4x4_MakeOrthographic(-CONFIG_SHADOW_FOV, CONFIG_SHADOW_FOV,
        CONFIG_SHADOW_FOV, -CONFIG_SHADOW_FOV, 0.1f, CONFIG_SHADOW_DRAWDIST, &light_proj);

    mat4x4_t light_space;
    PFM_Mat4x4_Mult4x4(&light_proj, &light_view, &light_space);
    *out_view = matrix_from_pf_mat4(&light_view);
    *out_proj = matrix_from_pf_mat4(&light_proj);
    *out_light_space = matrix_from_pf_mat4(&light_space);
}

static void present_clear(void)
{
    capture_maybe_start();
    update_drawable_size();
    if(!reserve_inflight_frame())
        return;

    id<CAMetalDrawable> drawable = [s_layer nextDrawable];
    if(!drawable) {
        release_inflight_frame();
        return;
    }

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.3, 0.3, 1.0);

    id<MTLCommandBuffer> command_buffer = [s_queue commandBuffer];
    if(!command_buffer) {
        release_inflight_frame();
        return;
    }
    if(s_frame_inflight_reserved && s_inflight_semaphore) {
        dispatch_semaphore_t inflight = s_inflight_semaphore;
        [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> buffer){
            (void)buffer;
            dispatch_semaphore_signal(inflight);
        }];
        s_frame_inflight_reserved = false;
    }
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if(encoder)
        [encoder endEncoding];
    [command_buffer presentDrawable:drawable];
    [command_buffer commit];
    capture_note_presented();
}

static NSUInteger desired_frame_sample_count(void)
{
    if(!s_device)
        return 1;
    NSUInteger requested = requested_frame_sample_count();
    if(requested <= 1)
        return 1;
    const NSUInteger candidates[] = {8, 4, 2};
    for(size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        NSUInteger candidate = candidates[i];
        if(candidate <= requested && [s_device supportsTextureSampleCount:candidate])
            return candidate;
    }
    return 1;
}

static bool frame_uses_msaa(void)
{
    return s_frame_sample_count > 1;
}

static NSUInteger active_msaa_sample_count(void)
{
    return s_frame_sample_count > 1 ? s_frame_sample_count : desired_frame_sample_count();
}

static const char *render_info_msaa_samples(void)
{
    snprintf(s_info_msaa_samples, sizeof(s_info_msaa_samples), "%lu",
        (unsigned long)desired_frame_sample_count());
    return s_info_msaa_samples;
}

static bool ensure_frame_msaa_texture(NSUInteger sample_count)
{
    if(sample_count <= 1)
        return false;

    NSUInteger width = (NSUInteger)s_layer.drawableSize.width;
    NSUInteger height = (NSUInteger)s_layer.drawableSize.height;
    if(!width || !height)
        return false;

    if(s_frame_msaa_texture
    && s_frame_msaa_texture.width == width
    && s_frame_msaa_texture.height == height
    && s_frame_msaa_texture.sampleCount == sample_count
    && s_frame_msaa_texture.pixelFormat == s_layer.pixelFormat) {
        return true;
    }

    s_frame_msaa_texture = nil;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s_layer.pixelFormat
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
    desc.textureType = MTLTextureType2DMultisample;
    desc.sampleCount = sample_count;
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    s_frame_msaa_texture = [s_device newTextureWithDescriptor:desc];
    return s_frame_msaa_texture != nil;
}

static bool ensure_frame_depth_texture(NSUInteger sample_count)
{
    NSUInteger width = (NSUInteger)s_layer.drawableSize.width;
    NSUInteger height = (NSUInteger)s_layer.drawableSize.height;
    if(!width || !height)
        return false;

    if(s_frame_depth_texture
    && s_frame_depth_texture.width == width
    && s_frame_depth_texture.height == height
    && s_frame_depth_texture.sampleCount == sample_count
    && s_frame_depth_texture.pixelFormat == MTLPixelFormatDepth32Float) {
        return true;
    }

    s_frame_depth_texture = nil;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
    desc.textureType = sample_count > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    desc.sampleCount = sample_count;
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget;
    s_frame_depth_texture = [s_device newTextureWithDescriptor:desc];
    return s_frame_depth_texture != nil;
}

static bool frame_has_depth(void)
{
    return s_frame_encoder && s_frame_depth_texture;
}

static id<MTLDepthStencilState> ensure_depth_state(bool write)
{
    id<MTLDepthStencilState> __strong *slot = write ? &s_depth_write_state : &s_depth_read_state;
    if(*slot)
        return *slot;

    MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
    desc.depthCompareFunction = MTLCompareFunctionLessEqual;
    desc.depthWriteEnabled = write ? YES : NO;
    *slot = [s_device newDepthStencilStateWithDescriptor:desc];
    return *slot;
}

static bool ensure_shadow_sampler(void)
{
    if(s_shadow_sampler)
        return true;

    MTLSamplerDescriptor *desc = [[MTLSamplerDescriptor alloc] init];
    desc.minFilter = MTLSamplerMinMagFilterNearest;
    desc.magFilter = MTLSamplerMinMagFilterNearest;
    desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_shadow_sampler = [s_device newSamplerStateWithDescriptor:desc];
    return s_shadow_sampler != nil;
}

static bool ensure_shadow_depth_texture(void)
{
    if(s_shadow_depth_texture
    && s_shadow_depth_texture.width == CONFIG_SHADOW_MAP_RES
    && s_shadow_depth_texture.height == CONFIG_SHADOW_MAP_RES
    && s_shadow_depth_texture.pixelFormat == MTLPixelFormatDepth32Float) {
        return true;
    }

    s_shadow_depth_texture = nil;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                     width:CONFIG_SHADOW_MAP_RES
                                                                                    height:CONFIG_SHADOW_MAP_RES
                                                                                 mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    s_shadow_depth_texture = [s_device newTextureWithDescriptor:desc];
    return s_shadow_depth_texture != nil;
}

static bool ensure_shadow_owner_texture(void)
{
    if(s_shadow_owner_texture
    && s_shadow_owner_texture.width == CONFIG_SHADOW_MAP_RES
    && s_shadow_owner_texture.height == CONFIG_SHADOW_MAP_RES
    && s_shadow_owner_texture.pixelFormat == MTLPixelFormatR32Uint) {
        return true;
    }

    s_shadow_owner_texture = nil;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                                                                     width:CONFIG_SHADOW_MAP_RES
                                                                                    height:CONFIG_SHADOW_MAP_RES
                                                                                 mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget;
    s_shadow_owner_texture = [s_device newTextureWithDescriptor:desc];
    return s_shadow_owner_texture != nil;
}

static bool ensure_shadow_owner_dummy_texture(void)
{
    if(s_shadow_owner_dummy_texture)
        return true;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                                                                     width:1
                                                                                    height:1
                                                                                 mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    s_shadow_owner_dummy_texture = [s_device newTextureWithDescriptor:desc];
    if(!s_shadow_owner_dummy_texture)
        return false;

    uint32_t zero = 0;
    [s_shadow_owner_dummy_texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                                    mipmapLevel:0
                                      withBytes:&zero
                                    bytesPerRow:sizeof(zero)];
    return true;
}

static bool ensure_shadow_screen_probe_buffers(void)
{
    if(!s_device)
        return false;
    if(!s_shadow_screen_probe_buffer) {
        s_shadow_screen_probe_buffer = [s_device newBufferWithLength:16 * sizeof(uint32_t)
            options:MTLResourceStorageModeShared];
        if(!s_shadow_screen_probe_buffer)
            return false;
    }
    if(!s_shadow_screen_probe_params_buffer) {
        s_shadow_screen_probe_params_buffer = [s_device newBufferWithLength:4 * sizeof(uint32_t)
            options:MTLResourceStorageModeShared];
        if(!s_shadow_screen_probe_params_buffer)
            return false;
    }
    if(!s_shadow_screen_probe_disabled_params_buffer) {
        s_shadow_screen_probe_disabled_params_buffer = [s_device newBufferWithLength:4 * sizeof(uint32_t)
            options:MTLResourceStorageModeShared];
        if(!s_shadow_screen_probe_disabled_params_buffer)
            return false;
        memset(s_shadow_screen_probe_disabled_params_buffer.contents, 0, 4 * sizeof(uint32_t));
    }
    return true;
}

static id<MTLRenderPipelineState> build_ui_pipeline(NSUInteger sample_count)
{
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:s_ui_shader_source];
    id<MTLLibrary> library = [s_device newLibraryWithSource:source options:nil error:&error];
    if(!library) {
        fprintf(stderr, "Metal UI shader compile failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"ui_vertex"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"ui_fragment"];
    if(!vertex || !fragment) {
        fprintf(stderr, "Metal UI shader entrypoint lookup failed.\n");
        return nil;
    }

    MTLVertexDescriptor *vertex_desc = [MTLVertexDescriptor vertexDescriptor];
    vertex_desc.attributes[0].format = MTLVertexFormatFloat2;
    vertex_desc.attributes[0].offset = offsetof(struct ui_vert, screen_pos);
    vertex_desc.attributes[0].bufferIndex = 0;
    vertex_desc.attributes[1].format = MTLVertexFormatFloat2;
    vertex_desc.attributes[1].offset = offsetof(struct ui_vert, uv);
    vertex_desc.attributes[1].bufferIndex = 0;
    vertex_desc.attributes[2].format = MTLVertexFormatUChar4Normalized;
    vertex_desc.attributes[2].offset = offsetof(struct ui_vert, color);
    vertex_desc.attributes[2].bufferIndex = 0;
    vertex_desc.layouts[0].stride = sizeof(struct ui_vert);
    vertex_desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_desc.vertexFunction = vertex;
    pipeline_desc.fragmentFunction = fragment;
    pipeline_desc.vertexDescriptor = vertex_desc;
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline_desc.rasterSampleCount = sample_count;
    pipeline_desc.colorAttachments[0].blendingEnabled = YES;
    pipeline_desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    pipeline_desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    pipeline_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipeline_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    pipeline_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeline_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    id<MTLRenderPipelineState> pipeline = [s_device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    if(!pipeline) {
        fprintf(stderr, "Metal UI pipeline creation failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
    sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_ui_sampler = [s_device newSamplerStateWithDescriptor:sampler_desc];
    if(!s_ui_sampler) {
        fprintf(stderr, "Metal UI sampler creation failed.\n");
        release_ui_resources();
        return nil;
    }

    return pipeline;
}

static id<MTLRenderPipelineState> ensure_ui_pipeline(bool multisampled)
{
    id<MTLRenderPipelineState> __strong *slot = multisampled ? &s_ui_msaa_pipeline : &s_ui_pipeline;
    if(*slot && s_ui_sampler)
        return *slot;
    *slot = build_ui_pipeline(multisampled ? active_msaa_sample_count() : 1);
    return *slot;
}

static id<MTLRenderPipelineState> build_minimap_pipeline(NSUInteger sample_count)
{
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:s_minimap_shader_source];
    id<MTLLibrary> library = [s_device newLibraryWithSource:source options:nil error:&error];
    if(!library) {
        fprintf(stderr, "Metal minimap shader compile failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"minimap_vertex"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"minimap_fragment"];
    if(!vertex || !fragment) {
        fprintf(stderr, "Metal minimap shader entrypoint lookup failed.\n");
        return nil;
    }

    MTLVertexDescriptor *vertex_desc = [MTLVertexDescriptor vertexDescriptor];
    vertex_desc.attributes[0].format = MTLVertexFormatFloat2;
    vertex_desc.attributes[0].offset = offsetof(struct ui_vert, screen_pos);
    vertex_desc.attributes[0].bufferIndex = 0;
    vertex_desc.attributes[1].format = MTLVertexFormatFloat2;
    vertex_desc.attributes[1].offset = offsetof(struct ui_vert, uv);
    vertex_desc.attributes[1].bufferIndex = 0;
    vertex_desc.attributes[2].format = MTLVertexFormatUChar4Normalized;
    vertex_desc.attributes[2].offset = offsetof(struct ui_vert, color);
    vertex_desc.attributes[2].bufferIndex = 0;
    vertex_desc.layouts[0].stride = sizeof(struct ui_vert);
    vertex_desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_desc.vertexFunction = vertex;
    pipeline_desc.fragmentFunction = fragment;
    pipeline_desc.vertexDescriptor = vertex_desc;
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline_desc.rasterSampleCount = sample_count;
    pipeline_desc.colorAttachments[0].blendingEnabled = YES;
    pipeline_desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    pipeline_desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    pipeline_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipeline_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    pipeline_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeline_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    id<MTLRenderPipelineState> pipeline = [s_device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    if(!pipeline) {
        fprintf(stderr, "Metal minimap pipeline creation failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
    sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_minimap_sampler = [s_device newSamplerStateWithDescriptor:sampler_desc];
    if(!s_minimap_sampler) {
        fprintf(stderr, "Metal minimap sampler creation failed.\n");
        return nil;
    }

    return pipeline;
}

static id<MTLRenderPipelineState> ensure_minimap_pipeline(bool multisampled)
{
    id<MTLRenderPipelineState> __strong *slot = multisampled ? &s_minimap_msaa_pipeline : &s_minimap_pipeline;
    if(*slot && s_minimap_sampler)
        return *slot;
    *slot = build_minimap_pipeline(multisampled ? active_msaa_sample_count() : 1);
    return *slot;
}

static id<MTLRenderPipelineState> build_sprite_pipeline(NSUInteger sample_count)
{
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:s_sprite_shader_source];
    id<MTLLibrary> library = [s_device newLibraryWithSource:source options:nil error:&error];
    if(!library) {
        fprintf(stderr, "Metal sprite shader compile failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"sprite_vertex"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"sprite_fragment"];
    if(!vertex || !fragment) {
        fprintf(stderr, "Metal sprite shader entrypoint lookup failed.\n");
        return nil;
    }

    MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_desc.vertexFunction = vertex;
    pipeline_desc.fragmentFunction = fragment;
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline_desc.rasterSampleCount = sample_count;

    id<MTLRenderPipelineState> pipeline = [s_device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    if(!pipeline) {
        fprintf(stderr, "Metal sprite pipeline creation failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
    sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.mipFilter = MTLSamplerMipFilterLinear;
    sampler_desc.sAddressMode = MTLSamplerAddressModeRepeat;
    sampler_desc.tAddressMode = MTLSamplerAddressModeRepeat;
    s_sprite_sampler = [s_device newSamplerStateWithDescriptor:sampler_desc];
    if(!s_sprite_sampler) {
        fprintf(stderr, "Metal sprite sampler creation failed.\n");
        s_sprite_pipeline = nil;
        s_sprite_msaa_pipeline = nil;
        return nil;
    }

    return pipeline;
}

static id<MTLRenderPipelineState> ensure_sprite_pipeline(bool multisampled)
{
    id<MTLRenderPipelineState> __strong *slot = multisampled ? &s_sprite_msaa_pipeline : &s_sprite_pipeline;
    if(*slot && s_sprite_sampler)
        return *slot;
    *slot = build_sprite_pipeline(multisampled ? active_msaa_sample_count() : 1);
    return *slot;
}

static id<MTLRenderPipelineState> build_terrain_pipeline(NSUInteger sample_count, bool depth_enabled)
{
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:s_terrain_shader_source];
    id<MTLLibrary> library = [s_device newLibraryWithSource:source options:nil error:&error];
    if(!library) {
        fprintf(stderr, "Metal terrain shader compile failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"terrain_vertex"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"terrain_fragment"];
    if(!vertex || !fragment) {
        fprintf(stderr, "Metal terrain shader entrypoint lookup failed.\n");
        return nil;
    }

    MTLVertexDescriptor *vertex_desc = [MTLVertexDescriptor vertexDescriptor];
    vertex_desc.attributes[0].format = MTLVertexFormatFloat3;
    vertex_desc.attributes[0].offset = offsetof(struct terrain_vert, pos);
    vertex_desc.attributes[0].bufferIndex = 0;
    vertex_desc.attributes[1].format = MTLVertexFormatFloat2;
    vertex_desc.attributes[1].offset = offsetof(struct terrain_vert, uv);
    vertex_desc.attributes[1].bufferIndex = 0;
    vertex_desc.attributes[2].format = MTLVertexFormatFloat3;
    vertex_desc.attributes[2].offset = offsetof(struct terrain_vert, normal);
    vertex_desc.attributes[2].bufferIndex = 0;
    vertex_desc.attributes[3].format = MTLVertexFormatInt;
    vertex_desc.attributes[3].offset = offsetof(struct terrain_vert, material_idx);
    vertex_desc.attributes[3].bufferIndex = 0;
    vertex_desc.attributes[4].format = MTLVertexFormatUChar;
    vertex_desc.attributes[4].offset = offsetof(struct terrain_vert, blend_mode);
    vertex_desc.attributes[4].bufferIndex = 0;
    vertex_desc.attributes[5].format = MTLVertexFormatUChar;
    vertex_desc.attributes[5].offset = offsetof(struct terrain_vert, no_bump_map);
    vertex_desc.attributes[5].bufferIndex = 0;
    vertex_desc.attributes[6].format = MTLVertexFormatUShort;
    vertex_desc.attributes[6].offset = offsetof(struct terrain_vert, middle_indices);
    vertex_desc.attributes[6].bufferIndex = 0;
    vertex_desc.attributes[7].format = MTLVertexFormatUInt2;
    vertex_desc.attributes[7].offset = offsetof(struct terrain_vert, c1_indices);
    vertex_desc.attributes[7].bufferIndex = 0;
    vertex_desc.attributes[8].format = MTLVertexFormatUInt2;
    vertex_desc.attributes[8].offset = offsetof(struct terrain_vert, c2_indices);
    vertex_desc.attributes[8].bufferIndex = 0;
    vertex_desc.attributes[9].format = MTLVertexFormatUInt;
    vertex_desc.attributes[9].offset = offsetof(struct terrain_vert, tb_indices);
    vertex_desc.attributes[9].bufferIndex = 0;
    vertex_desc.attributes[10].format = MTLVertexFormatUInt;
    vertex_desc.attributes[10].offset = offsetof(struct terrain_vert, lr_indices);
    vertex_desc.attributes[10].bufferIndex = 0;
    vertex_desc.attributes[11].format = MTLVertexFormatUInt;
    vertex_desc.attributes[11].offset = offsetof(struct terrain_vert, wang_index);
    vertex_desc.attributes[11].bufferIndex = 0;
    vertex_desc.layouts[0].stride = sizeof(struct terrain_vert);
    vertex_desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_desc.vertexFunction = vertex;
    pipeline_desc.fragmentFunction = fragment;
    pipeline_desc.vertexDescriptor = vertex_desc;
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline_desc.rasterSampleCount = sample_count;
    if(depth_enabled)
        pipeline_desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

    id<MTLRenderPipelineState> pipeline = [s_device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    if(!pipeline) {
        fprintf(stderr, "Metal terrain pipeline creation failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    return pipeline;
}

static id<MTLRenderPipelineState> ensure_terrain_pipeline(bool multisampled, bool depth_enabled)
{
    id<MTLRenderPipelineState> __strong *slot = NULL;
    if(depth_enabled) {
        slot = multisampled ? &s_terrain_depth_msaa_pipeline : &s_terrain_depth_pipeline;
    } else {
        slot = multisampled ? &s_terrain_msaa_pipeline : &s_terrain_pipeline;
    }
    if(*slot)
        return *slot;
    *slot = build_terrain_pipeline(multisampled ? active_msaa_sample_count() : 1, depth_enabled);
    return *slot;
}

static bool ensure_scene_sampler(void)
{
    if(s_scene_sampler)
        return true;

    MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
    sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.mipFilter = MTLSamplerMipFilterNotMipmapped;
    sampler_desc.sAddressMode = MTLSamplerAddressModeRepeat;
    sampler_desc.tAddressMode = MTLSamplerAddressModeRepeat;
    s_scene_sampler = [s_device newSamplerStateWithDescriptor:sampler_desc];
    return s_scene_sampler != nil;
}

static bool ensure_terrain_sampler(void)
{
    if(s_terrain_sampler)
        return true;

    MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
    sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.mipFilter = MTLSamplerMipFilterLinear;
    sampler_desc.sAddressMode = MTLSamplerAddressModeRepeat;
    sampler_desc.tAddressMode = MTLSamplerAddressModeRepeat;
    s_terrain_sampler = [s_device newSamplerStateWithDescriptor:sampler_desc];
    return s_terrain_sampler != nil;
}

static bool ensure_material_sampler(void)
{
    if(s_material_sampler)
        return true;

    MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
    sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.mipFilter = MTLSamplerMipFilterLinear;
    sampler_desc.sAddressMode = MTLSamplerAddressModeRepeat;
    sampler_desc.tAddressMode = MTLSamplerAddressModeRepeat;
    s_material_sampler = [s_device newSamplerStateWithDescriptor:sampler_desc];
    return s_material_sampler != nil;
}

static id<MTLRenderPipelineState> build_static_mesh_pipeline(bool translucent, NSUInteger sample_count,
                                                             bool depth_enabled)
{
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:s_static_mesh_shader_source];
    id<MTLLibrary> library = [s_device newLibraryWithSource:source options:nil error:&error];
    if(!library) {
        fprintf(stderr, "Metal static mesh shader compile failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"static_mesh_vertex"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"static_mesh_fragment"];
    if(!vertex || !fragment) {
        fprintf(stderr, "Metal static mesh shader entrypoint lookup failed.\n");
        return nil;
    }

    MTLVertexDescriptor *vertex_desc = [MTLVertexDescriptor vertexDescriptor];
    vertex_desc.attributes[0].format = MTLVertexFormatFloat3;
    vertex_desc.attributes[0].offset = offsetof(struct vertex, pos);
    vertex_desc.attributes[0].bufferIndex = 0;
    vertex_desc.attributes[1].format = MTLVertexFormatFloat2;
    vertex_desc.attributes[1].offset = offsetof(struct vertex, uv);
    vertex_desc.attributes[1].bufferIndex = 0;
    vertex_desc.attributes[2].format = MTLVertexFormatFloat3;
    vertex_desc.attributes[2].offset = offsetof(struct vertex, normal);
    vertex_desc.attributes[2].bufferIndex = 0;
    vertex_desc.attributes[3].format = MTLVertexFormatInt;
    vertex_desc.attributes[3].offset = offsetof(struct vertex, material_idx);
    vertex_desc.attributes[3].bufferIndex = 0;
    vertex_desc.layouts[0].stride = sizeof(struct vertex);
    vertex_desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_desc.vertexFunction = vertex;
    pipeline_desc.fragmentFunction = fragment;
    pipeline_desc.vertexDescriptor = vertex_desc;
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline_desc.rasterSampleCount = sample_count;
    if(depth_enabled)
        pipeline_desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    if(translucent) {
        pipeline_desc.colorAttachments[0].blendingEnabled = YES;
        pipeline_desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        pipeline_desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        pipeline_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceColor;
        pipeline_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceColor;
        pipeline_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        pipeline_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }

    id<MTLRenderPipelineState> pipeline = [s_device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    if(!pipeline) {
        fprintf(stderr, "Metal static mesh pipeline creation failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    return pipeline;
}

static id<MTLRenderPipelineState> ensure_static_mesh_pipeline(bool translucent, bool multisampled,
                                                              bool depth_enabled)
{
    id<MTLRenderPipelineState> __strong *slot = NULL;
    if(depth_enabled && translucent) {
        slot = multisampled ? &s_static_mesh_blend_depth_msaa_pipeline : &s_static_mesh_blend_depth_pipeline;
    } else if(depth_enabled) {
        slot = multisampled ? &s_static_mesh_depth_msaa_pipeline : &s_static_mesh_depth_pipeline;
    } else if(translucent) {
        slot = multisampled ? &s_static_mesh_blend_msaa_pipeline : &s_static_mesh_blend_pipeline;
    } else {
        slot = multisampled ? &s_static_mesh_msaa_pipeline : &s_static_mesh_pipeline;
    }
    if(*slot)
        return *slot;
    *slot = build_static_mesh_pipeline(translucent, multisampled ? active_msaa_sample_count() : 1, depth_enabled);
    return *slot;
}

static id<MTLRenderPipelineState> build_world_color_pipeline(NSUInteger sample_count, bool depth_enabled)
{
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:s_world_color_shader_source];
    id<MTLLibrary> library = [s_device newLibraryWithSource:source options:nil error:&error];
    if(!library) {
        fprintf(stderr, "Metal world-color shader compile failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"world_color_vertex"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"world_color_fragment"];
    if(!vertex || !fragment) {
        fprintf(stderr, "Metal world-color shader entrypoint lookup failed.\n");
        return nil;
    }

    MTLVertexDescriptor *vertex_desc = [MTLVertexDescriptor vertexDescriptor];
    vertex_desc.attributes[0].format = MTLVertexFormatFloat3;
    vertex_desc.attributes[0].offset = offsetof(struct colored_vert, pos);
    vertex_desc.attributes[0].bufferIndex = 0;
    vertex_desc.attributes[1].format = MTLVertexFormatFloat4;
    vertex_desc.attributes[1].offset = offsetof(struct colored_vert, color);
    vertex_desc.attributes[1].bufferIndex = 0;
    vertex_desc.layouts[0].stride = sizeof(struct colored_vert);
    vertex_desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_desc.vertexFunction = vertex;
    pipeline_desc.fragmentFunction = fragment;
    pipeline_desc.vertexDescriptor = vertex_desc;
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if(depth_enabled)
        pipeline_desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pipeline_desc.rasterSampleCount = sample_count;
    pipeline_desc.colorAttachments[0].blendingEnabled = YES;
    pipeline_desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    pipeline_desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    pipeline_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipeline_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    pipeline_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeline_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    id<MTLRenderPipelineState> pipeline = [s_device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    if(!pipeline) {
        fprintf(stderr, "Metal world-color pipeline creation failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    return pipeline;
}

static id<MTLRenderPipelineState> ensure_world_color_pipeline(bool multisampled, bool depth_enabled)
{
    id<MTLRenderPipelineState> __strong *slot = NULL;
    if(depth_enabled) {
        slot = multisampled ? &s_world_color_depth_msaa_pipeline : &s_world_color_depth_pipeline;
    } else {
        slot = multisampled ? &s_world_color_msaa_pipeline : &s_world_color_pipeline;
    }
    if(*slot)
        return *slot;
    *slot = build_world_color_pipeline(multisampled ? active_msaa_sample_count() : 1, depth_enabled);
    return *slot;
}

static id<MTLRenderPipelineState> build_water_surface_pipeline(NSUInteger sample_count)
{
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:s_water_surface_shader_source];
    id<MTLLibrary> library = [s_device newLibraryWithSource:source options:nil error:&error];
    if(!library) {
        fprintf(stderr, "Metal water-surface shader compile failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"water_surface_vertex"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"water_surface_fragment"];
    if(!vertex || !fragment) {
        fprintf(stderr, "Metal water-surface shader entrypoint lookup failed.\n");
        return nil;
    }

    MTLVertexDescriptor *vertex_desc = [MTLVertexDescriptor vertexDescriptor];
    vertex_desc.attributes[0].format = MTLVertexFormatFloat3;
    vertex_desc.attributes[0].offset = 0;
    vertex_desc.attributes[0].bufferIndex = 0;
    vertex_desc.layouts[0].stride = sizeof(vec3_t);
    vertex_desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_desc.vertexFunction = vertex;
    pipeline_desc.fragmentFunction = fragment;
    pipeline_desc.vertexDescriptor = vertex_desc;
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline_desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pipeline_desc.rasterSampleCount = sample_count;
    pipeline_desc.colorAttachments[0].blendingEnabled = YES;
    pipeline_desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    pipeline_desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    pipeline_desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipeline_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    pipeline_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeline_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    id<MTLRenderPipelineState> pipeline = [s_device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    if(!pipeline) {
        fprintf(stderr, "Metal water-surface pipeline creation failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    return pipeline;
}

static id<MTLRenderPipelineState> ensure_water_surface_pipeline(bool multisampled)
{
    id<MTLRenderPipelineState> __strong *slot = multisampled ? &s_water_surface_msaa_pipeline : &s_water_surface_pipeline;
    if(*slot)
        return *slot;
    *slot = build_water_surface_pipeline(multisampled ? active_msaa_sample_count() : 1);
    return *slot;
}

static id<MTLRenderPipelineState> build_skybox_pipeline(NSUInteger sample_count)
{
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:s_skybox_shader_source];
    id<MTLLibrary> library = [s_device newLibraryWithSource:source options:nil error:&error];
    if(!library) {
        fprintf(stderr, "Metal skybox shader compile failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"skybox_vertex"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"skybox_fragment"];
    if(!vertex || !fragment) {
        fprintf(stderr, "Metal skybox shader entrypoint lookup failed.\n");
        return nil;
    }

    MTLVertexDescriptor *vertex_desc = [MTLVertexDescriptor vertexDescriptor];
    vertex_desc.attributes[0].format = MTLVertexFormatFloat3;
    vertex_desc.attributes[0].offset = 0;
    vertex_desc.attributes[0].bufferIndex = 0;
    vertex_desc.layouts[0].stride = sizeof(vec3_t);
    vertex_desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_desc.vertexFunction = vertex;
    pipeline_desc.fragmentFunction = fragment;
    pipeline_desc.vertexDescriptor = vertex_desc;
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline_desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pipeline_desc.rasterSampleCount = sample_count;

    id<MTLRenderPipelineState> pipeline = [s_device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    if(!pipeline) {
        fprintf(stderr, "Metal skybox pipeline creation failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    return pipeline;
}

static id<MTLRenderPipelineState> ensure_skybox_pipeline(bool multisampled)
{
    id<MTLRenderPipelineState> __strong *slot = multisampled ? &s_skybox_msaa_pipeline : &s_skybox_pipeline;
    if(*slot)
        return *slot;
    *slot = build_skybox_pipeline(multisampled ? active_msaa_sample_count() : 1);
    return *slot;
}

static bool ensure_skybox_sampler(void)
{
    if(s_skybox_sampler)
        return true;

    MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
    sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.mipFilter = MTLSamplerMipFilterNotMipmapped;
    sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_desc.rAddressMode = MTLSamplerAddressModeClampToEdge;
    s_skybox_sampler = [s_device newSamplerStateWithDescriptor:sampler_desc];
    return s_skybox_sampler != nil;
}

static id<MTLRenderPipelineState> build_shadow_depth_pipeline(size_t vertex_stride, bool owner_output)
{
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:s_shadow_depth_shader_source];
    id<MTLLibrary> library = [s_device newLibraryWithSource:source options:nil error:&error];
    if(!library) {
        fprintf(stderr, "Metal shadow-depth shader compile failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"shadow_depth_vertex"];
    if(!vertex) {
        fprintf(stderr, "Metal shadow-depth shader entrypoint lookup failed.\n");
        return nil;
    }
    id<MTLFunction> fragment = nil;
    if(owner_output) {
        fragment = [library newFunctionWithName:@"shadow_owner_fragment"];
        if(!fragment) {
            fprintf(stderr, "Metal shadow-owner shader entrypoint lookup failed.\n");
            return nil;
        }
    }

    MTLVertexDescriptor *vertex_desc = [MTLVertexDescriptor vertexDescriptor];
    vertex_desc.attributes[0].format = MTLVertexFormatFloat3;
    vertex_desc.attributes[0].offset = 0;
    vertex_desc.attributes[0].bufferIndex = 0;
    vertex_desc.layouts[0].stride = vertex_stride;
    vertex_desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_desc.vertexFunction = vertex;
    pipeline_desc.fragmentFunction = fragment;
    pipeline_desc.vertexDescriptor = vertex_desc;
    pipeline_desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    if(owner_output)
        pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;

    id<MTLRenderPipelineState> pipeline = [s_device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    if(!pipeline) {
        fprintf(stderr, "Metal shadow-depth pipeline creation failed: %s\n",
            error ? [[error localizedDescription] UTF8String] : "unknown error");
        return nil;
    }

    return pipeline;
}

static id<MTLRenderPipelineState> ensure_shadow_depth_pipeline(bool terrain, bool owner_output)
{
    id<MTLRenderPipelineState> __strong *slot = NULL;
    if(owner_output)
        slot = terrain ? &s_shadow_owner_terrain_pipeline : &s_shadow_owner_mesh_pipeline;
    else
        slot = terrain ? &s_shadow_terrain_pipeline : &s_shadow_mesh_pipeline;
    if(*slot)
        return *slot;
    *slot = build_shadow_depth_pipeline(terrain ? sizeof(struct terrain_vert) : sizeof(struct vertex),
        owner_output);
    return *slot;
}

static void shadow_screen_probe_reset_frame(void);
static void shadow_screen_probe_dump_if_requested(void);
static void water_reflection_dump_finish_if_pending(void);

static void frame_begin(void)
{
    if(s_frame_command_buffer)
        return;

    capture_maybe_start();
    update_drawable_size();
    if(!reserve_inflight_frame())
        return;

    s_frame_drawable = [s_layer nextDrawable];
    if(!s_frame_drawable) {
        release_inflight_frame();
        return;
    }

    NSUInteger sample_count = desired_frame_sample_count();
    if(sample_count > 1 && !ensure_frame_msaa_texture(sample_count)) {
        sample_count = 1;
    }
    s_frame_sample_count = sample_count;
    (void)ensure_frame_depth_texture(sample_count);

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if(sample_count > 1) {
        pass.colorAttachments[0].texture = s_frame_msaa_texture;
        pass.colorAttachments[0].resolveTexture = s_frame_drawable.texture;
        pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    } else {
        pass.colorAttachments[0].texture = s_frame_drawable.texture;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.3, 0.3, 1.0);
    if(s_frame_depth_texture) {
        pass.depthAttachment.texture = s_frame_depth_texture;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 1.0;
    }

    s_frame_command_buffer = [s_queue commandBuffer];
    if(!s_frame_command_buffer) {
        reset_frame_state();
        release_inflight_frame();
        return;
    }
    s_frame_encoder = [s_frame_command_buffer renderCommandEncoderWithDescriptor:pass];
    if(!s_frame_encoder) {
        reset_frame_state();
        release_inflight_frame();
        return;
    }
    shadow_screen_probe_reset_frame();

    MTLViewport viewport = {
        .originX = 0.0,
        .originY = 0.0,
        .width = s_layer.drawableSize.width,
        .height = s_layer.drawableSize.height,
        .znear = 0.0,
        .zfar = 1.0
    };
    [s_frame_encoder setViewport:viewport];
}

static bool frame_resume(void)
{
    if(s_frame_encoder)
        return true;
    if(!s_frame_command_buffer || !s_frame_drawable)
        return false;

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if(s_frame_sample_count > 1 && s_frame_msaa_texture) {
        pass.colorAttachments[0].texture = s_frame_msaa_texture;
        pass.colorAttachments[0].resolveTexture = s_frame_drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    } else {
        pass.colorAttachments[0].texture = s_frame_drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    if(s_frame_depth_texture) {
        pass.depthAttachment.texture = s_frame_depth_texture;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
    }

    s_frame_encoder = [s_frame_command_buffer renderCommandEncoderWithDescriptor:pass];
    if(!s_frame_encoder)
        return false;

    MTLViewport viewport = {
        .originX = 0.0,
        .originY = 0.0,
        .width = s_layer.drawableSize.width,
        .height = s_layer.drawableSize.height,
        .znear = 0.0,
        .zfar = 1.0
    };
    [s_frame_encoder setViewport:viewport];
    return true;
}

static void frame_end(void)
{
    if(!s_frame_encoder)
        return;
    [s_frame_encoder endEncoding];
    s_frame_encoder = nil;
}

static void frame_present(void)
{
    if(!s_frame_command_buffer) {
        present_clear();
        return;
    }

    frame_end();
    if(s_frame_inflight_reserved && s_inflight_semaphore) {
        dispatch_semaphore_t inflight = s_inflight_semaphore;
        [s_frame_command_buffer addCompletedHandler:^(id<MTLCommandBuffer> buffer){
            (void)buffer;
            dispatch_semaphore_signal(inflight);
        }];
        s_frame_inflight_reserved = false;
    }
    [s_frame_command_buffer presentDrawable:s_frame_drawable];
    bool shadow_probe = shadow_screen_probe_enabled();
    bool water_dump_pending = s_water_reflection_dump_pending;
    [s_frame_command_buffer commit];
    if(shadow_probe || water_dump_pending) {
        [s_frame_command_buffer waitUntilCompleted];
    }
    if(shadow_probe) {
        shadow_screen_probe_dump_if_requested();
    }
    water_reflection_dump_finish_if_pending();
    capture_note_presented();
    reset_frame_state();
}

static void frame_abort(void)
{
    if(s_frame_encoder) {
        [s_frame_encoder endEncoding];
    }
    reset_frame_state();
    release_inflight_frame();
}

static bool shadow_enabled_for_draw(void)
{
    return s_shadows_enabled && s_shadow_map_valid && s_shadow_depth_texture && s_shadow_sampler;
}

static void shadow_screen_probe_reset_frame(void)
{
    if(!ensure_shadow_screen_probe_buffers())
        return;

    memset(s_shadow_screen_probe_buffer.contents, 0, 16 * sizeof(uint32_t));
    uint32_t *params = s_shadow_screen_probe_params_buffer.contents;
    params[0] = shadow_screen_probe_enabled() ? 1u : 0u;
    params[1] = s_shadow_screen_probe_x;
    params[2] = s_shadow_screen_probe_y;
    params[3] = s_shadow_screen_probe_radius;
}

static void bind_shadow_screen_probe_resources(id<MTLRenderCommandEncoder> encoder,
                                               NSUInteger owner_texture_index,
                                               NSUInteger probe_buffer_index,
                                               NSUInteger probe_params_index)
{
    if(!encoder || !ensure_shadow_screen_probe_buffers() || !ensure_shadow_owner_dummy_texture())
        return;

    bool enabled = (encoder == s_frame_encoder) && shadow_screen_probe_enabled();
    id<MTLBuffer> params_buffer = enabled
        ? s_shadow_screen_probe_params_buffer
        : s_shadow_screen_probe_disabled_params_buffer;
    if(enabled) {
        uint32_t *params = s_shadow_screen_probe_params_buffer.contents;
        params[0] = 1u;
        params[1] = s_shadow_screen_probe_x;
        params[2] = s_shadow_screen_probe_y;
        params[3] = s_shadow_screen_probe_radius;
    }

    id<MTLTexture> owner_texture = (enabled && s_shadow_owner_texture)
        ? s_shadow_owner_texture
        : s_shadow_owner_dummy_texture;
    [encoder setFragmentTexture:owner_texture
                        atIndex:owner_texture_index];
    [encoder setFragmentBuffer:s_shadow_screen_probe_buffer offset:0 atIndex:probe_buffer_index];
    [encoder setFragmentBuffer:params_buffer offset:0 atIndex:probe_params_index];
}

static void shadow_screen_probe_dump_if_requested(void)
{
    if(!shadow_screen_probe_enabled() || !s_shadow_screen_probe_buffer)
        return;
    if(!s_shadow_screen_probe_output_path[0])
        return;

    const uint32_t *row = s_shadow_screen_probe_buffer.contents;
    if(row[0] == 0)
        return;

    FILE *fp = fopen(s_shadow_screen_probe_output_path, "w");
    if(!fp)
        return;

    fprintf(fp,
        "hit=%u screen=%u,%u shadow_texel=%u,%u owner=%u surface=%s valid=%u "
        "proj_z=%.6f closest_depth=%.6f compare_depth=%.6f shadowed=%u "
        "world=(%.3f,%.3f,%.3f) material=%u\n",
        row[0], row[1], row[2], row[3], row[4], row[5],
        shadow_probe_kind_name(row[6]), row[7],
        uint_bits_to_float(row[8]), uint_bits_to_float(row[9]), uint_bits_to_float(row[14]),
        row[15],
        uint_bits_to_float(row[10]), uint_bits_to_float(row[11]), uint_bits_to_float(row[12]),
        row[13]);
    fclose(fp);
}

static void shadow_pass_begin(const vec3_t *light_pos, const vec3_t *cam_pos, const vec3_t *cam_dir)
{
    if(!light_pos || !cam_pos || !cam_dir)
        return;
    if(!ensure_shadow_depth_texture() || !ensure_shadow_sampler())
        return;
    const char *owner_dump_path = getenv("PF_METAL_SHADOW_OWNER_DUMP_U32_PATH");
    bool owner_requested = (owner_dump_path && *owner_dump_path) || shadow_screen_probe_enabled();
    if(owner_requested && !ensure_shadow_owner_texture())
        owner_requested = false;

    make_shadow_light_space(*light_pos, *cam_pos, *cam_dir,
        &s_shadow_view, &s_shadow_proj, &s_shadow_light_space);

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = s_shadow_depth_texture;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 1.0;
    if(owner_requested) {
        pass.colorAttachments[0].texture = s_shadow_owner_texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    }

    s_shadow_command_buffer = [s_queue commandBuffer];
    if(!s_shadow_command_buffer)
        return;

    s_shadow_encoder = [s_shadow_command_buffer renderCommandEncoderWithDescriptor:pass];
    if(!s_shadow_encoder) {
        s_shadow_command_buffer = nil;
        return;
    }

    MTLViewport viewport = {
        .originX = 0.0,
        .originY = 0.0,
        .width = CONFIG_SHADOW_MAP_RES,
        .height = CONFIG_SHADOW_MAP_RES,
        .znear = 0.0,
        .zfar = 1.0
    };
    [s_shadow_encoder setViewport:viewport];

    id<MTLDepthStencilState> depth_state = ensure_depth_state(true);
    if(depth_state)
        [s_shadow_encoder setDepthStencilState:depth_state];
    [s_shadow_encoder setCullMode:MTLCullModeFront];
    [s_shadow_encoder setFrontFacingWinding:MTLWindingCounterClockwise];

    s_shadow_pass_active = true;
    s_shadow_owner_pass_active = owner_requested;
    s_shadow_map_valid = false;
    memset(&s_shadow_caster_stats, 0, sizeof(s_shadow_caster_stats));
}

static float *metal_copy_shadow_depth(void)
{
    if(!s_shadow_depth_texture || !s_device || !s_queue)
        return NULL;

    NSUInteger w = s_shadow_depth_texture.width;
    NSUInteger h = s_shadow_depth_texture.height;
    size_t bpr = w * sizeof(float);
    size_t total = bpr * h;

    id<MTLBuffer> staging = [s_device newBufferWithLength:total
        options:MTLResourceStorageModeShared];
    if(!staging)
        return NULL;

    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:s_shadow_depth_texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:staging
        destinationOffset:0
   destinationBytesPerRow:bpr
 destinationBytesPerImage:total];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    float *depth = malloc(total);
    if(!depth)
        return NULL;
    memcpy(depth, staging.contents, total);
    return depth;
}

static uint32_t *metal_copy_shadow_owner(void)
{
    if(!s_shadow_owner_texture || !s_device || !s_queue)
        return NULL;

    NSUInteger w = s_shadow_owner_texture.width;
    NSUInteger h = s_shadow_owner_texture.height;
    size_t bpr = w * sizeof(uint32_t);
    size_t total = bpr * h;

    id<MTLBuffer> staging = [s_device newBufferWithLength:total
        options:MTLResourceStorageModeShared];
    if(!staging)
        return NULL;

    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:s_shadow_owner_texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:staging
        destinationOffset:0
   destinationBytesPerRow:bpr
 destinationBytesPerImage:total];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    uint32_t *owners = malloc(total);
    if(!owners)
        return NULL;
    memcpy(owners, staging.contents, total);
    return owners;
}

static void metal_dump_shadow_depth_preview_to_file(const char *path, const float *src)
{
    if(!path || !src || !s_shadow_depth_texture)
        return;

    NSUInteger w = s_shadow_depth_texture.width;
    NSUInteger h = s_shadow_depth_texture.height;
    FILE *fp = fopen(path, "wb");
    if(!fp)
        return;
    unsigned char *rgba = malloc(w * h * 4);
    if(rgba) {
        for(NSUInteger i = 0; i < w * h; i++) {
            float d = src[i];
            if(d < 0.0f) d = 0.0f;
            if(d > 1.0f) d = 1.0f;
            unsigned char g = (unsigned char)(d * 255.0f + 0.5f);
            rgba[i*4 + 0] = g;
            rgba[i*4 + 1] = g;
            rgba[i*4 + 2] = g;
            rgba[i*4 + 3] = 255;
        }
        fwrite(rgba, 1, w * h * 4, fp);
        free(rgba);
    }
    fclose(fp);
    fprintf(stderr, "PF_METAL_SHADOW_DUMP wrote %llux%llu RGBA8 raw to %s\n",
        (unsigned long long)w, (unsigned long long)h, path);
}

static void metal_dump_shadow_depth_f32_to_file(const char *path, const float *depth)
{
    if(!path || !depth || !s_shadow_depth_texture)
        return;

    NSUInteger w = s_shadow_depth_texture.width;
    NSUInteger h = s_shadow_depth_texture.height;
    FILE *fp = fopen(path, "wb");
    if(!fp)
        return;
    fwrite(depth, sizeof(float), w * h, fp);
    fclose(fp);
    fprintf(stderr, "PF_METAL_SHADOW_DUMP_F32 wrote %llux%llu float32 raw to %s\n",
        (unsigned long long)w, (unsigned long long)h, path);
}

static void metal_dump_shadow_depth_if_requested(void)
{
    const char *preview_path = getenv("PF_METAL_SHADOW_DUMP_PATH");
    const char *f32_path = getenv("PF_METAL_SHADOW_DUMP_F32_PATH");
    if((!preview_path || !*preview_path) && (!f32_path || !*f32_path))
        return;

    const char *min_static = getenv("PF_METAL_SHADOW_DUMP_MIN_STATIC_DRAWS");
    if(min_static && *min_static && s_shadow_caster_stats.static_draws < (unsigned)strtoul(min_static, NULL, 10))
        return;
    const char *min_anim = getenv("PF_METAL_SHADOW_DUMP_MIN_ANIM_DRAWS");
    if(min_anim && *min_anim && s_shadow_caster_stats.anim_draws < (unsigned)strtoul(min_anim, NULL, 10))
        return;

    float *depth = metal_copy_shadow_depth();
    if(!depth)
        return;

    if(f32_path && *f32_path)
        metal_dump_shadow_depth_f32_to_file(f32_path, depth);
    if(preview_path && *preview_path)
        metal_dump_shadow_depth_preview_to_file(preview_path, depth);

    free(depth);
}

static void metal_dump_shadow_owner_u32_to_file(const char *path, const uint32_t *owners)
{
    if(!path || !owners || !s_shadow_owner_texture)
        return;

    NSUInteger w = s_shadow_owner_texture.width;
    NSUInteger h = s_shadow_owner_texture.height;
    FILE *fp = fopen(path, "wb");
    if(!fp)
        return;
    fwrite(owners, sizeof(uint32_t), w * h, fp);
    fclose(fp);
    fprintf(stderr, "PF_METAL_SHADOW_OWNER_DUMP_U32 wrote %llux%llu uint32 raw to %s\n",
        (unsigned long long)w, (unsigned long long)h, path);
}

static void metal_dump_shadow_owner_if_requested(void)
{
    const char *path = getenv("PF_METAL_SHADOW_OWNER_DUMP_U32_PATH");
    if(!path || !*path || !s_shadow_owner_pass_active)
        return;

    const char *min_static = getenv("PF_METAL_SHADOW_DUMP_MIN_STATIC_DRAWS");
    if(min_static && *min_static && s_shadow_caster_stats.static_draws < (unsigned)strtoul(min_static, NULL, 10))
        return;
    const char *min_anim = getenv("PF_METAL_SHADOW_DUMP_MIN_ANIM_DRAWS");
    if(min_anim && *min_anim && s_shadow_caster_stats.anim_draws < (unsigned)strtoul(min_anim, NULL, 10))
        return;

    uint32_t *owners = metal_copy_shadow_owner();
    if(!owners)
        return;
    metal_dump_shadow_owner_u32_to_file(path, owners);
    free(owners);
}

static void shadow_pass_end(void)
{
    if(!s_shadow_pass_active)
        return;

    if(s_shadow_encoder) {
        [s_shadow_encoder endEncoding];
        s_shadow_encoder = nil;
    }
    if(s_shadow_command_buffer) {
        [s_shadow_command_buffer commit];
        [s_shadow_command_buffer waitUntilCompleted];
        s_shadow_command_buffer = nil;
        s_shadow_map_valid = true;

        if(getenv("PF_METAL_SHADOW_CASTER_LOG")) {
            fprintf(stderr,
                "PF_METAL_SHADOW_CASTERS terrain=%u/%zu static=%u/%zu anim=%u/%zu\n",
                s_shadow_caster_stats.terrain_draws, s_shadow_caster_stats.terrain_verts,
                s_shadow_caster_stats.static_draws, s_shadow_caster_stats.static_verts,
                s_shadow_caster_stats.anim_draws, s_shadow_caster_stats.anim_verts);
        }
        metal_dump_shadow_depth_if_requested();
        metal_dump_shadow_owner_if_requested();
    }
    s_shadow_pass_active = false;
    s_shadow_owner_pass_active = false;
}

static void render_shadow_vertex_stream(const void *verts, size_t verts_size, size_t vertex_count,
                                        const mat4x4_t *model, bool terrain,
                                        uint32_t owner_uid)
{
    if(!s_shadow_pass_active || !s_shadow_encoder || !verts || !verts_size || !vertex_count || !model)
        return;

    id<MTLRenderPipelineState> pipeline = ensure_shadow_depth_pipeline(terrain,
        s_shadow_owner_pass_active);
    if(!pipeline)
        return;

    id<MTLBuffer> vertex_buffer = [s_device newBufferWithBytes:verts
        length:verts_size options:MTLResourceStorageModeShared];
    if(!vertex_buffer)
        return;

    struct metal_shadow_uniforms uniforms = {
        .model = matrix_from_pf_mat4(model),
        .view = s_shadow_view,
        .proj = s_shadow_proj,
        .owner_params = {owner_uid, 0u, 0u, 0u},
    };
    id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
        length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    if(!uniform_buffer)
        return;

    [s_shadow_encoder setRenderPipelineState:pipeline];
    [s_shadow_encoder setFrontFacingWinding:terrain ? MTLWindingCounterClockwise : MTLWindingClockwise];
    [s_shadow_encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [s_shadow_encoder setVertexBuffer:uniform_buffer offset:0 atIndex:1];
    if(s_shadow_owner_pass_active)
        [s_shadow_encoder setFragmentBuffer:uniform_buffer offset:0 atIndex:1];
    [s_shadow_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                         vertexStart:0
                         vertexCount:vertex_count];
}

static void render_shadow_depth_draw(const struct render_private *priv, const mat4x4_t *model,
                                     uint32_t owner_uid)
{
    if(!priv || !model)
        return;

    if(priv->metal_is_terrain) {
        if(!priv->metal_terrain_verts || !priv->metal_terrain_verts_size || !priv->mesh.num_verts)
            return;
        s_shadow_caster_stats.terrain_draws++;
        s_shadow_caster_stats.terrain_verts += priv->mesh.num_verts;
        render_shadow_vertex_stream(priv->metal_terrain_verts, priv->metal_terrain_verts_size,
            priv->mesh.num_verts, model, true, owner_uid);
        return;
    }

    if(priv->metal_is_static_mesh && !priv->uses_pose_buffer) {
        if(!priv->metal_static_verts || !priv->metal_static_verts_size || !priv->mesh.num_verts)
            return;
        s_shadow_caster_stats.static_draws++;
        s_shadow_caster_stats.static_verts += priv->mesh.num_verts;
        render_shadow_vertex_stream(priv->metal_static_verts, priv->metal_static_verts_size,
            priv->mesh.num_verts, model, false, owner_uid);
        return;
    }

    if(priv->metal_is_anim_mesh && priv->uses_pose_buffer && s_have_anim_uid) {
        struct vertex *skinned = malloc(priv->mesh.num_verts * sizeof(*skinned));
        if(!skinned)
            return;

        size_t dst_idx = 0;
        bool ok = append_skinned_anim_mesh(priv, s_curr_anim_uid, model, skinned, &dst_idx);
        if(ok) {
            mat4x4_t identity;
            PFM_Mat4x4_Identity(&identity);
            s_shadow_caster_stats.anim_draws++;
            s_shadow_caster_stats.anim_verts += dst_idx;
            render_shadow_vertex_stream(skinned, dst_idx * sizeof(*skinned), dst_idx, &identity,
                false, owner_uid);
        }
        free(skinned);
    }
}

static void render_shadow_batched_stat_entities(const vec_rstat_t *ents)
{
    vec_rstat_t *mutable_ents = (vec_rstat_t *)ents;
    if(!mutable_ents)
        return;

    for(int i = 0; i < vec_size(mutable_ents); i++) {
        const struct ent_stat_rstate *curr = &vec_AT(mutable_ents, i);
        render_shadow_depth_draw(curr->render_private, &curr->model, curr->uid);
    }
}

static void render_shadow_batched_anim_entities(const vec_ranim_t *ents)
{
    vec_ranim_t *mutable_ents = (vec_ranim_t *)ents;
    if(!mutable_ents)
        return;

    for(int i = 0; i < vec_size(mutable_ents); i++) {
        const struct ent_anim_rstate *curr = &vec_AT(mutable_ents, i);
        set_current_anim_uid(curr->uid);
        render_shadow_depth_draw(curr->render_private, &curr->model, curr->uid);
    }
}

static MTLScissorRect scissor_rect_for_cmd(const struct nk_draw_command *cmd,
                                           struct nk_vec2i curr_vres,
                                           int drawable_w, int drawable_h)
{
    const float sx = (float)drawable_w / curr_vres.x;
    const float sy = (float)drawable_h / curr_vres.y;

    NSInteger x = (NSInteger)floorf(cmd->clip_rect.x * sx);
    NSInteger y = (NSInteger)floorf(cmd->clip_rect.y * sy);
    NSInteger w = (NSInteger)ceilf(cmd->clip_rect.w * sx);
    NSInteger h = (NSInteger)ceilf(cmd->clip_rect.h * sy);

    if(x < 0) {
        w += x;
        x = 0;
    }
    if(y < 0) {
        h += y;
        y = 0;
    }
    if(x > drawable_w)
        x = drawable_w;
    if(y > drawable_h)
        y = drawable_h;
    if(x + w > drawable_w)
        w = drawable_w - x;
    if(y + h > drawable_h)
        h = drawable_h - y;
    if(w < 0)
        w = 0;
    if(h < 0)
        h = 0;

    return (MTLScissorRect){
        .x = (NSUInteger)x,
        .y = (NSUInteger)y,
        .width = (NSUInteger)w,
        .height = (NSUInteger)h
    };
}

static id<MTLTexture> load_rgba_texture_2d(const char *path);

static id<MTLTexture> ui_texpath_get_or_load(const char *full_path)
{
    if(!full_path || !s_device)
        return nil;
    if(!s_ui_texpath_cache)
        s_ui_texpath_cache = [[NSMutableDictionary alloc] init];
    NSString *key = [NSString stringWithUTF8String:full_path];
    id<MTLTexture> tex = [s_ui_texpath_cache objectForKey:key];
    if(tex)
        return tex;
    tex = load_rgba_texture_2d(full_path);
    if(tex)
        [s_ui_texpath_cache setObject:tex forKey:key];
    return tex;
}

static void render_ui_draw_list(const struct nk_draw_list *dl)
{
    id<MTLRenderPipelineState> pipeline = nil;
    if(!s_ui_font_texture)
        return;

    frame_begin();
    if(!s_frame_encoder)
        return;
    pipeline = ensure_ui_pipeline(frame_uses_msaa());
    if(!pipeline)
        return;

    id<MTLBuffer> vertex_buffer = [s_device newBufferWithBytes:dl->vertices->memory.ptr
        length:dl->vertices->memory.size options:MTLResourceStorageModeShared];
    id<MTLBuffer> index_buffer = [s_device newBufferWithBytes:dl->elements->memory.ptr
        length:dl->elements->memory.size options:MTLResourceStorageModeShared];
    if(!vertex_buffer || !index_buffer)
        return;

    [s_frame_encoder setRenderPipelineState:pipeline];
    [s_frame_encoder setDepthStencilState:nil];
    [s_frame_encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [s_frame_encoder setFragmentSamplerState:s_ui_sampler atIndex:0];

    int drawable_w = (int)s_layer.drawableSize.width;
    int drawable_h = (int)s_layer.drawableSize.height;
    struct nk_vec2i curr_vres = {drawable_w, drawable_h};
    NSUInteger index_offset = 0;

    const struct nk_draw_command *cmd;
    for(cmd = nk__draw_list_begin(dl, dl->buffer); cmd;
        cmd = nk__draw_list_next(cmd, dl->buffer, dl)) {

        if(cmd->userdata.ptr) {
            struct nk_command_userdata *ud = cmd->userdata.ptr;
            enum nk_command_type ud_type = ud->type;
            if(ud_type == NK_COMMAND_IMAGE_TEXPATH
            || ud_type == NK_COMMAND_IMAGE_TEXPATH_REGION) {
                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s",
                    g_basepath, ud->image.texpath);
                s_pending_ui_texture = ui_texpath_get_or_load(full_path);
                PF_FREE(ud);
                /* Fall through to draw the image cmd using the loaded texture. */
            } else {
                if(ud_type == NK_COMMAND_SET_VRES)
                    curr_vres = ud->vec2i;
                PF_FREE(ud);
                continue;
            }
        }

        if(!cmd->elem_count) {
            continue;
        }

        bool use_image_texture = (s_pending_ui_texture != nil);
        if(!use_image_texture && (uintptr_t)cmd->texture.id != 1) {
            index_offset += cmd->elem_count * sizeof(nk_draw_index);
            continue;
        }

        MTLScissorRect scissor = scissor_rect_for_cmd(cmd, curr_vres, drawable_w, drawable_h);
        if(scissor.width == 0 || scissor.height == 0) {
            index_offset += cmd->elem_count * sizeof(nk_draw_index);
            s_pending_ui_texture = nil;
            continue;
        }

        struct metal_ui_uniforms uniforms = {
            .view_size = {curr_vres.x, curr_vres.y},
            ._padding = {0.0f, 0.0f}
        };
        id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
            length:sizeof(uniforms) options:MTLResourceStorageModeShared];
        if(!uniform_buffer) {
            index_offset += cmd->elem_count * sizeof(nk_draw_index);
            s_pending_ui_texture = nil;
            continue;
        }

        id<MTLTexture> binding = use_image_texture ? s_pending_ui_texture : s_ui_font_texture;
        [s_frame_encoder setFragmentTexture:binding atIndex:0];
        [s_frame_encoder setVertexBuffer:uniform_buffer offset:0 atIndex:1];
        [s_frame_encoder setScissorRect:scissor];
        [s_frame_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                    indexCount:cmd->elem_count
                                     indexType:MTLIndexTypeUInt32
                                   indexBuffer:index_buffer
                             indexBufferOffset:index_offset];

        index_offset += cmd->elem_count * sizeof(nk_draw_index);
        s_pending_ui_texture = nil;
    }
}

static void render_ui_triangles(const struct ui_vert *verts, size_t nverts, id<MTLTexture> texture)
{
    id<MTLRenderPipelineState> pipeline = nil;
    if(!verts || !nverts || !texture)
        return;

    frame_begin();
    if(!s_frame_encoder)
        return;
    pipeline = ensure_ui_pipeline(frame_uses_msaa());
    if(!pipeline)
        return;

    id<MTLBuffer> vertex_buffer = [s_device newBufferWithBytes:verts
        length:nverts * sizeof(*verts) options:MTLResourceStorageModeShared];
    if(!vertex_buffer)
        return;

    int drawable_w = (int)s_layer.drawableSize.width;
    int drawable_h = (int)s_layer.drawableSize.height;
    struct metal_ui_uniforms uniforms = {
        .view_size = {drawable_w, drawable_h},
        ._padding = {0.0f, 0.0f}
    };
    id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
        length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    if(!uniform_buffer)
        return;

    [s_frame_encoder setRenderPipelineState:pipeline];
    [s_frame_encoder setDepthStencilState:nil];
    [s_frame_encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [s_frame_encoder setVertexBuffer:uniform_buffer offset:0 atIndex:1];
    [s_frame_encoder setFragmentTexture:texture atIndex:0];
    [s_frame_encoder setFragmentSamplerState:s_ui_sampler atIndex:0];
    [s_frame_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:nverts];
}

static void render_minimap_triangles(const struct ui_vert *verts, size_t nverts,
                                     id<MTLTexture> texture, const struct map *map)
{
    if(!verts || !nverts || !texture || !map)
        return;

    frame_begin();
    if(!s_frame_encoder)
        return;

    id<MTLRenderPipelineState> pipeline = ensure_minimap_pipeline(frame_uses_msaa());
    if(!pipeline)
        return;

    id<MTLBuffer> vertex_buffer = [s_device newBufferWithBytes:verts
        length:nverts * sizeof(*verts) options:MTLResourceStorageModeShared];
    if(!vertex_buffer)
        return;

    int drawable_w = (int)s_layer.drawableSize.width;
    int drawable_h = (int)s_layer.drawableSize.height;
    struct metal_ui_uniforms ui_uniforms = {
        .view_size = {drawable_w, drawable_h},
        ._padding = {0.0f, 0.0f}
    };
    id<MTLBuffer> ui_uniform_buffer = [s_device newBufferWithBytes:&ui_uniforms
        length:sizeof(ui_uniforms) options:MTLResourceStorageModeShared];
    if(!ui_uniform_buffer)
        return;

    struct map_resolution res;
    M_GetResolution(map, &res);
    struct metal_minimap_uniforms minimap_uniforms = {
        .chunk_size = {(uint32_t)res.chunk_w, (uint32_t)res.chunk_h},
        .tiles_per_chunk = {(uint32_t)res.tile_w, (uint32_t)res.tile_h},
        .fog_enabled = s_fog_buffer ? 1u : 0u,
        ._padding = {0u, 0u, 0u}
    };
    id<MTLBuffer> minimap_uniform_buffer = [s_device newBufferWithBytes:&minimap_uniforms
        length:sizeof(minimap_uniforms) options:MTLResourceStorageModeShared];
    if(!minimap_uniform_buffer)
        return;

    [s_frame_encoder setRenderPipelineState:pipeline];
    [s_frame_encoder setDepthStencilState:nil];
    [s_frame_encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [s_frame_encoder setVertexBuffer:ui_uniform_buffer offset:0 atIndex:1];
    [s_frame_encoder setFragmentBuffer:minimap_uniform_buffer offset:0 atIndex:1];
    if(s_fog_buffer)
        [s_frame_encoder setFragmentBuffer:s_fog_buffer offset:0 atIndex:2];
    [s_frame_encoder setFragmentTexture:texture atIndex:0];
    [s_frame_encoder setFragmentSamplerState:s_minimap_sampler atIndex:0];
    [s_frame_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:nverts];
}

static vec2_t transform_model_point(const mat4x4_t *model, vec2_t point)
{
    mat4x4_t copy = *model;
    vec4_t in = {point.x, point.y, 0.0f, 1.0f};
    vec4_t out;
    PFM_Mat4x4_Mult4x1(&copy, &in, &out);
    return (vec2_t){out.x, out.y};
}

static vec2_t transform_model_xz_point(const mat4x4_t *model, vec2_t point)
{
    mat4x4_t copy = *model;
    vec4_t in = {point.x, 0.0f, point.z, 1.0f};
    vec4_t out;
    PFM_Mat4x4_Mult4x1(&copy, &in, &out);
    float inv_w = fabsf(out.w) > 0.00001f ? 1.0f / out.w : 1.0f;
    return (vec2_t){out.x * inv_w, out.z * inv_w};
}

static vec3_t transform_model_vec3_point(const mat4x4_t *model, vec3_t point)
{
    mat4x4_t copy = *model;
    vec4_t in = {point.x, point.y, point.z, 1.0f};
    vec4_t out;
    PFM_Mat4x4_Mult4x1(&copy, &in, &out);
    float inv_w = fabsf(out.w) > 0.00001f ? 1.0f / out.w : 1.0f;
    return (vec3_t){out.x * inv_w, out.y * inv_w, out.z * inv_w};
}

static void make_minimap_models(const vec2_t *center_pos, const int *side_len_px,
                                mat4x4_t *out_model, mat4x4_t *out_border_model)
{
    mat4x4_t tmp;
    mat4x4_t tilt, trans, scale;
    PFM_Mat4x4_MakeRotZ(DEG_TO_RAD(-45.0f), &tilt);
    PFM_Mat4x4_MakeScale((*side_len_px) / 2.0f, (*side_len_px) / 2.0f, 1.0f, &scale);
    PFM_Mat4x4_MakeTrans(center_pos->x, center_pos->y, 0.0f, &trans);
    PFM_Mat4x4_Mult4x4(&scale, &tilt, &tmp);
    PFM_Mat4x4_Mult4x4(&trans, &tmp, out_model);

    float scale_fac = ((*side_len_px) + 2 * MINIMAP_BORDER_WIDTH) / 2.0f;
    mat4x4_t border_scale;
    PFM_Mat4x4_MakeScale(scale_fac, scale_fac, scale_fac, &border_scale);
    PFM_Mat4x4_Mult4x4(&border_scale, &tilt, &tmp);
    PFM_Mat4x4_Mult4x4(&trans, &tmp, out_border_model);
}

static void build_minimap_quad(const mat4x4_t *model, struct ui_vert out[6])
{
    const vec2_t local[4] = {
        {-1.0f, -1.0f},
        {-1.0f,  1.0f},
        { 1.0f,  1.0f},
        { 1.0f, -1.0f},
    };
    const vec2_t uv[4] = {
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 1.0f},
        {1.0f, 0.0f},
    };
    const int idx[6] = {0, 1, 2, 0, 2, 3};

    for(int i = 0; i < 6; i++) {
        int src = idx[i];
        vec2_t screen = transform_model_point(model, local[src]);
        out[i] = (struct ui_vert){
            .screen_pos = screen,
            .uv = uv[src],
            .color = {255, 255, 255, 255},
        };
    }
}

static bool build_minimap_view_proj(const struct map *map,
                                    matrix_float4x4 *out_view,
                                    matrix_float4x4 *out_proj)
{
    struct map_resolution res;
    M_GetResolution(map, &res);

    vec3_t map_center = M_GetCenterPos(map);
    vec2_t map_size = {
        res.chunk_w * res.tile_w * X_COORDS_PER_TILE,
        res.chunk_h * res.tile_h * Z_COORDS_PER_TILE
    };

    struct camera *map_cam = Camera_New();
    if(!map_cam)
        return false;

    vec3_t offset = {0.0f, 200.0f, 0.0f};
    PFM_Vec3_Add(&map_center, &offset, &map_center);
    Camera_SetPos(map_cam, map_center);
    Camera_SetPitchAndYaw(map_cam, -90.0f, 90.0f);

    float map_dim = fmaxf(map_size.x, map_size.y);
    vec2_t bot_left = {-(map_dim / 2.0f), -(map_dim / 2.0f)};
    vec2_t top_right = {(map_dim / 2.0f), (map_dim / 2.0f)};
    mat4x4_t view, proj;
    Camera_MakeViewMat(map_cam, &view);
    PFM_Mat4x4_MakeOrthographic(bot_left.x, top_right.x, bot_left.y, top_right.y,
                                 CAM_Z_NEAR_DIST, CONFIG_DRAWDIST, &proj);
    mat4x4_t z_remap, metal_proj;
    PFM_Mat4x4_Identity(&z_remap);
    z_remap.cols[2][2] = 0.5f;
    z_remap.cols[3][2] = 0.5f;
    PFM_Mat4x4_Mult4x4(&z_remap, &proj, &metal_proj);
    proj = metal_proj;
    Camera_Free(map_cam);

    *out_view = matrix_from_pf_mat4(&view);
    *out_proj = matrix_from_pf_mat4(&proj);
    return true;
}

static id<MTLBuffer> ensure_persistent_vertex_buffer(void **slot,
                                                     const void *bytes,
                                                     size_t length)
{
    if(!slot || !bytes || !length || !s_device)
        return nil;
    if(*slot)
        return (__bridge id<MTLBuffer>)*slot;

    id<MTLBuffer> buffer = [s_device newBufferWithBytes:bytes
        length:length options:MTLResourceStorageModeShared];
    if(!buffer)
        return nil;

    *slot = (__bridge_retained void *)buffer;
    return buffer;
}

static void maybe_log_terrain_draw(id<MTLRenderCommandEncoder> encoder,
                                   const struct render_private *priv,
                                   const mat4x4_t *model)
{
    static bool checked;
    static bool enabled;
    static unsigned logged;

    if(!checked) {
        checked = true;
        enabled = getenv("PF_METAL_TERRAIN_DRAW_LOG") != NULL;
    }
    if(!enabled || !priv || !model || !priv->metal_terrain_verts)
        return;

    unsigned limit = capture_env_uint("PF_METAL_TERRAIN_DRAW_LOG_LIMIT", 256);
    if(logged >= limit)
        return;
    if(encoder != s_frame_encoder && getenv("PF_METAL_TERRAIN_DRAW_LOG_ALL_TARGETS") == NULL)
        return;

    const struct terrain_vert *verts = priv->metal_terrain_verts;
    size_t num_verts = priv->metal_terrain_verts_size / sizeof(struct terrain_vert);
    size_t tile_count = (size_t)s_map_tiles_per_chunk.x * (size_t)s_map_tiles_per_chunk.y;
    size_t top0 = 4 * VERTS_PER_SIDE_FACE;
    size_t center = ((tile_count / 2) * VERTS_PER_TILE) + top0;
    int top0_mat = (top0 < num_verts) ? verts[top0].material_idx : -1;
    int center_mat = (center < num_verts) ? verts[center].material_idx : -1;
    float field_w = s_map_tile_world_size.x * (float)s_map_tiles_per_chunk.x;
    float field_h = s_map_tile_world_size.y * (float)s_map_tiles_per_chunk.y;
    int chunk_c = (field_w > 0.0f) ? (int)lrintf((s_map_pos.x - model->cols[3][0]) / field_w) : -1;
    int chunk_r = (field_h > 0.0f) ? (int)lrintf((model->cols[3][2] - s_map_pos.y) / field_h) : -1;
    const char *target = (encoder == s_frame_encoder) ? "frame" :
                         (encoder == s_water_scene_encoder) ? "water" :
                         s_minimap_bake_pass_active ? "minimap" : "other";

    fprintf(stderr,
        "PF_METAL_TERRAIN_DRAW target=%s draw=%u priv=%p verts=%p model=(%.3f,%.3f,%.3f) chunk=%d,%d mats top0=%d center=%d top0_pos=(%.3f,%.3f,%.3f) center_pos=(%.3f,%.3f,%.3f)\n",
        target, logged, (const void *)priv, priv->metal_terrain_verts,
        model->cols[3][0], model->cols[3][1], model->cols[3][2],
        chunk_r, chunk_c, top0_mat, center_mat,
        (top0 < num_verts) ? verts[top0].pos.x : 0.0f,
        (top0 < num_verts) ? verts[top0].pos.y : 0.0f,
        (top0 < num_verts) ? verts[top0].pos.z : 0.0f,
        (center < num_verts) ? verts[center].pos.x : 0.0f,
        (center < num_verts) ? verts[center].pos.y : 0.0f,
        (center < num_verts) ? verts[center].pos.z : 0.0f);
    logged++;
}

static void release_retained_metal_object(void **slot)
{
    if(!slot || !*slot)
        return;
    id object = (__bridge_transfer id)*slot;
    (void)object;
    object = nil;
    *slot = NULL;
}

static id<MTLRenderCommandEncoder> active_scene_encoder(void)
{
    if(s_water_scene_pass_active && s_water_scene_encoder)
        return s_water_scene_encoder;
    return s_frame_encoder;
}

static bool active_scene_depth_enabled(void)
{
    if(s_water_scene_pass_active && s_water_scene_encoder)
        return true;
    return frame_has_depth();
}

static vector_float4 current_water_clip_params(void)
{
    return (vector_float4){
        (float)s_water_scene_clip_mode,
        0.0f,
        METAL_WATER_LEVEL,
        0.0f
    };
}

static bool ensure_heightmap_buffer(void)
{
    if(s_heightmap_buffer)
        return true;
    if(!s_device)
        return false;

    const size_t count = (size_t)METAL_HEIGHT_MAP_RES * (size_t)METAL_HEIGHT_MAP_RES;
    const size_t byte_count = count * sizeof(float);
    float *data = malloc(byte_count);
    if(!data)
        return false;

    Noise_GenerateOctavePerlinTile2D(METAL_HEIGHT_MAP_RES, METAL_HEIGHT_MAP_RES,
        1.0f / 128.0f, 4, 0.5f, data);
    Noise_Normalize2D(METAL_HEIGHT_MAP_RES, METAL_HEIGHT_MAP_RES, data);

    s_heightmap_buffer = [s_device newBufferWithBytes:data
        length:byte_count options:MTLResourceStorageModeShared];
    free(data);
    return s_heightmap_buffer != nil;
}

static bool ensure_splatmap_buffer(void)
{
    if(s_splatmap_buffer)
        return true;
    if(!s_device)
        return false;

    const size_t count = (size_t)METAL_SPLAT_MAP_RES * (size_t)METAL_SPLAT_MAP_RES;
    const size_t byte_count = count * sizeof(float);
    float *data = malloc(byte_count);
    if(!data)
        return false;

    Noise_GenerateOctavePerlinTile2D(METAL_SPLAT_MAP_RES, METAL_SPLAT_MAP_RES,
        1.0f / 128.0f, 4, 0.5f, data);

    s_splatmap_buffer = [s_device newBufferWithBytes:data
        length:byte_count options:MTLResourceStorageModeShared];
    free(data);
    return s_splatmap_buffer != nil;
}

static void fill_default_splat_indices(int indices[MAX_MAP_TEXTURES])
{
    for(size_t i = 0; i < MAX_MAP_TEXTURES; i++)
        indices[i] = -1;
}

static bool ensure_splat_indices_buffer(void)
{
    if(s_splat_indices_buffer)
        return true;
    if(!s_device)
        return false;

    int indices[MAX_MAP_TEXTURES];
    fill_default_splat_indices(indices);
    s_splat_indices_buffer = [s_device newBufferWithBytes:indices
        length:sizeof(indices) options:MTLResourceStorageModeShared];
    return s_splat_indices_buffer != nil;
}

static void update_splat_indices(const size_t *num_splats, const struct splatmap *splatmap)
{
    int indices[MAX_MAP_TEXTURES];
    fill_default_splat_indices(indices);

    if(num_splats && splatmap) {
        for(size_t i = 0; i < *num_splats; i++) {
            unsigned base_idx = splatmap->splats[i].base_mat_idx;
            unsigned accent_idx = splatmap->splats[i].accent_mat_idx;
            if(base_idx < MAX_MAP_TEXTURES && accent_idx < MAX_MAP_TEXTURES)
                indices[base_idx] = (int)accent_idx;
        }
    }

    if(!ensure_splat_indices_buffer())
        return;
    memcpy([s_splat_indices_buffer contents], indices, sizeof(indices));
}

static void draw_terrain_to_encoder(id<MTLRenderCommandEncoder> encoder,
                                    const struct render_private *priv,
                                    const mat4x4_t *model,
                                    matrix_float4x4 view,
                                    matrix_float4x4 proj)
{
    struct render_private *mutable_priv = (struct render_private *)priv;
    id<MTLRenderPipelineState> pipeline = nil;
    bool multisampled = (encoder == s_frame_encoder) && frame_uses_msaa();
    bool water_scene_target = (encoder == s_water_scene_encoder) && s_water_scene_pass_active;
    bool minimap_target = s_minimap_bake_pass_active;
    bool depth_enabled = ((encoder == s_frame_encoder) && frame_has_depth()) || water_scene_target;
    if(!encoder || !priv || !priv->metal_is_terrain)
        return;
    if(!priv->metal_terrain_verts || !priv->metal_terrain_verts_size || !priv->mesh.num_verts)
        return;
    pipeline = ensure_terrain_pipeline(multisampled, depth_enabled);
    if(!pipeline)
        return;

    id<MTLBuffer> vertex_buffer = ensure_persistent_vertex_buffer(
        &mutable_priv->metal_terrain_vertex_buffer,
        priv->metal_terrain_verts,
        priv->metal_terrain_verts_size);
    if(!vertex_buffer)
        return;
    maybe_log_terrain_draw(encoder, priv, model);

    bool heightmap_ready = ensure_heightmap_buffer();
    bool splatmap_ready = ensure_splatmap_buffer();
    bool splat_indices_ready = ensure_splat_indices_buffer();
    struct metal_terrain_uniforms uniforms = {
        .model = matrix_from_pf_mat4(model),
        .view = view,
        .proj = proj,
        .view_pos = {s_scene_view_pos.x, s_scene_view_pos.y, s_scene_view_pos.z, 1.0f},
        .map_pos = s_map_pos,
        .tile_world_size = s_map_tile_world_size,
        .chunk_size = s_map_chunk_size,
        .tiles_per_chunk = s_map_tiles_per_chunk,
        .terrain_params = {
            (float)s_terrain_texture_count,
            (float)METAL_HEIGHT_MAP_RES,
            heightmap_ready ? 1.0f : 0.0f,
            (splatmap_ready && splat_indices_ready) ? (float)METAL_SPLAT_MAP_RES : 0.0f,
        },
        .water_params = {
            SDL_GetTicks() / 1000.0f,
            (!water_scene_target && s_water_buffer) ? (minimap_target ? 2.0f : 1.0f) : 0.0f,
            (!minimap_target && s_fog_buffer) ? 1.0f : 0.0f,
            (!water_scene_target && s_water_dudv_texture && s_water_normal_texture) ? 1.0f : 0.0f,
        },
        .ambient_color = {s_light_ambient.x, s_light_ambient.y, s_light_ambient.z, 1.0f},
        .light_color = {s_light_color.x, s_light_color.y, s_light_color.z, 1.0f},
        .light_pos = {s_light_pos.x, s_light_pos.y, s_light_pos.z, 1.0f},
        .light_space_transform = s_shadow_light_space,
        .shadow_params = {
            shadow_enabled_for_draw() ? 1.0f : 0.0f,
            0.002f,
            0.55f,
            0.0f,
        },
        .clip_params = current_water_clip_params(),
    };
    id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
        length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    if(!uniform_buffer)
        return;

    [encoder setRenderPipelineState:pipeline];
    if(depth_enabled) {
        id<MTLDepthStencilState> depth_state = ensure_depth_state(true);
        if(depth_state)
            [encoder setDepthStencilState:depth_state];
    } else {
        [encoder setDepthStencilState:nil];
    }
    bool reflection_pass = water_scene_target && s_water_scene_clip_mode == METAL_WATER_CLIP_KEEP_ABOVE;
    [encoder setCullMode:(reflection_pass || minimap_target) ? MTLCullModeNone : MTLCullModeBack];
    [encoder setFrontFacingWinding:MTLWindingClockwise];
    [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [encoder setVertexBuffer:uniform_buffer offset:0 atIndex:1];
    [encoder setFragmentBuffer:uniform_buffer offset:0 atIndex:1];
    if(s_fog_buffer) {
        [encoder setFragmentBuffer:s_fog_buffer offset:0 atIndex:2];
    }
    if(s_water_buffer) {
        [encoder setFragmentBuffer:s_water_buffer offset:0 atIndex:3];
    }
    if(heightmap_ready) {
        [encoder setFragmentBuffer:s_heightmap_buffer offset:0 atIndex:4];
    }
    if(splatmap_ready) {
        [encoder setFragmentBuffer:s_splatmap_buffer offset:0 atIndex:7];
    }
    if(splat_indices_ready) {
        [encoder setFragmentBuffer:s_splat_indices_buffer offset:0 atIndex:8];
    }
    if(s_terrain_texture_array && ensure_terrain_sampler()) {
        [encoder setFragmentTexture:s_terrain_texture_array atIndex:0];
        [encoder setFragmentSamplerState:s_terrain_sampler atIndex:0];
    }
    if(s_water_dudv_texture && ensure_scene_sampler()) {
        [encoder setFragmentTexture:s_water_dudv_texture atIndex:1];
        [encoder setFragmentSamplerState:s_scene_sampler atIndex:2];
    }
    if(s_water_normal_texture && ensure_scene_sampler()) {
        [encoder setFragmentTexture:s_water_normal_texture atIndex:2];
        [encoder setFragmentSamplerState:s_scene_sampler atIndex:2];
    }
    if(shadow_enabled_for_draw()) {
        [encoder setFragmentTexture:s_shadow_depth_texture atIndex:3];
        [encoder setFragmentSamplerState:s_shadow_sampler atIndex:1];
    }
    bind_shadow_screen_probe_resources(encoder, 4, 5, 6);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:priv->mesh.num_verts];
}

static void update_fog_texture(void *buff, const size_t *size)
{
    if(!buff || !size)
        return;
    if(*size == 0)
        return;

    if(s_map_chunk_size.x > 0u && s_map_chunk_size.y > 0u) {
        size_t expected = (size_t)s_map_chunk_size.x * (size_t)s_map_chunk_size.y
                        * (size_t)s_map_tiles_per_chunk.x * (size_t)s_map_tiles_per_chunk.y;
        if(*size != expected)
            return;
    }

    s_fog_buffer = [s_device newBufferWithBytes:buff
        length:*size options:MTLResourceStorageModeShared];
}

static id<MTLTexture> load_rgba_texture_2d(const char *path)
{
    if(!path || !s_device)
        return nil;

    int width = 0, height = 0, nr_channels = 0;
    unsigned char *data = stbi_load(path, &width, &height, &nr_channels, 4);
    if(!data)
        return nil;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [s_device newTextureWithDescriptor:desc];
    if(texture) {
        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [texture replaceRegion:region
                   mipmapLevel:0
                     withBytes:data
                   bytesPerRow:width * 4];
    }

    stbi_image_free(data);
    return texture;
}

static id<MTLTexture> sprite_texpath_get_or_load(const char *full_path)
{
    if(!full_path || !s_device)
        return nil;
    if(!s_sprite_texpath_cache)
        s_sprite_texpath_cache = [[NSMutableDictionary alloc] init];
    NSString *key = [NSString stringWithUTF8String:full_path];
    id<MTLTexture> tex = [s_sprite_texpath_cache objectForKey:key];
    if(tex)
        return tex;
    tex = load_rgba_texture_2d(full_path);
    if(tex)
        [s_sprite_texpath_cache setObject:tex forKey:key];
    return tex;
}

static void sort_sprites_by_sheet(struct sprite_desc *sprites, size_t nsprites,
                                  struct metal_sprite_draw_call out[METAL_MAX_SPRITE_DRAWS],
                                  size_t *nout)
{
    if(!sprites || nsprites == 0) {
        *nout = 0;
        return;
    }

    int i = 1;
    while(i < nsprites) {
        int j = i;
        const char *first = sprites[i].sheet.filename;
        const char *second = sprites[j].sheet.filename;
        while(j > 0 && first && second && strcmp(first, second)) {
            struct sprite_desc tmp = sprites[j - 1];
            sprites[j - 1] = sprites[j];
            sprites[j] = tmp;
            j--;
        }
        i++;
    }

    size_t count = 0;
    size_t begin_idx = 0;
    for(size_t k = 1; k < nsprites && count < METAL_MAX_SPRITE_DRAWS; k++) {
        const char *curr = sprites[k].sheet.filename;
        const char *prev = sprites[k - 1].sheet.filename;
        if(!curr || !prev || 0 != strcmp(curr, prev)) {
            out[count++] = (struct metal_sprite_draw_call){begin_idx, k - 1};
            begin_idx = k;
        }
    }
    if(count < METAL_MAX_SPRITE_DRAWS)
        out[count++] = (struct metal_sprite_draw_call){begin_idx, nsprites - 1};
    *nout = count;
}

static void log_sprite_draw_stats(const struct sprite_sheet_desc *sheet,
                                  id<MTLTexture> texture, size_t nents)
{
    const char *stats_path = getenv("PF_METAL_SPRITE_STATS_PATH");
    if(!stats_path || !*stats_path || !sheet || !sheet->filename || !texture)
        return;

    FILE *fp = fopen(stats_path, "a");
    if(!fp)
        return;
    fprintf(fp, "sheet=%s instances=%zu nrows=%zu ncols=%zu nframes=%zu texture=%lux%lu\n",
        sheet->filename, nents, sheet->nrows, sheet->ncols, sheet->nframes,
        (unsigned long)texture.width, (unsigned long)texture.height);
    fclose(fp);
}

static void render_sprite_batch(struct sprite_desc *sprites, const size_t *nsprites,
                                const struct camera *cam)
{
    if(!sprites || !nsprites || *nsprites == 0 || !cam)
        return;
    if(!s_have_scene_view || !s_have_scene_proj)
        return;

    struct metal_sprite_draw_call draws[METAL_MAX_SPRITE_DRAWS];
    size_t ndraws = 0;
    sort_sprites_by_sheet(sprites, *nsprites, draws, &ndraws);
    if(ndraws == 0)
        return;

    Camera_TickFinishPerspective((struct camera *)cam);
    vec3_t dir = Camera_GetDir(cam);

    frame_begin();
    if(!s_frame_encoder)
        return;
    id<MTLRenderPipelineState> pipeline = ensure_sprite_pipeline(frame_uses_msaa());
    if(!pipeline)
        return;

    [s_frame_encoder setRenderPipelineState:pipeline];
    [s_frame_encoder setDepthStencilState:nil];
    [s_frame_encoder setCullMode:MTLCullModeNone];
    [s_frame_encoder setFragmentSamplerState:s_sprite_sampler atIndex:0];

    for(size_t i = 0; i < ndraws; i++) {
        struct metal_sprite_draw_call draw = draws[i];
        const struct sprite_sheet_desc *sheet = &sprites[draw.begin_idx].sheet;
        if(!sheet->filename)
            continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/assets/sprites/%s", g_basepath, sheet->filename);
        id<MTLTexture> texture = sprite_texpath_get_or_load(path);
        if(!texture)
            continue;

        size_t nents = draw.end_idx - draw.begin_idx + 1;
        if(nents > METAL_MAX_SPRITES)
            nents = METAL_MAX_SPRITES;
        log_sprite_draw_stats(sheet, texture, nents);

        struct metal_sprite_desc descs[METAL_MAX_SPRITES];
        for(size_t j = 0; j < nents; j++) {
            const struct sprite_desc *src = &sprites[draw.begin_idx + j];
            descs[j] = (struct metal_sprite_desc){
                .ws_pos = {src->ws_pos.x, src->ws_pos.y, src->ws_pos.z},
                .pad0 = 0.0f,
                .ws_size = {src->ws_size.x, src->ws_size.y},
                .frame_idx = src->frame,
                .pad1 = 0.0f,
            };
        }

        id<MTLBuffer> sprite_buffer = [s_device newBufferWithBytes:descs
            length:nents * sizeof(descs[0]) options:MTLResourceStorageModeShared];
        if(!sprite_buffer)
            continue;

        struct metal_sprite_uniforms uniforms = {
            .view = s_scene_view,
            .projection = s_scene_proj,
            .view_dir = {dir.x, dir.y, dir.z},
            .sprite_nrows = (uint32_t)MAX(sheet->nrows, 1),
            .sprite_ncols = (uint32_t)MAX(sheet->ncols, 1),
        };
        id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
            length:sizeof(uniforms) options:MTLResourceStorageModeShared];
        if(!uniform_buffer)
            continue;

        [s_frame_encoder setVertexBuffer:uniform_buffer offset:0 atIndex:0];
        [s_frame_encoder setVertexBuffer:sprite_buffer offset:0 atIndex:1];
        [s_frame_encoder setFragmentBuffer:uniform_buffer offset:0 atIndex:0];
        [s_frame_encoder setFragmentTexture:texture atIndex:0];
        [s_frame_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:0
                            vertexCount:6
                          instanceCount:nents];
    }
}

static void fill_black_rgba(unsigned char *dst, size_t npixels)
{
    for(size_t i = 0; i < npixels; i++) {
        dst[i * 4 + 0] = 0;
        dst[i * 4 + 1] = 0;
        dst[i * 4 + 2] = 0;
        dst[i * 4 + 3] = 255;
    }
}

static void free_metal_skybox(void)
{
    s_skybox_texture = nil;
    s_skybox_vertex_buffer = nil;
}

static bool ensure_skybox_vertex_buffer(void)
{
    if(s_skybox_vertex_buffer)
        return true;

    s_skybox_vertex_buffer = [s_device newBufferWithBytes:s_skybox_cube_verts
        length:sizeof(s_skybox_cube_verts) options:MTLResourceStorageModeShared];
    return s_skybox_vertex_buffer != nil;
}

static void load_metal_skybox(const char *dir, const char *extension)
{
    static const char *faces[] = {"right", "left", "top", "bottom", "back", "front"};
    unsigned char *face_data[6] = {0};
    int face_width[6] = {0};
    int face_height[6] = {0};
    int nr_channels = 0;
    int cube_size = 1;

    free_metal_skybox();
    if(!s_device)
        return;

    for(size_t i = 0; i < sizeof(faces) / sizeof(faces[0]); i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/%s.%s",
            g_basepath, dir ? dir : "", faces[i], extension ? extension : "");
        face_data[i] = stbi_load(path, &face_width[i], &face_height[i], &nr_channels, 4);
        if(face_data[i] && cube_size == 1) {
            cube_size = MAX(1, MIN(face_width[i], face_height[i]));
        }
    }

    const size_t bytes_per_row = (size_t)cube_size * 4;
    const size_t bytes_per_image = bytes_per_row * (size_t)cube_size;
    unsigned char *fallback = malloc(bytes_per_image);
    unsigned char *resized = malloc(bytes_per_image);
    if(!fallback || !resized) {
        free(fallback);
        free(resized);
        for(size_t i = 0; i < 6; i++)
            stbi_image_free(face_data[i]);
        return;
    }
    fill_black_rgba(fallback, (size_t)cube_size * (size_t)cube_size);

    MTLTextureDescriptor *desc = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       size:cube_size
                                                                                  mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [s_device newTextureWithDescriptor:desc];
    if(texture) {
        MTLRegion region = MTLRegionMake2D(0, 0, cube_size, cube_size);
        for(size_t i = 0; i < 6; i++) {
            const unsigned char *upload = fallback;
            if(face_data[i]) {
                if(face_width[i] == cube_size && face_height[i] == cube_size) {
                    upload = face_data[i];
                } else if(stbir_resize_uint8(face_data[i], face_width[i], face_height[i], 0,
                                             resized, cube_size, cube_size, 0, 4)) {
                    upload = resized;
                }
            }
            [texture replaceRegion:region
                        mipmapLevel:0
                              slice:i
                          withBytes:upload
                        bytesPerRow:bytes_per_row
                      bytesPerImage:bytes_per_image];
        }
    }

    for(size_t i = 0; i < 6; i++)
        stbi_image_free(face_data[i]);
    free(fallback);
    free(resized);

    s_skybox_texture = texture;
    (void)ensure_skybox_vertex_buffer();
}

static mat4x4_t skybox_view_matrix(const struct camera *cam, bool keep_translation)
{
    mat4x4_t view;
    Camera_MakeViewMat(cam, &view);

    if(keep_translation)
        return view;

    view.cols[3][0] = 0.0f;
    view.cols[3][1] = 0.0f;
    view.cols[3][2] = 0.0f;
    view.cols[0][3] = 0.0f;
    view.cols[1][3] = 0.0f;
    view.cols[2][3] = 0.0f;
    view.cols[3][3] = 1.0f;
    return view;
}

static void render_skybox_to_encoder(id<MTLRenderCommandEncoder> encoder,
                                     const struct camera *cam,
                                     const struct map *map,
                                     bool scaled)
{
    if(!encoder || !cam || !s_skybox_texture || !ensure_skybox_vertex_buffer() || !ensure_skybox_sampler())
        return;

    const bool multisampled = (encoder == s_frame_encoder) && frame_uses_msaa();
    id<MTLRenderPipelineState> pipeline = ensure_skybox_pipeline(multisampled);
    if(!pipeline)
        return;

    mat4x4_t model;
    if(scaled && map) {
        struct map_resolution res;
        M_GetResolution(map, &res);
        const float map_width = res.chunk_w * res.tile_w * X_COORDS_PER_TILE;
        const float map_height = res.chunk_h * res.tile_h * Z_COORDS_PER_TILE;
        const float scale = MAX(map_width, map_height) / 2.0f;
        PFM_Mat4x4_MakeScale(scale, scale, scale, &model);
    } else {
        PFM_Mat4x4_Identity(&model);
    }

    mat4x4_t proj;
    Camera_MakeProjMat(cam, &proj);
    mat4x4_t view = skybox_view_matrix(cam, scaled);
    struct metal_skybox_uniforms uniforms = {
        .model = matrix_from_pf_mat4(&model),
        .view_rot = matrix_from_pf_mat4(&view),
        .proj = matrix_from_pf_mat4(&proj),
    };
    id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
        length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    if(!uniform_buffer)
        return;

    [encoder setRenderPipelineState:pipeline];
    id<MTLDepthStencilState> depth_state = ensure_depth_state(false);
    if(depth_state)
        [encoder setDepthStencilState:depth_state];
    [encoder setCullMode:scaled ? MTLCullModeNone : MTLCullModeFront];
    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [encoder setVertexBuffer:s_skybox_vertex_buffer offset:0 atIndex:0];
    [encoder setVertexBuffer:uniform_buffer offset:0 atIndex:1];
    [encoder setFragmentTexture:s_skybox_texture atIndex:0];
    [encoder setFragmentSamplerState:s_skybox_sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:sizeof(s_skybox_cube_verts) / sizeof(s_skybox_cube_verts[0])];
}

static void render_metal_skybox(const struct camera *cam)
{
    if(!cam)
        return;

    frame_begin();
    if(!s_frame_encoder || !frame_has_depth())
        return;

    render_skybox_to_encoder(s_frame_encoder, cam, NULL, false);
}

static void init_water_resources(void)
{
    char path[1024];

    s_water_dudv_texture = nil;
    s_water_normal_texture = nil;
    s_water_move_factor = 0.0f;
    s_water_prev_frame_tick = 0;

    snprintf(path, sizeof(path), "%s/%s", g_basepath, METAL_WATER_DUDV_PATH);
    s_water_dudv_texture = load_rgba_texture_2d(path);

    snprintf(path, sizeof(path), "%s/%s", g_basepath, METAL_WATER_NORMAL_PATH);
    s_water_normal_texture = load_rgba_texture_2d(path);
}

static void fill_fallback_terrain_rgba(unsigned char *dst, size_t npixels, uint32_t idx)
{
    const unsigned char palette[][4] = {
        { 92, 140,  66, 255 },
        {112, 158,  77, 255 },
        {148, 138,  82, 255 },
        {105, 115,  69, 255 },
        {163, 153,  94, 255 },
        { 74, 110,  61, 255 },
        {128, 125,  79, 255 },
        {122, 145,  87, 255 },
    };
    const unsigned char *rgba = palette[idx % (sizeof(palette) / sizeof(palette[0]))];
    for(size_t i = 0; i < npixels; i++) {
        memcpy(dst + i * 4, rgba, 4);
    }
}

static void update_terrain_textures(const char map_texfiles[][256], const size_t *num_textures)
{
    s_terrain_texture_array = nil;
    s_terrain_texture_count = 0;

    if(!map_texfiles || !num_textures || !*num_textures || !s_device)
        return;

    size_t source_count = *num_textures;
    size_t texture_slots = source_count * 8;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:CONFIG_TILE_TEX_RES
                                                                                    height:CONFIG_TILE_TEX_RES
                                                                                 mipmapped:YES];
    desc.textureType = MTLTextureType2DArray;
    desc.arrayLength = texture_slots;
    desc.usage = MTLTextureUsageShaderRead;

    id<MTLTexture> texture = [s_device newTextureWithDescriptor:desc];
    if(!texture)
        return;

    const size_t bytes_per_row = CONFIG_TILE_TEX_RES * 4;
    const size_t bytes_per_image = bytes_per_row * CONFIG_TILE_TEX_RES;
    const size_t npixels = CONFIG_TILE_TEX_RES * CONFIG_TILE_TEX_RES;
    unsigned char *fallback = malloc(bytes_per_image);
    unsigned char *resized = malloc(bytes_per_image);
    if(!fallback || !resized) {
        free(fallback);
        free(resized);
        return;
    }

    MTLRegion region = MTLRegionMake2D(0, 0, CONFIG_TILE_TEX_RES, CONFIG_TILE_TEX_RES);
    for(size_t i = 0; i < source_count; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/assets/map_textures/%s", g_basepath, map_texfiles[i]);

        int width = 0, height = 0, nr_channels = 0;
        unsigned char *data = stbi_load(path, &width, &height, &nr_channels, 4);
        const unsigned char *upload = fallback;

        if(data) {
            if(width == CONFIG_TILE_TEX_RES && height == CONFIG_TILE_TEX_RES) {
                upload = data;
            } else if(stbir_resize_uint8(data, width, height, 0,
                                         resized, CONFIG_TILE_TEX_RES, CONFIG_TILE_TEX_RES, 0, 4)) {
                upload = resized;
            } else {
                fill_fallback_terrain_rgba(fallback, npixels, (uint32_t)i);
            }
        } else {
            fill_fallback_terrain_rgba(fallback, npixels, (uint32_t)i);
        }

        for(size_t wang = 0; wang < 8; wang++) {
            [texture replaceRegion:region
                       mipmapLevel:0
                             slice:(i * 8) + wang
                         withBytes:upload
                       bytesPerRow:bytes_per_row
                     bytesPerImage:bytes_per_image];
        }
        stbi_image_free(data);
    }

    free(fallback);
    free(resized);

    if(s_queue && texture.mipmapLevelCount > 1) {
        id<MTLCommandBuffer> command_buffer = [s_queue commandBuffer];
        if(command_buffer) {
            id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
            if(blit) {
                [blit generateMipmapsForTexture:texture];
                [blit endEncoding];
                [command_buffer commit];
                [command_buffer waitUntilCompleted];
            }
        }
    }

    s_terrain_texture_array = texture;
    s_terrain_texture_count = (uint32_t)texture_slots;
}

static void update_water_mask(const struct map *map, const struct map_resolution *res)
{
    if(!map || !res || !s_device)
        return;

    const size_t total = (size_t)res->chunk_w * (size_t)res->chunk_h
                       * (size_t)res->tile_w * (size_t)res->tile_h;
    unsigned char *mask = malloc(total);
    if(!mask)
        return;

    size_t idx = 0;
    for(int chunk_r = 0; chunk_r < res->chunk_h; chunk_r++) {
    for(int chunk_c = 0; chunk_c < res->chunk_w; chunk_c++) {
    for(int tile_r = 0; tile_r < res->tile_h; tile_r++) {
    for(int tile_c = 0; tile_c < res->tile_w; tile_c++, idx++) {
        struct tile *tile = NULL;
        struct tile_desc td = {chunk_r, chunk_c, tile_r, tile_c};
        bool exists = M_TileForDesc(map, td, &tile);
        mask[idx] = (exists && tile && M_Tile_BaseHeight(tile) < 0) ? 1u : 0u;
    }}}}

    s_water_buffer = [s_device newBufferWithBytes:mask
        length:total options:MTLResourceStorageModeShared];
    free(mask);
}

static void set_minimap_map_uniforms(const struct map_resolution *res)
{
    if(!res)
        return;

    s_map_pos = (vector_float2){
        (res->chunk_w * res->tile_w * X_COORDS_PER_TILE) / 2.0f,
        (res->chunk_h * res->tile_h * Z_COORDS_PER_TILE * -1.0f) / 2.0f,
    };
    s_map_tile_world_size = (vector_float2){
        res->field_w / res->tile_w,
        res->field_h / res->tile_h,
    };
    s_map_chunk_size = (vector_uint2){
        (uint32_t)res->chunk_w,
        (uint32_t)res->chunk_h,
    };
    s_map_tiles_per_chunk = (vector_uint2){
        (uint32_t)res->tile_w,
        (uint32_t)res->tile_h,
    };

    size_t num_splats = 0;
    struct splatmap empty = {0};
    update_splat_indices(&num_splats, &empty);
}

static void render_terrain_draw(const struct render_private *priv, const mat4x4_t *model)
{
    if(!priv || !priv->metal_is_terrain)
        return;
    if(!priv->metal_terrain_verts || !priv->metal_terrain_verts_size || !priv->mesh.num_verts)
        return;
    if(!s_have_scene_view || !s_have_scene_proj)
        return;
    if(s_water_scene_pass_active && s_water_scene_encoder) {
        draw_terrain_to_encoder(s_water_scene_encoder, priv, model, s_scene_view, s_scene_proj);
        return;
    }
    frame_begin();
    if(!s_frame_encoder)
        return;

    draw_terrain_to_encoder(s_frame_encoder, priv, model, s_scene_view, s_scene_proj);
}

static void water_target_size(NSUInteger *out_w, NSUInteger *out_h)
{
    const CGFloat drawable_w = s_layer.drawableSize.width;
    const CGFloat drawable_h = s_layer.drawableSize.height;
    NSUInteger width = drawable_w > 0.0 ? (NSUInteger)floor(drawable_w / 2.5) : 1;
    width = width > 0 ? width : 1;

    CGFloat aspect = (drawable_h > 0.0) ? (drawable_w / drawable_h) : 1.0;
    if(aspect <= 0.0)
        aspect = 1.0;

    NSUInteger height = (NSUInteger)floor(width / aspect);
    height = height > 0 ? height : 1;
    *out_w = width;
    *out_h = height;
}

static bool ensure_water_target_texture(id<MTLTexture> __strong *slot,
                                        MTLPixelFormat pixel_format,
                                        NSUInteger width,
                                        NSUInteger height,
                                        MTLTextureUsage usage)
{
    if(*slot
    && (*slot).width == width
    && (*slot).height == height
    && (*slot).pixelFormat == pixel_format) {
        return true;
    }

    *slot = nil;
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = usage;
    *slot = [s_device newTextureWithDescriptor:desc];
    return *slot != nil;
}

static bool ensure_water_scene_textures(void)
{
    NSUInteger width = 0, height = 0;
    water_target_size(&width, &height);

    const MTLTextureUsage color_usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    const MTLTextureUsage depth_usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return ensure_water_target_texture(&s_water_reflection_texture, MTLPixelFormatBGRA8Unorm, width, height, color_usage)
        && ensure_water_target_texture(&s_water_reflection_depth_texture, MTLPixelFormatDepth32Float, width, height, depth_usage)
        && ensure_water_target_texture(&s_water_refraction_texture, MTLPixelFormatBGRA8Unorm, width, height, color_usage)
        && ensure_water_target_texture(&s_water_refraction_depth_texture, MTLPixelFormatDepth32Float, width, height, depth_usage);
}

static void water_reflection_dump_request_if_needed(id<MTLTexture> texture)
{
    const char *path = getenv("PF_METAL_WATER_REFLECTION_DUMP_RGBA8_PATH");
    if(!path || !*path || !texture || !s_frame_command_buffer)
        return;

    const size_t bytes_per_pixel = 4;
    NSUInteger bpr = texture.width * bytes_per_pixel;
    NSUInteger total = bpr * texture.height;
    if(!s_water_reflection_dump_buffer || [s_water_reflection_dump_buffer length] < total) {
        s_water_reflection_dump_buffer = [s_device newBufferWithLength:total
            options:MTLResourceStorageModeShared];
    }
    if(!s_water_reflection_dump_buffer)
        return;

    id<MTLBlitCommandEncoder> blit = [s_frame_command_buffer blitCommandEncoder];
    if(!blit)
        return;
    [blit copyFromTexture:texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(texture.width, texture.height, 1)
                 toBuffer:s_water_reflection_dump_buffer
        destinationOffset:0
   destinationBytesPerRow:bpr
 destinationBytesPerImage:total];
    [blit endEncoding];

    snprintf(s_water_reflection_dump_path, sizeof(s_water_reflection_dump_path), "%s", path);
    s_water_reflection_dump_width = texture.width;
    s_water_reflection_dump_height = texture.height;
    s_water_reflection_dump_bpr = bpr;
    s_water_reflection_dump_pending = true;
}

static void water_reflection_dump_finish_if_pending(void)
{
    if(!s_water_reflection_dump_pending || !s_water_reflection_dump_buffer)
        return;

    FILE *fp = fopen(s_water_reflection_dump_path, "wb");
    if(fp) {
        fwrite([s_water_reflection_dump_buffer contents], 1,
            s_water_reflection_dump_bpr * s_water_reflection_dump_height, fp);
        fclose(fp);
        fprintf(stderr, "PF_METAL_WATER_REFLECTION_DUMP_RGBA8 wrote %llux%llu BGRA8 raw to %s\n",
            (unsigned long long)s_water_reflection_dump_width,
            (unsigned long long)s_water_reflection_dump_height,
            s_water_reflection_dump_path);
    }

    s_water_reflection_dump_pending = false;
}

static void render_water_scene_terrain(const struct render_input *in,
                                       const struct camera *cam,
                                       const struct camera *skybox_cam,
                                       id<MTLTexture> color,
                                       id<MTLTexture> depth,
                                       MTLClearColor clear_color,
                                       bool enabled,
                                       int clip_mode)
{
    if(!in || !in->map || !cam || !color || !depth || !s_frame_command_buffer)
        return;

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = clear_color;
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 1.0;

    s_water_scene_encoder = [s_frame_command_buffer renderCommandEncoderWithDescriptor:pass];
    if(!s_water_scene_encoder)
        return;

    MTLViewport viewport = {
        .originX = 0.0,
        .originY = 0.0,
        .width = color.width,
        .height = color.height,
        .znear = 0.0,
        .zfar = 1.0
    };
    [s_water_scene_encoder setViewport:viewport];

    s_water_scene_pass_active = true;
    s_water_scene_clip_mode = clip_mode;
    if(enabled) {
        M_RenderVisibleMap(in->map, cam, false, RENDER_PASS_REGULAR);
        render_batched_anim_entities(&in->cam_vis_anim);
        render_batched_stat_entities(&in->cam_vis_stat);
        if(clip_mode == METAL_WATER_CLIP_KEEP_ABOVE)
            render_skybox_to_encoder(s_water_scene_encoder, skybox_cam ? skybox_cam : cam, in->map, true);
    }
    s_water_scene_clip_mode = METAL_WATER_CLIP_NONE;
    s_water_scene_pass_active = false;

    [s_water_scene_encoder endEncoding];
    s_water_scene_encoder = nil;

    if(enabled && clip_mode == METAL_WATER_CLIP_KEEP_ABOVE && color == s_water_reflection_texture)
        water_reflection_dump_request_if_needed(color);
}

static void render_water_scene_textures(const struct render_input *in,
                                        const bool *refraction,
                                        const bool *reflection)
{
    if(!in || !in->map || !s_frame_command_buffer)
        return;
    if(!ensure_water_scene_textures())
        return;

    frame_end();

    matrix_float4x4 saved_view = s_scene_view;
    matrix_float4x4 saved_proj = s_scene_proj;
    vector_float3 saved_view_pos = s_scene_view_pos;
    bool saved_have_view = s_have_scene_view;
    bool saved_have_proj = s_have_scene_proj;
    bool saved_shadows = s_shadows_enabled;
    bool saved_have_anim_uid = s_have_anim_uid;
    uint32_t saved_anim_uid = s_curr_anim_uid;
    matrix_float4x4 saved_anim_normal_transform = s_curr_anim_normal_transform;
    bool saved_have_anim_normal_transform = s_have_anim_normal_transform;

    const bool refract_on = refraction ? *refraction : true;
    const bool reflect_on = reflection ? *reflection : true;
    s_shadows_enabled = saved_shadows && in->shadows;

    render_water_scene_terrain(in, in->cam, NULL, s_water_refraction_texture, s_water_refraction_depth_texture,
        MTLClearColorMake(0.0, 0.0, 0.0, 1.0), refract_on, METAL_WATER_CLIP_KEEP_BELOW);

    s_shadows_enabled = false;
    struct camera *reflect_cam = Camera_New();
    if(reflect_cam) {
        vec3_t cam_pos = Camera_GetPos(in->cam);
        vec3_t cam_dir = Camera_GetDir(in->cam);
        cam_pos.y -= (cam_pos.y - METAL_WATER_LEVEL) * 2.0f;
        cam_dir.y *= -1.0f;
        Camera_SetPos(reflect_cam, cam_pos);
        Camera_SetDir(reflect_cam, cam_dir);
        Camera_TickFinishPerspective(reflect_cam);

        render_water_scene_terrain(in, reflect_cam, reflect_cam, s_water_reflection_texture, s_water_reflection_depth_texture,
            MTLClearColorMake(0.2, 0.3, 0.3, 1.0), reflect_on, METAL_WATER_CLIP_KEEP_ABOVE);
        Camera_Free(reflect_cam);
    }

    s_scene_view = saved_view;
    s_scene_proj = saved_proj;
    s_scene_view_pos = saved_view_pos;
    s_have_scene_view = saved_have_view;
    s_have_scene_proj = saved_have_proj;
    s_shadows_enabled = saved_shadows;
    s_have_anim_uid = saved_have_anim_uid;
    s_curr_anim_uid = saved_anim_uid;
    s_curr_anim_normal_transform = saved_anim_normal_transform;
    s_have_anim_normal_transform = saved_have_anim_normal_transform;
}

static float metal_water_move_factor(void)
{
    const char *fixed_phase = getenv("PF_RENDER_WATER_MOVE_FACTOR");
    if(fixed_phase && *fixed_phase) {
        char *end = NULL;
        float parsed = strtof(fixed_phase, &end);
        if(end != fixed_phase) {
            float intpart = 0.0f;
            float phase = modff(parsed, &intpart);
            return phase < 0.0f ? phase + 1.0f : phase;
        }
    }

    uint32_t curr = SDL_GetTicks();
    uint32_t delta = curr - s_water_prev_frame_tick;
    s_water_prev_frame_tick = curr;
    float intpart = 0.0f;
    s_water_move_factor += METAL_WATER_WAVE_SPEED * (delta / 1000.0f);
    s_water_move_factor = modff(s_water_move_factor, &intpart);
    return s_water_move_factor;
}

static void render_water_surface(const struct render_input *in, const bool *refraction, const bool *reflection)
{
    if(!in || !in->map || !s_water_buffer)
        return;
    if(!s_have_scene_view || !s_have_scene_proj)
        return;
    if(s_map_chunk_size.x == 0u || s_map_chunk_size.y == 0u
    || s_map_tiles_per_chunk.x == 0u || s_map_tiles_per_chunk.y == 0u) {
        return;
    }

    frame_begin();
    if(!s_frame_encoder || !frame_has_depth())
        return;

    render_water_scene_textures(in, refraction, reflection);
    if(!frame_resume() || !frame_has_depth())
        return;

    id<MTLRenderPipelineState> pipeline = ensure_water_surface_pipeline(frame_uses_msaa());
    if(!pipeline)
        return;

    const vec3_t tl = (vec3_t){+1.0f, METAL_WATER_LEVEL, +1.0f};
    const vec3_t tr = (vec3_t){-1.0f, METAL_WATER_LEVEL, +1.0f};
    const vec3_t bl = (vec3_t){+1.0f, METAL_WATER_LEVEL, -1.0f};
    const vec3_t br = (vec3_t){-1.0f, METAL_WATER_LEVEL, -1.0f};
    const vec3_t verts[] = {
        tl, bl, tr,
        bl, br, tr
    };

    id<MTLBuffer> vertex_buffer = [s_device newBufferWithBytes:verts
        length:sizeof(verts) options:MTLResourceStorageModeShared];
    if(!vertex_buffer)
        return;

    vec3_t pos = M_GetCenterPos(in->map);
    mat4x4_t trans;
    PFM_Mat4x4_MakeTrans(pos.x, pos.y, pos.z, &trans);

    struct map_resolution res;
    M_GetResolution(in->map, &res);
    const float half_x = (res.chunk_w * res.tile_w * X_COORDS_PER_TILE) / 2.0f;
    const float half_z = (res.chunk_h * res.tile_h * Z_COORDS_PER_TILE) / 2.0f;

    mat4x4_t scale;
    PFM_Mat4x4_MakeScale(half_x, 1.0f, half_z, &scale);

    mat4x4_t model;
    PFM_Mat4x4_Mult4x4(&trans, &scale, &model);

    vec3_t map_pos = M_GetPos(in->map);
    struct metal_water_surface_uniforms uniforms = {
        .model = matrix_from_pf_mat4(&model),
        .view = s_scene_view,
        .proj = s_scene_proj,
        .view_pos = {s_scene_view_pos.x, s_scene_view_pos.y, s_scene_view_pos.z, 1.0f},
        .map_pos = {map_pos.x, map_pos.z},
        .tile_world_size = {
            res.field_w / res.tile_w,
            res.field_h / res.tile_h,
        },
        .chunk_size = {
            (uint32_t)res.chunk_w,
            (uint32_t)res.chunk_h,
        },
        .tiles_per_chunk = {
            (uint32_t)res.tile_w,
            (uint32_t)res.tile_h,
        },
        .water_params = {
            metal_water_move_factor(),
            s_water_buffer ? 1.0f : 0.0f,
            s_fog_buffer ? 1.0f : 0.0f,
            (s_water_dudv_texture && s_water_normal_texture) ? 1.0f : 0.0f,
        },
        .water_texture_params = {
            s_layer.drawableSize.width,
            s_layer.drawableSize.height,
            s_water_reflection_texture ? 1.0f : 0.0f,
            s_water_refraction_texture ? 1.0f : 0.0f,
        },
        .water_depth_params = {
            CAM_Z_NEAR_DIST,
            CONFIG_DRAWDIST,
            0.00025f,
            s_water_refraction_depth_texture ? 1.0f : 0.0f,
        },
        .light_color = {s_light_color.x, s_light_color.y, s_light_color.z, 1.0f},
        .light_pos = {s_light_pos.x, s_light_pos.y, s_light_pos.z, 1.0f},
    };

    id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
        length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    if(!uniform_buffer)
        return;

    [s_frame_encoder setRenderPipelineState:pipeline];
    id<MTLDepthStencilState> depth_state = ensure_depth_state(false);
    if(depth_state)
        [s_frame_encoder setDepthStencilState:depth_state];
    [s_frame_encoder setCullMode:MTLCullModeNone];
    [s_frame_encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [s_frame_encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [s_frame_encoder setVertexBuffer:uniform_buffer offset:0 atIndex:1];
    [s_frame_encoder setFragmentBuffer:uniform_buffer offset:0 atIndex:1];
    if(s_fog_buffer)
        [s_frame_encoder setFragmentBuffer:s_fog_buffer offset:0 atIndex:2];
    [s_frame_encoder setFragmentBuffer:s_water_buffer offset:0 atIndex:3];
    if(ensure_scene_sampler()) {
        [s_frame_encoder setFragmentSamplerState:s_scene_sampler atIndex:0];
        if(s_water_dudv_texture)
            [s_frame_encoder setFragmentTexture:s_water_dudv_texture atIndex:0];
        if(s_water_normal_texture)
            [s_frame_encoder setFragmentTexture:s_water_normal_texture atIndex:1];
    }
    if(s_water_reflection_texture)
        [s_frame_encoder setFragmentTexture:s_water_reflection_texture atIndex:2];
    if(s_water_refraction_texture)
        [s_frame_encoder setFragmentTexture:s_water_refraction_texture atIndex:3];
    if(s_water_refraction_depth_texture)
        [s_frame_encoder setFragmentTexture:s_water_refraction_depth_texture atIndex:4];
    [s_frame_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:sizeof(verts) / sizeof(verts[0])];
}

static void fill_material_uniforms(const struct render_private *priv,
                                   vector_float4 diffuse[MAX_MATERIALS],
                                   vector_float4 ambient[MAX_MATERIALS],
                                   vector_float4 specular[MAX_MATERIALS])
{
    for(size_t i = 0; i < MAX_MATERIALS; i++) {
        diffuse[i] = (vector_float4){0.65f, 0.65f, 0.65f, 1.0f};
        ambient[i] = (vector_float4){0.55f, 0.0f, 0.0f, 0.0f};
        specular[i] = (vector_float4){0.0f, 0.0f, 0.0f, 0.0f};
    }
    for(size_t i = 0; i < priv->num_materials && i < MAX_MATERIALS; i++) {
        diffuse[i] = (vector_float4){
            priv->materials[i].diffuse_clr.x,
            priv->materials[i].diffuse_clr.y,
            priv->materials[i].diffuse_clr.z,
            1.0f
        };
        ambient[i] = (vector_float4){
            priv->materials[i].ambient_intensity,
            0.0f,
            0.0f,
            0.0f
        };
        specular[i] = (vector_float4){
            priv->materials[i].specular_clr.x,
            priv->materials[i].specular_clr.y,
            priv->materials[i].specular_clr.z,
            1.0f
        };
    }
}

static void fill_fallback_material_rgba(unsigned char *dst, size_t npixels, const struct material *mat)
{
    unsigned char rgba[4] = {
        (unsigned char)SDL_clamp((int)lrintf(mat->diffuse_clr.x * 255.0f), 0, 255),
        (unsigned char)SDL_clamp((int)lrintf(mat->diffuse_clr.y * 255.0f), 0, 255),
        (unsigned char)SDL_clamp((int)lrintf(mat->diffuse_clr.z * 255.0f), 0, 255),
        255,
    };
    for(size_t i = 0; i < npixels; i++) {
        memcpy(dst + i * 4, rgba, 4);
    }
}

static bool texture_has_cutout_alpha(const unsigned char *rgba, size_t npixels)
{
    if(!rgba)
        return false;

    for(size_t i = 0; i < npixels; i++) {
        unsigned char alpha = rgba[i * 4 + 3];
        if(alpha < 250) {
            return true;
        }
    }
    return false;
}

static id<MTLTexture> ensure_material_texture_array(struct render_private *priv)
{
    if(!priv || !priv->num_materials || !s_device)
        return nil;
    if(priv->metal_material_texture_array)
        return (__bridge id<MTLTexture>)priv->metal_material_texture_array;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:CONFIG_ARR_TEX_RES
                                                                                    height:CONFIG_ARR_TEX_RES
                                                                                 mipmapped:YES];
    desc.textureType = MTLTextureType2DArray;
    desc.arrayLength = priv->num_materials;
    desc.usage = MTLTextureUsageShaderRead;

    id<MTLTexture> texture = [s_device newTextureWithDescriptor:desc];
    if(!texture)
        return nil;

    const size_t bytes_per_row = CONFIG_ARR_TEX_RES * 4;
    const size_t bytes_per_image = bytes_per_row * CONFIG_ARR_TEX_RES;
    const size_t npixels = CONFIG_ARR_TEX_RES * CONFIG_ARR_TEX_RES;
    unsigned char *fallback = malloc(bytes_per_image);
    unsigned char *resized = malloc(bytes_per_image);
    if(!fallback || !resized) {
        free(fallback);
        free(resized);
        return nil;
    }

    bool has_cutout_alpha = false;
    MTLRegion region = MTLRegionMake2D(0, 0, CONFIG_ARR_TEX_RES, CONFIG_ARR_TEX_RES);
    for(size_t i = 0; i < priv->num_materials; i++) {
        const struct material *mat = &priv->materials[i];
        unsigned char *data = NULL;
        int width = 0, height = 0, nr_channels = 0;

        if(mat->texname[0]) {
            char primary_path[1024];
            char secondary_path[1024];
            if(priv->metal_asset_basedir[0]) {
                snprintf(primary_path, sizeof(primary_path), "%s/%s", priv->metal_asset_basedir, mat->texname);
            } else {
                snprintf(primary_path, sizeof(primary_path), "%s", mat->texname);
            }
            snprintf(secondary_path, sizeof(secondary_path), "%s/%s", g_basepath, mat->texname);
            data = stbi_load(primary_path, &width, &height, &nr_channels, 4);
            if(!data) {
                data = stbi_load(secondary_path, &width, &height, &nr_channels, 4);
            }
        }

        const unsigned char *upload = fallback;
        if(data) {
            if(width == CONFIG_ARR_TEX_RES && height == CONFIG_ARR_TEX_RES) {
                upload = data;
            } else if(stbir_resize_uint8(data, width, height, 0,
                                         resized, CONFIG_ARR_TEX_RES, CONFIG_ARR_TEX_RES, 0, 4)) {
                upload = resized;
            } else {
                fill_fallback_material_rgba(fallback, npixels, mat);
            }
        } else {
            fill_fallback_material_rgba(fallback, npixels, mat);
        }

        if(upload != fallback && texture_has_cutout_alpha(upload, npixels)) {
            has_cutout_alpha = true;
        }

        [texture replaceRegion:region
                   mipmapLevel:0
                         slice:i
                     withBytes:upload
                   bytesPerRow:bytes_per_row
                 bytesPerImage:bytes_per_image];
        stbi_image_free(data);
    }

    free(fallback);
    free(resized);

    if(s_queue && texture.mipmapLevelCount > 1) {
        id<MTLCommandBuffer> command_buffer = [s_queue commandBuffer];
        if(command_buffer) {
            id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
            if(blit) {
                [blit generateMipmapsForTexture:texture];
                [blit endEncoding];
                [command_buffer commit];
                [command_buffer waitUntilCompleted];
            }
        }
    }

    priv->metal_material_texture_array = (__bridge_retained void *)texture;
    priv->metal_material_texture_count = priv->num_materials;
    priv->metal_materials_have_cutout_alpha = has_cutout_alpha;
    return texture;
}

static void render_static_vertex_stream(const struct render_private *priv,
                                        const mat4x4_t *model,
                                        matrix_float4x4 normal_transform,
                                        const struct vertex *verts,
                                        size_t verts_size,
                                        bool translucent)
{
    struct render_private *mutable_priv = (struct render_private *)priv;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLRenderCommandEncoder> encoder = nil;
    size_t vertex_count = verts_size / sizeof(*verts);
    bool depth_enabled = active_scene_depth_enabled();
    if(!priv || !verts || !verts_size || !priv->mesh.num_verts)
        return;
    if(!vertex_count)
        return;
    if(!s_have_scene_view || !s_have_scene_proj)
        return;
    if(!(s_water_scene_pass_active && s_water_scene_encoder))
        frame_begin();
    encoder = active_scene_encoder();
    if(!encoder)
        return;
    depth_enabled = active_scene_depth_enabled();
    pipeline = ensure_static_mesh_pipeline(translucent, encoder == s_frame_encoder && frame_uses_msaa(), depth_enabled);
    if(!pipeline)
        return;

    id<MTLTexture> texture_array = ensure_material_texture_array(mutable_priv);
    id<MTLBuffer> vertex_buffer = [s_device newBufferWithBytes:verts
        length:verts_size options:MTLResourceStorageModeShared];
    if(!vertex_buffer)
        return;

    struct metal_static_mesh_uniforms uniforms = {
        .model = matrix_from_pf_mat4(model),
        .normal_transform = normal_transform,
        .view = s_scene_view,
        .proj = s_scene_proj,
        .light_space_transform = s_shadow_light_space,
        .view_pos = {s_scene_view_pos.x, s_scene_view_pos.y, s_scene_view_pos.z, 1.0f},
        .ambient_color = {s_light_ambient.x, s_light_ambient.y, s_light_ambient.z, 1.0f},
        .light_color = {s_light_color.x, s_light_color.y, s_light_color.z, 1.0f},
        .light_pos = {s_light_pos.x, s_light_pos.y, s_light_pos.z, 1.0f},
        .effect_params = {
            (float)mutable_priv->metal_material_texture_count,
            raw_material_debug_enabled() ? 1.0f : 0.0f,
            (priv->uses_pose_buffer || mutable_priv->metal_materials_have_cutout_alpha) ? 1.0f : 0.0f,
            0.0f,
        },
        .shadow_params = {
            shadow_enabled_for_draw() ? 1.0f : 0.0f,
            0.002f,
            0.55f,
            0.0f,
        },
        .clip_params = current_water_clip_params(),
    };
    fill_material_uniforms(priv, uniforms.material_diffuse, uniforms.material_ambient, uniforms.material_specular);

    id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
        length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    if(!uniform_buffer)
        return;

    [encoder setRenderPipelineState:pipeline];
    if(depth_enabled) {
        id<MTLDepthStencilState> depth_state = ensure_depth_state(!translucent);
        if(depth_state)
            [encoder setDepthStencilState:depth_state];
    } else {
        [encoder setDepthStencilState:nil];
    }
    [encoder setCullMode:mutable_priv->metal_materials_have_cutout_alpha ? MTLCullModeNone : MTLCullModeBack];
    [encoder setFrontFacingWinding:MTLWindingClockwise];
    [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [encoder setVertexBuffer:uniform_buffer offset:0 atIndex:1];
    [encoder setFragmentBuffer:uniform_buffer offset:0 atIndex:1];
    if(texture_array && ensure_material_sampler()) {
        [encoder setFragmentTexture:texture_array atIndex:0];
        [encoder setFragmentSamplerState:s_material_sampler atIndex:0];
    }
    if(shadow_enabled_for_draw()) {
        [encoder setFragmentTexture:s_shadow_depth_texture atIndex:1];
        [encoder setFragmentSamplerState:s_shadow_sampler atIndex:1];
    }
    bind_shadow_screen_probe_resources(encoder, 2, 2, 3);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:vertex_count];
}

static void render_static_mesh_draw(const struct render_private *priv, const mat4x4_t *model, bool translucent)
{
    struct render_private *mutable_priv = (struct render_private *)priv;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLRenderCommandEncoder> encoder = nil;
    bool depth_enabled = false;
    if(!priv || !priv->metal_is_static_mesh)
        return;
    if(!priv->metal_static_verts || !priv->metal_static_verts_size)
        return;
    if(!s_have_scene_view || !s_have_scene_proj)
        return;
    if(!(s_water_scene_pass_active && s_water_scene_encoder))
        frame_begin();
    encoder = active_scene_encoder();
    if(!encoder)
        return;
    depth_enabled = active_scene_depth_enabled();
    pipeline = ensure_static_mesh_pipeline(translucent, encoder == s_frame_encoder && frame_uses_msaa(), depth_enabled);
    if(!pipeline)
        return;

    id<MTLTexture> texture_array = ensure_material_texture_array(mutable_priv);
    id<MTLBuffer> vertex_buffer = ensure_persistent_vertex_buffer(
        &mutable_priv->metal_static_vertex_buffer,
        priv->metal_static_verts,
        priv->metal_static_verts_size);
    if(!vertex_buffer)
        return;

    struct metal_static_mesh_uniforms uniforms = {
        .model = matrix_from_pf_mat4(model),
        .normal_transform = matrix_from_pf_mat4(model),
        .view = s_scene_view,
        .proj = s_scene_proj,
        .light_space_transform = s_shadow_light_space,
        .view_pos = {s_scene_view_pos.x, s_scene_view_pos.y, s_scene_view_pos.z, 1.0f},
        .ambient_color = {s_light_ambient.x, s_light_ambient.y, s_light_ambient.z, 1.0f},
        .light_color = {s_light_color.x, s_light_color.y, s_light_color.z, 1.0f},
        .light_pos = {s_light_pos.x, s_light_pos.y, s_light_pos.z, 1.0f},
        .effect_params = {
            (float)mutable_priv->metal_material_texture_count,
            raw_material_debug_enabled() ? 1.0f : 0.0f,
            mutable_priv->metal_materials_have_cutout_alpha ? 1.0f : 0.0f,
            0.0f,
        },
        .shadow_params = {
            shadow_enabled_for_draw() ? 1.0f : 0.0f,
            0.002f,
            0.55f,
            0.0f,
        },
        .clip_params = current_water_clip_params(),
    };
    fill_material_uniforms(priv, uniforms.material_diffuse, uniforms.material_ambient, uniforms.material_specular);

    id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
        length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    if(!uniform_buffer)
        return;

    [encoder setRenderPipelineState:pipeline];
    if(depth_enabled) {
        id<MTLDepthStencilState> depth_state = ensure_depth_state(!translucent);
        if(depth_state)
            [encoder setDepthStencilState:depth_state];
    } else {
        [encoder setDepthStencilState:nil];
    }
    [encoder setCullMode:mutable_priv->metal_materials_have_cutout_alpha ? MTLCullModeNone : MTLCullModeBack];
    [encoder setFrontFacingWinding:MTLWindingClockwise];
    [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [encoder setVertexBuffer:uniform_buffer offset:0 atIndex:1];
    [encoder setFragmentBuffer:uniform_buffer offset:0 atIndex:1];
    if(texture_array && ensure_material_sampler()) {
        [encoder setFragmentTexture:texture_array atIndex:0];
        [encoder setFragmentSamplerState:s_material_sampler atIndex:0];
    }
    if(shadow_enabled_for_draw()) {
        [encoder setFragmentTexture:s_shadow_depth_texture atIndex:1];
        [encoder setFragmentSamplerState:s_shadow_sampler atIndex:1];
    }
    bind_shadow_screen_probe_resources(encoder, 2, 2, 3);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:priv->mesh.num_verts];
}

static void render_world_colored_strip(const vec3_t *positions, size_t nverts, const vec3_t *color)
{
    id<MTLRenderPipelineState> pipeline = nil;
    if(!positions || !nverts || !color)
        return;
    if(!s_have_scene_view || !s_have_scene_proj)
        return;

    struct vertex *verts = malloc(nverts * sizeof(*verts));
    if(!verts)
        return;

    for(size_t i = 0; i < nverts; i++) {
        verts[i].pos = positions[i];
        verts[i].uv = (vec2_t){0.0f, 0.0f};
        verts[i].normal = (vec3_t){0.0f, 1.0f, 0.0f};
        verts[i].material_idx = 0;
    }

    frame_begin();
    if(!s_frame_encoder) {
        free(verts);
        return;
    }
    pipeline = ensure_static_mesh_pipeline(false, frame_uses_msaa(), false);
    if(!pipeline) {
        free(verts);
        return;
    }

    id<MTLBuffer> vertex_buffer = [s_device newBufferWithBytes:verts
        length:nverts * sizeof(*verts) options:MTLResourceStorageModeShared];
    free(verts);
    if(!vertex_buffer)
        return;

    mat4x4_t identity;
    PFM_Mat4x4_Identity(&identity);

    struct metal_static_mesh_uniforms uniforms = {
        .model = matrix_from_pf_mat4(&identity),
        .view = s_scene_view,
        .proj = s_scene_proj,
        .view_pos = {s_scene_view_pos.x, s_scene_view_pos.y, s_scene_view_pos.z, 1.0f},
        .ambient_color = {s_light_ambient.x, s_light_ambient.y, s_light_ambient.z, 1.0f},
        .light_color = {s_light_color.x, s_light_color.y, s_light_color.z, 1.0f},
        .light_pos = {s_light_pos.x, s_light_pos.y, s_light_pos.z, 1.0f},
    };
    for(size_t i = 0; i < MAX_MATERIALS; i++) {
        uniforms.material_diffuse[i] = (vector_float4){color->x, color->y, color->z, 1.0f};
        uniforms.material_ambient[i] = (vector_float4){1.0f, 0.0f, 0.0f, 0.0f};
        uniforms.material_specular[i] = (vector_float4){0.0f, 0.0f, 0.0f, 0.0f};
    }

    id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
        length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    if(!uniform_buffer)
        return;

    [s_frame_encoder setRenderPipelineState:pipeline];
    [s_frame_encoder setDepthStencilState:nil];
    [s_frame_encoder setCullMode:MTLCullModeNone];
    [s_frame_encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [s_frame_encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [s_frame_encoder setVertexBuffer:uniform_buffer offset:0 atIndex:1];
    [s_frame_encoder setFragmentBuffer:uniform_buffer offset:0 atIndex:1];
    [s_frame_encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                        vertexStart:0
                        vertexCount:nverts];
}

static void render_world_colored_verts(const struct colored_vert *verts, size_t nverts,
                                       MTLPrimitiveType primitive)
{
    id<MTLRenderPipelineState> pipeline = nil;
    if(!verts || !nverts)
        return;
    if(!s_have_scene_view || !s_have_scene_proj)
        return;

    frame_begin();
    if(!s_frame_encoder)
        return;
    const bool depth_enabled = frame_has_depth();
    pipeline = ensure_world_color_pipeline(frame_uses_msaa(), depth_enabled);
    if(!pipeline)
        return;

    id<MTLBuffer> vertex_buffer = [s_device newBufferWithBytes:verts
        length:nverts * sizeof(*verts) options:MTLResourceStorageModeShared];
    if(!vertex_buffer)
        return;

    struct metal_world_color_uniforms uniforms = {
        .view = s_scene_view,
        .proj = s_scene_proj,
    };
    id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
        length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    if(!uniform_buffer)
        return;

    [s_frame_encoder setRenderPipelineState:pipeline];
    if(depth_enabled) {
        id<MTLDepthStencilState> depth_state = ensure_depth_state(false);
        if(depth_state)
            [s_frame_encoder setDepthStencilState:depth_state];
    } else {
        [s_frame_encoder setDepthStencilState:nil];
    }
    [s_frame_encoder setCullMode:MTLCullModeNone];
    [s_frame_encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [s_frame_encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [s_frame_encoder setVertexBuffer:uniform_buffer offset:0 atIndex:1];
    [s_frame_encoder drawPrimitives:primitive
                        vertexStart:0
                        vertexCount:nverts];
}

static void render_screenspace_colored_triangles(const vec3_t *positions, size_t nverts, const vec3_t *color)
{
    id<MTLRenderPipelineState> pipeline = nil;
    if(!positions || !nverts || !color)
        return;

    struct colored_vert *verts = malloc(nverts * sizeof(*verts));
    if(!verts)
        return;

    for(size_t i = 0; i < nverts; i++) {
        verts[i].pos = positions[i];
        verts[i].color = (vec4_t){color->x, color->y, color->z, 1.0f};
    }

    frame_begin();
    if(!s_frame_encoder) {
        free(verts);
        return;
    }
    pipeline = ensure_world_color_pipeline(frame_uses_msaa(), false);
    if(!pipeline) {
        free(verts);
        return;
    }

    id<MTLBuffer> vertex_buffer = [s_device newBufferWithBytes:verts
        length:nverts * sizeof(*verts) options:MTLResourceStorageModeShared];
    free(verts);
    if(!vertex_buffer)
        return;

    int win_width = 0, win_height = 0;
    Engine_WinDrawableSize(&win_width, &win_height);

    mat4x4_t view, proj;
    PFM_Mat4x4_Identity(&view);
    PFM_Mat4x4_MakeOrthographic(0.0f, win_width, win_height, 0.0f, -1.0f, 1.0f, &proj);

    struct metal_world_color_uniforms uniforms = {
        .view = matrix_from_pf_mat4(&view),
        .proj = matrix_from_pf_mat4(&proj),
    };

    id<MTLBuffer> uniform_buffer = [s_device newBufferWithBytes:&uniforms
        length:sizeof(uniforms) options:MTLResourceStorageModeShared];
    if(!uniform_buffer)
        return;

    [s_frame_encoder setRenderPipelineState:pipeline];
    [s_frame_encoder setDepthStencilState:nil];
    [s_frame_encoder setCullMode:MTLCullModeNone];
    [s_frame_encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [s_frame_encoder setVertexBuffer:uniform_buffer offset:0 atIndex:1];
    [s_frame_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:nverts];
}

static void render_screenspace_line_segment(vec2_t a, vec2_t b, float width, const vec3_t *color)
{
    vec2_t ab;
    PFM_Vec2_Sub(&b, &a, &ab);
    float len = PFM_Vec2_Len(&ab);
    if(len <= 0.0f)
        return;

    vec2_t dir;
    PFM_Vec2_Scale(&ab, 1.0f / len, &dir);
    vec2_t perp = {-dir.y, dir.x};
    float half = fmaxf(width, 1.0f) * 0.5f;
    PFM_Vec2_Scale(&perp, half, &perp);

    vec2_t a0, a1, b0, b1;
    PFM_Vec2_Sub(&a, &perp, &a0);
    PFM_Vec2_Add(&a, &perp, &a1);
    PFM_Vec2_Sub(&b, &perp, &b0);
    PFM_Vec2_Add(&b, &perp, &b1);

    const vec3_t verts[6] = {
        {a0.x, a0.y, 0.0f},
        {a1.x, a1.y, 0.0f},
        {b1.x, b1.y, 0.0f},
        {a0.x, a0.y, 0.0f},
        {b1.x, b1.y, 0.0f},
        {b0.x, b0.y, 0.0f},
    };
    render_screenspace_colored_triangles(verts, 6, color);
}

static void render_screenspace_line_loop(const vec2_t *points, size_t npoints,
                                         float width, const vec3_t *color)
{
    if(!points || npoints < 2 || !color)
        return;

    for(size_t i = 0; i < npoints; i++) {
        render_screenspace_line_segment(points[i], points[(i + 1) % npoints], width, color);
    }
}

static bool clip_axis(float p, float q, float *t0, float *t1)
{
    if(p == 0.0f)
        return q >= 0.0f;

    float r = q / p;
    if(p < 0.0f) {
        if(r > *t1)
            return false;
        if(r > *t0)
            *t0 = r;
    } else {
        if(r < *t0)
            return false;
        if(r < *t1)
            *t1 = r;
    }
    return true;
}

static bool clip_segment_to_unit_square(vec2_t a, vec2_t b, vec2_t *out_a, vec2_t *out_b)
{
    float t0 = 0.0f;
    float t1 = 1.0f;
    vec2_t d;
    PFM_Vec2_Sub(&b, &a, &d);

    if(!clip_axis(-d.x, a.x + 1.0f, &t0, &t1))
        return false;
    if(!clip_axis( d.x, 1.0f - a.x, &t0, &t1))
        return false;
    if(!clip_axis(-d.y, a.y + 1.0f, &t0, &t1))
        return false;
    if(!clip_axis( d.y, 1.0f - a.y, &t0, &t1))
        return false;

    *out_a = (vec2_t){a.x + d.x * t0, a.y + d.y * t0};
    *out_b = (vec2_t){a.x + d.x * t1, a.y + d.y * t1};
    return true;
}

static void render_box2d(const vec2_t *screen_pos, const vec2_t *signed_size,
                         const vec3_t *color, const float *width)
{
    if(!screen_pos || !signed_size || !color || !width)
        return;

    float x0 = screen_pos->x;
    float y0 = screen_pos->y;
    float x1 = screen_pos->x + signed_size->x;
    float y1 = screen_pos->y + signed_size->y;

    float left = fminf(x0, x1);
    float right = fmaxf(x0, x1);
    float top = fminf(y0, y1);
    float bottom = fmaxf(y0, y1);

    float w = right - left;
    float h = bottom - top;
    if(w <= 0.0f || h <= 0.0f)
        return;

    float t = fmaxf(*width, 1.0f);
    t = fminf(t, w * 0.5f);
    t = fminf(t, h * 0.5f);
    if(t <= 0.0f)
        return;

    const vec3_t verts[] = {
        {left, top, 0.0f}, {right, top, 0.0f}, {right, top + t, 0.0f},
        {left, top, 0.0f}, {right, top + t, 0.0f}, {left, top + t, 0.0f},

        {left, bottom - t, 0.0f}, {right, bottom - t, 0.0f}, {right, bottom, 0.0f},
        {left, bottom - t, 0.0f}, {right, bottom, 0.0f}, {left, bottom, 0.0f},

        {left, top + t, 0.0f}, {left + t, top + t, 0.0f}, {left + t, bottom - t, 0.0f},
        {left, top + t, 0.0f}, {left + t, bottom - t, 0.0f}, {left, bottom - t, 0.0f},

        {right - t, top + t, 0.0f}, {right, top + t, 0.0f}, {right, bottom - t, 0.0f},
        {right - t, top + t, 0.0f}, {right, bottom - t, 0.0f}, {right - t, bottom - t, 0.0f},
    };

    render_screenspace_colored_triangles(verts, sizeof(verts) / sizeof(verts[0]), color);
}

static void render_screenspace_rect(float left, float top, float right, float bottom,
                                    const vec3_t *color)
{
    if(!color)
        return;
    if(right <= left || bottom <= top)
        return;

    const vec3_t verts[] = {
        {left,  top,    0.0f},
        {right, top,    0.0f},
        {right, bottom, 0.0f},
        {left,  top,    0.0f},
        {right, bottom, 0.0f},
        {left,  bottom, 0.0f},
    };
    render_screenspace_colored_triangles(verts, sizeof(verts) / sizeof(verts[0]), color);
}

static void render_healthbars(const size_t *num_ents, GLfloat *ent_health_pc,
                              vec3_t *ent_top_pos_ws, int *yoffsets,
                              const struct camera *cam)
{
    if(!num_ents || !ent_health_pc || !ent_top_pos_ws || !yoffsets || !cam)
        return;
    if(*num_ents == 0)
        return;

    int width, height;
    Engine_WinDrawableSize(&width, &height);

    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view);
    Camera_MakeProjMat(cam, &proj);

    const float half_h = fmaxf(4.0f / 1080.0f * height, 4.0f);
    const float half_w = 40.0f / 1080.0f * height;
    const float border = 2.0f;
    const vec3_t bg = {0.0f, 0.0f, 0.0f};

    for(size_t i = 0; i < *num_ents; i++) {
        vec4_t ent_top_homo = {
            ent_top_pos_ws[i].x,
            ent_top_pos_ws[i].y,
            ent_top_pos_ws[i].z,
            1.0f
        };

        vec4_t tmp = {0};
        vec4_t clip = {0};
        PFM_Mat4x4_Mult4x1(&view, &ent_top_homo, &tmp);
        PFM_Mat4x4_Mult4x1(&proj, &tmp, &clip);
        if(fabsf(clip.w) < 0.0001f)
            continue;

        vec3_t ndc = {clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
        if(ndc.z < -1.0f || ndc.z > 1.0f)
            continue;

        float screen_x = (ndc.x + 1.0f) * width * 0.5f;
        float screen_y = height - ((ndc.y + 1.0f) * height * 0.5f);
        screen_y += yoffsets[i];

        float left = screen_x - half_w;
        float right = screen_x + half_w;
        float top = screen_y - half_h;
        float bottom = screen_y + half_h;

        render_screenspace_rect(left, top, right, bottom, &bg);

        float inner_left = left + border;
        float inner_right = right - border;
        float inner_top = top + border;
        float inner_bottom = bottom - border;
        float health_pc = SDL_clamp(ent_health_pc[i], 0.0f, 1.0f);
        float fill_right = left + (right - left) * health_pc;
        fill_right = fminf(fill_right, inner_right);

        vec3_t fill_top = {
            1.0f - health_pc,
            health_pc,
            0.0f
        };
        vec3_t fill_bottom = {
            fill_top.x * 0.8f,
            fill_top.y * 0.8f,
            fill_top.z * 0.8f
        };
        render_screenspace_rect(inner_left, inner_top, fill_right, screen_y, &fill_top);
        render_screenspace_rect(inner_left, screen_y, fill_right, inner_bottom, &fill_bottom);
    }
}

static void render_map_overlay_quads(vec2_t *xz_corners, vec3_t *colors, const size_t *count,
                                     mat4x4_t *model, bool *on_water_surface,
                                     const struct map *map)
{
    if(!xz_corners || !colors || !count || !model || !on_water_surface || !map || !*count)
        return;

    const size_t surf_verts = *count * 4 * 3;
    const size_t line_verts = *count * 4 * 2;
    struct colored_vert *surf_vbuff = malloc(surf_verts * sizeof(*surf_vbuff));
    struct colored_vert *line_vbuff = malloc(line_verts * sizeof(*line_vbuff));
    if(!surf_vbuff || !line_vbuff) {
        free(surf_vbuff);
        free(line_vbuff);
        return;
    }

    struct colored_vert *surf_base = surf_vbuff;
    struct colored_vert *line_base = line_vbuff;

    for(size_t i = 0; i < *count; i++, xz_corners += 4, colors++) {
        vec2_t center = (vec2_t){
            (xz_corners[0].x + xz_corners[1].x + xz_corners[2].x + xz_corners[3].x) / 4.0f,
            (xz_corners[0].y + xz_corners[1].y + xz_corners[2].y + xz_corners[3].y) / 4.0f,
        };
        vec2_t verts[5] = {
            center,
            xz_corners[0], xz_corners[1],
            xz_corners[2], xz_corners[3],
        };
        vec3_t verts_3d[5];

        for(int j = 0; j < (int)(sizeof(verts) / sizeof(verts[0])); j++) {
            vec4_t xz_homo = {verts[j].x, 0.0f, verts[j].y, 1.0f};
            vec4_t ws_homo;
            PFM_Mat4x4_Mult4x1(model, &xz_homo, &ws_homo);
            ws_homo.x /= ws_homo.w;
            ws_homo.z /= ws_homo.w;

            float height = M_HeightAtPoint(map, (vec2_t){ws_homo.x, ws_homo.z}) + 0.1f;
            if(*on_water_surface)
                height = MAX(height, 0.1f);

            verts_3d[j] = (vec3_t){ws_homo.x, height, ws_homo.z};
        }

        vec4_t surf_color = {colors->x, colors->y, colors->z, 0.25f};
        vec4_t line_color = {colors->x, colors->y, colors->z, 0.75f};

        *surf_base++ = (struct colored_vert){verts_3d[0], surf_color};
        *surf_base++ = (struct colored_vert){verts_3d[1], surf_color};
        *surf_base++ = (struct colored_vert){verts_3d[2], surf_color};

        *surf_base++ = (struct colored_vert){verts_3d[0], surf_color};
        *surf_base++ = (struct colored_vert){verts_3d[2], surf_color};
        *surf_base++ = (struct colored_vert){verts_3d[3], surf_color};

        *surf_base++ = (struct colored_vert){verts_3d[0], surf_color};
        *surf_base++ = (struct colored_vert){verts_3d[3], surf_color};
        *surf_base++ = (struct colored_vert){verts_3d[4], surf_color};

        *surf_base++ = (struct colored_vert){verts_3d[0], surf_color};
        *surf_base++ = (struct colored_vert){verts_3d[4], surf_color};
        *surf_base++ = (struct colored_vert){verts_3d[1], surf_color};

        *line_base++ = (struct colored_vert){verts_3d[1], line_color};
        *line_base++ = (struct colored_vert){verts_3d[2], line_color};

        *line_base++ = (struct colored_vert){verts_3d[2], line_color};
        *line_base++ = (struct colored_vert){verts_3d[3], line_color};

        *line_base++ = (struct colored_vert){verts_3d[3], line_color};
        *line_base++ = (struct colored_vert){verts_3d[4], line_color};

        *line_base++ = (struct colored_vert){verts_3d[4], line_color};
        *line_base++ = (struct colored_vert){verts_3d[1], line_color};
    }

    render_world_colored_verts(surf_vbuff, surf_verts, MTLPrimitiveTypeTriangle);
    render_world_colored_verts(line_vbuff, line_verts, MTLPrimitiveTypeLine);

    free(surf_vbuff);
    free(line_vbuff);
}

static void render_minimap_bake(const struct map *map, void **chunk_rprivates, mat4x4_t *chunk_model_mats)
{
    if(!map || !chunk_rprivates || !chunk_model_mats)
        return;
    if(!ensure_terrain_pipeline(false, false))
        return;

    matrix_float4x4 view, proj;
    if(!build_minimap_view_proj(map, &view, &proj))
        return;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:METAL_MINIMAP_RES
                                                                                    height:METAL_MINIMAP_RES
                                                                                 mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    s_minimap_texture = [s_device newTextureWithDescriptor:desc];
    if(!s_minimap_texture)
        return;

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = s_minimap_texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLCommandBuffer> command_buffer = [s_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if(!encoder)
        return;

    MTLViewport viewport = {
        .originX = 0.0,
        .originY = 0.0,
        .width = METAL_MINIMAP_RES,
        .height = METAL_MINIMAP_RES,
        .znear = 0.0,
        .zfar = 1.0
    };
    [encoder setViewport:viewport];

    struct map_resolution res;
    M_GetResolution(map, &res);
    set_minimap_map_uniforms(&res);
    update_water_mask(map, &res);
    s_minimap_bake_pass_active = true;
    for(int r = 0; r < res.chunk_h; r++) {
    for(int c = 0; c < res.chunk_w; c++) {
        size_t idx = r * res.chunk_w + c;
        draw_terrain_to_encoder(encoder, chunk_rprivates[idx], &chunk_model_mats[idx], view, proj);
    }}
    s_minimap_bake_pass_active = false;

    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
}

static MTLScissorRect minimap_chunk_scissor(const struct map_resolution *res,
                                            const int *chunk_r,
                                            const int *chunk_c)
{
    if(!res || !chunk_r || !chunk_c || res->chunk_w <= 0 || res->chunk_h <= 0)
        return (MTLScissorRect){0, 0, METAL_MINIMAP_RES, METAL_MINIMAP_RES};

    float chunk_width_px = fminf((float)METAL_MINIMAP_RES / (float)res->chunk_w,
                                 (float)METAL_MINIMAP_RES / (float)res->chunk_h);
    float center = (float)METAL_MINIMAP_RES / 2.0f;
    float center_rel_r = (float)(*chunk_r) - (float)res->chunk_h / 2.0f;
    float center_rel_c = (float)(*chunk_c) - (float)res->chunk_w / 2.0f;
    float gl_x = center + center_rel_c * chunk_width_px;
    float gl_y = center + center_rel_r * chunk_width_px;
    float metal_y = (float)METAL_MINIMAP_RES - (gl_y + chunk_width_px);

    int x0 = (int)floorf(gl_x);
    int y0 = (int)floorf(metal_y);
    int x1 = (int)ceilf(gl_x + chunk_width_px);
    int y1 = (int)ceilf(metal_y + chunk_width_px);

    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 > METAL_MINIMAP_RES) x1 = METAL_MINIMAP_RES;
    if(y1 > METAL_MINIMAP_RES) y1 = METAL_MINIMAP_RES;
    if(x1 < x0) x1 = x0;
    if(y1 < y0) y1 = y0;

    return (MTLScissorRect){
        .x = (NSUInteger)x0,
        .y = (NSUInteger)y0,
        .width = (NSUInteger)(x1 - x0),
        .height = (NSUInteger)(y1 - y0),
    };
}

static void render_minimap_update_chunk(const struct map *map, void *chunk_rprivate,
                                        mat4x4_t *chunk_model,
                                        const int *chunk_r, const int *chunk_c)
{
    if(!map || !chunk_rprivate || !chunk_model || !s_minimap_texture)
        return;
    if(!ensure_terrain_pipeline(false, false))
        return;

    matrix_float4x4 view, proj;
    if(!build_minimap_view_proj(map, &view, &proj))
        return;

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = s_minimap_texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLCommandBuffer> command_buffer = [s_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if(!encoder)
        return;

    MTLViewport viewport = {
        .originX = 0.0,
        .originY = 0.0,
        .width = METAL_MINIMAP_RES,
        .height = METAL_MINIMAP_RES,
        .znear = 0.0,
        .zfar = 1.0
    };
    [encoder setViewport:viewport];
    struct map_resolution res;
    M_GetResolution(map, &res);
    set_minimap_map_uniforms(&res);
    update_water_mask(map, &res);
    [encoder setScissorRect:minimap_chunk_scissor(&res, chunk_r, chunk_c)];
    s_minimap_bake_pass_active = true;
    draw_terrain_to_encoder(encoder, chunk_rprivate, chunk_model, view, proj);
    s_minimap_bake_pass_active = false;
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
}

static void render_minimap_frustum(const struct map *map, const struct camera *cam,
                                   const mat4x4_t *model)
{
    if(!map || !cam || !model)
        return;

    vec3_t tr, tl, br, bl;
    struct frustum cam_frust;
    Camera_MakeFrustum(cam, &cam_frust);
    vec3_t cam_pos = Camera_GetPos(cam);

    struct plane ground_plane = {
        .point = {0.0f, 0.0f, 0.0f},
        .normal = {0.0f, 1.0f, 0.0f},
    };

    vec3_t tr_dir, tl_dir, br_dir, bl_dir;
    PFM_Vec3_Sub(&cam_frust.ftr, &cam_frust.ntr, &tr_dir);
    PFM_Vec3_Sub(&cam_frust.ftl, &cam_frust.ntl, &tl_dir);
    PFM_Vec3_Sub(&cam_frust.fbr, &cam_frust.nbr, &br_dir);
    PFM_Vec3_Sub(&cam_frust.fbl, &cam_frust.nbl, &bl_dir);
    PFM_Vec3_Normal(&tr_dir, &tr_dir);
    PFM_Vec3_Normal(&tl_dir, &tl_dir);
    PFM_Vec3_Normal(&br_dir, &br_dir);
    PFM_Vec3_Normal(&bl_dir, &bl_dir);

    float t;
    if(!C_RayIntersectsPlane(cam_pos, tr_dir, ground_plane, &t))
        t = 1e10f;
    PFM_Vec3_Scale(&tr_dir, t, &tr_dir);
    PFM_Vec3_Add(&cam_pos, &tr_dir, &tr);

    if(!C_RayIntersectsPlane(cam_pos, tl_dir, ground_plane, &t))
        t = 1e10f;
    PFM_Vec3_Scale(&tl_dir, t, &tl_dir);
    PFM_Vec3_Add(&cam_pos, &tl_dir, &tl);

    if(!C_RayIntersectsPlane(cam_pos, br_dir, ground_plane, &t))
        return;
    PFM_Vec3_Scale(&br_dir, t, &br_dir);
    PFM_Vec3_Add(&cam_pos, &br_dir, &br);

    if(!C_RayIntersectsPlane(cam_pos, bl_dir, ground_plane, &t))
        return;
    PFM_Vec3_Scale(&bl_dir, t, &bl_dir);
    PFM_Vec3_Add(&cam_pos, &bl_dir, &bl);

    vec2_t norm[4] = {
        M_WorldCoordsToNormMapCoords(map, (vec2_t){tr.x, tr.z}),
        M_WorldCoordsToNormMapCoords(map, (vec2_t){tl.x, tl.z}),
        M_WorldCoordsToNormMapCoords(map, (vec2_t){bl.x, bl.z}),
        M_WorldCoordsToNormMapCoords(map, (vec2_t){br.x, br.z}),
    };

    vec3_t black = {0.0f, 0.0f, 0.0f};
    vec3_t white = {1.0f, 1.0f, 1.0f};
    for(int i = 0; i < 4; i++) {
        vec2_t clipped_a, clipped_b;
        if(!clip_segment_to_unit_square(norm[i], norm[(i + 1) % 4], &clipped_a, &clipped_b))
            continue;

        vec2_t black_a = transform_model_point(model, clipped_a);
        vec2_t black_b = transform_model_point(model, clipped_b);
        vec2_t white_a = {black_a.x - 1.0f, black_a.y - 1.0f};
        vec2_t white_b = {black_b.x - 1.0f, black_b.y - 1.0f};

        render_screenspace_line_segment(black_a, black_b, 1.0f, &black);
        render_screenspace_line_segment(white_a, white_b, 1.0f, &white);
    }
}

static void render_minimap(const struct map *map, const struct camera *cam,
                           vec2_t *center_pos, const int *side_len_px, vec4_t *border_clr)
{
    if(!center_pos || !side_len_px || !border_clr || !s_minimap_texture)
        return;

    mat4x4_t model, border_model;
    make_minimap_models(center_pos, side_len_px, &model, &border_model);

    const vec2_t border_local[4] = {
        {-1.0f, -1.0f},
        {-1.0f,  1.0f},
        { 1.0f,  1.0f},
        { 1.0f, -1.0f},
    };
    const vec3_t border_verts[6] = {
        {transform_model_point(&border_model, border_local[0]).x, transform_model_point(&border_model, border_local[0]).y, 0.0f},
        {transform_model_point(&border_model, border_local[1]).x, transform_model_point(&border_model, border_local[1]).y, 0.0f},
        {transform_model_point(&border_model, border_local[2]).x, transform_model_point(&border_model, border_local[2]).y, 0.0f},
        {transform_model_point(&border_model, border_local[0]).x, transform_model_point(&border_model, border_local[0]).y, 0.0f},
        {transform_model_point(&border_model, border_local[2]).x, transform_model_point(&border_model, border_local[2]).y, 0.0f},
        {transform_model_point(&border_model, border_local[3]).x, transform_model_point(&border_model, border_local[3]).y, 0.0f},
    };
    vec3_t border_color = {border_clr->x, border_clr->y, border_clr->z};
    render_screenspace_colored_triangles(border_verts, 6, &border_color);

    struct ui_vert quad[6];
    build_minimap_quad(&model, quad);
    render_minimap_triangles(quad, 6, s_minimap_texture, map);

    if(cam)
        render_minimap_frustum(map, cam, &model);
}

static void render_minimap_units(const struct map *map, vec2_t *center_pos,
                                 const int *side_len_px, size_t *nunits,
                                 vec2_t *posbuff, vec3_t *colorbuff)
{
    (void)map;
    if(!center_pos || !side_len_px || !nunits || !posbuff || !colorbuff)
        return;
    if(*nunits == 0)
        return;

    mat4x4_t model, border_model;
    make_minimap_models(center_pos, side_len_px, &model, &border_model);

    float half_extent = 4.0f / (*side_len_px);
    for(size_t i = 0; i < *nunits; i++) {
        vec2_t offset = posbuff[i];
        const vec2_t local[4] = {
            {offset.x - half_extent, offset.y - half_extent},
            {offset.x - half_extent, offset.y + half_extent},
            {offset.x + half_extent, offset.y + half_extent},
            {offset.x + half_extent, offset.y - half_extent},
        };

        const vec3_t verts[6] = {
            {transform_model_point(&model, local[0]).x, transform_model_point(&model, local[0]).y, 0.0f},
            {transform_model_point(&model, local[1]).x, transform_model_point(&model, local[1]).y, 0.0f},
            {transform_model_point(&model, local[2]).x, transform_model_point(&model, local[2]).y, 0.0f},
            {transform_model_point(&model, local[0]).x, transform_model_point(&model, local[0]).y, 0.0f},
            {transform_model_point(&model, local[2]).x, transform_model_point(&model, local[2]).y, 0.0f},
            {transform_model_point(&model, local[3]).x, transform_model_point(&model, local[3]).y, 0.0f},
        };
        render_screenspace_colored_triangles(verts, 6, &colorbuff[i]);
    }
}

static void render_minimap_free(void)
{
    s_minimap_texture = nil;
}

static void render_selection_circle(const vec2_t *xz, const float *radius, const float *width,
                                    const vec3_t *color, const struct map *map)
{
    if(!xz || !radius || !width || !color || !map)
        return;

    enum { NUM_SAMPLES = 48, NUM_VERTS = NUM_SAMPLES * 2 + 2 };
    vec3_t vbuff[NUM_VERTS];

    for(int i = 0; i < NUM_SAMPLES * 2; i += 2) {
        float theta = (2.0f * (float)M_PI) * ((float)i / NUM_SAMPLES);

        float x_near = xz->x + (*radius) * cosf(theta);
        float z_near = xz->z - (*radius) * sinf(theta);

        float x_far = xz->x + (*radius + *width) * cosf(theta);
        float z_far = xz->z - (*radius + *width) * sinf(theta);

        float height_near = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, (vec2_t){x_near, z_near}));
        float height_far = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, (vec2_t){x_far, z_far}));

        vbuff[i] = (vec3_t){x_near, height_near + 0.1f, z_near};
        vbuff[i + 1] = (vec3_t){x_far, height_far + 0.1f, z_far};
    }
    vbuff[NUM_SAMPLES * 2] = vbuff[0];
    vbuff[NUM_SAMPLES * 2 + 1] = vbuff[1];

    render_world_colored_strip(vbuff, NUM_VERTS, color);
}

static void render_selection_rectangle(const struct obb *box, const float *width,
                                       const vec3_t *color, const struct map *map)
{
    if(!box || !width || !color || !map)
        return;

    const float pad = 1.0f;
    vec2_t corners[4] = {
        {box->corners[0].x, box->corners[0].z},
        {box->corners[1].x, box->corners[1].z},
        {box->corners[5].x, box->corners[5].z},
        {box->corners[4].x, box->corners[4].z},
    };

    float lens[4];
    vec2_t deltas[4];
    PFM_Vec2_Sub(&corners[1], &corners[0], &deltas[0]);
    PFM_Vec2_Sub(&corners[2], &corners[1], &deltas[1]);
    PFM_Vec2_Sub(&corners[3], &corners[2], &deltas[2]);
    PFM_Vec2_Sub(&corners[0], &corners[3], &deltas[3]);

    for(int i = 0; i < 4; i++) {
        lens[i] = PFM_Vec2_Len(&deltas[i]);
        PFM_Vec2_Normal(&deltas[i], &deltas[i]);
    }

    float sample_dist = fminf(X_COORDS_PER_TILE, Z_COORDS_PER_TILE);
    size_t nsamples = 0;
    for(int i = 0; i < 4; i++) {
        nsamples += (size_t)ceilf(lens[i] / sample_dist) + 1;
    }

    const int nverts = (int)(nsamples * 2 + 2);
    vec3_t *vbuff = malloc(nverts * sizeof(*vbuff));
    if(!vbuff)
        return;

    int vbuff_idx = 0;
    for(int i = 0; i < 4; i++) {
        vec3_t pdir = (vec3_t){-deltas[i].z, 0.0f, deltas[i].x};
        PFM_Vec3_Scale(&pdir, *width / 2.0f, &pdir);

        int side_samples = (int)ceilf(lens[i] / sample_dist) + 1;
        for(int j = 0; j < side_samples; j++) {
            vec2_t dir = deltas[i];
            PFM_Vec2_Scale(&dir, fminf(j * sample_dist, lens[i]), &dir);

            vec2_t xz;
            PFM_Vec2_Add(&corners[i], &dir, &xz);

            vec3_t point = (vec3_t){
                xz.x,
                M_HeightAtPoint(map, M_ClampedMapCoordinate(map, xz)) + 0.1f,
                xz.z
            };

            vec3_t nudge = (vec3_t){-deltas[i].z, 0.0f, deltas[i].x};
            vec3_t nudged;
            PFM_Vec3_Scale(&nudge, pad, &nudge);
            PFM_Vec3_Add(&point, &nudge, &nudged);

            PFM_Vec3_Sub(&nudged, &pdir, &vbuff[vbuff_idx++]);
            PFM_Vec3_Add(&nudged, &pdir, &vbuff[vbuff_idx++]);
        }
    }

    vbuff[nsamples * 2 + 0] = vbuff[0];
    vbuff[nsamples * 2 + 1] = vbuff[1];
    render_world_colored_strip(vbuff, nverts, color);
    free(vbuff);
}

static void render_world_line(vec2_t endpoints[], const float *width,
                              const vec3_t *color, const struct map *map)
{
    if(!endpoints || !width || !color || !map || *width <= 0.0f)
        return;

    vec2_t delta;
    PFM_Vec2_Sub(&endpoints[1], &endpoints[0], &delta);
    const float len = PFM_Vec2_Len(&delta);
    if(len < 0.00001f)
        return;

    vec2_t perp = (vec2_t){delta.z, -delta.x};
    PFM_Vec2_Normal(&perp, &perp);
    PFM_Vec2_Scale(&perp, *width / 2.0f, &perp);

    const int num_samples = (int)ceilf(len / 4.0f);
    const int nverts = num_samples * 2 + 2;
    vec3_t *vbuff = malloc(nverts * sizeof(*vbuff));
    if(!vbuff)
        return;

    float t = 0.0f;
    for(int i = 0; i <= num_samples * 2; i += 2) {
        vec2_t dir;
        PFM_Vec2_Sub(&endpoints[1], &endpoints[0], &dir);
        PFM_Vec2_Normal(&dir, &dir);
        PFM_Vec2_Scale(&dir, t, &dir);

        vec2_t point;
        PFM_Vec2_Add(&endpoints[0], &dir, &point);

        vec2_t point_left, point_right;
        PFM_Vec2_Add(&point, &perp, &point_left);
        PFM_Vec2_Sub(&point, &perp, &point_right);

        float height_left = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, point_left));
        float height_right = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, point_right));
        float height = fmaxf(height_left, height_right);

        vbuff[i + 0] = (vec3_t){point_left.x, height + 0.2f, point_left.z};
        vbuff[i + 1] = (vec3_t){point_right.x, height + 0.2f, point_right.z};

        t += (1.0f / num_samples) * len;
    }

    render_world_colored_strip(vbuff, nverts, color);
    free(vbuff);
}

static void render_world_quad(vec2_t corners[], const float *width,
                              const vec3_t *color, const struct map *map)
{
    if(!corners || !width || !color || !map)
        return;

    vec2_t lines[][2] = {
        {corners[0], corners[1]},
        {corners[1], corners[2]},
        {corners[2], corners[3]},
        {corners[3], corners[0]},
    };

    for(size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        render_world_line(lines[i], width, color, map);
}

static void render_flow_field(vec2_t *xz_positions, vec2_t *xz_directions,
                              const size_t *count, mat4x4_t *model,
                              const struct map *map)
{
    if(!xz_positions || !xz_directions || !count || !model || !map || *count == 0)
        return;

    const size_t verts_per_entry = 6;
    const size_t nverts = (*count) * verts_per_entry;
    struct colored_vert *vbuff = malloc(nverts * sizeof(*vbuff));
    if(!vbuff)
        return;

    const vec4_t red = {1.0f, 0.0f, 0.0f, 1.0f};
    const float marker_half = 0.75f;
    size_t idx = 0;

    for(size_t i = 0; i < *count; i++) {
        vec2_t tip = xz_positions[i];
        vec2_t to_add = xz_directions[i];
        PFM_Vec2_Scale(&to_add, 2.5f, &to_add);
        PFM_Vec2_Add(&tip, &to_add, &tip);

        vec2_t base_ws = transform_model_xz_point(model, xz_positions[i]);
        vec2_t tip_ws = transform_model_xz_point(model, tip);

        float base_h = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, base_ws)) + 0.3f;
        float tip_h = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, tip_ws)) + 0.3f;

        vec3_t base = {base_ws.x, base_h, base_ws.z};
        vec3_t end = {tip_ws.x, tip_h, tip_ws.z};
        vec3_t base_x0 = {base_ws.x - marker_half, base_h, base_ws.z};
        vec3_t base_x1 = {base_ws.x + marker_half, base_h, base_ws.z};
        vec3_t base_z0 = {base_ws.x, base_h, base_ws.z - marker_half};
        vec3_t base_z1 = {base_ws.x, base_h, base_ws.z + marker_half};

        vbuff[idx++] = (struct colored_vert){base, red};
        vbuff[idx++] = (struct colored_vert){end, red};
        vbuff[idx++] = (struct colored_vert){base_x0, red};
        vbuff[idx++] = (struct colored_vert){base_x1, red};
        vbuff[idx++] = (struct colored_vert){base_z0, red};
        vbuff[idx++] = (struct colored_vert){base_z1, red};
    }

    render_world_colored_verts(vbuff, nverts, MTLPrimitiveTypeLine);
    free(vbuff);
}

static void render_combined_hrvo(vec2_t *apexes, vec2_t *left_rays,
                                 vec2_t *right_rays, const size_t *num_vos,
                                 const struct map *map)
{
    if(!apexes || !left_rays || !right_rays || !num_vos || !map || *num_vos == 0)
        return;

    enum { NUM_SAMPLES = 150 };
    const float ray_len = 150.0f;
    const size_t nverts = (*num_vos) * (NUM_SAMPLES - 1) * 4;
    struct colored_vert *vbuff = malloc(nverts * sizeof(*vbuff));
    if(!vbuff)
        return;

    const vec4_t red = {1.0f, 0.0f, 0.0f, 1.0f};
    size_t idx = 0;

    for(size_t i = 0; i < *num_vos; i++) {
        vec2_t rays[2] = {left_rays[i], right_rays[i]};
        for(int side = 0; side < 2; side++) {
            float ray_norm = PFM_Vec2_Len(&rays[side]);
            if(ray_norm < 0.00001f)
                continue;
            PFM_Vec2_Scale(&rays[side], 1.0f / ray_norm, &rays[side]);

            for(int s = 0; s < NUM_SAMPLES - 1; s++) {
                vec2_t xz_off_a = rays[side];
                vec2_t xz_off_b = rays[side];
                PFM_Vec2_Scale(&xz_off_a, ((float)s / NUM_SAMPLES) * ray_len, &xz_off_a);
                PFM_Vec2_Scale(&xz_off_b, ((float)(s + 1) / NUM_SAMPLES) * ray_len, &xz_off_b);

                vec2_t xz_pos_a, xz_pos_b;
                PFM_Vec2_Add(&apexes[i], &xz_off_a, &xz_pos_a);
                PFM_Vec2_Add(&apexes[i], &xz_off_b, &xz_pos_b);

                xz_pos_a = M_ClampedMapCoordinate(map, xz_pos_a);
                xz_pos_b = M_ClampedMapCoordinate(map, xz_pos_b);

                vec3_t a = {
                    xz_pos_a.x,
                    M_HeightAtPoint(map, xz_pos_a) + 0.1f,
                    xz_pos_a.z
                };
                vec3_t b = {
                    xz_pos_b.x,
                    M_HeightAtPoint(map, xz_pos_b) + 0.1f,
                    xz_pos_b.z
                };

                vbuff[idx++] = (struct colored_vert){a, red};
                vbuff[idx++] = (struct colored_vert){b, red};
            }
        }
    }

    if(idx > 0)
        render_world_colored_verts(vbuff, idx, MTLPrimitiveTypeLine);
    free(vbuff);
}

static void render_debug_origin(const mat4x4_t *model)
{
    if(!model)
        return;

    const vec3_t points[6] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
    };
    const vec4_t colors[3] = {
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
    };
    struct colored_vert vbuff[6];

    for(int i = 0; i < 3; i++) {
        vbuff[i * 2 + 0] = (struct colored_vert){
            transform_model_vec3_point(model, points[i * 2 + 0]),
            colors[i]
        };
        vbuff[i * 2 + 1] = (struct colored_vert){
            transform_model_vec3_point(model, points[i * 2 + 1]),
            colors[i]
        };
    }

    render_world_colored_verts(vbuff, 6, MTLPrimitiveTypeLine);
}

static void render_debug_ray(const vec3_t *origin, const vec3_t *dir,
                             const mat4x4_t *model, const vec3_t *color,
                             const float *t)
{
    if(!origin || !dir || !model || !color || !t)
        return;

    vec3_t dircopy = *dir;
    if(PFM_Vec3_Len(&dircopy) < 0.00001f)
        return;
    PFM_Vec3_Normal(&dircopy, &dircopy);
    PFM_Vec3_Scale(&dircopy, *t, &dircopy);

    vec3_t end;
    PFM_Vec3_Add((vec3_t *)origin, &dircopy, &end);

    const vec4_t color4 = {color->x, color->y, color->z, 1.0f};
    struct colored_vert vbuff[2] = {
        {transform_model_vec3_point(model, *origin), color4},
        {transform_model_vec3_point(model, end), color4},
    };

    render_world_colored_verts(vbuff, 2, MTLPrimitiveTypeLine);
}

static void render_debug_obb(const struct aabb *aabb, const mat4x4_t *model)
{
    if(!aabb || !model)
        return;

    vec3_t points[24] = {
        [0] = {aabb->x_min, aabb->y_min, aabb->z_min},
        [1] = {aabb->x_min, aabb->y_min, aabb->z_max},
        [2] = {aabb->x_min, aabb->y_max, aabb->z_min},
        [3] = {aabb->x_min, aabb->y_max, aabb->z_max},
        [4] = {aabb->x_max, aabb->y_min, aabb->z_min},
        [5] = {aabb->x_max, aabb->y_min, aabb->z_max},
        [6] = {aabb->x_max, aabb->y_max, aabb->z_min},
        [7] = {aabb->x_max, aabb->y_max, aabb->z_max},
    };
    points[8 ] = points[0];
    points[9 ] = points[2];
    points[10] = points[1];
    points[11] = points[3];
    points[12] = points[4];
    points[13] = points[6];
    points[14] = points[5];
    points[15] = points[7];
    points[16] = points[0];
    points[17] = points[4];
    points[18] = points[1];
    points[19] = points[5];
    points[20] = points[2];
    points[21] = points[6];
    points[22] = points[3];
    points[23] = points[7];

    const vec4_t blue = {0.0f, 0.0f, 1.0f, 1.0f};
    struct colored_vert vbuff[24];
    for(size_t i = 0; i < sizeof(vbuff) / sizeof(vbuff[0]); i++) {
        vbuff[i] = (struct colored_vert){
            transform_model_vec3_point(model, points[i]),
            blue
        };
    }

    render_world_colored_verts(vbuff, sizeof(vbuff) / sizeof(vbuff[0]), MTLPrimitiveTypeLine);
}

static void render_skinned_mesh_draw(const struct render_private *priv, const mat4x4_t *model, bool translucent)
{
    if(!priv || !priv->metal_is_anim_mesh || !priv->uses_pose_buffer)
        return;
    if(!priv->metal_anim_verts || !priv->metal_anim_verts_size || !priv->mesh.num_verts)
        return;
    if(!s_have_anim_uid)
        return;

    const struct skeleton *skel = A_GetBindSkeleton(s_curr_anim_uid);
    if(!skel || !skel->inv_bind_poses)
        return;

    size_t njoints = 0;
    mat4x4_t curr_pose[METAL_MAX_JOINTS];
    A_GetCurrPoseMats(s_curr_anim_uid, &njoints, curr_pose);
    if(!njoints)
        return;
    if(njoints > METAL_MAX_JOINTS)
        njoints = METAL_MAX_JOINTS;

    mat4x4_t skin_mats[METAL_MAX_JOINTS];
    mat4x4_t skin_normal_mats[METAL_MAX_JOINTS];
    for(size_t i = 0; i < njoints; i++) {
        PFM_Mat4x4_Mult4x4(&curr_pose[i], &skel->inv_bind_poses[i], &skin_mats[i]);
        normal_transform_from_mat4(&skin_mats[i], &skin_normal_mats[i]);
    }

    matrix_float4x4 normal_transform = s_curr_anim_normal_transform;
    if(!s_have_anim_normal_transform) {
        mat4x4_t normal_model;
        normal_transform_from_mat4(model, &normal_model);
        normal_transform = matrix_from_pf_mat4(&normal_model);
    }

    struct anim_vert *src = priv->metal_anim_verts;
    struct vertex *skinned = malloc(priv->mesh.num_verts * sizeof(*skinned));
    if(!skinned)
        return;

    for(int i = 0; i < priv->mesh.num_verts; i++) {
        struct anim_vert *curr = &src[i];
        vec4_t blended_pos = {0};
        vec4_t blended_normal = {0};
        float total_weight = 0.0f;
        bool weighted = false;

        for(int j = 0; j < 6; j++)
            total_weight += curr->weights[j];

        for(int j = 0; j < 6; j++) {
            float weight = total_weight > 0.0f ? curr->weights[j] / total_weight : 0.0f;
            uint32_t joint_idx = curr->joint_indices[j];
            if(weight <= 0.0f || joint_idx >= njoints)
                continue;

            vec4_t in_pos = {curr->pos.x, curr->pos.y, curr->pos.z, 1.0f};
            vec4_t out_pos = {0};
            PFM_Mat4x4_Mult4x1(&skin_mats[joint_idx], &in_pos, &out_pos);
            blended_pos.x += out_pos.x * weight;
            blended_pos.y += out_pos.y * weight;
            blended_pos.z += out_pos.z * weight;
            blended_pos.w += out_pos.w * weight;

            vec4_t in_normal = {curr->normal.x, curr->normal.y, curr->normal.z, 0.0f};
            vec4_t out_normal = {0};
            PFM_Mat4x4_Mult4x1(&skin_normal_mats[joint_idx], &in_normal, &out_normal);
            blended_normal.x += out_normal.x * weight;
            blended_normal.y += out_normal.y * weight;
            blended_normal.z += out_normal.z * weight;
            weighted = true;
        }

        skinned[i].uv = curr->uv;
        skinned[i].material_idx = curr->material_idx;

        if(weighted) {
            skinned[i].pos = (vec3_t){blended_pos.x, blended_pos.y, blended_pos.z};
            vec3_t normal = (vec3_t){blended_normal.x, blended_normal.y, blended_normal.z};
            if(PFM_Vec3_Len(&normal) > 0.0001f) {
                PFM_Vec3_Normal(&normal, &skinned[i].normal);
            }else{
                skinned[i].normal = curr->normal;
            }
        }else{
            skinned[i].pos = curr->pos;
            skinned[i].normal = curr->normal;
        }
    }

    render_static_vertex_stream(priv, model, normal_transform, skinned, priv->mesh.num_verts * sizeof(*skinned), translucent);
    free(skinned);
}

static void append_transformed_static_mesh(const struct render_private *priv,
                                           const mat4x4_t *model,
                                           struct vertex *dst,
                                           size_t *dst_idx)
{
    if(!priv || !priv->metal_static_verts || !dst || !dst_idx)
        return;

    const struct vertex *src = priv->metal_static_verts;
    mat4x4_t model_copy = *model;

    for(int i = 0; i < priv->mesh.num_verts; i++) {
        vec4_t in_pos = {src[i].pos.x, src[i].pos.y, src[i].pos.z, 1.0f};
        vec4_t out_pos = {0};
        PFM_Mat4x4_Mult4x1(&model_copy, &in_pos, &out_pos);

        struct vertex *curr = &dst[(*dst_idx)++];
        curr->pos = (vec3_t){out_pos.x, out_pos.y, out_pos.z};
        curr->uv = src[i].uv;
        curr->material_idx = src[i].material_idx;
        curr->normal = transform_normal_with_mat4(&model_copy, src[i].normal);
    }
}

static bool append_skinned_anim_mesh(const struct render_private *priv,
                                     uint32_t uid,
                                     const mat4x4_t *model,
                                     struct vertex *dst,
                                     size_t *dst_idx)
{
    if(!priv || !priv->metal_is_anim_mesh || !priv->uses_pose_buffer)
        return false;
    if(!priv->metal_anim_verts || !priv->mesh.num_verts || !dst || !dst_idx)
        return false;

    const struct skeleton *skel = A_GetBindSkeleton(uid);
    if(!skel || !skel->inv_bind_poses)
        return false;

    size_t njoints = 0;
    mat4x4_t curr_pose[METAL_MAX_JOINTS];
    A_GetCurrPoseMats(uid, &njoints, curr_pose);
    if(!njoints)
        return false;
    if(njoints > METAL_MAX_JOINTS)
        njoints = METAL_MAX_JOINTS;

    mat4x4_t skin_mats[METAL_MAX_JOINTS];
    mat4x4_t skin_normal_mats[METAL_MAX_JOINTS];
    for(size_t i = 0; i < njoints; i++) {
        PFM_Mat4x4_Mult4x4(&curr_pose[i], &skel->inv_bind_poses[i], &skin_mats[i]);
        normal_transform_from_mat4(&skin_mats[i], &skin_normal_mats[i]);
    }

    mat4x4_t model_copy = *model;

    struct anim_vert *src = priv->metal_anim_verts;
    for(int i = 0; i < priv->mesh.num_verts; i++) {
        struct anim_vert *curr = &src[i];
        vec4_t blended_pos = {0};
        vec4_t blended_normal = {0};
        float total_weight = 0.0f;
        bool weighted = false;

        for(int j = 0; j < 6; j++)
            total_weight += curr->weights[j];

        for(int j = 0; j < 6; j++) {
            float weight = total_weight > 0.0f ? curr->weights[j] / total_weight : 0.0f;
            uint32_t joint_idx = curr->joint_indices[j];
            if(weight <= 0.0f || joint_idx >= njoints)
                continue;

            vec4_t in_pos = {curr->pos.x, curr->pos.y, curr->pos.z, 1.0f};
            vec4_t out_pos = {0};
            PFM_Mat4x4_Mult4x1(&skin_mats[joint_idx], &in_pos, &out_pos);
            blended_pos.x += out_pos.x * weight;
            blended_pos.y += out_pos.y * weight;
            blended_pos.z += out_pos.z * weight;
            blended_pos.w += out_pos.w * weight;

            vec4_t in_normal = {curr->normal.x, curr->normal.y, curr->normal.z, 0.0f};
            vec4_t out_normal = {0};
            PFM_Mat4x4_Mult4x1(&skin_normal_mats[joint_idx], &in_normal, &out_normal);
            blended_normal.x += out_normal.x * weight;
            blended_normal.y += out_normal.y * weight;
            blended_normal.z += out_normal.z * weight;
            weighted = true;
        }

        vec3_t local_pos;
        vec3_t local_normal;
        if(weighted) {
            local_pos = (vec3_t){blended_pos.x, blended_pos.y, blended_pos.z};
            local_normal = (vec3_t){blended_normal.x, blended_normal.y, blended_normal.z};
            if(PFM_Vec3_Len(&local_normal) > 0.0001f) {
                PFM_Vec3_Normal(&local_normal, &local_normal);
            }else{
                local_normal = curr->normal;
            }
        }else{
            local_pos = curr->pos;
            local_normal = curr->normal;
        }

        vec4_t in_pos = {local_pos.x, local_pos.y, local_pos.z, 1.0f};
        vec4_t out_pos = {0};
        PFM_Mat4x4_Mult4x1(&model_copy, &in_pos, &out_pos);

        struct vertex *out = &dst[(*dst_idx)++];
        out->pos = (vec3_t){out_pos.x, out_pos.y, out_pos.z};
        out->uv = curr->uv;
        out->material_idx = curr->material_idx;
        out->normal = transform_normal_with_mat4(&model_copy, local_normal);
    }

    return true;
}

static void render_batched_stat_entities(const vec_rstat_t *ents)
{
    vec_rstat_t *mutable_ents = (vec_rstat_t *)ents;
    if(!mutable_ents)
        return;

    size_t nents = vec_size(mutable_ents);
    bool *consumed = calloc(nents, sizeof(*consumed));
    if(!consumed) {
        for(int i = 0; i < nents; i++) {
            const struct ent_stat_rstate *curr = &vec_AT(mutable_ents, i);
            const struct render_private *priv = curr->render_private;
            if(!priv || !priv->metal_is_static_mesh || priv->uses_pose_buffer)
                continue;
            render_static_mesh_draw(priv, &curr->model, curr->translucent);
        }
        return;
    }

    for(int i = 0; i < nents; i++) {
        if(consumed[i])
            continue;

        const struct ent_stat_rstate *curr = &vec_AT(mutable_ents, i);
        const struct render_private *priv = curr->render_private;
        if(!priv || !priv->metal_is_static_mesh || priv->uses_pose_buffer)
            continue;

        if(curr->translucent && !priv->metal_static_verts) {
            consumed[i] = true;
            render_static_mesh_draw(priv, &curr->model, true);
            continue;
        }

        if(curr->translucent) {
            size_t total_verts = priv->mesh.num_verts;
            int j = i + 1;
            while(j < nents) {
                const struct ent_stat_rstate *other = &vec_AT(mutable_ents, j);
                if(!other->translucent || other->render_private != priv)
                    break;
                total_verts += priv->mesh.num_verts;
                j++;
            }

            if(j == i + 1) {
                consumed[i] = true;
                render_static_mesh_draw(priv, &curr->model, true);
                continue;
            }

            struct vertex *combined = malloc(total_verts * sizeof(*combined));
            if(!combined) {
                consumed[i] = true;
                render_static_mesh_draw(priv, &curr->model, true);
                continue;
            }

            size_t dst_idx = 0;
            for(int k = i; k < j; k++) {
                const struct ent_stat_rstate *other = &vec_AT(mutable_ents, k);
                consumed[k] = true;
                append_transformed_static_mesh(priv, &other->model, combined, &dst_idx);
            }

            mat4x4_t identity;
            PFM_Mat4x4_Identity(&identity);
            render_static_vertex_stream(priv, &identity, matrix_from_pf_mat4(&identity), combined, dst_idx * sizeof(*combined), true);
            free(combined);
            continue;
        }

        if(!priv->metal_static_verts || !priv->mesh.num_verts) {
            consumed[i] = true;
            render_static_mesh_draw(priv, &curr->model, false);
            continue;
        }

        size_t group_count = 1;
        size_t total_verts = priv->mesh.num_verts;
        consumed[i] = true;

        for(int j = i + 1; j < nents; j++) {
            if(consumed[j])
                continue;

            const struct ent_stat_rstate *other = &vec_AT(mutable_ents, j);
            if(other->translucent || other->render_private != priv)
                continue;

            consumed[j] = true;
            group_count++;
            total_verts += priv->mesh.num_verts;
        }

        if(group_count == 1) {
            render_static_mesh_draw(priv, &curr->model, false);
            continue;
        }

        struct vertex *combined = malloc(total_verts * sizeof(*combined));
        if(!combined) {
            render_static_mesh_draw(priv, &curr->model, false);
            for(int j = i + 1; j < nents; j++) {
                const struct ent_stat_rstate *other = &vec_AT(mutable_ents, j);
                if(other->translucent || other->render_private != priv)
                    continue;
                render_static_mesh_draw(priv, &other->model, false);
            }
            continue;
        }

        size_t dst_idx = 0;
        append_transformed_static_mesh(priv, &curr->model, combined, &dst_idx);
        for(int j = i + 1; j < nents; j++) {
            const struct ent_stat_rstate *other = &vec_AT(mutable_ents, j);
            if(other->translucent || other->render_private != priv)
                continue;
            append_transformed_static_mesh(priv, &other->model, combined, &dst_idx);
        }

        mat4x4_t identity;
        PFM_Mat4x4_Identity(&identity);
        render_static_vertex_stream(priv, &identity, matrix_from_pf_mat4(&identity), combined, dst_idx * sizeof(*combined), false);
        free(combined);
    }

    free(consumed);
}

static void render_batched_anim_entities(const vec_ranim_t *ents)
{
    vec_ranim_t *mutable_ents = (vec_ranim_t *)ents;
    if(!mutable_ents)
        return;

    size_t nents = vec_size(mutable_ents);
    bool *consumed = calloc(nents, sizeof(*consumed));
    if(!consumed) {
        for(int i = 0; i < nents; i++) {
            const struct ent_anim_rstate *curr = &vec_AT(mutable_ents, i);
            const struct render_private *priv = curr->render_private;
            if(!priv || !priv->metal_is_anim_mesh || !priv->uses_pose_buffer)
                continue;
            set_current_anim_uid(curr->uid);
            render_skinned_mesh_draw(priv, &curr->model, curr->translucent);
        }
        return;
    }

    for(int i = 0; i < nents; i++) {
        if(consumed[i])
            continue;

        const struct ent_anim_rstate *curr = &vec_AT(mutable_ents, i);
        const struct render_private *priv = curr->render_private;
        if(!priv || !priv->metal_is_anim_mesh || !priv->uses_pose_buffer)
            continue;

        if(curr->translucent && !priv->metal_anim_verts) {
            consumed[i] = true;
            set_current_anim_uid(curr->uid);
            render_skinned_mesh_draw(priv, &curr->model, true);
            continue;
        }

        if(curr->translucent) {
            size_t total_verts = priv->mesh.num_verts;
            int j = i + 1;
            while(j < nents) {
                const struct ent_anim_rstate *other = &vec_AT(mutable_ents, j);
                if(!other->translucent || other->render_private != priv)
                    break;
                total_verts += priv->mesh.num_verts;
                j++;
            }

            if(j == i + 1) {
                consumed[i] = true;
                set_current_anim_uid(curr->uid);
                render_skinned_mesh_draw(priv, &curr->model, true);
                continue;
            }

            struct vertex *combined = malloc(total_verts * sizeof(*combined));
            if(!combined) {
                consumed[i] = true;
                set_current_anim_uid(curr->uid);
                render_skinned_mesh_draw(priv, &curr->model, true);
                continue;
            }

            bool ok = true;
            size_t dst_idx = 0;
            for(int k = i; ok && k < j; k++) {
                const struct ent_anim_rstate *other = &vec_AT(mutable_ents, k);
                consumed[k] = true;
                ok = append_skinned_anim_mesh(priv, other->uid, &other->model, combined, &dst_idx);
            }

            if(!ok) {
                free(combined);
                for(int k = i; k < j; k++) {
                    const struct ent_anim_rstate *other = &vec_AT(mutable_ents, k);
                    set_current_anim_uid(other->uid);
                    render_skinned_mesh_draw(priv, &other->model, true);
                }
                continue;
            }

            mat4x4_t identity;
            PFM_Mat4x4_Identity(&identity);
            render_static_vertex_stream(priv, &identity, matrix_from_pf_mat4(&identity), combined, dst_idx * sizeof(*combined), true);
            free(combined);
            continue;
        }

        if(!priv->metal_anim_verts || !priv->mesh.num_verts) {
            consumed[i] = true;
            set_current_anim_uid(curr->uid);
            render_skinned_mesh_draw(priv, &curr->model, false);
            continue;
        }

        size_t group_count = 1;
        size_t total_verts = priv->mesh.num_verts;
        consumed[i] = true;

        for(int j = i + 1; j < nents; j++) {
            if(consumed[j])
                continue;

            const struct ent_anim_rstate *other = &vec_AT(mutable_ents, j);
            if(other->translucent || other->render_private != priv)
                continue;

            consumed[j] = true;
            group_count++;
            total_verts += priv->mesh.num_verts;
        }

        if(group_count == 1) {
            set_current_anim_uid(curr->uid);
            render_skinned_mesh_draw(priv, &curr->model, false);
            continue;
        }

        struct vertex *combined = malloc(total_verts * sizeof(*combined));
        if(!combined) {
            set_current_anim_uid(curr->uid);
            render_skinned_mesh_draw(priv, &curr->model, false);
            for(int j = i + 1; j < nents; j++) {
                const struct ent_anim_rstate *other = &vec_AT(mutable_ents, j);
                if(other->translucent || other->render_private != priv)
                    continue;
                set_current_anim_uid(other->uid);
                render_skinned_mesh_draw(priv, &other->model, false);
            }
            continue;
        }

        bool ok = true;
        size_t dst_idx = 0;
        ok = append_skinned_anim_mesh(priv, curr->uid, &curr->model, combined, &dst_idx);
        for(int j = i + 1; ok && j < nents; j++) {
            const struct ent_anim_rstate *other = &vec_AT(mutable_ents, j);
            if(other->translucent || other->render_private != priv)
                continue;
            ok = append_skinned_anim_mesh(priv, other->uid, &other->model, combined, &dst_idx);
        }

        if(!ok) {
            free(combined);
            set_current_anim_uid(curr->uid);
            render_skinned_mesh_draw(priv, &curr->model, false);
            for(int j = i + 1; j < nents; j++) {
                const struct ent_anim_rstate *other = &vec_AT(mutable_ents, j);
                if(other->translucent || other->render_private != priv)
                    continue;
                set_current_anim_uid(other->uid);
                render_skinned_mesh_draw(priv, &other->model, false);
            }
            continue;
        }

        mat4x4_t identity;
        PFM_Mat4x4_Identity(&identity);
        render_static_vertex_stream(priv, &identity, matrix_from_pf_mat4(&identity), combined, dst_idx * sizeof(*combined), false);
        free(combined);
    }

    free(consumed);
}

static void dispatch_or_drop_cmd(struct rcmd cmd)
{
    if(cmd.func == R_Cmd_BeginFrame) {
        R_Metal_FrameBegin();
        return;
    }
    if(cmd.func == R_Cmd_EndFrame) {
        R_Metal_FrameEnd();
        return;
    }
    if(cmd.func == R_Cmd_SetScreenspaceDrawMode
    || cmd.func == R_Cmd_DrawLoadingScreen) {
        return;
    }
    if(cmd.func == R_Cmd_SetViewMatAndPos) {
        s_scene_view = matrix_from_pf_mat4(cmd.args[0]);
        if(cmd.args[1]) {
            const vec3_t *pos = cmd.args[1];
            s_scene_view_pos = (vector_float3){pos->x, pos->y, pos->z};
        }
        s_have_scene_view = true;
        return;
    }
    if(cmd.func == R_Cmd_SetProj) {
        s_scene_proj = matrix_from_pf_mat4(cmd.args[0]);
        s_have_scene_proj = true;
        return;
    }
    if(cmd.func == R_Cmd_SetAmbientLightColor) {
        const vec3_t *color = cmd.args[0];
        if(color)
            s_light_ambient = (vector_float3){color->x, color->y, color->z};
        return;
    }
    if(cmd.func == R_Cmd_SetLightEmitColor) {
        const vec3_t *color = cmd.args[0];
        if(color)
            s_light_color = (vector_float3){color->x, color->y, color->z};
        return;
    }
    if(cmd.func == R_Cmd_SetLightPos) {
        const vec3_t *pos = cmd.args[0];
        if(pos)
            s_light_pos = (vector_float3){pos->x, pos->y, pos->z};
        return;
    }
    if(cmd.func == R_Cmd_AnimSetUniforms) {
        const uint32_t *uid = cmd.args[2];
        if(uid)
            set_current_anim_uid(*uid);
        if(cmd.args[0]) {
            s_curr_anim_normal_transform = matrix_from_pf_mat4(cmd.args[0]);
            s_have_anim_normal_transform = true;
        }
        return;
    }
    if(cmd.func == R_Cmd_MapInit) {
        update_terrain_textures(cmd.args[0], cmd.args[1]);
        ensure_heightmap_buffer();
        ensure_splatmap_buffer();
        ensure_splat_indices_buffer();
        return;
    }
    if(cmd.func == R_Cmd_WaterInit) {
        init_water_resources();
        return;
    }
    if(cmd.func == R_Cmd_WaterShutdown) {
        s_water_dudv_texture = nil;
        s_water_normal_texture = nil;
        return;
    }
    if(cmd.func == R_Cmd_SkyboxLoad) {
        load_metal_skybox(cmd.args[0], cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_SkyboxFree) {
        free_metal_skybox();
        return;
    }
    if(cmd.func == R_Cmd_DrawSkybox) {
        render_metal_skybox(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_DrawWater) {
        render_water_surface(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_MapBegin) {
        const bool *shadows = cmd.args[0];
        const vec2_t *pos = cmd.args[1];
        const size_t *num_splats = cmd.args[2];
        const struct splatmap *splatmap = cmd.args[3];
        const struct map_resolution *res = cmd.args[4];
        const struct map *map = cmd.args[5];
        s_shadows_enabled = shadows ? *shadows : false;
        s_map_pos = (vector_float2){pos->x, pos->y};
        s_map_tile_world_size = (vector_float2){
            res->field_w / res->tile_w,
            res->field_h / res->tile_h,
        };
        s_map_chunk_size = (vector_uint2){
            (uint32_t)res->chunk_w,
            (uint32_t)res->chunk_h,
        };
        s_map_tiles_per_chunk = (vector_uint2){
            (uint32_t)res->tile_w,
            (uint32_t)res->tile_h,
        };
        update_splat_indices(num_splats, splatmap);
        update_water_mask(map, res);
        return;
    }
    if(cmd.func == R_Cmd_MapEnd) {
        return;
    }
    if(cmd.func == R_Cmd_MapShutdown) {
        s_terrain_texture_array = nil;
        s_terrain_texture_count = 0;
        s_water_buffer = nil;
        s_fog_buffer = nil;
        s_heightmap_buffer = nil;
        s_splatmap_buffer = nil;
        s_splat_indices_buffer = nil;
        return;
    }
    if(cmd.func == R_Cmd_MapUpdateFog) {
        update_fog_texture(cmd.args[0], cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_TilePatchVertsBlend
    || cmd.func == R_Cmd_TilePatchVertsSmooth
    || cmd.func == R_Cmd_TileUpdate) {
        struct render_private *priv = cmd.args[0];
        if(cmd.func == R_Cmd_TilePatchVertsBlend) {
            R_TilePatchVertsBlend_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        }else if(cmd.func == R_Cmd_TilePatchVertsSmooth) {
            R_TilePatchVertsSmooth_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        }else{
            R_TileUpdate_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        }
        if(priv)
            release_retained_metal_object(&priv->metal_terrain_vertex_buffer);
        return;
    }
    if(cmd.func == R_Cmd_SetShadowsEnabled) {
        s_shadows_enabled = cmd.args[0] ? *(const bool *)cmd.args[0] : false;
        return;
    }
    if(cmd.func == R_Cmd_DepthPassBegin) {
        shadow_pass_begin(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_DepthPassEnd) {
        shadow_pass_end();
        return;
    }
    if(cmd.func == R_Cmd_RenderDepthMap) {
        const struct render_private *priv = cmd.args[0];
        uint32_t owner_uid = METAL_SHADOW_OWNER_UNKNOWN_UID;
        if(priv && priv->metal_is_terrain)
            owner_uid = METAL_SHADOW_OWNER_TERRAIN_UID;
        else if(priv && priv->metal_is_anim_mesh && s_have_anim_uid)
            owner_uid = s_curr_anim_uid;
        render_shadow_depth_draw(priv, cmd.args[1], owner_uid);
        return;
    }
    if(cmd.func == R_Cmd_Batch_AllocChunks
    || cmd.func == R_Cmd_Batch_Reset) {
        return;
    }
    if(cmd.func == R_Cmd_Batch_RenderDepthMap) {
        const struct render_input *in = cmd.args[0];
        if(!getenv("PF_SHADOW_SKIP_ANIM"))
            render_shadow_batched_anim_entities(&in->light_vis_anim);
        if(!getenv("PF_SHADOW_SKIP_STATIC"))
            render_shadow_batched_stat_entities(&in->light_vis_stat);
        return;
    }
    if(cmd.func == R_Cmd_Batch_Draw) {
        const struct render_input *in = cmd.args[0];
        render_batched_anim_entities(&in->cam_vis_anim);
        render_batched_stat_entities(&in->cam_vis_stat);
        return;
    }
    if(cmd.func == R_Cmd_Batch_DrawWithID) {
        const struct render_input *in = cmd.args[0];
        render_batched_anim_entities(&in->cam_vis_anim);
        render_batched_stat_entities(&in->cam_vis_stat);
        return;
    }
    if(cmd.func == R_Cmd_Draw) {
        const struct render_private *priv = cmd.args[0];
        bool translucent = cmd.args[2] ? *(const bool *)cmd.args[2] : false;
        if(priv && priv->metal_is_terrain) {
            render_terrain_draw(priv, cmd.args[1]);
            return;
        }
        if(priv && priv->metal_is_static_mesh && !priv->uses_pose_buffer) {
            render_static_mesh_draw(priv, cmd.args[1], translucent);
            return;
        }
        if(priv && priv->metal_is_anim_mesh && priv->uses_pose_buffer) {
            render_skinned_mesh_draw(priv, cmd.args[1], translucent);
        }
        return;
    }
    if(cmd.func == R_Cmd_DrawSelectionCircle) {
        render_selection_circle(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_DrawSelectionRectangle) {
        render_selection_rectangle(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
        return;
    }
    if(cmd.func == R_Cmd_DrawLine) {
        render_world_line(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
        return;
    }
    if(cmd.func == R_Cmd_DrawQuad) {
        render_world_quad(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
        return;
    }
    if(cmd.func == R_Cmd_DrawFlowField) {
        render_flow_field(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_DrawCombinedHRVO) {
        render_combined_hrvo(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_DrawOrigin) {
        render_debug_origin(cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_DrawRay) {
        render_debug_ray(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_DrawOBB) {
        render_debug_obb(cmd.args[0], cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_DrawBox2D) {
        render_box2d(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
        return;
    }
    if(cmd.func == R_Cmd_DrawHealthbars) {
        render_healthbars(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_DrawMapOverlayQuads) {
        render_map_overlay_quads(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4], cmd.args[5]);
        return;
    }
    if(cmd.func == R_Cmd_MinimapBake) {
        render_minimap_bake(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_MinimapUpdateChunk) {
        render_minimap_update_chunk(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_MinimapRender) {
        render_minimap(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_MinimapRenderUnits) {
        render_minimap_units(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4], cmd.args[5]);
        return;
    }
    if(cmd.func == R_Cmd_MinimapFree) {
        render_minimap_free();
        return;
    }
    if(cmd.func == R_Cmd_SpriteRenderBatch) {
        render_sprite_batch(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_UI_Init) {
        R_Metal_UI_Init();
        return;
    }
    if(cmd.func == R_Cmd_UI_Shutdown) {
        R_Metal_UI_Shutdown();
        return;
    }
    if(cmd.func == R_Cmd_UI_Render) {
        R_Metal_UI_Render(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_UI_UploadFontAtlas) {
        R_Metal_UI_UploadFontAtlas(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }

    if(cmd.func == R_Metal_Backend_CommandSetSwapInterval
    || cmd.func == R_Metal_Backend_CommandSetDebugLogMask
    || cmd.func == R_Metal_Backend_CommandSetTraceGPU) {
        render_dispatch_cmd(cmd);
        return;
    }
}

static void process_cmds(queue_rcmd_t *cmds)
{
    while(queue_size(*cmds) > 0) {
        struct rcmd curr;
        queue_rcmd_pop(cmds, &curr);
        dispatch_or_drop_cmd(curr);
    }
}

static bool render_init_ctx(struct render_init_arg *arg)
{
    (void)arg;

    s_device = MTLCreateSystemDefaultDevice();
    if(!s_device) {
        fprintf(stderr, "Failed to create Metal device.\n");
        return false;
    }

    s_queue = [s_device newCommandQueue];
    if(!s_queue) {
        fprintf(stderr, "Failed to create Metal command queue.\n");
        return false;
    }

    s_layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(s_metal_view);
    if(!s_layer) {
        fprintf(stderr, "Failed to acquire CAMetalLayer.\n");
        return false;
    }

    s_layer.device = s_device;
    s_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    s_layer.framebufferOnly = NO;
    s_layer.displaySyncEnabled = YES;
    s_layer.allowsNextDrawableTimeout = NO;
    if([s_layer respondsToSelector:@selector(setMaximumDrawableCount:)]) {
        s_layer.maximumDrawableCount = 3;
    }
    update_drawable_size();

    strncpy(s_info_vendor, "Apple", sizeof(s_info_vendor) - 1);
    const char *name = [[s_device name] UTF8String];
    strncpy(s_info_renderer, name ? name : "Metal Device", sizeof(s_info_renderer) - 1);
    strncpy(s_info_version, "Metal", sizeof(s_info_version) - 1);
    strncpy(s_info_sl_version, "MSL", sizeof(s_info_sl_version) - 1);
    strncpy(s_info_msaa_samples, "1", sizeof(s_info_msaa_samples) - 1);
    s_have_scene_view = false;
    s_have_scene_proj = false;
    s_scene_view_pos = (vector_float3){0.0f, 0.0f, 0.0f};
    s_light_ambient = (vector_float3){1.0f, 1.0f, 1.0f};
    s_light_color = (vector_float3){1.0f, 1.0f, 1.0f};
    s_light_pos = (vector_float3){1.0f, 1.0f, 1.0f};
    s_have_anim_uid = false;
    s_have_anim_normal_transform = false;
    mat4x4_t identity;
    PFM_Mat4x4_Identity(&identity);
    s_curr_anim_normal_transform = matrix_from_pf_mat4(&identity);
    s_shadow_pass_active = false;
    s_shadow_owner_pass_active = false;
    s_minimap_bake_pass_active = false;
    s_shadow_map_valid = false;
    s_shadows_enabled = false;
    s_frame_inflight_reserved = false;
    return true;
}

static void render_destroy_ctx(void)
{
    frame_abort();
    shadow_pass_end();
    release_scene_resources();
    release_ui_resources();
#if !OS_OBJECT_USE_OBJC
    if(s_inflight_semaphore)
        dispatch_release(s_inflight_semaphore);
#endif
    s_inflight_semaphore = NULL;
    s_layer = nil;
    s_queue = nil;
    s_device = nil;
    if(s_metal_view) {
        SDL_Metal_DestroyView(s_metal_view);
        s_metal_view = NULL;
    }
}

static int render(void *data)
{
    @autoreleasepool {
        s_rstate = (struct render_sync_state *)data;
        s_window = s_rstate->arg->in_window;

        Engine_SetRenderThreadID(SDL_ThreadID());

        bool quit = render_wait_cmd(s_rstate);
        assert(!quit);
        bool initialized = render_init_ctx(s_rstate->arg);
        s_rstate->arg->out_success = initialized;
        s_rstate->arg = NULL;
        render_signal_done(s_rstate, RSTAT_DONE);

        while(initialized) {
            @autoreleasepool {
                quit = render_wait_cmd(s_rstate);
                if(quit)
                    break;

                process_cmds(&G_GetRenderWS()->commands);
                if(s_rstate->swap_buffers || s_frame_command_buffer) {
                    frame_present();
                }

                render_signal_done(s_rstate, RSTAT_DONE);
            }
        }

        if(initialized)
            render_destroy_ctx();
    }
    return 0;
}

SDL_Thread *R_Metal_Backend_Run(struct render_sync_state *rstate)
{
    ASSERT_IN_MAIN_THREAD();

    s_metal_view = SDL_Metal_CreateView(rstate->arg->in_window);
    if(!s_metal_view) {
        fprintf(stderr, "SDL_Metal_CreateView failed: %s\n", SDL_GetError());
        return NULL;
    }

    return SDL_CreateThread(render, "render", rstate);
}

void R_Metal_Backend_InitAttributes(void)
{
}

bool R_Metal_Backend_ComputeShaderSupported(void)
{
    return false;
}

const char *R_Metal_Backend_GetInfo(enum render_info attr)
{
    switch(attr) {
    case RENDER_INFO_VENDOR:     return s_info_vendor;
    case RENDER_INFO_RENDERER:   return s_info_renderer;
    case RENDER_INFO_VERSION:    return s_info_version;
    case RENDER_INFO_SL_VERSION: return s_info_sl_version;
    case RENDER_INFO_BACKEND:    return "METAL";
    case RENDER_INFO_MSAA_SAMPLES: return render_info_msaa_samples();
    default: assert(0); return NULL;
    }
}

Uint32 R_Metal_Backend_WindowFlags(void)
{
    return SDL_WINDOW_METAL | SDL_WINDOW_SHOWN;
}

void R_Metal_Backend_WindowDrawableSize(SDL_Window *window, int *out_w, int *out_h)
{
    SDL_Metal_GetDrawableSize(window, out_w, out_h);
}

void R_Metal_Backend_PresentWindow(SDL_Window *window)
{
    (void)window;
    frame_present();
}

void R_Metal_Backend_Yield(void)
{
    ASSERT_IN_RENDER_THREAD();
    frame_present();
}

void R_Metal_Backend_CommandSetSwapInterval(const bool *on)
{
    (void)on;
}

void R_Metal_Backend_CommandSetDebugLogMask(const int *mask)
{
    (void)mask;
}

void R_Metal_Backend_CommandSetTraceGPU(const bool *on)
{
    g_trace_gpu = *on;
}

void R_Metal_Backend_DispatchCmd(struct rcmd cmd)
{
    dispatch_or_drop_cmd(cmd);
}

void R_Metal_FrameBegin(void)
{
    frame_begin();
}

void R_Metal_FrameEnd(void)
{
    frame_end();
}

void R_Metal_UI_Init(void)
{
    (void)ensure_ui_pipeline(false);
}

void R_Metal_UI_Shutdown(void)
{
    release_ui_resources();
}

void R_Metal_UI_UploadFontAtlas(void *image, const int *w, const int *h)
{
    if(!s_device)
        return;

    s_ui_font_texture = nil;

    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:*w
                                                                                    height:*h
                                                                                 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    s_ui_font_texture = [s_device newTextureWithDescriptor:desc];
    if(!s_ui_font_texture)
        return;

    MTLRegion region = {
        {0, 0, 0},
        {(NSUInteger)*w, (NSUInteger)*h, 1}
    };
    [s_ui_font_texture replaceRegion:region mipmapLevel:0 withBytes:image bytesPerRow:(NSUInteger)(*w * 4)];
}

void R_Metal_UI_Render(const struct nk_draw_list *dl)
{
    render_ui_draw_list(dl);
}
