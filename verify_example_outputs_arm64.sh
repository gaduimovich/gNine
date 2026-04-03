#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build-arm64"
ARM_CMAKE_BIN="${ARM_CMAKE_BIN:-}"
SHOULD_BUILD=0
MANIFEST="${ROOT_DIR}/regression/example-output-hashes-arm64.sha256"
BASELINE_COMMIT="e534a48"

usage() {
  cat <<'EOF'
Usage:
  ./verify_example_outputs_arm64.sh [options]

Options:
  --arm-build DIR     ARM build directory. Default: build-arm64
  --arm-cmake PATH    arm64 cmake binary. Default: auto-detect
  --build             Force rebuild of examples and game_of_life outputs
  --help              Show this message

This script:
  1. Ensures the ARM example outputs exist.
  2. Hashes every generated PNG under build-arm64/examples.
  3. Compares the result against regression/example-output-hashes-arm64.sha256.

The committed manifest was generated from baseline commit e534a48.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --arm-build)
      BUILD_DIR="$2"
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

if [[ ! -f "${MANIFEST}" ]]; then
  echo "Missing regression manifest: ${MANIFEST}" >&2
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

if [[ "${SHOULD_BUILD}" -eq 1 || ! -d "${BUILD_DIR}/examples/game_of_life" ]]; then
  SDK_PATH="$(xcrun --show-sdk-path)"
  "${ARM_CMAKE_BIN}" -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_OSX_SYSROOT="${SDK_PATH}" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  "${ARM_CMAKE_BIN}" --build "${BUILD_DIR}" --target examples example_game_of_life -j4
fi

TMP_MANIFEST="$(mktemp)"
trap 'rm -f "${TMP_MANIFEST}"' EXIT

find "${BUILD_DIR}/examples" -type f -name '*.png' -exec shasum -a 256 {} \; \
  | LC_ALL=C sort \
  | sed "s#${BUILD_DIR}/examples/##" > "${TMP_MANIFEST}"

if ! diff -u "${MANIFEST}" "${TMP_MANIFEST}"; then
  echo
  echo "Example output regression mismatch against baseline commit ${BASELINE_COMMIT}." >&2
  exit 2
fi

echo "Example outputs match regression manifest from baseline commit ${BASELINE_COMMIT}."
