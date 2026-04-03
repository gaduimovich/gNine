#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARM_BUILD_DIR="${ROOT_DIR}/build-arm64"
ARM_CMAKE_BIN="${ARM_CMAKE_BIN:-}"
SHOULD_BUILD=0
REPEAT_TIMES=500
CHAIN_TIMES=5000

usage() {
  cat <<'EOF'
Usage:
  ./benchmark_all_examples_arm.sh [options]

Options:
  --arm-build DIR     ARM build directory. Default: build-arm64
  --arm-cmake PATH    arm64 cmake binary. Default: auto-detect
  --repeat-times N    Repeat benchmark count for non-chained examples. Default: 500
  --chain-times N     Chain benchmark count for chained examples. Default: 5000
  --build             Force configure/build before benchmarking.
  --help              Show this message.

This script:
  1. Runs baseline vs candidate benchmarks for optimized examples on arm64.
  2. Verifies output images are identical for every pair.
  3. Prints per-example results plus aggregate total execution speedup.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --arm-build)
      ARM_BUILD_DIR="$2"
      shift 2
      ;;
    --arm-cmake)
      ARM_CMAKE_BIN="$2"
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

if [[ -z "${ARM_CMAKE_BIN}" ]]; then
  if [[ -x /opt/homebrew/bin/cmake ]]; then
    ARM_CMAKE_BIN="/opt/homebrew/bin/cmake"
  else
    ARM_CMAKE_BIN="$(command -v cmake)"
  fi
fi

require_positive_int() {
  local value="$1"
  local label="$2"
  if ! [[ "${value}" =~ ^[0-9]+$ ]] || [[ "${value}" -le 0 ]]; then
    echo "${label} must be a positive integer." >&2
    exit 1
  fi
}

require_positive_int "${REPEAT_TIMES}" "--repeat-times"
require_positive_int "${CHAIN_TIMES}" "--chain-times"

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

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

TOTAL_BASE_MS=0
TOTAL_CANDIDATE_MS=0
RESULTS_TSV="${TMP_DIR}/results.tsv"

write_head_baseline() {
  local repo_path="$1"
  local dest_path="$2"
  git -C "${ROOT_DIR}" show "HEAD:${repo_path}" > "${dest_path}"
}

