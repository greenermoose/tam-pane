#!/bin/sh
set -eu
dir=$(mktemp -d "${TMPDIR:-/tmp}/tam-pane-input.XXXXXX")
trap 'rm -rf "$dir"' EXIT HUP INT TERM
chmod 700 "$dir"
env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$dir" \
	WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 "$1"
