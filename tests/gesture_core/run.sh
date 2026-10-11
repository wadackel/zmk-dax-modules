#!/usr/bin/env bash
# Builds the gesture recognizer and its tests for the host and runs them.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/include" \
  -o "$out/test_gesture_core" \
  "$root/src/gesture/gesture_core.c" \
  "$root/tests/gesture_core/test_gesture_core.c"
"$out/test_gesture_core"
