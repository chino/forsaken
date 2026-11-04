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
