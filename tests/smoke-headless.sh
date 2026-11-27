#!/bin/sh
# Headless checks: tam-pane runs on wlroots' headless backend with a
# private XDG_RUNTIME_DIR, so there is no screen, no input device and no
# effect on the running desktop.
#
#   0. -v and --version print identical version lines and exit 0;
#   1. start, open the Wayland socket, exit 0 on SIGTERM;
#   2. when foot is installed: map two windows, let one close, and still
#      exit 0 on SIGTERM while the other is connected;
#   3. tam-pane msg queries (version, monitors) and live reload (valid + broken);
#   4. a startup hook runs once, after WAYLAND_DISPLAY and TAM_PANE_SOCKET are
#      set, and not again on reload;
#   5. SIGHUP shuts down cleanly with exit status 0 (R1 finding 17).
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

# 0. Version flags: -v and --version print identical version output and exit 0.
name=version-flags
v_out=$("$bin" -v 2>&1)
v_rc=$?
ver_flag_out=$("$bin" --version 2>&1)
ver_flag_rc=$?
log=$(printf '%s\n%s\n' "$v_out" "$ver_flag_out")
if [ "$v_rc" -eq 0 ] && [ "$ver_flag_rc" -eq 0 ] &&
	[ "$v_out" = "$ver_flag_out" ] &&
	printf '%s\n' "$v_out" | grep -q '^tam-pane '; then
	report ok
else
	report bad "-v rc=$v_rc out='$v_out', --version rc=$ver_flag_rc out='$ver_flag_out'"
fi

run start-stop socket
if [ "$socket" = yes ] && [ "$status" -eq 0 ] &&
	printf '%s\n' "$log" | grep -Eq 'tam-pane startup: lua=[0-9.]+ms init=[0-9.]+ms start=[0-9.]+ms total=[0-9.]+ms'; then
	report ok
else
	report bad "socket: $socket, exit status: $status (expected startup timing log)"
fi

if command -v foot >/dev/null 2>&1; then
	run windows windows -d -s "foot sh -c 'sleep 0.15' & foot sh -c 'sleep 5'"
	mapped=$(printf '%s\n' "$log" | grep -c 'window mapped: foot')
	if [ "$mapped" -eq 2 ] && [ "$status" -eq 0 ]; then report ok
	else report bad "windows mapped: $mapped of 2, exit status: $status"; fi
else
	echo "smoke-headless: windows: skipped (foot is not installed)"
fi

# 3. IPC queries and live reload via `tam-pane msg`: valid reload succeeds,
#    broken reload returns non-zero with diagnostic and keeps running.
name=ipc-msg-reload
dir=$(mktemp -d "${TMPDIR:-/tmp}/tam-pane-smoke.XXXXXX") || exit 1
chmod 700 "$dir"
cfg="$dir/init.lua"
printf 'tp.config{ gaps = 10 }\n' >"$cfg"
env -u WAYLAND_DISPLAY -u DISPLAY -u TAM_PANE_SOCKET XDG_RUNTIME_DIR="$dir" \
	XDG_CONFIG_HOME="$dir/config" \
	WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
	"$bin" -c "$cfg" >"$dir/log" 2>&1 &
pid=$!
wait_for 40 has_socket
wsock=$(basename "$(ls "$dir"/wayland-* 2>/dev/null | head -n 1)")
ipcsock="$dir/tam-pane/$wsock.sock"
wait_for 40 test -S "$ipcsock"
ver_out=$("$bin" msg -S "$ipcsock" version 2>&1)
ver_rc=$?
mon_out=$("$bin" msg -S "$ipcsock" monitors 2>&1)
mon_rc=$?
printf 'tp.config{ gaps = 24 }\n' >"$cfg"
rel_ok_out=$("$bin" msg -S "$ipcsock" reload 2>&1)
rel_ok_rc=$?
printf 'tp.config{ gaps = \n' >"$cfg"
rel_bad_out=$("$bin" msg -S "$ipcsock" reload 2>&1)
rel_bad_rc=$?
# A subscriber whose reader has gone away exits at once, even with no
# events coming (it used to wait for the next event, or forever).
timeout 3 sh -c '"$1" msg -S "$2" subscribe | true' sh "$bin" "$ipcsock"
sub_rc=$?
kill -TERM "$pid" 2>/dev/null
wait "$pid"
status=$?
sock_gone=no
[ ! -e "$ipcsock" ] && sock_gone=yes
log=$(cat "$dir/log")
rm -rf "$dir"
if [ "$ver_rc" -eq 0 ] && printf '%s\n' "$ver_out" | grep -q '^version	' &&
	[ "$mon_rc" -eq 0 ] && printf '%s\n' "$mon_out" | grep -q '^monitor	' &&
	[ "$rel_ok_rc" -eq 0 ] && [ "$rel_bad_rc" -ne 0 ] &&
	[ -n "$rel_bad_out" ] && [ "$sock_gone" = yes ] &&
	[ "$sub_rc" -ne 124 ] && [ "$status" -eq 0 ]; then
	report ok
