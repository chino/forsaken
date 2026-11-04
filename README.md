# Forsaken

This repo holds the community port of Forsaken!

Check out the [Wiki](https://github.com/ForsakenX/forsaken/wiki) for more information.

## ARM, OpenGL ES and cross compiling

`make GLES=1` builds the GL1 renderer against OpenGL ES 1.1 instead of
desktop OpenGL (`SDL=2` lets SDL create the context, `SDL=1` uses EGL).

Board presets: `make PANDORA=1` (OpenPandora: SDL 1.2, GLES, 800x480
fullscreen, nub/shoulder controls) and `make RPI=1` (Raspberry Pi with the
legacy `/opt/vc` driver; on current Raspberry Pi OS just use `make SDL=2`
or `make SDL=2 GLES=1`).

To cross compile give the toolchain prefix and point pkg-config at the
target libraries, for example:

    PKG_CONFIG_SYSROOT_DIR=/path/to/sysroot \
    PKG_CONFIG_LIBDIR=/path/to/sysroot/usr/lib/aarch64-linux-gnu/pkgconfig \
    make CROSS=aarch64-linux-gnu- SDL=2 GLES=1

`$(CROSS)pkg-config` is used when it exists, `PKG_CONFIG=` overrides it.
