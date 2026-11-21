#include "datasave.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <map>

// the same addresses as uboot.h (this module calls no uboot function)
namespace datasave {
namespace {
const uint32_t SECTOR_BYTES = 0x1000;
const uint32_t FLASH_BYTES = 0x100000;  // 1 MiB
const uint32_t FIRMWARE_FROM = 0x4000;  // below it is the flash head, which no restore writes
const uint32_t KEY_MAC_AT = 0xFF000;    // from it up is key_mac, which no restore writes
const uint32_t SMK37_MARK_AT = 0xC1224;
const uint16_t FM1_CHIP_KEY = 0x980F;
}  // namespace
}  // namespace datasave

// the N of an old name ("fm1-4-FM-1_904.bin", "smk-9.bin"): 0 for any other name
static long oldNumber(const std::string& file) {
  bool old = !strncasecmp(file.c_str(), "fm1-", 4) || !strncasecmp(file.c_str(), "smk-", 4);
  return old && file.size() > 4 && isdigit((unsigned char)file[4]) ? atol(file.c_str() + 4) : 0;
}

long datasave::age(const Save& save) {
  return save.number ? save.number : oldNumber(save.file);
}

bool datasave::hasOwnName(const Save& save) {
  bool bin = save.file.size() > 4 && !strcasecmp(save.file.c_str() + save.file.size() - 4, ".bin");
  return !save.defaultStem.empty() && (bin ? save.file.substr(0, save.file.size() - 4) : save.file) != save.defaultStem;
}

// the name of the .txt file of a .bin file
static std::string textOf(const std::string& file) {
  return file.size() > 4 && !strcasecmp(file.c_str() + file.size() - 4, ".bin") ? file.substr(0, file.size() - 4) + ".txt"
                                                                                : file + ".txt";
}

std::vector<std::string> datasave::toRemove(const std::vector<Save>& saves, const std::string& keep) {
  std::map<std::string, std::vector<const Save*>> byFirmware;
  for (const Save& s : saves) {
    if (s.file != keep && !hasOwnName(s)) {
      byFirmware[s.firmware].push_back(&s);
    }
  }
  std::vector<std::string> out;
  for (auto& group : byFirmware) {
    std::vector<const Save*>& list = group.second;
    std::stable_sort(list.begin(), list.end(), [](const Save* a, const Save* b) { return age(*a) > age(*b); });
    for (size_t i = DATA_SAVES_KEPT; i < list.size(); i++) {
      out.push_back(list[i]->file);
      out.push_back(textOf(list[i]->file));
    }
  }
  return out;
}

datasave::Save datasave::fromText(const std::string& file, const std::string& txt) {
  Save save = {file, "", "", ""};
  size_t at = 0;
  while (at < txt.size()) {
    size_t end = txt.find('\n', at);
    if (end == std::string::npos) {
      end = txt.size();
    }
    std::string line = txt.substr(at, end - at);
    at = end + 1;
    while (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.compare(0, 9, "firmware ") == 0) {
      save.firmware = line.substr(9);
    } else if (line.compare(0, 9, "flash id ") == 0) {
      save.uid = line.substr(9);
    } else if (line.compare(0, 9, "sha256   ") == 0) {
      save.sha256 = line.substr(9);
    } else if (line.compare(0, 7, "number ") == 0) {
      save.number = atol(line.c_str() + 7);
    } else if (line.compare(0, 8, "default ") == 0) {
      save.defaultStem = line.substr(8);
    }
  }
  return save;
}

std::vector<datasave::Save> datasave::newestFirst(const std::vector<Save>& saves) {
  std::vector<Save> out = saves;
  std::stable_sort(out.begin(), out.end(), [](const Save& a, const Save& b) {
    long x = age(a), y = age(b);
    return x != y ? x > y : a.file < b.file;
  });
  return out;
}

bool datasave::sameFm1(const Save& save, const std::string& uid) {
  return !save.uid.empty() && save.uid == uid;
}

