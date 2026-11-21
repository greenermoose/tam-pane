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

# wait_for MAX_TICKS CONDITION_CMD: poll every 50ms until condition succeeds.
wait_for() {
	ticks=$1
	shift
	while [ "$ticks" -gt 0 ]; do
		if "$@" >/dev/null 2>&1; then
			return 0
		fi
		sleep 0.05
		ticks=$((ticks - 1))
	done
	return 1
}

has_socket() {
	ls "$dir"/wayland-* >/dev/null 2>&1
}

two_mapped() {
	[ "$(grep -c 'window mapped: foot' "$dir/log" 2>/dev/null || echo 0)" -ge 2 ]
}

# run NAME MODE [tam-pane args...]: start, poll until ready, SIGTERM, collect.
run() {
	name=$1 mode=$2
	shift 2
	dir=$(mktemp -d "${TMPDIR:-/tmp}/tam-pane-smoke.XXXXXX") || exit 1
	chmod 700 "$dir"
	env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$dir" \
		XDG_CONFIG_HOME="$dir/config" \
		WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
		"$bin" "$@" >"$dir/log" 2>&1 &
	pid=$!
	wait_for 40 has_socket
	if [ "$mode" = windows ]; then
		if wait_for 60 two_mapped; then
			# Let the short-lived foot exit while the second stays connected.
			sleep 0.2
		fi
	fi
	socket=no
	has_socket && socket=yes
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

run start-stop socket
if [ "$socket" = yes ] && [ "$status" -eq 0 ]; then report ok
else report bad "socket: $socket, exit status: $status"; fi

if command -v foot >/dev/null 2>&1; then
	run windows windows -d -s "foot sh -c 'sleep 0.15' & foot sh -c 'sleep 5'"
	mapped=$(printf '%s\n' "$log" | grep -c 'window mapped: foot')
	if [ "$mapped" -eq 2 ] && [ "$status" -eq 0 ]; then report ok
	else report bad "windows mapped: $mapped of 2, exit status: $status"; fi
else
	echo "smoke-headless: windows: skipped (foot is not installed)"
fi

# 3. Live reload via SIGHUP: valid reload succeeds, broken reload keeps running.
name=sighup-reload
dir=$(mktemp -d "${TMPDIR:-/tmp}/tam-pane-smoke.XXXXXX") || exit 1
chmod 700 "$dir"
cfg="$dir/init.lua"
printf 'tp.config{ gaps = 10 }\n' >"$cfg"
env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$dir" \
	XDG_CONFIG_HOME="$dir/config" \
	WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
	"$bin" -c "$cfg" >"$dir/log" 2>&1 &
pid=$!
wait_for 40 has_socket
printf 'tp.config{ gaps = 24 }\n' >"$cfg"
kill -HUP "$pid" 2>/dev/null
wait_for 40 grep -q 'reloaded config:' "$dir/log"
printf 'tp.config{ gaps = \n' >"$cfg"
kill -HUP "$pid" 2>/dev/null
wait_for 40 grep -q 'reload failed' "$dir/log"
kill -TERM "$pid" 2>/dev/null
wait "$pid"
status=$?
log=$(cat "$dir/log")
rm -rf "$dir"
if printf '%s\n' "$log" | grep -q 'reloaded config:' &&
	printf '%s\n' "$log" | grep -q 'reload failed' &&
	[ "$status" -eq 0 ]; then
	report ok
else
	report bad "sighup reload check failed (exit status: $status)"
fi

# 4. Startup hook: runs once, after tam-pane's own WAYLAND_DISPLAY is set,
#    and not again on reload. tp.spawn{...} runs sh directly, no shell
#    command string.
name=startup-once
dir=$(mktemp -d "${TMPDIR:-/tmp}/tam-pane-smoke.XXXXXX") || exit 1
chmod 700 "$dir"
cfg="$dir/init.lua"
started="$dir/started"
cat >"$cfg" <<EOF
tp.on('startup', function()
	tp.spawn{'sh', '-c', 'echo "\$WAYLAND_DISPLAY" >> "\$1"', 'sh', '$started'}
end)
EOF
env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$dir" \
	XDG_CONFIG_HOME="$dir/config" \
	WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
	"$bin" -c "$cfg" >"$dir/log" 2>&1 &
pid=$!
wait_for 40 has_socket
wait_for 40 test -s "$started"
kill -HUP "$pid" 2>/dev/null
wait_for 40 grep -q 'reloaded config:' "$dir/log"
sleep 0.3
kill -TERM "$pid" 2>/dev/null
wait "$pid"
status=$?
lines=$(cat "$started" 2>/dev/null)
log=$(cat "$dir/log")
rm -rf "$dir"
case $lines in
wayland-*) once=yes ;;
*) once=no ;;
esac
if [ "$once" = yes ] && [ "$(printf '%s\n' "$lines" | wc -l)" -eq 1 ] &&
	[ "$status" -eq 0 ]; then
	report ok
else
	report bad "startup hook output '$lines' (want one wayland-N line), exit status: $status"
fi

exit "$fail"
