#pragma once

#include <SDL2/SDL.h>
#include <chiaki/session.h>

typedef struct InputContext InputContext;

InputContext *input_init(void);
void input_fini(InputContext *ctx);

/* Controller hotplug/input and Magic Remote keys; call for every SDL event. */
void input_handle_event(InputContext *ctx, const SDL_Event *event);

/* Attach/detach the currently active Chiaki session. */
void input_set_session(InputContext *ctx, ChiakiSession *session);

/* Queue feedback from Chiaki's callback thread. */
void input_handle_session_event(InputContext *ctx, const ChiakiEvent *event);
ChiakiAudioSink input_make_haptics_sink(InputContext *ctx);

/* Run controller rumble/LED work on the SDL thread once per event-loop tick. */
void input_pump(InputContext *ctx);
