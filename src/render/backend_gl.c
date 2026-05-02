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

#include "backend_local.h"
#include "gl_shader.h"
#include "gl_texture.h"
#include "gl_render.h"
#include "gl_assert.h"
#include "gl_state.h"
#include "gl_batch.h"
#include "gl_anim.h"
#include "gl_swapchain.h"
#include "render_private.h"
#include "../main.h"
#include "../ui.h"
#include "../game/public/game.h"
#include "../lib/public/windows.h"
#include "gl_loader.h"

#include <assert.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>

#include <mimalloc-stats.h>


#define ARR_SIZE(a) (sizeof(a) / sizeof((a)[0]))

extern bool g_trace_gpu;

static SDL_GLContext             s_context;
static SDL_Window               *s_window;
static struct render_sync_state *s_rstate;

static jmp_buf                   s_jmpbuf;
static intptr_t                  s_jmpbuf2[5];

static char s_info_vendor[128];
static char s_info_renderer[128];
static char s_info_version[128];
static char s_info_sl_version[128];

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

static const char *source_str(GLenum source)
{
    switch(source) {
    case GL_DEBUG_SOURCE_API:             return "API";
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM:   return "Window System";
    case GL_DEBUG_SOURCE_SHADER_COMPILER: return "Shader Compiler";
    case GL_DEBUG_SOURCE_THIRD_PARTY:     return "Third Party";
    case GL_DEBUG_SOURCE_APPLICATION:     return "Application";
    case GL_DEBUG_SOURCE_OTHER:           return "Other";
    default: assert(0); return NULL;
    }
}

static const char *type_str(GLenum type)
{
    switch(type) {
    case GL_DEBUG_TYPE_ERROR:               return "Error";
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "Depricated Behavior";
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  return "Undefined Behavior";
    case GL_DEBUG_TYPE_PORTABILITY:         return "Portability";
    case GL_DEBUG_TYPE_PERFORMANCE:         return "Performance";
    case GL_DEBUG_TYPE_MARKER:              return "Marker";
    case GL_DEBUG_TYPE_PUSH_GROUP:          return "Push Group";
    case GL_DEBUG_TYPE_POP_GROUP:           return "Pop Group";
    case GL_DEBUG_TYPE_OTHER:               return "Other";
    default: assert(0); return NULL;
    }
}

static const char *severity_str(GLenum severity)
{
    switch(severity) {
    case GL_DEBUG_SEVERITY_HIGH:         return "High";
    case GL_DEBUG_SEVERITY_MEDIUM:       return "Medium";
    case GL_DEBUG_SEVERITY_LOW:          return "Low";
    case GL_DEBUG_SEVERITY_NOTIFICATION: return "Notification";
    default: assert(0); return NULL;
    }
}

static void debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity,
                           GLsizei length, const GLchar *message, const void *user)
{
    (void)id;
    (void)length;
    (void)user;
#ifdef _MSC_VER
    char buff[512];
    pf_snprintf(buff, sizeof(buff), " *** [%s][%s][%s] %s\n", source_str(source), type_str(type),
        severity_str(severity), message);
    OutputDebugString(buff);
#else
    fprintf(stderr, " *** [%s][%s][%s] %s\n", source_str(source), type_str(type),
        severity_str(severity), message);
    fflush(stderr);
#endif
}

