// The name test of the shared core: which USB name and vendor ID can be an FM-1, and the build ID of a reply. Plain logic: no hal, no midi, so a
// program on the computer tests it. The shared core asks only such a device for its firmware identity.
#pragma once

#include <stdint.h>

#include <string>

namespace identify {
// the USB name and the vendor ID can be an FM-1 (the name in any letter case)
bool mayBeFm1(const char* name, uint16_t vid);
// the build ID that the reply to command 49 holds (its payload, n bytes): 64 lower-case hex characters, or "" when the
// payload is not 47 bytes that start with the mark "FMC1"
std::string buildId(const uint8_t* payload, uint32_t n);
}  // namespace identify
