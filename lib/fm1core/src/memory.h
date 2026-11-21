// Memory, on a board with PSRAM (fm1buddy's) or without (cardbuddy's Cardputer): what doesn't fit stays on the SD card
// (FlashBuf, Pkg), and a big file read whole gets an error where there's no room for it, not a crash
#pragma once

#include <stddef.h>

// always as if there were no room (the simulator's --lowmem: a board with no PSRAM)
extern bool lowMemory;
// n bytes would fit in memory now
bool roomFor(size_t n);
// The calling task read or wrote n bytes of the SD card. On a board with its card on SPI (the Cardputer), a read or a
// write does not wait: a job that reads the card for a long time keeps its CPU, the idle task of that CPU does not run,
// and after 5 s the task watchdog restarts the board. So after each 64 KiB of a task, hal::idle lets the idle task run
void sdPace(size_t n);
