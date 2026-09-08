#!/bin/sh

set -eu

if ! command -v script >/dev/null 2>&1; then
	echo "integration: skipped (script not installed)"
	exit 0
fi

tmp=${TMPDIR:-/tmp}/mwin-test.$$
trap 'rm -f "$tmp" "$tmp.status" "$tmp.prefix"' EXIT HUP INT TERM

# Create a second shell, close it, then close the original shell.  The timeout
# catches event-loop, PTY and child-reaping regressions without inspecting the
# terminal escape stream.
if command -v timeout >/dev/null 2>&1; then
	{
		sleep 0.2
		printf '\001c'
		sleep 0.1
		printf '\001x'
		sleep 0.1
		printf '\001x'
	} | timeout 5 script -qefc './mwin' "$tmp" >/dev/null
else
	{
		sleep 0.2
		printf '\001c'
		sleep 0.1
		printf '\001x'
		sleep 0.1
		printf '\001x'
	} | script -qefc './mwin' "$tmp" >/dev/null
fi

echo "integration: ok"

if command -v timeout >/dev/null 2>&1; then
	{
		sleep 0.2
		printf '\001x'
	} | timeout 5 script -qefc './mwin -s' "$tmp.status" >/dev/null

	{
		sleep 0.2
		printf '\007x'
	} | timeout 5 script -qefc "./mwin -c '^G'" "$tmp.prefix" >/dev/null

	echo "options: ok"
fi
