#!/bin/sh
# The game reads and writes everything relative to its working directory,
# so give it a writable home that links back to the read-only install.
set -e
GAME=/app/share/forsaken
HOME_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/forsaken"
mkdir -p "$HOME_DIR"
cd "$HOME_DIR"
for dir in configs pilots savegame demos logs; do
	mkdir -p "$dir"
done
ln -sfn "$GAME/data" data
exec /app/bin/projectx "$@"