static void render_dispatch_cmd(struct rcmd cmd)
{
    if(cmd.func == R_Cmd_Init) {
        R_GL_Init_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_BeginFrame) {
        R_GL_BeginFrame_Impl();
        return;
    }
    if(cmd.func == R_Cmd_EndFrame) {
        R_GL_EndFrame_Impl();
        return;
    }
    if(cmd.func == R_Cmd_SetViewMatAndPos) {
        R_GL_SetViewMatAndPos_Impl(cmd.args[0], cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_SetProj) {
        R_GL_SetProj_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_SetAmbientLightColor) {
        R_GL_SetAmbientLightColor_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_SetLightEmitColor) {
        R_GL_SetLightEmitColor_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_SetLightPos) {
        R_GL_SetLightPos_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_Draw) {
        R_GL_Draw_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_DepthPassBegin) {
        R_GL_DepthPassBegin_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_DepthPassEnd) {
        R_GL_DepthPassEnd_Impl();
        return;
    }
    if(cmd.func == R_Cmd_RenderDepthMap) {
        R_GL_RenderDepthMap_Impl(cmd.args[0], cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_SetShadowsEnabled) {
        R_GL_SetShadowsEnabled_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_Batch_Draw) {
        R_GL_Batch_Draw_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_Batch_DrawWithID) {
        R_GL_Batch_DrawWithID_Impl(cmd.args[0], cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_Batch_RenderDepthMap) {
        R_GL_Batch_RenderDepthMap_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_Batch_Reset) {
        R_GL_Batch_Reset_Impl();
        return;
    }
    if(cmd.func == R_Cmd_Batch_AllocChunks) {
        R_GL_Batch_AllocChunks_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_AnimAppendData) {
        R_GL_AnimAppendData_Impl(cmd.args[0], cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_AnimSetUniforms) {
        R_GL_AnimSetUniforms_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_SpriteRenderBatch) {
        R_GL_SpriteRenderBatch_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_UI_Init) {
        R_GL_UI_Init_Impl();
        return;
    }
    if(cmd.func == R_Cmd_UI_Shutdown) {
        R_GL_UI_Shutdown_Impl();
        return;
    }
    if(cmd.func == R_Cmd_UI_Render) {
        R_GL_UI_Render_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_UI_UploadFontAtlas) {
        R_GL_UI_UploadFontAtlas_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_TileDrawSelected) {
        R_GL_TileDrawSelected_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_TilePatchVertsBlend) {
        R_TilePatchVertsBlend_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_TilePatchVertsSmooth) {
        R_TilePatchVertsSmooth_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_TileUpdate) {
        R_TileUpdate_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_MapInit) {
        R_GL_MapInit_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_MapShutdown) {
        R_GL_MapShutdown_Impl();
        return;
    }
    if(cmd.func == R_Cmd_MapBegin) {
        R_GL_MapBegin_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4], cmd.args[5]);
        return;
    }
    if(cmd.func == R_Cmd_MapEnd) {
        R_GL_MapEnd_Impl();
        return;
    }
    if(cmd.func == R_Cmd_MapUpdateFog) {
        R_GL_MapUpdateFog_Impl(cmd.args[0], cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_MapInvalidate) {
        R_GL_MapInvalidate_Impl();
        return;
    }
    if(cmd.func == R_Cmd_Texture_GetOrLoad) {
        R_GL_Texture_GetOrLoad_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_PositionsUploadData) {
        R_GL_PositionsUploadData_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
        return;
    }
    if(cmd.func == R_Cmd_PositionsInvalidateData) {
        R_GL_PositionsInvalidateData_Impl();
        return;
    }
    if(cmd.func == R_Cmd_MoveUpdateUniforms) {
        R_GL_MoveUpdateUniforms_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
        return;
    }
    if(cmd.func == R_Cmd_MoveUploadData) {
        R_GL_MoveUploadData_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4],
                                 cmd.args[5], cmd.args[6], cmd.args[7], cmd.args[8], cmd.args[9]);
        return;
    }
    if(cmd.func == R_Cmd_MoveInvalidateData) {
        R_GL_MoveInvalidateData_Impl();
        return;
    }
    if(cmd.func == R_Cmd_MoveDispatchWork) {
        R_GL_MoveDispatchWork_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_MoveReadNewVelocities) {
        R_GL_MoveReadNewVelocities_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_MovePollCompletion) {
        R_GL_MovePollCompletion_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_MoveClearState) {
        R_GL_MoveClearState_Impl();
        return;
    }
    if(cmd.func == R_Cmd_WaterInit) {
        R_GL_WaterInit_Impl();
        return;
    }
    if(cmd.func == R_Cmd_WaterShutdown) {
        R_GL_WaterShutdown_Impl();
        return;
    }
    if(cmd.func == R_Cmd_SkyboxLoad) {
        R_GL_SkyboxLoad_Impl(cmd.args[0], cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_SkyboxFree) {
        R_GL_SkyboxFree_Impl();
        return;
    }
    if(cmd.func == R_Cmd_DrawSkybox) {
        R_GL_DrawSkybox_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_DrawWater) {
        R_GL_DrawWater_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_MinimapBake) {
        R_GL_MinimapBake_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_MinimapUpdateChunk) {
        R_GL_MinimapUpdateChunk_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_MinimapRender) {
        R_GL_MinimapRender_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_MinimapRenderUnits) {
        R_GL_MinimapRenderUnits_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4], cmd.args[5]);
        return;
    }
    if(cmd.func == R_Cmd_MinimapFree) {
        R_GL_MinimapFree_Impl();
        return;
    }
    if(cmd.func == R_Cmd_SetScreenspaceDrawMode) {
        R_GL_SetScreenspaceDrawMode_Impl();
        return;
    }
    if(cmd.func == R_Cmd_DrawLoadingScreen) {
        R_GL_DrawLoadingScreen_Impl(cmd.args[0]);
        return;
    }
    if(cmd.func == R_Cmd_DrawBox2D) {
        R_GL_DrawBox2D_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
        return;
    }
    if(cmd.func == R_Cmd_DrawLine) {
        R_GL_DrawLine_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
        return;
    }
    if(cmd.func == R_Cmd_DrawQuad) {
        R_GL_DrawQuad_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
        return;
    }
    if(cmd.func == R_Cmd_DrawOrigin) {
        R_GL_DrawOrigin_Impl(cmd.args[0], cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_DrawRay) {
        R_GL_DrawRay_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_DrawOBB) {
        R_GL_DrawOBB_Impl(cmd.args[0], cmd.args[1]);
        return;
    }
    if(cmd.func == R_Cmd_DrawSelectionCircle) {
        R_GL_DrawSelectionCircle_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_DrawSelectionRectangle) {
        R_GL_DrawSelectionRectangle_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
        return;
    }
    if(cmd.func == R_Cmd_DrawMapOverlayQuads) {
        R_GL_DrawMapOverlayQuads_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4], cmd.args[5]);
        return;
    }
    if(cmd.func == R_Cmd_DrawFlowField) {
        R_GL_DrawFlowField_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_DrawCombinedHRVO) {
        R_GL_DrawCombinedHRVO_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_DrawHealthbars) {
        R_GL_DrawHealthbars_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3], cmd.args[4]);
        return;
    }
    if(cmd.func == R_Cmd_DrawSkeleton) {
        R_GL_DrawSkeleton_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }
    if(cmd.func == R_Cmd_DrawModelToTexture) {
        R_GL_DrawModelToTexture_Impl(cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
        return;
    }
    if(cmd.func == R_Cmd_DrawNormals) {
        R_GL_DrawNormals_Impl(cmd.args[0], cmd.args[1], cmd.args[2]);
        return;
    }

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

static void yield_maybe(uint32_t *timestamp)
{
    if(!Engine_InRunningState()) {
        uint32_t now = SDL_GetTicks();
        if(SDL_TICKS_PASSED(now, *timestamp + 100)) {
            R_GL_Backend_Yield();
            *timestamp = SDL_GetTicks();
        }
    }
}

static void render_process_cmds(queue_rcmd_t *cmds)
{
    uint32_t start = SDL_GetTicks();
    while(queue_size(*cmds) > 0) {
        struct rcmd curr;
        queue_rcmd_pop(cmds, &curr);
        render_dispatch_cmd(curr);
        GL_ASSERT_OK();
        yield_maybe(&start);
    }
}

static void render_init_ctx(struct render_init_arg *arg)
{
    if(SDL_GL_MakeCurrent(arg->in_window, s_context) != 0) {
        fprintf(stderr, "SDL_GL_MakeCurrent failed in render_init_ctx: %s\n", SDL_GetError());
        arg->out_success = false;
        return;
    }

    if(!PFGL_Load()) {
        fprintf(stderr, "Failed to initialize OpenGL loader\n");
        arg->out_success = false;
        return;
    }

    if(!PFGL_HasVersion33()) {
        fprintf(stderr, "Required OpenGL 3.3 support is unavailable\n");
        arg->out_success = false;
        return;
    }

    if(PFGL_KHRDebugSupported()
#if defined(__APPLE__) && defined(__aarch64__)
    && false
#endif
    ) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(debug_callback, NULL);
    }

    int vp[4] = {0, 0, arg->in_width, arg->in_height};
    R_GL_SetViewport(&vp[0], &vp[1], &vp[2], &vp[3]);
    R_GL_GlobalConfig();

    if(!R_GL_Shader_InitAll(g_basepath)
    || !R_GL_Texture_Init()
    || !R_GL_StateInit()
    || !R_GL_Batch_Init()
    || !R_GL_AnimInit()
    || !R_GL_SwapchainInit()) {
        arg->out_success = false;
        return;
    }

    R_GL_InitShadows();

    strncpy(s_info_vendor, (const char*)glGetString(GL_VENDOR), ARR_SIZE(s_info_vendor) - 1);
    strncpy(s_info_renderer, (const char*)glGetString(GL_RENDERER), ARR_SIZE(s_info_renderer) - 1);
    strncpy(s_info_version, (const char*)glGetString(GL_VERSION), ARR_SIZE(s_info_version) - 1);
    strncpy(s_info_sl_version, (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION), ARR_SIZE(s_info_sl_version) - 1);

    arg->out_success = true;
}

