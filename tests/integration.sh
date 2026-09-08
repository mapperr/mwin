#!/bin/sh

set -eu

if ! command -v script >/dev/null 2>&1; then
	echo "integration: skipped (script not installed)"
	exit 0
fi

tmp=${TMPDIR:-/tmp}/mwin-test.$$
trap 'rm -f "$tmp" "$tmp.status" "$tmp.prefix" "$tmp.scroll" "$tmp.zero" "$tmp.compat"' EXIT HUP INT TERM

run_script() {
	output=$1
	command=$2
	if command -v timeout >/dev/null 2>&1; then
		timeout 6 script -qefc "$command" "$output" >/dev/null
	else
		script -qefc "$command" "$output" >/dev/null
	fi
}

# Show the pending-prefix status, create a second shell, then close both.
{
	sleep 0.2
	printf '\017'
	sleep 0.1
	printf 'm'
	sleep 0.1
	printf '\017\030'
	sleep 0.1
	printf '\017\030'
} | run_script "$tmp" './mwin'

if ! grep -aF 'prefix ^O: waiting for key' "$tmp" >/dev/null; then
	echo "prefix: pending status not observed" >&2
	exit 1
fi

echo "integration: ok"

{
	sleep 0.2
	printf '\017\030'
} | run_script "$tmp.status" './mwin -s'

if grep -aF 'prefix ^O: waiting for key' "$tmp.status" >/dev/null; then
	echo "prefix: pending status shown with -s" >&2
	exit 1
fi

{
	sleep 0.2
	printf '\007'
	sleep 0.1
	printf '\030'
} | run_script "$tmp.prefix" "./mwin -c '^G'"

if ! grep -aF 'prefix ^G: waiting for key' "$tmp.prefix" >/dev/null; then
	echo "prefix: alternate key not shown in status" >&2
	exit 1
fi

echo "options: ok"

# Produce enough output to fill a five-line ring, browse it with every
# scrollback binding, return live with G, and close the window.
{
	sleep 0.2
	printf '%s' 'i=0; while [ "$i" -lt 40 ]; do echo "line-$i"; i=$((i+1)); done'
	printf '\015'
	sleep 0.3
	printf '\017\025'
	sleep 0.1
	printf '\025ye\004gG'
	sleep 0.1
	printf '\017\030'
} | MWIN_SCROLLBACK=5 run_script "$tmp.scroll" './mwin'

if ! grep -a 'scroll 5/5' "$tmp.scroll" >/dev/null; then
	echo "scrollback: status not observed" >&2
	exit 1
fi

for value in invalid -1 ' 5' 5x; do
	if MWIN_SCROLLBACK=$value ./mwin >/dev/null 2>&1; then
		echo "scrollback: invalid limit accepted: $value" >&2
		exit 1
	fi
done

{
	sleep 0.2
	printf '%s' 'i=0; while [ "$i" -lt 30 ]; do echo "$i"; i=$((i+1)); done'
	printf '\015'
	sleep 0.2
	printf '\017\025'
	printf '\017\030'
} | run_script "$tmp.zero" 'env MWIN_SCROLLBACK=0 ./mwin'

if grep -a 'scroll [0-9]' "$tmp.zero" >/dev/null; then
	echo "scrollback: zero did not disable history" >&2
	exit 1
fi

echo "scrollback: ok"

# Simulate a config.h from 0.1, before DEFAULT_SCROLLBACK was introduced.
"${CC:-cc}" -DMWIN_CONFIG_H -DCOMMAND_KEY=109 -DSHOW_STATUS=1 \
	-DMAX_WINDOWS=32 '-DCHILD_TERM="screen-256color"' \
	-std=c99 -pedantic -Wall -Wextra -Wshadow -Wconversion \
	-c -o "$tmp.compat" mwin.c

echo "config compatibility: ok"
