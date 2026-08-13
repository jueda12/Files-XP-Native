#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir="$repo_root/build-local"
mkdir -p "$output_dir"

(
  cd "$output_dir"
  g++ -pipe -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread \
    "$repo_root/tests/core_tests.cpp" \
    -o "$output_dir/core_tests"
)

"$output_dir/core_tests"
