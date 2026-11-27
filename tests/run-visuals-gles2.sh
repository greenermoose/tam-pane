#!/bin/sh
# Run the headless Mesa GLES2 visuals integration test in an isolated XDG_RUNTIME_DIR.
set -eu
rt="$(mktemp -d /tmp/tam-pane-visuals-XXXXXX)"
trap 'rm -rf "$rt"' EXIT INT TERM
chmod 700 "$rt"
exec env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$rt" \
	WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_HEADLESS_OUTPUTS=1 "$1"
