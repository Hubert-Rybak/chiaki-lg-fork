#pragma once

#include <SDL2/SDL.h>
#include <stdbool.h>

/*
 * Magic Remote keycodes observed with webosbrew/SDL-webOS. Back and Red are
 * bare webOS keycodes (no useful SDL scancode); older SDL builds can expose
 * Back as SDLK_AC_BACK and colour buttons as their raw IR values instead.
 */
#define WEBOS_KEY_BACK_RAW  ((SDL_Keycode)2097155)
#define WEBOS_KEY_RED_RAW   ((SDL_Keycode)2097169)
#define WEBOS_KEY_RED_IR    ((SDL_Keycode)403)
#define WEBOS_KEY_GREEN_IR  ((SDL_Keycode)404)
#define WEBOS_KEY_YELLOW_IR ((SDL_Keycode)405)
#define WEBOS_KEY_BLUE_IR   ((SDL_Keycode)406)
#define WEBOS_SCANCODE_EXIT ((SDL_Scancode)505)

static inline bool webos_key_is_back(SDL_Keycode key)
{
    return key == WEBOS_KEY_BACK_RAW || key == SDLK_AC_BACK || key == SDLK_ESCAPE;
}

static inline bool webos_key_is_red(SDL_Keycode key)
{
    return key == WEBOS_KEY_RED_RAW || key == WEBOS_KEY_RED_IR;
}

static inline bool webos_event_is_back_or_red(const SDL_KeyboardEvent *event)
{
    return event && (webos_key_is_back(event->keysym.sym) ||
                     webos_key_is_red(event->keysym.sym) ||
                     event->keysym.scancode == WEBOS_SCANCODE_EXIT);
}
