#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARM_BUILD_DIR="${ROOT_DIR}/build-arm64"
X86_BUILD_DIR="${ROOT_DIR}/build-x86_64"
ARM_CMAKE_BIN="${ARM_CMAKE_BIN:-}"
X86_CMAKE_BIN="${X86_CMAKE_BIN:-}"
REPEAT_TIMES=1000
CHAIN_TIMES=100000
SHOULD_BUILD=0
RESULTS_TSV="$(mktemp)"

cleanup() {
  rm -f "${RESULTS_TSV}"
}
trap cleanup EXIT

usage() {
  cat <<'EOF'
Usage:
  ./compare_all_arch_examples.sh [options]

Options:
  --arm-build DIR      ARM build directory. Default: build-arm64
  --x86-build DIR      x86_64 build directory. Default: build-x86_64
  --arm-cmake PATH     arm64 cmake binary. Default: auto-detect
  --x86-cmake PATH     x86_64 cmake binary. Default: auto-detect /usr/local/bin/cmake
  --repeat-times N     Repeat benchmark count. Default: 1000
  --chain-times N      Chain benchmark count. Default: 100000
  --build              Force configure/build before benchmarking.
  --help               Show this message.

This script:
  1. Benchmarks the kept optimized examples on arm64 and x86_64.
  2. Prints per-example architecture comparisons and aggregate totals.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --arm-build)
      ARM_BUILD_DIR="$2"
      shift 2
      ;;
    --x86-build)
      X86_BUILD_DIR="$2"
      shift 2
      ;;
    --arm-cmake)
      ARM_CMAKE_BIN="$2"
      shift 2
      ;;
    --x86-cmake)
      X86_CMAKE_BIN="$2"
      shift 2
      ;;
    --repeat-times)
      REPEAT_TIMES="$2"
      shift 2
      ;;
    --chain-times)
      CHAIN_TIMES="$2"
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

require_positive_int() {
  local value="$1"
  local label="$2"
  if ! [[ "${value}" =~ ^[0-9]+$ ]] || [[ "${value}" -le 0 ]]; then
    echo "${label} must be a positive integer." >&2
    exit 1
  fi
}

require_arch_match() {
  local path="$1"
  local expected="$2"
  local label="$3"
  local info

  if [[ ! -x "${path}" ]]; then
    echo "Missing ${label}: ${path}" >&2
    exit 1
  fi

  info="$(file "${path}")"
  if [[ "${info}" != *"${expected}"* ]]; then
    echo "${label} is not ${expected}: ${info}" >&2
    exit 1
  fi
}

build_tree() {
  local build_dir="$1"
  local cmake_bin="$2"
  local arch_prefix="$3"

  if [[ "${SHOULD_BUILD}" -eq 1 || ! -x "${build_dir}/gnine" ]]; then
    ${arch_prefix} "${cmake_bin}" -S "${ROOT_DIR}" -B "${build_dir}" -DCMAKE_OSX_SYSROOT="${SDK_PATH}" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    ${arch_prefix} "${cmake_bin}" --build "${build_dir}" --target gnine -j4
  fi

  if [[ ! -x "${build_dir}/gnine" ]]; then
    echo "Missing executable: ${build_dir}/gnine" >&2
    exit 1
  fi
}

extract_metric() {
  local key="$1"
  local file="$2"
  awk -F= -v key="${key}" '$1 == key {print $2}' "${file}"
}

run_one() {
  local arch_name="$1"
  local build_dir="$2"
  local arch_prefix="$3"
  local example_path="$4"
  local input_args="$5"
  local mode="$6"
  local iterations="$7"
  local label="$8"
  local log_file="${build_dir}/${label}_${arch_name}.txt"
  local png_file="${build_dir}/${label}_${arch_name}.png"
  local benchmark_flag="--times=${iterations}"

  if [[ "${mode}" == "chain" ]]; then
    benchmark_flag="--chain-times=${iterations}"
  fi

  rm -f "${log_file}" "${png_file}"

  (
    cd "${build_dir}"
    # shellcheck disable=SC2086
    ${arch_prefix} ./gnine "../${example_path}" ${input_args} "$(basename "${png_file}")" "${benchmark_flag}" --benchmark
  ) | tee "${log_file}" >/dev/null

  extract_metric "benchmark.execute_ms" "${log_file}"
}

require_positive_int "${REPEAT_TIMES}" "--repeat-times"
require_positive_int "${CHAIN_TIMES}" "--chain-times"

SDK_PATH="$(xcrun --show-sdk-path)"

