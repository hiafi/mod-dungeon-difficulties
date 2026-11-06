#!/usr/bin/env bash
# Builds and runs the standalone mod-dungeon-difficulties tests with the host compiler (CXX, default g++).
# Pure headers only (src/HeroicSpellMath.h, src/ReferenceHealth.h): no server build needed.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

CXX="${CXX:-g++}"
status=0
for test in heroic_spell_test; do
  "$CXX" -std=c++20 -Wall -Wextra -Werror -O1 -I "$root/src" -I "$here" "$here/$test.cpp" -o "$out/$test"
  if ! "$out/$test"; then
    status=1
  fi
done
exit $status
