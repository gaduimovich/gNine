#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
CHAIN_TIMES="${CHAIN_TIMES:-300}"
BENCH_REPEATS="${BENCH_REPEATS:-3}"
JOBS="${JOBS:-4}"
OMR_SUBMODULE_URL="${OMR_SUBMODULE_URL:-}"

usage() {
  cat <<USAGE
Usage: ./run_runtime_perf_x86.sh [--build-dir DIR] [--chain-times N] [--benchmark-repeats N] [--jobs N] [--omr-submodule-url URL]

Runs local x86 workflow for runtime performance validation:
  1) configure + build gnine and test binaries
  2) run runtime/semantic/color tests
  3) run pong/snake v2 chained runtime benchmarks

Env overrides:
  CHAIN_TIMES (default: 300)
  BENCH_REPEATS (default: 3)
  JOBS (default: 4)
  OMR_SUBMODULE_URL (optional internal mirror URL for libs/omr)
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --chain-times)
      CHAIN_TIMES="$2"
      shift 2
      ;;
    --benchmark-repeats)
      BENCH_REPEATS="$2"
      shift 2
      ;;
    --jobs)
      JOBS="$2"
      shift 2
      ;;
    --omr-submodule-url)
      OMR_SUBMODULE_URL="$2"
      shift 2
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -n "${OMR_SUBMODULE_URL}" ]]; then
  git -C "${ROOT_DIR}" submodule set-url libs/omr "${OMR_SUBMODULE_URL}"
  git -C "${ROOT_DIR}" submodule sync --recursive
fi

if [[ ! -d "${ROOT_DIR}/libs/omr/.git" ]]; then
  if ! git -C "${ROOT_DIR}" submodule update --init --recursive; then
    echo "Failed to fetch libs/omr submodule." >&2
    echo "In Codex/containerized environments this is often a proxy allowlist issue." >&2
    echo "If GitHub is blocked, provide an internal mirror URL with:" >&2
    echo "  --omr-submodule-url https://<internal-git>/eclipse/omr.git" >&2
    echo "or env var:" >&2
    echo "  OMR_SUBMODULE_URL=https://<internal-git>/eclipse/omr.git" >&2
    exit 2
  fi
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --target gnine gnine_runtime_tests gnine_semantic_tests gnine_color_tests -j"${JOBS}"

"${BUILD_DIR}/tests/gnine_runtime_tests"
"${BUILD_DIR}/tests/gnine_semantic_tests"
"${BUILD_DIR}/tests/gnine_color_tests"

"${BUILD_DIR}/gnine" --runtime --benchmark --benchmark-no-write \
  --chain-times="${CHAIN_TIMES}" --benchmark-repeats="${BENCH_REPEATS}" \
  "${ROOT_DIR}/examples/runtime_pong_v2.psm" /tmp/runtime_pong_v2_bench.png

"${BUILD_DIR}/gnine" --runtime --benchmark --benchmark-no-write \
  --chain-times="${CHAIN_TIMES}" --benchmark-repeats="${BENCH_REPEATS}" \
  "${ROOT_DIR}/examples/runtime_snake_v2.psm" /tmp/runtime_snake_v2_bench.png
