#!/bin/bash
# An install on the simulator's pretend FM-1 first makes a data save: /backups/switch holds the same bytes as the
# flash had before the install, and the flash after it is the package's. Run as it is and with a board's memory and no
# PSRAM (--lowmem): both must give the same data save and the same flash. Then the data of that firmware is lost
# (another firmware wrote its own), and a restore of the data save puts it back: the data sectors are as the data save
# has them, the flash head and the key_mac sector stay as they are. A damaged data save (changed bytes, not 1 MiB) is
# refused, and the flash stays as it is. A data save of the first pretend FM-1 is not put on the second one (another
# unique ID, --uid 2) with no transfer switch: no data save, no write. With the switch, the data of the second FM-1 goes
# to a data save first, the matching package is installed when it runs another build, and the data sectors are the data
# save's: its flash head (the package's after an install) and its key_mac sector stay. Uses the first .fwsc in sim/sd/fm1 (or
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
# the data saves of /backups/switch from the newest to the oldest, by the number line of the .txt file (an old data
# save has none: the N of its name), as "number name" lines
by_number() {
  for f in "$work"/sd/backups/switch/*.bin; do
    n=$(sed -n 's/^number //p' "${f%.bin}.txt")
    [ -n "$n" ] || n=$(basename "$f" | sed -n 's/^fm1-\([0-9]*\).*/\1/p')
    echo "${n:-0} $(basename "$f")"
  done | sort -rn
}
# one run on the card in $work/sd: prints the flash's sha256 when it ends
run() {
  FM1_SD="$work/sd" .pio/build/sim/program --headless --flash "$work/flash.bin" "$@" > "$work/log" 2>&1 || {
    grep -E 'sim:|job:' "$work/log" >&2  # (to stderr: a caller takes stdout into a variable)
    exit 1
  }
  grep 'sim: flash' "$work/log" | awk '{print $NF}'
}
for how in "" "--lowmem"; do
  rm -rf "$work/sd" "$work"/flash.bin*
  mkdir -p "$work/sd/fm1"
  cp "$sd/fm1/$pkg" "$work/sd/fm1/"
  before=$(run $how --backup)
  after=$(run $how --install "$pkg")
  [ "$before" != "$after" ] || fail "${how:-(no flags)}: the install left the flash as it was"
  count=$(ls "$work/sd/backups/switch" | grep -c '\.bin$' || true)
  [ "$count" = 1 ] || fail "${how:-(no flags)}: $count data saves in /backups/switch, not 1"
  bin=$(ls "$work/sd/backups/switch" | grep '\.bin$')
  txt="$work/sd/backups/switch/${bin%.bin}.txt"
  [ -f "$txt" ] || fail "${how:-(no flags)}: no .txt file for $bin"
  saved=$(shasum -a 256 "$work/sd/backups/switch/$bin" | cut -c1-64)
  [ "$saved" = "$before" ] || fail "${how:-(no flags)}: the data save differs from the flash before the install"
  grep -q "^sha256   $before\$" "$txt" || fail "${how:-(no flags)}: the .txt file has no sha256 of the data save"
  grep -q '^firmware FM-1_015$' "$txt" || fail "${how:-(no flags)}: the .txt file has no firmware identity"
  [ "$(ls "$work/sd/backups" | grep -c '\.bin$')" = 1 ] || fail "${how:-(no flags)}: the install changed /backups"
  echo "${how:-(no flags)}: $bin $saved, flash after $after"
  # a switch back: the install of the same package again makes a data save of the flash as the firmware left it
  again=$(run $how --install "$pkg")
  [ "$again" = "$after" ] || fail "${how:-(no flags)}: the install of the same package changed the flash"
  second=$(by_number | head -1 | cut -d' ' -f2)
  [ "$second" != "$bin" ] || fail "${how:-(no flags)}: no second data save"
  [ "$(shasum -a 256 "$work/sd/backups/switch/$second" | cut -c1-64)" = "$after" ] || fail "${how:-(no flags)}: the second data save differs from the flash"
  # the firmware that ran meanwhile changed the data sectors, the flash head, and the key_mac sector
  python3 -I - "$work/flash.bin" "$work/sd/backups/switch/$second" "$work/expected.bin" <<'PY'
