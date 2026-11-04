#ifndef SDL2_SCANCODE_KEYS_INCLUDED
#define SDL2_SCANCODE_KEYS_INCLUDED

/*
 * Control bindings are looked up in key_state[], which ReadKeyboard() fills
 * from SDL_GetKeyboardState(). SDL 1.2 indexed that array by key code (SDLK_*),
 * SDL2 indexes it by scancode, and SDL2 key codes do not even fit the short
 * VirtualKeycode (SDLK_UP is 0x40000052). So under SDL2 the bindings have to
 * be scancodes.
 *
 * Include this only where the default control tables are written: it makes
 * the SDLK_* names used there expand to the matching SDL2 scancode, so the
 * tables read the same for both SDL versions. Code that compares against
 * key codes from input_buffer (menus etc) must NOT include it.
 */

#include <SDL.h>

#define SDLK_a              SDL_SCANCODE_A
#define SDLK_b              SDL_SCANCODE_B
#define SDLK_c              SDL_SCANCODE_C
#define SDLK_d              SDL_SCANCODE_D
#define SDLK_e              SDL_SCANCODE_E
#define SDLK_f              SDL_SCANCODE_F
#define SDLK_g              SDL_SCANCODE_G
#define SDLK_h              SDL_SCANCODE_H
#define SDLK_i              SDL_SCANCODE_I
#define SDLK_j              SDL_SCANCODE_J
#define SDLK_k              SDL_SCANCODE_K
#define SDLK_l              SDL_SCANCODE_L
#define SDLK_m              SDL_SCANCODE_M
#define SDLK_n              SDL_SCANCODE_N
#define SDLK_o              SDL_SCANCODE_O
#define SDLK_p              SDL_SCANCODE_P
#define SDLK_q              SDL_SCANCODE_Q
#define SDLK_r              SDL_SCANCODE_R
#define SDLK_s              SDL_SCANCODE_S
#define SDLK_t              SDL_SCANCODE_T
#define SDLK_u              SDL_SCANCODE_U
#define SDLK_v              SDL_SCANCODE_V
#define SDLK_w              SDL_SCANCODE_W
#define SDLK_x              SDL_SCANCODE_X
#define SDLK_y              SDL_SCANCODE_Y
#define SDLK_z              SDL_SCANCODE_Z
#define SDLK_0              SDL_SCANCODE_0
#define SDLK_1              SDL_SCANCODE_1
#define SDLK_2              SDL_SCANCODE_2
#define SDLK_3              SDL_SCANCODE_3
#define SDLK_4              SDL_SCANCODE_4
#define SDLK_5              SDL_SCANCODE_5
#define SDLK_6              SDL_SCANCODE_6
#define SDLK_7              SDL_SCANCODE_7
#define SDLK_8              SDL_SCANCODE_8
#define SDLK_9              SDL_SCANCODE_9
#define SDLK_RETURN         SDL_SCANCODE_RETURN
#define SDLK_ESCAPE         SDL_SCANCODE_ESCAPE
#define SDLK_BACKSPACE      SDL_SCANCODE_BACKSPACE
#define SDLK_TAB            SDL_SCANCODE_TAB
#define SDLK_SPACE          SDL_SCANCODE_SPACE
#define SDLK_MINUS          SDL_SCANCODE_MINUS
#define SDLK_EQUALS         SDL_SCANCODE_EQUALS
#define SDLK_LEFTBRACKET    SDL_SCANCODE_LEFTBRACKET
#define SDLK_RIGHTBRACKET   SDL_SCANCODE_RIGHTBRACKET
#define SDLK_BACKSLASH      SDL_SCANCODE_BACKSLASH
#define SDLK_SEMICOLON      SDL_SCANCODE_SEMICOLON
#define SDLK_COMMA          SDL_SCANCODE_COMMA
#define SDLK_PERIOD         SDL_SCANCODE_PERIOD
#define SDLK_SLASH          SDL_SCANCODE_SLASH
#define SDLK_CAPSLOCK       SDL_SCANCODE_CAPSLOCK
#define SDLK_F1             SDL_SCANCODE_F1
#define SDLK_F2             SDL_SCANCODE_F2
#define SDLK_F3             SDL_SCANCODE_F3
#define SDLK_F4             SDL_SCANCODE_F4
#define SDLK_F5             SDL_SCANCODE_F5
#define SDLK_F6             SDL_SCANCODE_F6
#define SDLK_F7             SDL_SCANCODE_F7
#define SDLK_F8             SDL_SCANCODE_F8
#define SDLK_F9             SDL_SCANCODE_F9
#define SDLK_F10            SDL_SCANCODE_F10
#define SDLK_F11            SDL_SCANCODE_F11
#define SDLK_F12            SDL_SCANCODE_F12
#define SDLK_PRINTSCREEN    SDL_SCANCODE_PRINTSCREEN
#define SDLK_SCROLLLOCK     SDL_SCANCODE_SCROLLLOCK
#define SDLK_PAUSE          SDL_SCANCODE_PAUSE
#define SDLK_INSERT         SDL_SCANCODE_INSERT
#define SDLK_HOME           SDL_SCANCODE_HOME
#define SDLK_PAGEUP         SDL_SCANCODE_PAGEUP
#define SDLK_DELETE         SDL_SCANCODE_DELETE
#define SDLK_END            SDL_SCANCODE_END
#define SDLK_PAGEDOWN       SDL_SCANCODE_PAGEDOWN
#define SDLK_RIGHT          SDL_SCANCODE_RIGHT
#define SDLK_LEFT           SDL_SCANCODE_LEFT
#define SDLK_DOWN           SDL_SCANCODE_DOWN
#define SDLK_UP             SDL_SCANCODE_UP
#define SDLK_NUMLOCKCLEAR   SDL_SCANCODE_NUMLOCKCLEAR
#define SDLK_KP_DIVIDE      SDL_SCANCODE_KP_DIVIDE
#define SDLK_KP_MULTIPLY    SDL_SCANCODE_KP_MULTIPLY
#define SDLK_KP_MINUS       SDL_SCANCODE_KP_MINUS
#define SDLK_KP_PLUS        SDL_SCANCODE_KP_PLUS
#define SDLK_KP_ENTER       SDL_SCANCODE_KP_ENTER
#define SDLK_KP_1           SDL_SCANCODE_KP_1
#define SDLK_KP_2           SDL_SCANCODE_KP_2
#define SDLK_KP_3           SDL_SCANCODE_KP_3
#define SDLK_KP_4           SDL_SCANCODE_KP_4
#define SDLK_KP_5           SDL_SCANCODE_KP_5
#define SDLK_KP_6           SDL_SCANCODE_KP_6
#define SDLK_KP_7           SDL_SCANCODE_KP_7
#define SDLK_KP_8           SDL_SCANCODE_KP_8
#define SDLK_KP_9           SDL_SCANCODE_KP_9
#define SDLK_KP_0           SDL_SCANCODE_KP_0
#define SDLK_KP_PERIOD      SDL_SCANCODE_KP_PERIOD
#define SDLK_KP_EQUALS      SDL_SCANCODE_KP_EQUALS
#define SDLK_LCTRL          SDL_SCANCODE_LCTRL
#define SDLK_LSHIFT         SDL_SCANCODE_LSHIFT
#define SDLK_LALT           SDL_SCANCODE_LALT
#define SDLK_LGUI           SDL_SCANCODE_LGUI
#define SDLK_RCTRL          SDL_SCANCODE_RCTRL
#define SDLK_RSHIFT         SDL_SCANCODE_RSHIFT
#define SDLK_RALT           SDL_SCANCODE_RALT
#define SDLK_RGUI           SDL_SCANCODE_RGUI
#define SDLK_QUOTE          SDL_SCANCODE_APOSTROPHE
#define SDLK_BACKQUOTE      SDL_SCANCODE_GRAVE

#endif
