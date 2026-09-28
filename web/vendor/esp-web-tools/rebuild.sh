#!/bin/sh
# Rebuild the vendored esp-web-tools bundle in this directory.
#
# Why this exists: esp-web-tools 10.4.0 (the newest release) bundles esptool-js 0.6.x, which does
# not recognise ESP32-C5 revision v1.2 silicon (CHIP magic 0x30e1706f). Every current C5 devkit is
# that revision, so the flasher failed with "Failed to initialize" before it ever reached our code.
# See issue #13 and espressif/esptool-js#262. esptool-js 0.7.0 fixes detection, but no esp-web-tools
# release carries it yet, so we build 10.4.0 against it ourselves.
#
# The only change from upstream is the esptool-js pin. esp-web-tools uses ESPLoader, Transport and
# HardReset, all API-compatible across 0.6 -> 0.7; its writeFlash call already passes Uint8Array
# data (0.7 now rejects anything else) and never calls detectFlashSize (the one 0.7 breaking change).
#
# Drop this directory and go back to a released esp-web-tools once one ships esptool-js >= 0.7.0.
#
# Usage: sh web/vendor/esp-web-tools/rebuild.sh     (needs git, node/npm)
set -e

EWT_TAG=10.4.0
ESPTOOL_JS=0.7.0

OUT="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

git clone -q --depth 1 --branch "$EWT_TAG" https://github.com/esphome/esp-web-tools "$WORK/src"
cd "$WORK/src"
sed -i "s/\"esptool-js\": \"[^\"]*\"/\"esptool-js\": \"$ESPTOOL_JS\"/" package.json
npm install --no-audit --no-fund
npm ls esptool-js | grep -q "esptool-js@$ESPTOOL_JS"

echo "export const version = \"$EWT_TAG+esptool-js.$ESPTOOL_JS\";" > src/version.ts
rm -rf dist
NODE_ENV=production npx tsc
NODE_ENV=production npx rollup -c

# The fix is the whole point of this build, so refuse to install one without it.
grep -q 820080751 dist/web/*.js || { echo "built bundle lacks the C5 v1.2 magic (0x30e1706f)"; exit 1; }

rm -f "$OUT"/*.js
cp dist/web/*.js "$OUT"/
cp LICENSE "$OUT"/LICENSE
echo "vendored esp-web-tools $EWT_TAG with esptool-js $ESPTOOL_JS into $OUT"
