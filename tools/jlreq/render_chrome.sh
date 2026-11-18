#!/bin/bash
# render_chrome.sh <dir> <meta.json>: each <dir>/<id>.html in Chromium (headless shell), 296x236 (h) or 236x296 (v), dpr 2.
CH=~/.cache/ms-playwright/chromium_headless_shell-1248/chrome-headless-shell-linux64/chrome-headless-shell
for f in "$1"/*.html; do id=$(basename "$f" .html)
  mode=$(python3 -c "import json,sys; print(json.load(open('$2'))['$id']['mode'])")
  [ "$mode" = v ] && size=236,296 || size=296,236
  $CH --headless --no-sandbox --disable-gpu --hide-scrollbars --force-device-scale-factor=2 --window-size=$size \
      --virtual-time-budget=1500 --screenshot="$1/$id.png" "file://$f" > /dev/null 2>&1 || echo "FAIL $id"
done
ls "$1"/*.png | wc -l
