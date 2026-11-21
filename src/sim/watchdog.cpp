// The board's task watchdog, modelled (--watchdog). On a board with its SD card on SPI (the Cardputer), a job task
// that reads or writes the card keeps its CPU: the SPI driver polls and never waits. Core 0's idle task then gets no
// time, and after 5 s the task watchdog restarts the board (last reset 6). The job lets the idle task run only where
// it waits: hal::idle, and a USB transfer (hal::storageCommand) that waits for the FM-1.
//
// The model counts the bytes that a job thread reads and writes through stdio after its last wait. Past the limit
// (--watchdog BYTES), it logs the count and where the job is, and ends the program as the board ends: at once, with
// nothing cleaned up. The Cardputer ADV read a 1 MiB file in less than 1.2 s (2026-11-19 log), so its 5 s are about
// 4 MiB: with that limit, the data restore of that day stops in datasave::refusal, as the board did. A test uses a
// smaller limit, for a margin.
//
// The functions fread and fwrite here replace the C library's for the program's own code (the linker takes a
// definition in the program before one in a library); they pass each call on to the C library's.
#include <dlfcn.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "hal.h"
#include "watchdog.h"

static uint64_t limit;  // 0: off
static thread_local bool job;        // this thread is a job's (hal::spawn)
static thread_local uint64_t since;  // bytes since the job's last wait

void watchdog::enable(uint64_t bytes) {
  limit = bytes;
}

void watchdog::jobThread() {
  job = true;
}

void watchdog::waited() {
  since = 0;
}

static void count(size_t bytes) {
  if (!limit || !job) {
    return;
  }
  since += bytes;
  if (since > limit) {
    hal::log("sim: watchdog: the job read and wrote %llu bytes on the SD card with no wait: the board restarts",
             (unsigned long long)since);
    void* at[32];
    backtrace_symbols_fd(at, backtrace(at, 32), 1);  // (where the board would stop, as its crash record has it)
    _exit(3);
  }
}

using Io = size_t (*)(void*, size_t, size_t, FILE*);

extern "C" size_t fread(void* __restrict p, size_t size, size_t n, FILE* __restrict f) {
  static Io real = (Io)dlsym(RTLD_NEXT, "fread");
  size_t got = real(p, size, n, f);
  count(got * size);
  return got;
}

extern "C" size_t fwrite(const void* __restrict p, size_t size, size_t n, FILE* __restrict f) {
  static Io real = (Io)dlsym(RTLD_NEXT, "fwrite");
  size_t put = real((void*)p, size, n, f);
  count(put * size);
  return put;
}
