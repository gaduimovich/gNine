#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_PATH="${ROOT_DIR}/build-arm64/image_compare.png"
MODE="horizontal"
SHOULD_OPEN=0

usage() {
  cat <<'EOF'
Usage:
  ./compare_images.sh [options] image1 image2 [image3 ...]

Options:
  --output PATH      Output composite image path.
                     Default: build-arm64/image_compare.png
  --vertical         Stack images top-to-bottom.
  --horizontal       Place images left-to-right. Default.
  --open             Open the output image after generating it.
  --help             Show this message.

Examples:
  ./compare_images.sh build-arm64/abs_edges.png build-arm64/detail_boost.png
  ./compare_images.sh --vertical --open example_data/lena.png build-arm64/midtones_notch.png
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output)
      OUTPUT_PATH="$2"
      shift 2
      ;;
    --vertical)
      MODE="vertical"
      shift
      ;;
    --horizontal)
      MODE="horizontal"
      shift
      ;;
    --open)
      SHOULD_OPEN=1
      shift
      ;;
    --help)
      usage
      exit 0
      ;;
    --*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      break
      ;;
  esac
done

if [[ $# -lt 2 ]]; then
  echo "Need at least two input images." >&2
  usage >&2
  exit 1
fi

if ! command -v magick >/dev/null 2>&1; then
  echo "ImageMagick 'magick' command is required." >&2
  exit 1
fi

for image_path in "$@"; do
  if [[ ! -f "${image_path}" ]]; then
    echo "Missing input image: ${image_path}" >&2
    exit 1
  fi
done

mkdir -p "$(dirname "${OUTPUT_PATH}")"

append_flag="+append"
if [[ "${MODE}" == "vertical" ]]; then
  append_flag="-append"
fi

magick "$@" "${append_flag}" "${OUTPUT_PATH}"

echo "Wrote ${OUTPUT_PATH}"

if [[ "${SHOULD_OPEN}" -eq 1 ]]; then
  open "${OUTPUT_PATH}"
fi