namespace datasave {

std::string matchSave(const std::vector<Save>& saves, const std::string& product, const std::string& uid) {
  const Save* best = nullptr;
  for (const Save& s : saves) {
    if (!uid.empty() && s.uid == uid && s.firmware == product && (!best || age(s) > age(*best))) {
      best = &s;
    }
  }
  return best ? best->file : "";
}

// the bytes of a and b from address from to address to are the same (false too when a reader fails)
static bool sameBytes(const Reader& a, const Reader& b, uint32_t from, uint32_t to) {
  std::vector<uint8_t> x(SECTOR_BYTES), y(SECTOR_BYTES);
  for (uint32_t at = from; at < to; at += SECTOR_BYTES) {
    uint32_t n = std::min(SECTOR_BYTES, to - at);
    if (!a(at, x.data(), n) || !b(at, y.data(), n) || memcmp(x.data(), y.data(), n)) {
      return false;
    }
  }
  return true;
}

int matchPackage(const Reader& save, const std::vector<Package>& packages) {
  for (size_t i = 0; i < packages.size(); i++) {
    uint32_t end = packages[i].end;
    if (end > FIRMWARE_FROM && end <= KEY_MAC_AT && sameBytes(save, packages[i].read, FIRMWARE_FROM, end)) {
      return (int)i;
    }
  }
  return -1;
}

Transfer transfer(const Reader& save, const Reader& flash, const std::vector<Package>& packages) {
  int i = matchPackage(save, packages);
  if (i < 0) {
    return {TRANSFER_NO_PACKAGE, -1};
  }
  return {sameBytes(save, flash, FIRMWARE_FROM, packages[i].end) ? TRANSFER_WRITE : TRANSFER_INSTALL, i};
}

std::vector<uint32_t> sectors(const Reader& save, const Reader& flash, uint32_t imageEnd, bool* failed) {
  std::vector<uint32_t> out;
  std::vector<uint8_t> x(SECTOR_BYTES), y(SECTOR_BYTES);
  if (failed) {
    *failed = false;
  }
  for (uint32_t at = std::max(imageEnd - imageEnd % SECTOR_BYTES, FIRMWARE_FROM); at < KEY_MAC_AT; at += SECTOR_BYTES) {
    if (!save(at, x.data(), SECTOR_BYTES) || !flash(at, y.data(), SECTOR_BYTES)) {
      if (failed) {
        *failed = true;
      }
      return out;
    }
    if (memcmp(x.data(), y.data(), SECTOR_BYTES)) {
      out.push_back(at);
    }
  }
  return out;
}

// "trimdat" at the SMK-37's mark: 1 yes, 0 no, -1 the reader failed
static int smkMark(const Reader& r) {
  uint8_t m[7];
  if (!r(SMK37_MARK_AT, m, sizeof m)) {
    return -1;
  }
  return !memcmp(m, "trimdat", 7);
}

Refusal refusal(const Restore& r, const Reader& save, const Reader& flash) {
  if (r.fileSize != FLASH_BYTES) {
    return REFUSE_FILE_SIZE;
  }
  if (r.fileSha256.size() != r.savedSha256.size() || strcasecmp(r.fileSha256.c_str(), r.savedSha256.c_str())) {
    return REFUSE_SHA256;
  }
  if (r.chipKey != FM1_CHIP_KEY) {
    return REFUSE_CHIP;
  }
  if (r.flashSize != FLASH_BYTES) {
    return REFUSE_FLASH_SIZE;
  }
  int inSave = smkMark(save);
  if (inSave > 0) {
    return REFUSE_SMK37_SAVE;
  }
  int onDevice = smkMark(flash);
  if (onDevice > 0) {
    return REFUSE_SMK37_DEVICE;
  }
  if (inSave < 0 || onDevice < 0) {
    return REFUSE_UNREADABLE;
  }
  // with no package (imageEnd 0) there is no firmware range to compare: the caller stops for that reason
  if (r.imageEnd > FIRMWARE_FROM) {
    uint32_t end = std::min(r.imageEnd, FLASH_BYTES);
    std::vector<uint8_t> x(SECTOR_BYTES), y(SECTOR_BYTES);
    for (uint32_t at = FIRMWARE_FROM; at < end; at += SECTOR_BYTES) {
      uint32_t n = std::min(SECTOR_BYTES, end - at);
      if (!save(at, x.data(), n) || !flash(at, y.data(), n)) {
        return REFUSE_UNREADABLE;
      }
      if (memcmp(x.data(), y.data(), n)) {
        return REFUSE_OTHER_BUILD;
      }
    }
  }
  return REFUSE_NONE;
}

const char* reason(Refusal r) {
  switch (r) {
    case REFUSE_FILE_SIZE:
      return "That data save isn't all of a flash (1 MB): nothing was written.";
    case REFUSE_SHA256:
      return "That data save isn't what its .txt file says it is (sha256): nothing was written.";
    case REFUSE_OTHER_BUILD:
      return "The FM-1 runs another build of the firmware than the data save: nothing was written.";
    case REFUSE_SMK37_DEVICE:
      return "This is an SMK-37, not an FM-1: nothing was written.";
    case REFUSE_SMK37_SAVE:
      return "That data save is from an SMK-37, not an FM-1: nothing was written.";
    case REFUSE_CHIP:
      return "That isn't the FM-1's chip: nothing was written.";
    case REFUSE_FLASH_SIZE:
      return "That isn't a flash of 1 MB: nothing was written.";
    case REFUSE_UNREADABLE:
      return "Can't read it back from the SD card: nothing was written.";
    case REFUSE_NONE:
      break;
  }
  return "";
}

}  // namespace datasave
