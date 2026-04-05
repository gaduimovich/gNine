#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="/Users/gduimovi/dev/gNine"
GNINE_BIN="${ROOT_DIR}/build-arm64/gnine"
EXAMPLES_DIR="${ROOT_DIR}/examples"
DATA_DIR="${ROOT_DIR}/example_data"

for spec in \
  "canvas_bench_gradient_4k.psm|" \
  "canvas_bench_checkerboard_4k.psm|" \
  "canvas_bench_color_squares_4k.psm|" \
  "canvas_bench_color_squares_1080p.psm|" \
  "canvas_bench_rgb_triptych.psm|${DATA_DIR}/lena.png" \
  "canvas_bench_side_by_side_compare.psm|${DATA_DIR}/lena.png ${DATA_DIR}/duck.png" \
  "canvas_bench_contact_sheet_2x2.psm|${DATA_DIR}/lena.png ${DATA_DIR}/duck.png ${DATA_DIR}/lena.png ${DATA_DIR}/duck.png"
do
  example="${spec%%|*}"
  inputs="${spec#*|}"
  label="${example%.psm}"

  echo "== ${label} =="
  rtk "${GNINE_BIN}" --runtime --benchmark \
    "${EXAMPLES_DIR}/${example}" \
    ${inputs} \
    "/tmp/${label}.png"
  echo
done