import sys
flash = bytearray(open(sys.argv[1], 'rb').read())
saved = open(sys.argv[2], 'rb').read()
for a in range(0xD0000, 0xF0000):
    flash[a] ^= 0x5A
flash[0xFE000:0xFF000] = bytes(0x1000)
flash[0x100] ^= 0xFF
flash[0xFF010] ^= 0xFF
open(sys.argv[1], 'wb').write(flash)
# what the restore must leave: the data save's firmware and data sectors, the flash head and key_mac as they are now
expected = bytearray(flash)
expected[0x4000:0xFF000] = saved[0x4000:0xFF000]
open(sys.argv[3], 'wb').write(expected)
PY
  want=$(shasum -a 256 "$work/expected.bin" | cut -c1-64)
  switched=$(shasum -a 256 "$work/flash.bin" | cut -c1-64)
  [ "$switched" != "$after" ] || fail "${how:-(no flags)}: the test left the data as it was"
  restored=$(run $how --watchdog $((1024 * 1024)) --restore "$second")
  [ "$restored" = "$want" ] || fail "${how:-(no flags)}: after the restore the flash is not as the data save has its data sectors"
  [ -f "$work/sd/backups/switch/$second" ] || fail "${how:-(no flags)}: the restore removed its data save"
  [ "$(ls "$work/sd/backups/switch" | grep -c '\.bin$')" = 2 ] || fail "${how:-(no flags)}: the restore made a data save"
  # a data save with changed bytes (its sha256 differs), and one that is not 1 MiB: each is refused
  cp "$work/flash.bin" "$work/flash-before.bin"
  for damage in bytes size; do
    stem="${second%.bin}"
    cp "$work/sd/backups/switch/$second" "$work/sd/backups/switch/fm1-90-bad.bin"
    cp "$work/sd/backups/switch/$stem.txt" "$work/sd/backups/switch/fm1-90-bad.txt"
    if [ "$damage" = bytes ]; then
      printf '\377' | dd of="$work/sd/backups/switch/fm1-90-bad.bin" bs=1 seek=$((0xE0000)) conv=notrunc 2> /dev/null
    else
      truncate -s $((0x80000)) "$work/sd/backups/switch/fm1-90-bad.bin"
    fi
    if FM1_SD="$work/sd" .pio/build/sim/program --headless --flash "$work/flash.bin" $how --restore fm1-90-bad.bin > "$work/log" 2>&1; then
      fail "${how:-(no flags)}: a damaged data save ($damage) was restored"
    fi
    grep -q 'nothing was written' "$work/log" || fail "${how:-(no flags)}: a damaged data save ($damage): no reason"
    [ "$(shasum -a 256 "$work/flash.bin" | cut -c1-64)" = "$restored" ] || fail "${how:-(no flags)}: a damaged data save ($damage) changed the flash"
    [ "$(ls "$work/sd/backups/switch" | grep -c '\.bin$')" = 3 ] || fail "${how:-(no flags)}: a refused restore made or removed a data save ($damage)"
  done
  # a restore with the data save first: a new data save holds the data as it was, and the picked data save stays
  # (the flash is as the data save has it already: what is written is the same, the new data save is the news)
  rm -f "$work"/sd/backups/switch/fm1-90-bad.*
  cp "$work/flash.bin" "$work/flash-lost.bin"
  python3 -I - "$work/flash.bin" <<'PY'
import sys
flash = bytearray(open(sys.argv[1], 'rb').read())
for a in range(0xD0000, 0xF0000):
    flash[a] ^= 0xA5
