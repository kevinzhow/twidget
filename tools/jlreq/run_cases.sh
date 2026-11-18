#!/bin/bash
# run_cases.sh <reader-cli> <cases dir> <out dir>: every case, horizontal and vertical, 14 em lines (280 px at 20 px),
# page 1 at dpr 2, plus the drawn lines (--dump-lines) for the checks.
set -u
cli=$1; in=$2; out=$3; mkdir -p "$out"
for f in "$in"/*.xhtml; do
  id=$(basename "$f" .xhtml)
  "$cli" "$f" "$out/$id-h" --width 296 --height 236 --inset 8,8,8,8 --dpr 2 --pages 1 --dump-lines "$out/$id-h.lines.json" > "$out/$id-h.json" 2>&1 || echo "FAIL $id-h"
  "$cli" "$f" "$out/$id-v" --width 236 --height 296 --inset 8,8,8,8 --dpr 2 --pages 1 --vertical --dump-lines "$out/$id-v.lines.json" > "$out/$id-v.json" 2>&1 || echo "FAIL $id-v"
done
ls "$out" | grep -c png
