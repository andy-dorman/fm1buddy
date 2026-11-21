#include "memory.h"

#include <stdlib.h>

#include "hal.h"

bool lowMemory = false;

bool roomFor(size_t n) {
  void* p = malloc(n);
  free(p);
  return p;
}

void sdPace(size_t n) {
  static thread_local size_t since;  // bytes of this task since its last hal::idle
  since += n;
  if (since >= 64 * 1024) {
    since = 0;
    hal::idle();
  }
}
