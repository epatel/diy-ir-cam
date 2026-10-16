#!/bin/bash
# PostToolUse hook: incremental firmware build after edits to src/ or lib/.
# Exit 2 sends the compiler errors back to the agent. Fails open when pio or
# python3 is missing so a fresh machine isn't blocked.

file=$(/usr/bin/python3 -c 'import sys, json; print(json.load(sys.stdin).get("tool_input", {}).get("file_path", ""))' 2>/dev/null) || exit 0
root="${CLAUDE_PROJECT_DIR:-$(pwd)}"
case "$file" in
  "$root"/src/*|"$root"/lib/*) ;;
  *) exit 0 ;;
esac

PIO="$HOME/.platformio/penv/bin/pio"
[ -x "$PIO" ] || exit 0

cd "$root" || exit 0
if ! out=$("$PIO" run 2>&1); then
  echo "Firmware build failed after editing ${file#$root/}:" >&2
  printf '%s\n' "$out" | grep -E 'error|Error' | head -20 >&2
  exit 2
fi
exit 0
