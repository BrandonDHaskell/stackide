#!/bin/sh
# .claude/hooks/format-cpp.sh
#
# PostToolUse hook. Runs clang-format in place on a C++ file that was just
# edited or written, so formatting never becomes a review topic.
#
# Input: hook JSON on stdin, containing tool_input.file_path.
# Output: nothing on success. A non-zero exit here does not block the edit;
#         PostToolUse runs after the tool has already completed.
#
# chmod +x this file. It is committed, so every contributor gets the same
# behavior without configuring anything.

set -eu

payload=$(cat)

file=$(printf '%s' "$payload" | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(0)
print(data.get("tool_input", {}).get("file_path", ""))
')

[ -n "$file" ] || exit 0
[ -f "$file" ] || exit 0

case "$file" in
    *.cpp|*.cc|*.cxx|*.h|*.hpp|*.hxx|*.ipp) ;;
    *) exit 0 ;;
esac

command -v clang-format >/dev/null 2>&1 || exit 0

clang-format -i --style=file "$file" || exit 0