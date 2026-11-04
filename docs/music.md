# Music

Forsaken played its soundtrack as CD audio. This port plays it from ripped
tracks instead. They are not part of the game data.

Rip the audio tracks of the Forsaken CD into `data/sound/music/`, named by
track number: `track02.ogg`, `track03.flac`, `track07.wav`, ... (lower case;
Ogg Vorbis, FLAC or 8/16/24 bit PCM WAV). Track 1 is the data track.

The title screen plays track 7. Each level plays the track the original game
gave it. A level list (`data/levels/mission.dat`, `battle.dat`, ...) can give
a level another track by putting the number after its name:

    vol2 5
    mylevel 3

Levels without a number use their original track, or track 2 if they are not
one of the original levels. Tracks loop.

Missing tracks are simply silent. Set the volume with "music volume" in the
sound options; it is saved as `MusicVolume`. Music pauses while a single
player game is paused. `-nomusic` turns it off.

Build options: music is on by default (`MUSIC=0` to leave it out). WAV needs
no extra libraries; Ogg and FLAC are used when pkg-config finds `vorbisfile`
and `flac`, or force them with `MUSIC_OGG=1` / `MUSIC_FLAC=1`.
