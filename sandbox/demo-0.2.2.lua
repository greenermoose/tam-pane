-- Demo configuration for tam-pane 0.2.2 (P3 text-input-v3, input-method-v2,
-- xdg-activation-v1, primary-selection, wlr/ext data-control, xdg-decoration).
-- Run with: sandbox/demo-0.2.2, which puts GUIDE, CLIP_RES, TAM_HIDE above this.

tp.config{ gaps = 8 }

if TAM_HIDE ~= "" then
	tp.bind("Meta+l", function()
		tp.lock{TAM_HIDE}
	end)
end

tp.on("startup", function()
	tp.spawn{"foot", "--title=guide", "sh", "-c",
		'cat "$1"; exec "${SHELL:-/bin/sh}"', "sh", GUIDE}
	-- Verify both regular clipboard (data-control) and primary selection.
	tp.spawn{"sh", "-c",
		'wl-copy "tam-pane-0.2.2-clip" && ' ..
		'c=$(wl-paste --no-newline) && ' ..
		'wl-copy --primary "tam-pane-0.2.2-prim" && ' ..
		'p=$(wl-paste --primary --no-newline) && ' ..
		'if [ "$c" = "tam-pane-0.2.2-clip" ] && [ "$p" = "tam-pane-0.2.2-prim" ]; then ' ..
		'  echo OK > "$1"; ' ..
		'else ' ..
		'  printf "clip=%s prim=%s\\n" "$c" "$p" > "$1"; ' ..
		'fi',
		"sh", CLIP_RES}
end)

print("demo-0.2.2.lua loaded")
