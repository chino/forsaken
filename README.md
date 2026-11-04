# Forsaken

This repo holds the community port of Forsaken!

Check out the [Wiki](https://github.com/ForsakenX/forsaken/wiki) for more information.

## Flatpak

    flatpak-builder --user --install --force-clean build-dir \
        packaging/flatpak/io.github.ForsakenX.Forsaken.yml
    flatpak run io.github.ForsakenX.Forsaken

Pilots, settings and saved games are kept in
`~/.var/app/io.github.ForsakenX.Forsaken/data/forsaken`.

## Music

The soundtrack is played from the CD audio tracks ripped to
`data/sound/music/trackNN.ogg` (or `.flac` / `.wav`). See
[docs/music.md](docs/music.md).

## ARM, OpenGL ES and cross compiling

`make SDL=2 GLES=1` builds the GL1 renderer against OpenGL ES 1.1 instead
of desktop OpenGL, SDL creates the context (SDL 1.2 is not supported).

`make PANDORA=1` is the OpenPandora preset: SDL 2, GLES, 800x480
fullscreen, nub/shoulder controls.  On a Raspberry Pi just use `make SDL=2`
or `make SDL=2 GLES=1`.

To cross compile give the toolchain prefix and point pkg-config at the
target libraries, for example:

    PKG_CONFIG_SYSROOT_DIR=/path/to/sysroot \
    PKG_CONFIG_LIBDIR=/path/to/sysroot/usr/lib/aarch64-linux-gnu/pkgconfig \
    make CROSS=aarch64-linux-gnu- SDL=2 GLES=1

`$(CROSS)pkg-config` is used when it exists, `PKG_CONFIG=` overrides it.

## Virtual reality

Builds made where the OpenXR headers are installed can render to a VR headset
through OpenXR when started with `-vr`. See [docs/VR.md](docs/VR.md).
