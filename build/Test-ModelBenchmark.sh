#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir="$repo_root/build-local"
mkdir -p "$output_dir"

(
  cd "$output_dir"
  g++ -pipe -std=c++20 -O2 -Wall -Wextra -Wpedantic \
    "$repo_root/benchmarks/model_benchmark.cpp" \
    -o "$output_dir/model_benchmark"
)

"$output_dir/model_benchmark" "${1:-100000}"
