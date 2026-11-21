#!/bin/bash
# The default name of a data save and of a backup on the simulator's pretend FM-1: the firmware and the date of today
# (UTC), "<firmware>_<date>.bin". The first install finds no install record, so the firmware part is the firmware
# identity of the pretend FM-1. The second install finds the record of the stock package, so the firmware part is the
# display name of that package (the script reads it from the run, because the cache of the firmware library decides
# it). A second file on one day gets -2. A data save holds a number line and a default line, and a backup holds a
# number line. A backup with a typed name takes it as an own name, a rename moves a data save's .bin and .txt file
# together (and adds the lines that an old data save lacks), and a restore of the renamed data save puts the data back.
# Run as it is and with a board's memory and no PSRAM (--lowmem). Uses the first .fwsc in sim/sd/fm1 (or
# FM1_SD/fm1); with none, the Hub's stock firmware is got first.
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
# one run on the card in $work/sd
run() {
  FM1_SD="$work/sd" .pio/build/sim/program --headless --flash "$work/flash.bin" "$@" > "$work/log" 2>&1 || {
    grep -E 'sim:|job:' "$work/log" >&2
    exit 1
  }
}
# the data saves of /backups/switch, one name a line
saves() {
  ls "$work/sd/backups/switch" | grep '\.bin$' | sort || true
}
# the file name that a firmware part makes safe: a character that is not a letter, a digit, - _ or . becomes -
safe() {
  printf '%s' "$1" | sed 's/[^A-Za-z0-9._-]/-/g' | cut -c1-20
}
for how in "" "--lowmem"; do
  label=${how:-(no flags)}
  rm -rf "$work/sd" "$work"/flash.bin*
  mkdir -p "$work/sd/fm1"
  cp "$sd/fm1/$pkg" "$work/sd/fm1/"
  today=$(date -u +%F)
  # the first install: no install record yet, so the firmware part is the firmware identity of the pretend FM-1
  run $how --install "$pkg"
  [ "$(saves)" = "FM-1_015_$today.bin" ] || fail "$label: the first data save is '$(saves)', not FM-1_015_$today.bin"
  txt="$work/sd/backups/switch/FM-1_015_$today.txt"
  grep -q '^number 1$' "$txt" || fail "$label: the first data save has no 'number 1' line"
  grep -q "^default FM-1_015_$today\$" "$txt" || fail "$label: the first data save has no default line with its stem"
  # the second install: the install record names the stock package, so the firmware part is its display name
  run $how --install "$pkg"
  # (the log does not give the display name, and the Hub list can change it: the script reads the firmware part from
  # the new data save, and checks the rest of its name)
  made=$(saves | grep -v "^FM-1_015_$today\.bin\$" || true)
  [ "$(printf '%s\n' "$made" | grep -c .)" = 1 ] || fail "$label: the second install made no single new data save: $(saves | tr '\n' ' ')"
  case "$made" in
    *_"$today".bin) ;;
    *) fail "$label: the second data save '$made' does not end in _$today.bin" ;;
  esac
  part=${made%_"$today".bin}
  [ -n "$part" ] || fail "$label: the second data save has no firmware part"
  [ "$(safe "$part")" = "$part" ] || fail "$label: the firmware part '$part' holds a character that a file name cannot hold"
  second="${part}_$today"
  [ -f "$work/sd/backups/switch/$second.bin" ] || fail "$label: no data save $second.bin after the second install: $(saves | tr '\n' ' ')"
  grep -q '^number 2$' "$work/sd/backups/switch/$second.txt" || fail "$label: the second data save has no 'number 2' line"
  grep -q "^default $second\$" "$work/sd/backups/switch/$second.txt" || fail "$label: the second data save has no default line with its stem"
  # the third install, on the same day: -2, and its default line holds the -2
  run $how --install "$pkg"
  [ -f "$work/sd/backups/switch/$second-2.bin" ] || fail "$label: no data save $second-2.bin after the third install: $(saves | tr '\n' ' ')"
  grep -q '^number 3$' "$work/sd/backups/switch/$second-2.txt" || fail "$label: the third data save has no 'number 3' line"
  grep -q "^default $second-2\$" "$work/sd/backups/switch/$second-2.txt" || fail "$label: the third data save has no default line with -2"
  # a backup: the same form, in /backups, with a number line and no default line
  run $how --backup
  run $how --backup
  [ -f "$work/sd/backups/$second.bin" ] || fail "$label: no backup $second.bin"
  [ -f "$work/sd/backups/$second-2.bin" ] || fail "$label: no backup $second-2.bin"
  [ "$(ls "$work/sd/backups" | grep -c '\.bin$')" = 2 ] || fail "$label: /backups does not hold 2 backups"
  grep -q '^number 1$' "$work/sd/backups/$second.txt" || fail "$label: the first backup has no 'number 1' line"
  grep -q '^number 2$' "$work/sd/backups/$second-2.txt" || fail "$label: the second backup has no 'number 2' line"
  ! grep -q '^default ' "$work/sd/backups/$second.txt" || fail "$label: a backup has a default line"
  # a backup with a typed name: the own name, with each character that a file name cannot hold changed to -
  run $how --backup --name "Studio A!"
  [ -f "$work/sd/backups/Studio-A-.bin" ] || fail "$label: no backup Studio-A-.bin: $(ls "$work/sd/backups" | tr '\n' ' ')"
  [ -f "$work/sd/backups/Studio-A-.txt" ] || fail "$label: no Studio-A-.txt"
  grep -q '^number 3$' "$work/sd/backups/Studio-A-.txt" || fail "$label: Studio-A-.txt has no 'number 3' line"
  # a rename of a data save: its .bin file and its .txt file together, with the .txt file as it was
  sw="$work/sd/backups/switch"
  cp "$sw/$second.txt" "$work/second.txt"
  run $how --rename "/backups/switch/$second.bin" "My Jiant"
  grep -q 'sim: renamed My-Jiant.bin$' "$work/log" || fail "$label: the rename did not log My-Jiant.bin"
  [ -f "$sw/My-Jiant.bin" ] && [ -f "$sw/My-Jiant.txt" ] || fail "$label: no My-Jiant.bin and My-Jiant.txt: $(saves | tr '\n' ' ')"
  [ ! -e "$sw/$second.bin" ] && [ ! -e "$sw/$second.txt" ] || fail "$label: the rename left $second"
  cmp -s "$work/second.txt" "$sw/My-Jiant.txt" || fail "$label: the rename changed the .txt file"
  # a second file with the same name gets -2
  run $how --rename "/backups/switch/$second-2.bin" "My Jiant"
  [ -f "$sw/My-Jiant-2.bin" ] && [ -f "$sw/My-Jiant-2.txt" ] || fail "$label: no My-Jiant-2: $(saves | tr '\n' ' ')"
  # a rename to the same name, and to an empty name, changes nothing
  run $how --rename "/backups/switch/My-Jiant.bin" "My-Jiant"
  run $how --rename "/backups/switch/My-Jiant.bin" ""
  [ -f "$sw/My-Jiant.bin" ] && [ ! -e "$sw/My-Jiant-3.bin" ] || fail "$label: a rename with no change changed the name"
  # a rename that changes only the case is a rename
  run $how --rename "/backups/switch/My-Jiant-2.bin" "my-jiant-2"
  ls "$sw" | grep -q '^my-jiant-2\.bin$' && ls "$sw" | grep -q '^my-jiant-2\.txt$' || fail "$label: the case rename failed: $(ls "$sw" | tr '\n' ' ')"
  # an old data save with no number line and no default line: the rename writes both, so it keeps its age
  grep -v -e '^number ' -e '^default ' "$work/second.txt" > "$sw/fm1-3-FM-1_015.txt"
  cp "$sw/My-Jiant.bin" "$sw/fm1-3-FM-1_015.bin"
  run $how --rename "/backups/switch/fm1-3-FM-1_015.bin" "Gig set"
  [ -f "$sw/Gig-set.bin" ] && [ -f "$sw/Gig-set.txt" ] || fail "$label: no Gig-set: $(saves | tr '\n' ' ')"
  grep -q '^number 3$' "$sw/Gig-set.txt" || fail "$label: Gig-set.txt has no 'number 3' line"
  grep -q '^default fm1-3-FM-1_015$' "$sw/Gig-set.txt" || fail "$label: Gig-set.txt has no default line with the old stem"
  grep -q '^sha256 ' "$sw/Gig-set.txt" || fail "$label: Gig-set.txt lost its sha256 line"
  # an old backup with no .txt file gets a new .txt file
  cp "$work/sd/backups/$second.bin" "$work/sd/backups/fm1-2.bin"
  run $how --rename "/backups/fm1-2.bin" "Old one"
  grep -q '^number 2$' "$work/sd/backups/Old-one.txt" && grep -q '^default fm1-2$' "$work/sd/backups/Old-one.txt" || fail "$label: Old-one.txt lacks its number and default lines"
  # the data sectors change, then a restore of the renamed data save puts them back
  python3 -I - "$work/flash.bin" "$sw/My-Jiant.bin" "$work/expected.bin" <<'PY'
import sys
flash = bytearray(open(sys.argv[1], 'rb').read())
saved = open(sys.argv[2], 'rb').read()
for a in range(0xD0000, 0xF0000):
    flash[a] ^= 0x5A
open(sys.argv[1], 'wb').write(flash)
expected = bytearray(flash)
expected[0x4000:0xFF000] = saved[0x4000:0xFF000]
open(sys.argv[3], 'wb').write(expected)
PY
  run $how --watchdog $((1024 * 1024)) --restore My-Jiant.bin
  [ "$(shasum -a 256 "$work/flash.bin" | cut -c1-64)" = "$(shasum -a 256 "$work/expected.bin" | cut -c1-64)" ] || fail "$label: the restore of My-Jiant.bin did not put the data back"
  [ -f "$sw/My-Jiant.bin" ] && [ -f "$sw/Gig-set.bin" ] || fail "$label: the restore removed a data save with an own name"
  echo "$label: $(saves | tr '\n' ' ')"
done
echo ok
