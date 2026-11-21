#!/bin/bash
# The scan of /fm1 with 1,000 small packages. With --lowmem (a board with no PSRAM) the SD list stays on the SD card,
# in /fm1buddy/sd-list.txt: a head line and a line for each package, and the scan ends and reports 1,000 packages found
# and 1,000 shown. The SHA task then reads the 1,000 packages that the SHA cache does not know, and writes a line for
# each to /fm1buddy/sd-sha256.txt; at the next start no package has an unknown sha256. With no --lowmem the list is in
# memory, and the start line of the log reports 1,000 packages.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
pio run -s -e sim
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail() {
  echo "$*" >&2
  exit 1
}
# the card: 600 packages in fm1, 400 in fm1/old-chip, each of 100 bytes or more
mkdir -p "$work/card/fm1/old-chip"
for n in $(seq 1 1000); do
  dir="$work/card/fm1"
  [ $((n % 5)) -ge 3 ] && dir="$dir/old-chip"
  name=$(printf 'library-package-%04d-1.0.fwsc' "$n")
  head -c $((100 + n)) /dev/zero > "$dir/$name"
done
for how in "--lowmem" ""; do
  rm -rf "$work/sd"
  cp -R "$work/card" "$work/sd"
  FM1_SD="$work/sd" .pio/build/sim/program --headless $how --script 'wait 3000; quit' > "$work/log" 2>&1 ||
    fail "${how:-(no flags)}: the simulator did not end by itself"
  grep -qiE 'abort|watchdog|out of memory' "$work/log" && fail "${how:-(no flags)}: the log reports an abort"
  if [ -n "$how" ]; then
    grep -q 'sd: 1000 found, 1000 shown' "$work/log" || fail "$how: the log does not report 1,000 found and 1,000 shown"
    list="$work/sd/fm1buddy/sd-list.txt"
    [ -f "$list" ] || fail "$how: no SD list on the card"
    lines=$(wc -l < "$list" | tr -d ' ')
    [ "$lines" = 1001 ] || fail "$how: the SD list holds $lines lines, not a head line and 1,000 packages"
    [ "$(sed -n 2,1001p "$list" | awk -F'\t' 'NF == 11' | wc -l | tr -d ' ')" = 1000 ] ||
      fail "$how: a package line of the SD list does not hold 11 fields"
    sed -n 2,1001p "$list" | cut -f1 | LC_ALL=C sort -c || fail "$how: the SD list is not in the order of the names"
    grep -q 'sd: 1000 read' "$work/log" || fail "$how: the SHA task did not read 1,000 packages"
    cache="$work/sd/fm1buddy/sd-sha256.txt"
    [ -f "$cache" ] || fail "$how: no SHA cache on the card"
    lines=$(wc -l < "$cache" | tr -d ' ')
    [ "$lines" = 1000 ] || fail "$how: the SHA cache holds $lines lines, not 1,000"
    ls "$work/sd/fm1buddy" | grep -q '\.tmp$' && fail "$how: the scan or the SHA task left a temporary file"
    FM1_SD="$work/sd" .pio/build/sim/program --headless $how --script 'wait 1500; quit' > "$work/log" 2>&1 ||
      fail "$how: the simulator did not end by itself the second time"
    grep -qiE 'abort|watchdog|out of memory' "$work/log" && fail "$how: the second log reports an abort"
    grep -q 'sd: 1000 found, 1000 shown, 0 with no sha256' "$work/log" ||
      fail "$how: the second start still has a package with no sha256"
  else
    grep -q ', sd 1000 (' "$work/log" || fail "(no flags): the start line does not report 1,000 packages"
  fi
  echo "${how:-(no flags)}: 1,000 packages"
done
echo ok
