#include "identify.h"

#include <ctype.h>
#include <string.h>

#include <string>

namespace identify {
namespace {
// the names that the USB name can hold: the FM-1 and its loader, Felucca, and the keyboards on the FM-1's chip
const char* const NAMES[] = {"fm1", "fm-1", "felucca", "ota", "composite", "sinco", "usb-midi", "smk-37", "mke-p37", "starrykey"};
const uint32_t BUILD_REPLY = 47;  // the bytes of a reply to command 49
const uint32_t BUILD_AT = 14;     // where the build ID starts in it
const uint32_t BUILD_BYTES = 32;  // its length
}  // namespace
}  // namespace identify

bool identify::mayBeFm1(const char* name, uint16_t vid) {
  std::string lower;
  for (const char* c = name ? name : ""; *c; c++) {
    lower += (char)tolower((unsigned char)*c);
  }
  for (const char* n : NAMES) {
    if (lower.find(n) != std::string::npos) {
      return true;
    }
  }
  return vid == 0x1209;  // Felucca, its loader, and the firmwares built on it (each has its own PID)
}

std::string identify::buildId(const uint8_t* payload, uint32_t n) {
  if (n != BUILD_REPLY || memcmp(payload, "FMC1", 4)) {
    return "";
  }
  static const char HEX[] = "0123456789abcdef";
  std::string id;
  for (uint32_t i = BUILD_AT; i < BUILD_AT + BUILD_BYTES; i++) {
    id += HEX[payload[i] >> 4];
    id += HEX[payload[i] & 15];
  }
  return id;
}
