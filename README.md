# Forsaken

This repo holds the community port of Forsaken!

Check out the [Wiki](https://github.com/ForsakenX/forsaken/wiki) for more information.

## Building on Linux

Install the development packages (Ubuntu/Debian names):

    sudo apt-get install build-essential pkg-config libsdl2-dev libsdl1.2-dev \
        liblua5.1-0-dev lua-socket-dev libenet-dev libpng-dev zlib1g-dev \
        libopenal-dev libgl-dev libglu1-mesa-dev

Then `make` (SDL 1.2, OpenGL 1) or e.g. `make SDL=2 GL=3`.  The game expects
to run from a folder holding the [game data](https://github.com/ForsakenX/forsaken-data)
as `data/`, this repo's `scripts/`, and writable `configs/` and `pilots/` folders.

## Flatpak

    flatpak-builder --user --install --force-clean build-dir \
        packaging/flatpak/io.github.ForsakenX.Forsaken.yml
    flatpak run io.github.ForsakenX.Forsaken

The bundle includes the game data.  Pilots, settings and saved games are kept in
`~/.var/app/io.github.ForsakenX.Forsaken/data/forsaken`.
