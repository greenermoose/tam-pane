#!/bin/sh
# Headless checks: tam-pane runs on wlroots' headless backend with a
# private XDG_RUNTIME_DIR, so there is no screen, no input device and no
# effect on the running desktop.
#
#   1. start, open the Wayland socket, exit 0 on SIGTERM;
#   2. when foot is installed: map two windows, let one close, and still
#      exit 0 on SIGTERM while the other is connected.
set -u
bin=${1:-build/tam-pane}
fail=0

# run NAME WAIT_SECONDS [tam-pane args...]: start, wait, SIGTERM, collect.
run() {
	name=$1 wait=$2
	shift 2
	dir=$(mktemp -d "${TMPDIR:-/tmp}/tam-pane-smoke.XXXXXX") || exit 1
	chmod 700 "$dir"
	env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$dir" \
		XDG_CONFIG_HOME="$dir/config" \
		WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
		"$bin" "$@" >"$dir/log" 2>&1 &
	pid=$!
	sleep "$wait"
	socket=no
	ls "$dir"/wayland-* >/dev/null 2>&1 && socket=yes
	kill -TERM "$pid" 2>/dev/null
	wait "$pid"
	status=$?
	log=$(cat "$dir/log")
	rm -rf "$dir"
}

report() {
	if [ "$1" = ok ]; then
		echo "smoke-headless: $name: ok"
	else
		echo "smoke-headless: $name: FAILED ($2)"
		printf '%s\n' "$log" | grep -E '^\[(INFO|WARN|ERROR|DEBUG)\]|ssert' |
			sed 's/^/  | /'
		fail=1
	fi
}

run start-stop 1
if [ "$socket" = yes ] && [ "$status" -eq 0 ]; then report ok
else report bad "socket: $socket, exit status: $status"; fi

if command -v foot >/dev/null 2>&1; then
	run windows 2 -d -s "foot sh -c 'sleep 0.5' & foot sh -c 'sleep 5'"
	mapped=$(printf '%s\n' "$log" | grep -c 'window mapped: foot')
	if [ "$mapped" -eq 2 ] && [ "$status" -eq 0 ]; then report ok
	else report bad "windows mapped: $mapped of 2, exit status: $status"; fi
else
	echo "smoke-headless: windows: skipped (foot is not installed)"
fi

exit "$fail"