static void render_destroy_ctx(void)
{
    R_GL_SwapchainShutdown();
    R_GL_AnimShutdown();
    R_GL_Batch_Shutdown();
    R_GL_StateShutdown();
    R_GL_Texture_Shutdown();
    SDL_GL_DeleteContext(s_context);
    s_context = NULL;
}

static int render(void *data)
{
    s_rstate = data;
    s_window = s_rstate->arg->in_window;

    Engine_SetRenderThreadID(SDL_ThreadID());
    SDL_GL_MakeCurrent(s_window, s_context);

    bool quit = render_wait_cmd(s_rstate);
    assert(!quit);
    render_init_ctx(s_rstate->arg);
    bool initialized = s_rstate->arg->out_success;

    s_rstate->arg = NULL;
    render_signal_done(s_rstate, RSTAT_DONE);

#ifdef __MINGW32__
    if(__builtin_setjmp(s_jmpbuf2))
        return 0;
#else
    if(setjmp(s_jmpbuf))
        return 0;
#endif

    while(true) {
        quit = render_wait_cmd(s_rstate);
        if(quit)
            break;

        render_process_cmds(&G_GetRenderWS()->commands);
        if(s_rstate->swap_buffers) {
            R_Cmd_SwapchainPresentLast();
#if defined(__APPLE__) && defined(__aarch64__)
            render_signal_done(s_rstate, RSTAT_YIELD);
#endif
            SDL_GL_SwapWindow(s_window);
        }

#if MI_STAT_VERSION < 5
        mi_heap_stats_merge_to_subproc(mi_heap_get_default());
#endif
        render_signal_done(s_rstate, RSTAT_DONE);
    }

    if(initialized)
        render_destroy_ctx();
    return 0;
}

