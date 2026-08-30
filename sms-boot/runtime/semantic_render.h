#pragma once

struct SDL_Window;

extern "C" {

bool sb_semantic_render_configure(void);
bool sb_semantic_render_initialize(SDL_Window* window);
bool sb_semantic_render_consume(void);
bool sb_semantic_render_validate(void);
bool sb_semantic_render_shutdown(void);
const char* sb_semantic_render_last_error(void);
}
