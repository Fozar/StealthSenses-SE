#!/bin/sh
# Renders promo.html#features and #hunt to docs/media with headless Edge. Usage: sh render.sh [features|hunt ...]
EDGE=${EDGE:-"/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"}
HERE=$(cd "$(dirname "$0")" && pwd -W 2>/dev/null || pwd)
OUT=$(cd "$(dirname "$0")/../../docs/media" && pwd -W 2>/dev/null || pwd)
for k in ${@:-features hunt}; do
  "$EDGE" --headless=new --hide-scrollbars --window-size=1920,1080 --virtual-time-budget=8000 \
    --screenshot="$OUT/promo-$k.png" "file:///$HERE/promo.html#$k"
done
