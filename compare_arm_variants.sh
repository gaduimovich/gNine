#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARM_BUILD_DIR="${ROOT_DIR}/build-arm64"
BASE_EXAMPLE="examples/game_of_life/game_of_life.psm"
CANDIDATE_EXAMPLE="examples/game_of_life/game_of_life_optimized.psm"
INPUT_PATH="example_data/glider_gun.png"
ITERATIONS=5000
CHAIN_MODE=1
SHOULD_BUILD=0
ARM_CMAKE_BIN="${ARM_CMAKE_BIN:-}"

usage() {
  cat <<'EOF'
Usage:
  ./compare_arm_variants.sh [options]

Options:
  --base PATH         Baseline example relative to repo root.
  --candidate PATH    Candidate example relative to repo root.
  --input PATH        Input image relative to repo root.
  --times N           Benchmark iteration count. Default: 5000
  --chain-times N     Run a chained simulation benchmark for N iterations.
  --repeat-times N    Run a repeat benchmark for N iterations.
  --arm-build DIR     ARM build directory. Default: build-arm64
  --arm-cmake PATH    arm64 cmake binary. Default: auto-detect
  --build             Force configure/build before benchmarking.
  --help              Show this message.

This script:
  1. Runs a baseline and candidate example on the arm64 binary.
  2. Verifies their output images are identical.
  3. Prints side-by-side benchmark metrics and speedup.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base)
      BASE_EXAMPLE="$2"
      shift 2
      ;;
    --candidate)
      CANDIDATE_EXAMPLE="$2"
      shift 2
      ;;
    --input)
      INPUT_PATH="$2"
      shift 2
      ;;
    --times|--chain-times)
      ITERATIONS="$2"
      CHAIN_MODE=1
      shift 2
      ;;
    --repeat-times)
      ITERATIONS="$2"
      CHAIN_MODE=0
      shift 2
      ;;
    --arm-build)
      ARM_BUILD_DIR="$2"
      shift 2
      ;;
    --arm-cmake)
      ARM_CMAKE_BIN="$2"
      shift 2
      ;;
    --build)
      SHOULD_BUILD=1
      shift
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

require_file() {
  local path="$1"
  local label="$2"

  if [[ ! -f "${ROOT_DIR}/${path}" ]]; then
    echo "${label} not found: ${ROOT_DIR}/${path}" >&2
    exit 1
  fi
}

require_file "${BASE_EXAMPLE}" "Base example"
require_file "${CANDIDATE_EXAMPLE}" "Candidate example"
require_file "${INPUT_PATH}" "Input image"

if ! [[ "${ITERATIONS}" =~ ^[0-9]+$ ]] || [[ "${ITERATIONS}" -le 0 ]]; then
  echo "Iteration count must be a positive integer." >&2
  exit 1
fi

if [[ -z "${ARM_CMAKE_BIN}" ]]; then
  if [[ -x /opt/homebrew/bin/cmake ]]; then
    ARM_CMAKE_BIN="/opt/homebrew/bin/cmake"
  else
    ARM_CMAKE_BIN="$(command -v cmake)"
  fi
fi

if [[ ! -x "${ARM_CMAKE_BIN}" ]]; then
  echo "Missing arm64 cmake binary: ${ARM_CMAKE_BIN}" >&2
  exit 1
fi

ARM_CMAKE_INFO="$(file "${ARM_CMAKE_BIN}")"
if [[ "${ARM_CMAKE_INFO}" != *"arm64"* ]]; then
  echo "ARM cmake is not arm64: ${ARM_CMAKE_INFO}" >&2
  exit 1
fi

if [[ "${SHOULD_BUILD}" -eq 1 || ! -x "${ARM_BUILD_DIR}/gnine" ]]; then
  SDK_PATH="$(xcrun --show-sdk-path)"
  "${ARM_CMAKE_BIN}" -S "${ROOT_DIR}" -B "${ARM_BUILD_DIR}" -DCMAKE_OSX_SYSROOT="${SDK_PATH}" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  "${ARM_CMAKE_BIN}" --build "${ARM_BUILD_DIR}" --target gnine -j4
fi

if [[ ! -x "${ARM_BUILD_DIR}/gnine" ]]; then
  echo "Missing executable: ${ARM_BUILD_DIR}/gnine" >&2
  exit 1
fi

BENCH_FLAG="--chain-times=${ITERATIONS}"
MODE_LABEL="chain"
if [[ "${CHAIN_MODE}" -eq 0 ]]; then
  BENCH_FLAG="--times=${ITERATIONS}"
  MODE_LABEL="repeat"
fi