SDL_Thread *R_GL_Backend_Run(struct render_sync_state *rstate)
{
    ASSERT_IN_MAIN_THREAD();

    s_context = SDL_GL_CreateContext(rstate->arg->in_window);
    if(!s_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return NULL;
    }
    if(SDL_GL_MakeCurrent(rstate->arg->in_window, NULL) != 0) {
        fprintf(stderr, "SDL_GL_MakeCurrent release failed: %s\n", SDL_GetError());
        SDL_GL_DeleteContext(s_context);
        s_context = NULL;
        return NULL;
    }

    return SDL_CreateThread(render, "render", rstate);
}

void R_GL_Backend_InitAttributes(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);

    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

#ifndef NDEBUG
#if !defined(__APPLE__) || !defined(__aarch64__)
    int ctx_flags = 0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_FLAGS, &ctx_flags);
    ctx_flags |= SDL_GL_CONTEXT_DEBUG_FLAG;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, ctx_flags);
#endif
#endif
}

bool R_GL_Backend_ComputeShaderSupported(void)
{
    return PFGL_ComputeShaderSupported();
}

const char *R_GL_Backend_GetInfo(enum render_info attr)
{
    switch(attr) {
    case RENDER_INFO_VENDOR:     return s_info_vendor;
    case RENDER_INFO_RENDERER:   return s_info_renderer;
    case RENDER_INFO_VERSION:    return s_info_version;
    case RENDER_INFO_SL_VERSION: return s_info_sl_version;
    case RENDER_INFO_BACKEND:    return "OPENGL";
    case RENDER_INFO_MSAA_SAMPLES: return "1";
    default: assert(0); return NULL;
    }
}

