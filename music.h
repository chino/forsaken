#ifndef MUSIC_INCLUDED
#define MUSIC_INCLUDED

/*
 * Streamed soundtrack playback (replaces the original game's CD audio).
 *
 * Tracks are Ogg Vorbis, FLAC or WAV files in data/sound/music/, mapped to
 * levels by data/sound/music/playlist.txt (optional, see docs/music.md).
 * Playback uses its own OpenAL source on the device opened by sound_openal.c
 * and is fed from a small streaming thread, so level loads do not starve it.
 *
 * Everything here is optional: built with MUSIC=0, run with -nosfx/-nomusic,
 * with no OpenAL device or with no music files the calls below do nothing.
 */

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
