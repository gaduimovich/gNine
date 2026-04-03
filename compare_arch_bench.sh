#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARM_BUILD_DIR="${ROOT_DIR}/build-arm64"
X86_BUILD_DIR="${ROOT_DIR}/build-x86_64"
EXAMPLE_PATH="examples/box_3x3.psm"
INPUT_PATH="example_data/lena.png"
ITERATIONS=2000
CHAIN_MODE=0
SHOULD_BUILD=0
ARM_CMAKE_BIN="${ARM_CMAKE_BIN:-}"
X86_CMAKE_BIN="${X86_CMAKE_BIN:-}"

usage() {
  cat <<'EOF'
Usage:
  ./compare_arch_bench.sh [options]

Options:
  --example PATH       Example program relative to repo root.
  --input PATH         Input image relative to repo root.
  --times N            Benchmark iteration count. Default: 2000
  --chain-times N      Run a chained simulation benchmark for N iterations.
  --arm-build DIR      ARM build directory. Default: build-arm64
  --x86-build DIR      x86_64 build directory. Default: build-x86_64
  --arm-cmake PATH     arm64 cmake binary. Default: current cmake in PATH
  --x86-cmake PATH     x86_64 cmake binary. Default: auto-detect /usr/local/bin/cmake
  --build              Force configure/build before benchmarking.
  --no-build           Alias for the default behavior: reuse existing binaries.
  --help               Show this message.

This script:
  1. Builds native arm64 and Rosetta x86_64 versions of gnine.
  2. Runs the same benchmark in both trees with --benchmark.
  3. Prints side-by-side metrics and relative speedup.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --example)
      EXAMPLE_PATH="$2"
      shift 2
      ;;
    --input)
      INPUT_PATH="$2"
      shift 2
      ;;
    --times)
      ITERATIONS="$2"
      shift 2
      ;;
    --chain-times)
      ITERATIONS="$2"
      CHAIN_MODE=1
      shift 2
      ;;
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
    --build)
      SHOULD_BUILD=1
      shift
      ;;
    --no-build)
      SHOULD_BUILD=0
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

if [[ ! -f "${ROOT_DIR}/${EXAMPLE_PATH}" ]]; then
  echo "Example not found: ${ROOT_DIR}/${EXAMPLE_PATH}" >&2
  exit 1
fi

if [[ ! -f "${ROOT_DIR}/${INPUT_PATH}" ]]; then
  echo "Input image not found: ${ROOT_DIR}/${INPUT_PATH}" >&2
  exit 1
fi

if ! [[ "${ITERATIONS}" =~ ^[0-9]+$ ]] || [[ "${ITERATIONS}" -le 0 ]]; then
  echo "--times must be a positive integer." >&2
  exit 1
fi

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
    X86_CMAKE_BIN="cmake"
  fi
fi

require_binary() {
  local path="$1"
  local label="$2"

  if [[ ! -x "${path}" ]]; then
    echo "Missing ${label} binary: ${path}" >&2
    exit 1
  fi
}

require_arch_match() {
  local path="$1"
  local expected="$2"
  local label="$3"
  local info

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
    echo "Run without --no-build or check the build logs." >&2
    exit 1
  fi
}

extract_metric() {
  local key="$1"
  local file="$2"
  awk -F= -v key="${key}" '$1 == key {print $2}' "${file}"
}

run_bench() {
  local arch_name="$1"
  local build_dir="$2"
  local arch_prefix="$3"
  local output_file="${build_dir}/benchmark_${arch_name}.txt"
  local output_png="${build_dir}/benchmark_${arch_name}.png"
  local benchmark_flag="--times=${ITERATIONS}"

  if [[ "${CHAIN_MODE}" -eq 1 ]]; then
    benchmark_flag="--chain-times=${ITERATIONS}"
  fi

  rm -f "${output_file}" "${output_png}"
  (
    cd "${build_dir}"
    ${arch_prefix} ./gnine "../${EXAMPLE_PATH}" "../${INPUT_PATH}" "$(basename "${output_png}")" "${benchmark_flag}" --benchmark
  ) | tee "${output_file}"
}

print_row() {
  printf "%-28s %14s %14s\n" "$1" "$2" "$3"
}

