#pragma once

#include <SDL2/SDL.h>
#include <chiaki/log.h>
#include "config.h"
#include "input.h"

typedef enum {
    UI_RESULT_CONNECT,      // user chose to connect
    UI_RESULT_QUIT,         // user dismissed the setup/launcher screen
    UI_RESULT_ERROR,
} UIResult;

// Minimal fullscreen loading screen (SDL renderer).
// Call once per tick while you're doing blocking startup work.
// This draws, but does NOT call SDL_RenderPresent().
void ui_render_loading(SDL_Renderer *renderer, const char *base_text);

// Show the launcher/setup screen.
// Displayed on every launch. Lets the user:
//  - enter PS5 IP address
//  - import chiaki-ng config (chiaki-ng-Default.ini) into config.json
//  - connect to start the stream
UIResult ui_run_registration(SDL_Renderer *renderer, AppConfig *cfg,
                             const char *config_path, ChiakiLog *log,
                             const char *initial_message,
                             InputContext *input_ctx);

// Draw a semi-transparent stats overlay on top of the current frame.
// This draws, but does NOT call SDL_RenderPresent().
void ui_render_stats_overlay(SDL_Renderer *renderer, const char *text);

// Draw temporary pop up at launch for stats overlay

void ui_render_hint(SDL_Renderer *r, const char *text, float opacity);

// Compute the integer output-pixel scale for the current renderer (1 on a
// 1080p surface, 2 on a native 2160p surface, ...). Call once after renderer
// creation; scales all UI drawing primitives.
void ui_set_output_scale(SDL_Renderer *renderer);