run_pair() {
  local label="$1"
  local baseline_path="$2"
  local candidate_path="$3"
  local input_args="$4"
  local mode="$5"
  local iterations="$6"

  local benchmark_flag="--times=${iterations}"
  if [[ "${mode}" == "chain" ]]; then
    benchmark_flag="--chain-times=${iterations}"
  fi
  local baseline_arg="../${baseline_path}"
  local candidate_arg="../${candidate_path}"

  if [[ "${baseline_path}" = /* ]]; then
    baseline_arg="${baseline_path}"
  fi

  if [[ "${candidate_path}" = /* ]]; then
    candidate_arg="${candidate_path}"
  fi

  local base_png="${ARM_BUILD_DIR}/${label}_base.png"
  local cand_png="${ARM_BUILD_DIR}/${label}_candidate.png"
  local base_log="${ARM_BUILD_DIR}/${label}_base.txt"
  local cand_log="${ARM_BUILD_DIR}/${label}_candidate.txt"

  rm -f "${base_png}" "${cand_png}" "${base_log}" "${cand_log}"

  (
    cd "${ARM_BUILD_DIR}"
    # shellcheck disable=SC2086
    ./gnine "${baseline_arg}" ${input_args} "$(basename "${base_png}")" "${benchmark_flag}" --benchmark
  ) | tee "${base_log}" >/dev/null

  (
    cd "${ARM_BUILD_DIR}"
    # shellcheck disable=SC2086
    ./gnine "${candidate_arg}" ${input_args} "$(basename "${cand_png}")" "${benchmark_flag}" --benchmark
  ) | tee "${cand_log}" >/dev/null

  local base_sha cand_sha
  base_sha="$(shasum "${base_png}" | awk '{print $1}')"
  cand_sha="$(shasum "${cand_png}" | awk '{print $1}')"

  if [[ "${base_sha}" != "${cand_sha}" ]]; then
    echo "Output mismatch for ${label}" >&2
    echo "  base:      ${base_sha} ${base_png}" >&2
    echo "  candidate: ${cand_sha} ${cand_png}" >&2
    exit 2
  fi

  local base_execute candidate_execute
  base_execute="$(awk -F= '$1 == "benchmark.execute_ms" {print $2}' "${base_log}")"
  candidate_execute="$(awk -F= '$1 == "benchmark.execute_ms" {print $2}' "${cand_log}")"

  printf "%s\t%s\t%s\t%s\t%s\t%s\n" "${label}" "${mode}" "${iterations}" "${base_execute}" "${candidate_execute}" "${base_sha}" >> "${RESULTS_TSV}"
}

# Baseline from HEAD, candidate from explicit optimized variants only.
for path in \
  "examples/metaballs.psm|examples/metaballs_optimized.psm|../example_data/lena.png|repeat|${REPEAT_TIMES}" \
  "examples/metaballs_binary.psm|examples/metaballs_binary_optimized.psm|../example_data/lena.png|repeat|${REPEAT_TIMES}" \
  "examples/metaballs_fancy.psm|examples/metaballs_fancy_optimized.psm|../example_data/lena.png|repeat|${REPEAT_TIMES}" \
  "examples/parobala2.psm|examples/parobala2_optimized.psm|../example_data/lena.png|repeat|${REPEAT_TIMES}"; do
  IFS='|' read -r repo_path candidate_path input_args mode iterations <<< "${path}"
  baseline_tmp="${TMP_DIR}/$(basename "${repo_path}")"
  write_head_baseline "${repo_path}" "${baseline_tmp}"
  run_pair "$(basename "${repo_path}" .psm)" "${baseline_tmp}" "${candidate_path}" "${input_args}" "${mode}" "${iterations}"
done

# Explicit optimized chained variant.
run_pair "game_of_life" \
  "examples/game_of_life/game_of_life.psm" \
  "examples/game_of_life/game_of_life_optimized.psm" \
  "../example_data/glider_gun.png" \
  "chain" \
  "${CHAIN_TIMES}"

echo "ARM optimized example benchmark summary"
printf "%-22s %-8s %12s %14s %14s %12s\n" "example" "mode" "iterations" "base_ms" "candidate_ms" "speedup"

while IFS=$'\t' read -r label mode iterations base_ms candidate_ms sha; do
  speedup="$(python3 - <<PY
base = float("${base_ms}")
candidate = float("${candidate_ms}")
print(f"{base / candidate:.3f}x" if candidate > 0 else "n/a")
PY
)"
  TOTAL_BASE_MS="$(python3 - <<PY
print(float("${TOTAL_BASE_MS}") + float("${base_ms}"))
PY
)"
  TOTAL_CANDIDATE_MS="$(python3 - <<PY
print(float("${TOTAL_CANDIDATE_MS}") + float("${candidate_ms}"))
PY
)"
  printf "%-22s %-8s %12s %14s %14s %12s\n" "${label}" "${mode}" "${iterations}" "${base_ms}" "${candidate_ms}" "${speedup}"
done < "${RESULTS_TSV}"

TOTAL_SPEEDUP="$(python3 - <<PY
base = float("${TOTAL_BASE_MS}")
candidate = float("${TOTAL_CANDIDATE_MS}")
print(f"{base / candidate:.3f}x" if candidate > 0 else "n/a")
PY
)"

echo
printf "%-22s %-8s %12s %14.3f %14.3f %12s\n" "TOTAL" "-" "-" "${TOTAL_BASE_MS}" "${TOTAL_CANDIDATE_MS}" "${TOTAL_SPEEDUP}"
echo "All compared outputs matched exactly."
