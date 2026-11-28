#!/bin/sh
# Renders tools/overview/<name>.html to pictures/<png> (2x, cropped to the content).
#   render.sh            -> overview.html -> pictures/overview.png
#   render.sh minimal    -> minimal.html  -> pictures/minimal-wireless-build.png
set -e
cd "$(dirname "$0")"
NAME="${1:-overview}"
case "$NAME" in
  overview) OUT=overview.png ;;
  minimal)  OUT=minimal-wireless-build.png ;;
  *)        OUT="$NAME.png" ;;
esac
C="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
P="$(mktemp -d)"
H=$("$C" --headless=new --user-data-dir="$P" --allow-file-access-from-files --window-size=1400,3000 --virtual-time-budget=3000 \
      --run-all-compositor-stages-before-draw --dump-dom "file://$PWD/$NAME.html?measure" 2>/dev/null \
    | sed -n 's/.*data-h="\([0-9]*\)".*/\1/p' | head -1)
"$C" --headless=new --user-data-dir="$P" --allow-file-access-from-files --hide-scrollbars --force-device-scale-factor=2 \
     --window-size=1400,"$H" --virtual-time-budget=3000 --screenshot="$PWD/../../pictures/$OUT" \
     "file://$PWD/$NAME.html" 2>/dev/null
rm -rf "$P"
echo "pictures/$OUT: 1400 x $H CSS px (2x)"
