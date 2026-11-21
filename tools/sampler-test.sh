#!/bin/bash
# An install on the simulator's pretend fm1-sampler ("FM1 Community", 3654:7756, firmware identity FM-1_8000014): the
# shared core finds the FM-1, makes a data save named for the firmware identity, and the install ends with success.
# The pretend fm1-sampler answers command 49: its build ID is in the log, in the .txt file of the data save, and in the
# install record. The pretend stock FM-1 gives no answer: none of the three holds a build ID.
# Run as it is and with a board's memory and no PSRAM (--lowmem). Uses the first .fwsc in sim/sd/fm1 (or FM1_SD/fm1);
# with none, the Hub's stock firmware is got first.
set -e
cd "$(dirname "$0")/.."
sd=${FM1_SD:-sim/sd}
mkdir -p "$sd/fm1" build
pkg=$(ls "$sd/fm1" | grep -i '\.fwsc$' | head -1 || true)
if [ -z "$pkg" ]; then
  url=$(curl -fsS https://mutant.cloud/catalog.json | python3 -I -c '
import json, sys
print(json.load(sys.stdin)["stock"]["pkg"])')
  pkg=$(basename "$url")
  curl -fsS -o "$sd/fm1/$pkg" "$url"
fi
pio run -s -e sim
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail() {
  echo "$*" >&2
  exit 1
}
BUILD_ID=a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf
# the case of the pretend fm1-sampler, then the case of the pretend stock FM-1 (--sampler in the first only)
for who in "fm1-sampler" "stock"; do
  [ "$who" = "stock" ] && sampler="" || sampler="--sampler"
  for how in "" "--lowmem"; do
    name="$who ${how:-(no flags)}"
    # a new flash file for each case: a kept .ident file would replace the first firmware identity
    rm -rf "$work/sd" "$work"/flash.bin*
    mkdir -p "$work/sd/fm1"
    cp "$sd/fm1/$pkg" "$work/sd/fm1/"
    FM1_SD="$work/sd" .pio/build/sim/program --headless --flash "$work/flash.bin" --fm1 $sampler $how --install "$pkg" > "$work/log" 2>&1 || {
      grep -E 'sim:|job:|fm1:' "$work/log" >&2
      fail "$name: the install did not end with success"
    }
    grep -q 'sim: installed' "$work/log" || fail "$name: no 'sim: installed' in the log"
    count=$(ls "$work/sd/backups/switch" | grep -c '\.bin$' || true)
    [ "$count" = 1 ] || fail "$name: $count data saves in /backups/switch, not 1"
    bin=$(ls "$work/sd/backups/switch" | grep '\.bin$')
    txt="$work/sd/backups/switch/${bin%.bin}.txt"
    [ -f "$txt" ] || fail "$name: no .txt file for $bin"
    record=$(ls "$work"/sd/fm1buddy/installs/*.txt 2>/dev/null | head -1 || true)
    [ -f "$record" ] || fail "$name: no install record"
    if [ "$who" = "fm1-sampler" ]; then
      grep -q 'is FM-1_8000014' "$work/log" || fail "$name: the shared core did not find the fm1-sampler"
      case "$bin" in
        *FM-1_8000014.bin) ;;
        *) fail "$name: the data save $bin is not named for FM-1_8000014" ;;
      esac
      grep -q '^firmware FM-1_8000014$' "$txt" || fail "$name: the .txt file has no firmware identity FM-1_8000014"
      grep -q "^build    $BUILD_ID\$" "$txt" || fail "$name: the .txt file has no build line with the build ID"
      grep -q "^from build $BUILD_ID\$" "$record" || fail "$name: the install record has no from build line"
      grep -q "build $BUILD_ID" "$work/log" || fail "$name: the log has no build ID"
      [ "$(grep -n "build $BUILD_ID" "$work/log" | head -1 | cut -d: -f1)" -lt "$(grep -n 'sim: install, try 1' "$work/log" | head -1 | cut -d: -f1)" ] ||
        fail "$name: the build ID is in the log after the install starts"
      [ "$(grep -n 'is FM-1_8000014' "$work/log" | head -1 | cut -d: -f1)" -lt "$(grep -n "build $BUILD_ID" "$work/log" | head -1 | cut -d: -f1)" ] ||
        fail "$name: the build ID is in the log before the firmware identity"
    else
      grep -q '^firmware ' "$txt" || fail "$name: the .txt file has no firmware identity"
      ! grep -q '^build ' "$txt" || fail "$name: the .txt file has a build line"
      ! grep -q '^from build' "$record" || fail "$name: the install record has a from build line"
      ! grep -q 'fm1: .* build ' "$work/log" || fail "$name: the log has a build ID"
    fi
    echo "$name: $bin"
  done
done
echo ok