open(sys.argv[1], 'wb').write(flash)
PY
  lost=$(shasum -a 256 "$work/flash.bin" | cut -c1-64)
  [ "$lost" != "$restored" ] || fail "${how:-(no flags)}: the test left the data as it was"
  # (the board's task watchdog, modelled: on the Cardputer ADV the restore read the data save, the flash copy, and the
  # package on the SD card for more than 5 s with no wait, and the watchdog restarted it. Each restore here may read
  # and write at most 1 MiB of the SD card between two waits)
  first=$(run $how --watchdog $((1024 * 1024)) --restore "$second" --save-first)
  [ "$first" = "$restored" ] || fail "${how:-(no flags)}: after the restore with a data save first the data sectors differ from the picked data save"
  [ "$(ls "$work/sd/backups/switch" | grep -c '\.bin$')" = 3 ] || fail "${how:-(no flags)}: the restore with a data save first made no new data save"
  newest=$(by_number | head -1 | cut -d' ' -f2)
  [ "$newest" != "$second" ] || fail "${how:-(no flags)}: no new data save"
  [ "$(shasum -a 256 "$work/sd/backups/switch/$newest" | cut -c1-64)" = "$lost" ] || fail "${how:-(no flags)}: the new data save differs from the data before the restore"
  [ -f "$work/sd/backups/switch/$second" ] || fail "${how:-(no flags)}: the restore removed the picked data save"
  # the oldest of 3 data saves of the running firmware: it stays, though it is the oldest of 4 then (the first data
  # save holds the flash from before the first install, which no package matches: it goes, and a copy takes its place)
  rm -f "$work/sd/backups/switch/$bin" "$work/sd/backups/switch/${bin%.bin}".txt
  cp "$work/sd/backups/switch/$second" "$work/sd/backups/switch/fm1-9-FM-1_015.bin"
  # (a copy that stands for an old data save: its .txt file has no number line and no default line)
  grep -v '^number \|^default ' "$work/sd/backups/switch/${second%.bin}.txt" > "$work/sd/backups/switch/fm1-9-FM-1_015.txt"
  oldest=$(by_number | tail -1 | cut -d' ' -f2)
  count=$(grep -l '^firmware FM-1_015$' "$work"/sd/backups/switch/*.txt | wc -l | tr -d ' ')
  [ "$count" = 3 ] || fail "${how:-(no flags)}: $count data saves of FM-1_015, not 3"
  run $how --restore "$oldest" --save-first > /dev/null
  [ -f "$work/sd/backups/switch/$oldest" ] || fail "${how:-(no flags)}: the restore removed the picked data save, the oldest"
  [ -f "$work/sd/backups/switch/${oldest%.bin}.txt" ] || fail "${how:-(no flags)}: the restore removed the .txt file of the picked data save"
  # a data save from another FM-1 (another unique ID): the job stops before its data save and writes nothing
  stem="${oldest%.bin}"
  cp "$work/sd/backups/switch/$oldest" "$work/sd/backups/switch/fm1-91-other.bin"
  sed 's/^flash id .*/flash id 0000000000000001/' "$work/sd/backups/switch/$stem.txt" > "$work/sd/backups/switch/fm1-91-other.txt"
  other_before=$(shasum -a 256 "$work/flash.bin" | cut -c1-64)
  bins=$(ls "$work/sd/backups/switch" | grep -c '\.bin$')
  if FM1_SD="$work/sd" .pio/build/sim/program --headless --flash "$work/flash.bin" $how --restore fm1-91-other.bin --save-first > "$work/log" 2>&1; then
    fail "${how:-(no flags)}: a data save of another FM-1 was restored"
  fi
  grep -q 'another FM-1' "$work/log" || fail "${how:-(no flags)}: another FM-1: no reason"
  [ "$(shasum -a 256 "$work/flash.bin" | cut -c1-64)" = "$other_before" ] || fail "${how:-(no flags)}: another FM-1: the flash changed"
  [ "$(ls "$work/sd/backups/switch" | grep -c '\.bin$')" = "$bins" ] || fail "${how:-(no flags)}: another FM-1: a data save was made or removed"
  rm -f "$work"/sd/backups/switch/fm1-91-other.*
  # a data save that no firmware in /fm1 matches: the job says so, writes nothing, and makes no data save
  cp "$work/sd/backups/switch/$oldest" "$work/sd/backups/switch/fm1-92-none.bin"
  cp "$work/sd/backups/switch/$stem.txt" "$work/sd/backups/switch/fm1-92-none.txt"
  printf '\377\377\377\377' | dd of="$work/sd/backups/switch/fm1-92-none.bin" bs=1 seek=$((0x4100)) conv=notrunc 2> /dev/null
  sha=$(shasum -a 256 "$work/sd/backups/switch/fm1-92-none.bin" | cut -c1-64)
  sed -i.orig "s/^sha256   .*/sha256   $sha/" "$work/sd/backups/switch/fm1-92-none.txt"
  rm -f "$work/sd/backups/switch/fm1-92-none.txt.orig"
  none_before=$(shasum -a 256 "$work/flash.bin" | cut -c1-64)
  bins=$(ls "$work/sd/backups/switch" | grep -c '\.bin$')
  if FM1_SD="$work/sd" .pio/build/sim/program --headless --flash "$work/flash.bin" $how --restore fm1-92-none.bin --save-first > "$work/log" 2>&1; then
    fail "${how:-(no flags)}: a data save with no matching package was restored"
  fi
  grep -q 'No firmware in /fm1' "$work/log" || fail "${how:-(no flags)}: no matching package: no reason"
  [ "$(shasum -a 256 "$work/flash.bin" | cut -c1-64)" = "$none_before" ] || fail "${how:-(no flags)}: no matching package: the flash changed"
  [ "$(ls "$work/sd/backups/switch" | grep -c '\.bin$')" = "$bins" ] || fail "${how:-(no flags)}: no matching package: a data save was made or removed"
  rm -f "$work"/sd/backups/switch/fm1-92-none.*
  # a transfer: the second pretend FM-1 (--uid 2: its own unique ID, flash head, and key_mac sector) gets the data of the
  # first. Its card holds the same firmware library; $work/sd is the card of the Cardputer that does the transfer
  sha() { shasum -a 256 "$1" | cut -c1-64; }
  rm -rf "$work/tsd" "$work"/target.bin*
  mkdir -p "$work/tsd/fm1"
  cp "$sd/fm1/$pkg" "$work/tsd/fm1/"
  FM1_SD="$work/tsd" .pio/build/sim/program --headless --uid 2 --flash "$work/target.bin" $how --install "$pkg" > "$work/log" 2>&1 || fail "${how:-(no flags)}: the install on the second FM-1 failed"
  cp "$work/target.bin" "$work/target-stock.bin"
  cp "$work/target.bin.ident" "$work/target-stock.bin.ident"
  # the data of the second FM-1 is not the data of the data save
  python3 -I - "$work/target.bin" <<'PY'
import sys
flash = bytearray(open(sys.argv[1], 'rb').read())
for a in range(0xD0000, 0xE0000):
    flash[a] ^= 0x33
open(sys.argv[1], 'wb').write(flash)
PY
  cp "$work/target.bin" "$work/target-before.bin"
  bins=$(ls "$work/sd/backups/switch" | grep -c '\.bin$')
  # without the transfer switch: another FM-1, no data save, no write
  if FM1_SD="$work/sd" .pio/build/sim/program --headless --uid 2 --flash "$work/target.bin" $how --restore "$second" --save-first > "$work/log" 2>&1; then
    fail "${how:-(no flags)}: a data save of another FM-1 was restored with no transfer"
  fi
  grep -q 'another FM-1' "$work/log" || fail "${how:-(no flags)}: no transfer: no reason"
  [ "$(sha "$work/target.bin")" = "$(sha "$work/target-before.bin")" ] || fail "${how:-(no flags)}: no transfer: the flash changed"
  [ "$(ls "$work/sd/backups/switch" | grep -c '\.bin$')" = "$bins" ] || fail "${how:-(no flags)}: no transfer: a data save was made or removed"
  # a data save that no firmware in /fm1 matches: no data save, no write
  cp "$work/sd/backups/switch/$second" "$work/sd/backups/switch/fm1-93-none.bin"
  cp "$work/sd/backups/switch/${second%.bin}.txt" "$work/sd/backups/switch/fm1-93-none.txt"
  printf '\377\377\377\377' | dd of="$work/sd/backups/switch/fm1-93-none.bin" bs=1 seek=$((0x4100)) conv=notrunc 2> /dev/null
  sed -i.orig "s/^sha256   .*/sha256   $(sha "$work/sd/backups/switch/fm1-93-none.bin")/" "$work/sd/backups/switch/fm1-93-none.txt"
  rm -f "$work/sd/backups/switch/fm1-93-none.txt.orig"
  bins=$(ls "$work/sd/backups/switch" | grep -c '\.bin$')
  if FM1_SD="$work/sd" .pio/build/sim/program --headless --uid 2 --flash "$work/target.bin" $how --restore fm1-93-none.bin --save-first --transfer > "$work/log" 2>&1; then
    fail "${how:-(no flags)}: a transfer with no matching package went on"
  fi
  grep -q 'No firmware in /fm1' "$work/log" || fail "${how:-(no flags)}: transfer, no package: no reason"
  [ "$(sha "$work/target.bin")" = "$(sha "$work/target-before.bin")" ] || fail "${how:-(no flags)}: transfer, no package: the flash changed"
  [ "$(ls "$work/sd/backups/switch" | grep -c '\.bin$')" = "$bins" ] || fail "${how:-(no flags)}: transfer, no package: a data save was made or removed"
  rm -f "$work"/sd/backups/switch/fm1-93-none.*
  # the second FM-1 runs the build of the data save: the transfer writes now. A data save of its data comes first
  ls "$work/sd/backups/switch" | grep '\.bin$' | sort > "$work/bins-before"
  FM1_SD="$work/sd" .pio/build/sim/program --headless --uid 2 --flash "$work/target.bin" $how --restore "$second" --transfer > "$work/log" 2>&1 || {
    grep -E 'sim:|job:' "$work/log"
    fail "${how:-(no flags)}: the transfer failed"
  }
  ! grep -q 'WROTE ITS HEAD OR KEY_MAC' "$work/log" || fail "${how:-(no flags)}: the transfer wrote the flash head or the key_mac sector"
  python3 -I - "$work/target.bin" "$work/target-before.bin" "$work/sd/backups/switch/$second" <<'PY' || fail "${how:-(no flags)}: the transfer: the flash is not as it must be"
import sys
now, before, saved = (open(a, 'rb').read() for a in sys.argv[1:4])
assert now[0x4000:0xFF000] == saved[0x4000:0xFF000], 'the data sectors are not the data save'
assert now[:0x4000] == before[:0x4000], 'the flash head changed'
assert now[0xFF000:] == before[0xFF000:], 'the key_mac sector changed'
assert before[0xFF000:] != saved[0xFF000:], 'the test has no own key_mac'  # (an install gives both the package's head)
PY
  # (old data saves of the same firmware go: the data saves that are new are what counts)
  ls "$work/sd/backups/switch" | grep '\.bin$' | sort > "$work/bins-after"
  [ "$(comm -13 "$work/bins-before" "$work/bins-after" | wc -l)" -eq 1 ] || fail "${how:-(no flags)}: the transfer made no data save, or more than one"
  made=$(comm -13 "$work/bins-before" "$work/bins-after")
  [ "$(sha "$work/sd/backups/switch/$made")" = "$(sha "$work/target-before.bin")" ] || fail "${how:-(no flags)}: the data save of the target is not its data before the transfer"
  [ -f "$work/sd/backups/switch/$second" ] || fail "${how:-(no flags)}: the transfer removed its data save"
  # the second FM-1 runs another build: the matching package is installed first, with no second data save
  cp "$work/target-stock.bin" "$work/target.bin"
  cp "$work/target-stock.bin.ident" "$work/target.bin.ident"
  python3 -I - "$work/target.bin" <<'PY'
import sys
flash = bytearray(open(sys.argv[1], 'rb').read())
for a in range(0x5000, 0x6000):
    flash[a] ^= 0x77  # another build
for a in range(0xD0000, 0xE0000):
    flash[a] ^= 0x33
open(sys.argv[1], 'wb').write(flash)
PY
  cp "$work/target.bin" "$work/target-before.bin"
  # what the install alone leaves on the second FM-1 (its flash head is the package's then)
  rm -rf "$work/csd"
  cp -R "$work/tsd" "$work/csd"
  cp "$work/target.bin" "$work/control.bin"
  cp "$work/target.bin.ident" "$work/control.bin.ident"
  FM1_SD="$work/csd" .pio/build/sim/program --headless --uid 2 --flash "$work/control.bin" $how --install "$pkg" > "$work/log" 2>&1 || fail "${how:-(no flags)}: the control install failed"
  ls "$work/sd/backups/switch" | grep '\.bin$' | sort > "$work/bins-before"
  FM1_SD="$work/sd" .pio/build/sim/program --headless --uid 2 --flash "$work/target.bin" $how --restore "$second" --transfer > "$work/log" 2>&1 || {
    grep -E 'sim:|job:' "$work/log"
    fail "${how:-(no flags)}: the transfer with an install failed"
  }
  ! grep -q 'WROTE ITS HEAD OR KEY_MAC' "$work/log" || fail "${how:-(no flags)}: the transfer with an install wrote the flash head or the key_mac sector"
  python3 -I - "$work/target.bin" "$work/control.bin" "$work/target-before.bin" "$work/sd/backups/switch/$second" <<'PY' || fail "${how:-(no flags)}: the transfer with an install: the flash is not as it must be"
import sys
now, control, before, saved = (open(a, 'rb').read() for a in sys.argv[1:5])
assert now[0x4000:0xFF000] == saved[0x4000:0xFF000], 'the data sectors are not the data save'
assert now[:0x4000] == control[:0x4000], 'the flash head is not what the install leaves'
assert now[0xFF000:] == before[0xFF000:], 'the key_mac sector changed'
assert before[0xFF000:] != saved[0xFF000:], 'the test has no own key_mac'
PY
  # (old data saves of the same firmware go: the data saves that are new are what counts)
  ls "$work/sd/backups/switch" | grep '\.bin$' | sort > "$work/bins-after"
  [ "$(comm -13 "$work/bins-before" "$work/bins-after" | wc -l)" -eq 1 ] || fail "${how:-(no flags)}: the transfer with an install made no data save, or more than one"
  made=$(comm -13 "$work/bins-before" "$work/bins-after")
  [ "$(sha "$work/sd/backups/switch/$made")" = "$(sha "$work/target-before.bin")" ] || fail "${how:-(no flags)}: the data save of the target is not its flash before the install"
  echo "${how:-(no flags)}: transfer ok"
  echo "${how:-(no flags)}: restored $restored"
  # each .txt file of a data save or a backup holds the sha256 of its own .bin (a hash of nothing, or of other bytes, fails)
  checked=0
  for t in "$work"/sd/backups/switch/*.txt "$work"/sd/backups/*.txt; do
    [ -f "$t" ] || continue
    recorded=$(sed -n 's/^sha256   //p' "$t")
    [ "$recorded" = "$(sha "${t%.txt}.bin")" ] || fail "${how:-(no flags)}: $t: the sha256 line is not the sha256 of its .bin"
    checked=$((checked + 1))
  done
  [ "$checked" -gt 0 ] || fail "${how:-(no flags)}: no .txt file to check"
  echo "$saved $after $restored" > "$work/result${how:+-lowmem}"
done
[ "$(cat "$work/result")" = "$(cat "$work/result-lowmem")" ] || fail "--lowmem: NOT THE SAME"
echo ok
