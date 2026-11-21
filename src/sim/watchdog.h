// The board's task watchdog, modelled for the simulator's jobs (watchdog.cpp says how)
#pragma once

#include <stdint.h>

namespace watchdog {
void enable(uint64_t bytes);  // --watchdog BYTES: on for this run, past BYTES with no wait
void jobThread();             // the calling thread is a job's: its SD card bytes count
void waited();                // the job waited (the idle task ran): the count starts again
}  // namespace watchdog