if [[ -z "${ARM_CMAKE_BIN}" ]]; then
  if [[ -x /opt/homebrew/bin/cmake ]]; then
    ARM_CMAKE_BIN="/opt/homebrew/bin/cmake"
  else
    ARM_CMAKE_BIN="$(command -v cmake)"
  fi
fi

if [[ -z "${X86_CMAKE_BIN}" ]]; then
  if [[ -x /usr/local/bin/cmake ]]; then
    X86_CMAKE_BIN="/usr/local/bin/cmake"
  else
    X86_CMAKE_BIN="$(command -v cmake)"
  fi
fi

require_arch_match "${ARM_CMAKE_BIN}" "arm64" "arm64 cmake"
require_arch_match "${X86_CMAKE_BIN}" "x86_64" "x86_64 cmake"

build_tree "${ARM_BUILD_DIR}" "${ARM_CMAKE_BIN}" ""
build_tree "${X86_BUILD_DIR}" "${X86_CMAKE_BIN}" "arch -x86_64"

require_arch_match "${ARM_BUILD_DIR}/gnine" "arm64" "arm64 gnine"
require_arch_match "${X86_BUILD_DIR}/gnine" "x86_64" "x86_64 gnine"

TOTAL_ARM_MS=0
TOTAL_X86_MS=0

run_pair() {
  local label="$1"
  local example_path="$2"
  local input_args="$3"
  local mode="$4"
  local iterations="$5"
  local arm_ms
  local x86_ms
  local ratio

  arm_ms="$(run_one "arm64" "${ARM_BUILD_DIR}" "" "${example_path}" "${input_args}" "${mode}" "${iterations}" "${label}")"
  x86_ms="$(run_one "x86_64" "${X86_BUILD_DIR}" "arch -x86_64" "${example_path}" "${input_args}" "${mode}" "${iterations}" "${label}")"
  ratio="$(python3 - <<PY
arm = float("${arm_ms}")
x86 = float("${x86_ms}")
if arm < x86:
    print(f"arm64 {x86 / arm:.3f}x faster")
else:
    print(f"x86_64 {arm / x86:.3f}x faster")
PY
)"

  TOTAL_ARM_MS="$(python3 - <<PY
print(float("${TOTAL_ARM_MS}") + float("${arm_ms}"))
PY
)"
  TOTAL_X86_MS="$(python3 - <<PY
print(float("${TOTAL_X86_MS}") + float("${x86_ms}"))
PY
)"

  printf "%s\t%s\t%s\t%s\t%s\t%s\n" "${label}" "${mode}" "${iterations}" "${arm_ms}" "${x86_ms}" "${ratio}" >> "${RESULTS_TSV}"
}

run_pair "metaballs_optimized" "examples/metaballs_optimized.psm" "../example_data/lena.png" "repeat" "${REPEAT_TIMES}"
run_pair "metaballs_binary_optimized" "examples/metaballs_binary_optimized.psm" "../example_data/lena.png" "repeat" "${REPEAT_TIMES}"
run_pair "metaballs_fancy_optimized" "examples/metaballs_fancy_optimized.psm" "../example_data/lena.png" "repeat" "${REPEAT_TIMES}"
run_pair "parobala2_optimized" "examples/parobala2_optimized.psm" "../example_data/lena.png" "repeat" "${REPEAT_TIMES}"
run_pair "game_of_life_optimized" "examples/game_of_life/game_of_life_optimized.psm" "../example_data/glider_gun.png" "chain" "${CHAIN_TIMES}"

echo "Architecture comparison summary"
printf "%-28s %-8s %12s %14s %14s %18s\n" "example" "mode" "iterations" "arm64_ms" "x86_64_ms" "result"

while IFS=$'\t' read -r label mode iterations arm_ms x86_ms ratio; do
  printf "%-28s %-8s %12s %14s %14s %18s\n" "${label}" "${mode}" "${iterations}" "${arm_ms}" "${x86_ms}" "${ratio}"
done < "${RESULTS_TSV}"

TOTAL_RATIO="$(python3 - <<PY
arm = float("${TOTAL_ARM_MS}")
x86 = float("${TOTAL_X86_MS}")
if arm < x86:
    print(f"arm64 {x86 / arm:.3f}x faster")
else:
    print(f"x86_64 {arm / x86:.3f}x faster")
PY
)"

echo
printf "%-28s %-8s %12s %14.3f %14.3f %18s\n" "TOTAL" "-" "-" "${TOTAL_ARM_MS}" "${TOTAL_X86_MS}" "${TOTAL_RATIO}"
