#ifndef MUSIC_INCLUDED
#define MUSIC_INCLUDED

// plays the cd soundtrack from ripped tracks, see docs/music.md

#ifdef MUSIC

#include <stdbool.h>

// set by -nomusic on the command line
extern bool music_disabled;

// once per main loop iteration: initialises lazily once OpenAL is up,
// picks the track for the current game state and handles pausing
void music_update( void );

// stops playback and frees the OpenAL source; call before sound_destroy()
void music_shutdown( void );

// 0.0 (silent) .. 1.0 (full), driven by the "music volume" slider
void music_set_volume( float volume );

#else // !MUSIC

#define music_update()       ((void)0)
#define music_shutdown()     ((void)0)
#define music_set_volume(v)  ((void)(v))

#endif // MUSIC

#endif // MUSIC_INCLUDED