run_variant() {
  local label="$1"
  local example_path="$2"
  local example_slug
  example_slug="$(basename "${example_path}" .psm | tr -c '[:alnum:]' '_')"
  local out_png="${ARM_BUILD_DIR}/${label}_${example_slug}_arm64.png"
  local out_txt="${ARM_BUILD_DIR}/${label}_${example_slug}_arm64.txt"

  rm -f "${out_png}" "${out_txt}"
  (
    cd "${ARM_BUILD_DIR}"
    ./gnine "../${example_path}" "../${INPUT_PATH}" "$(basename "${out_png}")" "${BENCH_FLAG}" --benchmark
  ) | tee "${out_txt}"
}

extract_metric() {
  local key="$1"
  local file="$2"
  awk -F= -v key="${key}" '$1 == key {print $2}' "${file}"
}

print_row() {
  printf "%-28s %14s %14s\n" "$1" "$2" "$3"
}

run_variant "base" "${BASE_EXAMPLE}"
run_variant "candidate" "${CANDIDATE_EXAMPLE}"

BASE_PNG="${ARM_BUILD_DIR}/base_arm64.png"
BASE_SLUG="$(basename "${BASE_EXAMPLE}" .psm | tr -c '[:alnum:]' '_')"
CANDIDATE_SLUG="$(basename "${CANDIDATE_EXAMPLE}" .psm | tr -c '[:alnum:]' '_')"
BASE_PNG="${ARM_BUILD_DIR}/base_${BASE_SLUG}_arm64.png"
CANDIDATE_PNG="${ARM_BUILD_DIR}/candidate_${CANDIDATE_SLUG}_arm64.png"
BASE_TXT="${ARM_BUILD_DIR}/base_${BASE_SLUG}_arm64.txt"
CANDIDATE_TXT="${ARM_BUILD_DIR}/candidate_${CANDIDATE_SLUG}_arm64.txt"

BASE_SHA="$(shasum "${BASE_PNG}" | awk '{print $1}')"
CANDIDATE_SHA="$(shasum "${CANDIDATE_PNG}" | awk '{print $1}')"

if [[ "${BASE_SHA}" != "${CANDIDATE_SHA}" ]]; then
  echo "Output mismatch detected." >&2
  echo "base:      ${BASE_SHA}  ${BASE_PNG}" >&2
  echo "candidate: ${CANDIDATE_SHA}  ${CANDIDATE_PNG}" >&2
  exit 2
fi

BASE_COMPILE_MS="$(extract_metric "benchmark.compile_ms" "${BASE_TXT}")"
BASE_EXECUTE_MS="$(extract_metric "benchmark.execute_ms" "${BASE_TXT}")"
BASE_AVG_MS="$(extract_metric "benchmark.avg_iter_ms" "${BASE_TXT}")"
BASE_PIXELS_PER_SECOND="$(extract_metric "benchmark.pixels_per_second" "${BASE_TXT}")"

CANDIDATE_COMPILE_MS="$(extract_metric "benchmark.compile_ms" "${CANDIDATE_TXT}")"
CANDIDATE_EXECUTE_MS="$(extract_metric "benchmark.execute_ms" "${CANDIDATE_TXT}")"
CANDIDATE_AVG_MS="$(extract_metric "benchmark.avg_iter_ms" "${CANDIDATE_TXT}")"
CANDIDATE_PIXELS_PER_SECOND="$(extract_metric "benchmark.pixels_per_second" "${CANDIDATE_TXT}")"

SPEEDUP="$(python3 - <<PY
base = float("${BASE_EXECUTE_MS}")
candidate = float("${CANDIDATE_EXECUTE_MS}")
print(f"{base / candidate:.3f}x" if candidate > 0 else "n/a")
PY
)"

echo
echo "ARM variant benchmark comparison"
echo "input:     ${INPUT_PATH}"
echo "iterations:${ITERATIONS}"
echo "mode:      ${MODE_LABEL}"
echo
print_row "metric" "base" "candidate"
print_row "compile_ms" "${BASE_COMPILE_MS}" "${CANDIDATE_COMPILE_MS}"
print_row "execute_ms" "${BASE_EXECUTE_MS}" "${CANDIDATE_EXECUTE_MS}"
print_row "avg_iter_ms" "${BASE_AVG_MS}" "${CANDIDATE_AVG_MS}"
print_row "pixels_per_second" "${BASE_PIXELS_PER_SECOND}" "${CANDIDATE_PIXELS_PER_SECOND}"
echo
echo "Output check: identical (${BASE_SHA})"
echo "Relative result: candidate execution is ${SPEEDUP} faster than base."
echo "Images:"
echo "  ${BASE_PNG}"
echo "  ${CANDIDATE_PNG}"