require_binary "${ARM_CMAKE_BIN}" "arm64 cmake"
require_arch_match "${ARM_CMAKE_BIN}" "arm64" "ARM cmake"

if [[ ! -x "${X86_CMAKE_BIN}" ]]; then
  cat >&2 <<EOF
No x86_64 cmake binary was found.

Current ARM cmake:
  ${ARM_CMAKE_BIN}

Expected x86_64 cmake:
  typically /usr/local/bin/cmake from an x86 Homebrew install under Rosetta

You can either:
  1. install x86_64 cmake under Rosetta, or
  2. rerun with --x86-cmake /path/to/x86_64/cmake
EOF
  exit 1
fi

require_arch_match "${X86_CMAKE_BIN}" "x86_64" "x86 cmake"

build_tree "${ARM_BUILD_DIR}" "${ARM_CMAKE_BIN}" ""
build_tree "${X86_BUILD_DIR}" "${X86_CMAKE_BIN}" "arch -x86_64"

ARM_FILE_INFO="$(file "${ARM_BUILD_DIR}/gnine")"
X86_FILE_INFO="$(file "${X86_BUILD_DIR}/gnine")"

if [[ "${ARM_FILE_INFO}" != *"arm64"* ]]; then
  echo "ARM build is not arm64: ${ARM_FILE_INFO}" >&2
  exit 1
fi

if [[ "${X86_FILE_INFO}" != *"x86_64"* ]]; then
  echo "x86 build is not x86_64: ${X86_FILE_INFO}" >&2
  exit 1
fi

run_bench "arm64" "${ARM_BUILD_DIR}" ""
run_bench "x86_64" "${X86_BUILD_DIR}" "arch -x86_64"

ARM_RESULTS="${ARM_BUILD_DIR}/benchmark_arm64.txt"
X86_RESULTS="${X86_BUILD_DIR}/benchmark_x86_64.txt"

ARM_COMPILE_MS="$(extract_metric "benchmark.compile_ms" "${ARM_RESULTS}")"
ARM_EXECUTE_MS="$(extract_metric "benchmark.execute_ms" "${ARM_RESULTS}")"
ARM_MODE="$(extract_metric "benchmark.mode" "${ARM_RESULTS}")"
ARM_AVG_MS="$(extract_metric "benchmark.avg_iter_ms" "${ARM_RESULTS}")"
ARM_PIXELS_PER_SECOND="$(extract_metric "benchmark.pixels_per_second" "${ARM_RESULTS}")"

X86_COMPILE_MS="$(extract_metric "benchmark.compile_ms" "${X86_RESULTS}")"
X86_EXECUTE_MS="$(extract_metric "benchmark.execute_ms" "${X86_RESULTS}")"
X86_MODE="$(extract_metric "benchmark.mode" "${X86_RESULTS}")"
X86_AVG_MS="$(extract_metric "benchmark.avg_iter_ms" "${X86_RESULTS}")"
X86_PIXELS_PER_SECOND="$(extract_metric "benchmark.pixels_per_second" "${X86_RESULTS}")"

SPEEDUP="$(python3 - <<PY
arm = float("${ARM_EXECUTE_MS}")
x86 = float("${X86_EXECUTE_MS}")
print(f"{x86 / arm:.3f}x" if arm > 0 else "n/a")
PY
)"

echo
echo "Architecture benchmark comparison"
echo "example: ${EXAMPLE_PATH}"
echo "input:   ${INPUT_PATH}"
echo "times:   ${ITERATIONS}"
echo "mode:    ${ARM_MODE}"
echo
print_row "metric" "arm64" "x86_64"
print_row "compile_ms" "${ARM_COMPILE_MS}" "${X86_COMPILE_MS}"
print_row "execute_ms" "${ARM_EXECUTE_MS}" "${X86_EXECUTE_MS}"
print_row "avg_iter_ms" "${ARM_AVG_MS}" "${X86_AVG_MS}"
print_row "pixels_per_second" "${ARM_PIXELS_PER_SECOND}" "${X86_PIXELS_PER_SECOND}"
echo
echo "Relative result: arm64 execution is ${SPEEDUP} faster than x86_64 under Rosetta."