else
	report bad "ver_rc=$ver_rc mon_rc=$mon_rc rel_ok=$rel_ok_rc rel_bad=$rel_bad_rc sub_rc=$sub_rc sock_gone=$sock_gone status=$status"
fi

# 4. Startup hook: runs once, after tam-pane's own WAYLAND_DISPLAY and
#    TAM_PANE_SOCKET are set, and not again on reload.
name=startup-once
dir=$(mktemp -d "${TMPDIR:-/tmp}/tam-pane-smoke.XXXXXX") || exit 1
chmod 700 "$dir"
cfg="$dir/init.lua"
started="$dir/started"
cat >"$cfg" <<EOF
tp.on('startup', function()
	tp.spawn{'sh', '-c', 'printf "%s|%s\n" "\$WAYLAND_DISPLAY" "\$TAM_PANE_SOCKET" >> "\$1"', 'sh', '$started'}
end)
EOF
env -u WAYLAND_DISPLAY -u DISPLAY -u TAM_PANE_SOCKET XDG_RUNTIME_DIR="$dir" \
	XDG_CONFIG_HOME="$dir/config" \
	WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
	"$bin" -c "$cfg" >"$dir/log" 2>&1 &
pid=$!
wait_for 40 has_socket
wait_for 40 test -s "$started"
wsock=$(basename "$(ls "$dir"/wayland-* 2>/dev/null | head -n 1)")
ipcsock="$dir/tam-pane/$wsock.sock"
"$bin" msg -S "$ipcsock" reload >/dev/null 2>&1
wait_for 40 grep -q 'reloaded config:' "$dir/log"
sleep 0.2
kill -TERM "$pid" 2>/dev/null
wait "$pid"
status=$?
lines=$(cat "$started" 2>/dev/null)
log=$(cat "$dir/log")
rm -rf "$dir"
case $lines in
wayland-*\|"$dir"/tam-pane/wayland-*.sock) once=yes ;;
*) once=no ;;
esac
if [ "$once" = yes ] && [ "$(printf '%s\n' "$lines" | wc -l)" -eq 1 ] &&
	[ "$status" -eq 0 ]; then
	report ok
else
	report bad "startup hook output '$lines' (want one wayland-N|sock line), exit status: $status"
fi

# 5. SIGHUP shuts down cleanly (R1 finding 17), not reload.
name=sighup-shutdown
dir=$(mktemp -d "${TMPDIR:-/tmp}/tam-pane-smoke.XXXXXX") || exit 1
chmod 700 "$dir"
env -u WAYLAND_DISPLAY -u DISPLAY -u TAM_PANE_SOCKET XDG_RUNTIME_DIR="$dir" \
	XDG_CONFIG_HOME="$dir/config" \
	WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
	"$bin" >"$dir/log" 2>&1 &
pid=$!
wait_for 40 has_socket
kill -HUP "$pid" 2>/dev/null
wait_for 40 sh -c "! kill -0 $pid 2>/dev/null"
still_alive=no
if kill -0 "$pid" 2>/dev/null; then
	still_alive=yes
	kill -TERM "$pid" 2>/dev/null
fi
wait "$pid"
status=$?
log=$(cat "$dir/log")
rm -rf "$dir"
if [ "$still_alive" = no ] && [ "$status" -eq 0 ]; then
	report ok
else
	report bad "still_alive=$still_alive exit_status=$status"
fi

exit "$fail"
