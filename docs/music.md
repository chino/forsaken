# Music

The original Forsaken played its soundtrack as CD audio. This port plays it
from music files instead. The soundtrack is not included in the game data, so
you have to add your own copy.

## Adding the soundtrack

Put the tracks in `data/sound/music/` (make the folder if it isn't there).
The game reads Ogg Vorbis (`.ogg`), FLAC (`.flac`) and WAV (`.wav`, 8, 16 or 24
bit PCM). Mono or stereo, at any sample rate. Restart the game after adding
files.

* **Forsaken Remastered**: copy the `.ogg` files from its `music/OGG` folder
  into `data/sound/music/`. Then copy [`docs/playlist.txt`](playlist.txt) to
  `data/sound/music/playlist.txt`, which gives every level its track.
* **CD rip**: rip the audio tracks and name them by CD track number, e.g.
  `track02.flac`, `track03.flac`, ... (`Track 2.wav` works too). Track 1 on
  the game CD is the data track. Without a playlist the title screen plays the
  lowest numbered track and levels take turns with the rest.
* **Your own music**: any files will do. Without a playlist they are played
  in alphabetical order: the first one on the title screen, the others in turn
  for the levels.

If there are no music files the game runs silently, without errors.

## playlist.txt

`data/sound/music/playlist.txt` chooses which track plays where. Each line
has a key and a file name:

    # comment
    menu      01_Forsaken.ogg
    vol2      08_Flameout
    thermal   12
    *         02_Gargantuan.ogg

* The key is `menu` (title screen and menus between games), a level's folder
  name from `data/levels/` (e.g. `vol2`, `thermal`, `ship`), or `*`.
* The file is the rest of the line, so names with spaces are fine. Letter case
  and the extension are optional. A plain number `N` means CD track `N`
  (`trackNN.*`). Paths with a `/` are relative to `data/sound/music/`.
* Several lines with the same key are played one after the other, and then
  start again. A key with one line repeats that track.
* `*` lines are a pool for levels the playlist doesn't name (custom levels,
  for example). Each level gets one of them, chosen by its number.

When a level isn't in the playlist, the game looks for a file named after it
(`vol2.ogg`, `menu.flac`, ...), then uses the `*` pool, then the numbered or
alphabetical order described above.

## In game

* Set the volume with **music volume** in the sound options (title screen
  Options > Sound, or the in-game Sound FX and Music menu). It is saved with
  your other settings as `MusicVolume`.
* The track fades over when a level starts or the game returns to the menus.
  It pauses while a single player game is paused, but keeps playing in the
  sound menu so you can hear the volume change.
* `-nomusic` on the command line turns music off. `-nosfx` turns off all
  sound, music included.
* Run with `-debug` to have the log (lines beginning with `music:`) show which
  file was picked for each level and why a file was rejected.

## Building

Music is on by default (`MUSIC=1`). WAV support needs nothing extra. Ogg and
FLAC are added automatically when `pkg-config` finds `vorbisfile` and `flac`
(Debian/Ubuntu: `libvorbis-dev libflac-dev`). `make check` shows what was
found.

    make MUSIC=0                  # no music at all
    make MUSIC_FLAC=0             # leave out FLAC
    make MUSIC_OGG=1 MUSIC_FLAC=1 # force them on, e.g. cross compiling without
                                  # pkg-config (links -lvorbisfile -lvorbis -logg / -lFLAC)

When you link a static libFLAC on Windows, also pass
`ADD_CFLAGS=-DFLAC__NO_DLL`.

Playback goes through the OpenAL device that the sound effects use, on its own
source. A small thread keeps it fed, so music doesn't stop while a level loads.