Uint32 R_GL_Backend_WindowFlags(void)
{
    return SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
}

void R_GL_Backend_WindowDrawableSize(SDL_Window *window, int *out_w, int *out_h)
{
    SDL_GL_GetDrawableSize(window, out_w, out_h);
}

void R_GL_Backend_PresentWindow(SDL_Window *window)
{
    SDL_GL_SwapWindow(window);
}

void R_GL_Backend_Yield(void)
{
    ASSERT_IN_RENDER_THREAD();

    if(Engine_InRunningState())
        return;

    struct render_workspace *ws = G_GetRenderWS();
    size_t left = queue_size(ws->commands);

    const char *stack[32] = {0};
    size_t nitems = 0;
    do{
        if(nitems >= ARR_SIZE(stack))
            break;
        Perf_Pop(&stack[nitems]);
    }while(stack[nitems++] != NULL);

#if defined(__APPLE__) && defined(__aarch64__)
    render_signal_done(s_rstate, RSTAT_YIELD);
#endif
    SDL_GL_SwapWindow(s_window);

#if !defined(__APPLE__) || !defined(__aarch64__)
    render_signal_done(s_rstate, RSTAT_YIELD);
#endif

    bool quit = render_wait_cmd(s_rstate);
    if(quit) {
#ifdef __MINGW32__
        __builtin_longjmp(&s_jmpbuf2, 1);
#else
        longjmp(s_jmpbuf, 1);
#endif
    }

    while(queue_size(ws->commands) > left) {
        struct rcmd curr = {0};
        queue_rcmd_pop(&ws->commands, &curr);
        render_dispatch_cmd(curr);
        GL_ASSERT_OK();
    }

    for(int i = (int)nitems - 2; i >= 0; i--) {
        Perf_Push(stack[i]);
    }
}

void R_GL_Backend_CommandSetSwapInterval(const bool *on)
{
    SDL_GL_SetSwapInterval(*on);
}

void R_GL_Backend_CommandSetDebugLogMask(const int *mask)
{
    if(!PFGL_KHRDebugSupported())
        return;

    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE,
        GL_DEBUG_SEVERITY_HIGH, 0, NULL, (*mask & 0x1) ? GL_TRUE : GL_FALSE);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE,
        GL_DEBUG_SEVERITY_MEDIUM, 0, NULL, (*mask & 0x2) ? GL_TRUE : GL_FALSE);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE,
        GL_DEBUG_SEVERITY_LOW, 0, NULL, (*mask & 0x4) ? GL_TRUE : GL_FALSE);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE,
        GL_DEBUG_SEVERITY_NOTIFICATION, 0, NULL, (*mask & 0x8) ? GL_TRUE : GL_FALSE);
}

void R_GL_Backend_CommandSetTraceGPU(const bool *on)
{
    g_trace_gpu = *on;
}

void R_GL_Backend_DispatchCmd(struct rcmd cmd)
{
    render_dispatch_cmd(cmd);
}
