#include "savename.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <algorithm>

namespace savename {
namespace {
const size_t FIRMWARE_PART_MAX = 20;
const size_t STEM_MAX = 32;  // a default name stem with no word
const size_t OWN_NAME_MAX = 32;

// the N of an old name: "fm1-3-FM-1_904.bin" and "smk-9.bin" (0: not an old name)
long oldNumber(const std::string& name) {
  size_t from = !strncasecmp(name.c_str(), "fm1-", 4) ? 4 : !strncasecmp(name.c_str(), "smk-", 4) ? 4
                                                                                                  : 0;
  return from && from < name.size() && isdigit((unsigned char)name[from]) ? atol(name.c_str() + from) : 0;
}

// the N of a name with no date or an old name: "Jiant-FM_no-date-4_before-restore.json", "voice-3.syx" (0: none)
long numberOfName(const std::string& name) {
  static const char* const OLD[] = {"voice-", "setup-", "backup-", "song-", "before-restore-", "before-song-"};
  for (const char* prefix : OLD) {
    size_t k = strlen(prefix);
    if (!strncmp(name.c_str(), prefix, k) && isdigit((unsigned char)name[k])) {
      return atol(name.c_str() + k);
    }
  }
  size_t from = std::string::npos;
  if (!name.compare(0, 8, "no-date-")) {
    from = 8;
  } else if (size_t at = name.find("_no-date-"); at != std::string::npos) {
    from = at + 9;
  }
  return from != std::string::npos && isdigit((unsigned char)name[from]) ? atol(name.c_str() + from) : 0;
}

std::string safe(const std::string& text) {
  std::string out;
  for (char c : text) {
    out += isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' ? c : '-';
  }
  return out;
}
}  // namespace

const char* extension(Kind kind) {
  static const char* const EXT[] = {".bin", ".bin", ".syx", "", ".json", ".sloopsong"};
  return EXT[kind];
}

std::string dateText(time_t t) {
  if (t <= 1600000000) {
    return "";
  }
  struct tm utc;
  gmtime_r(&t, &utc);
  char out[16];
  strftime(out, sizeof out, "%Y-%m-%d", &utc);
  return out;
}

std::string firmwarePart(const std::string& identity, const std::string& displayName, const std::string& product,
                         bool smk) {
  std::string part = identity.empty()                              ? (smk ? "SMK-37" : "FM-1")
                     : !displayName.empty() && product == identity ? displayName
                                                                   : identity;
  return safe(part).substr(0, FIRMWARE_PART_MAX);
}

std::string defaultStem(const std::string& firmwarePart, const std::string& date, long n, const std::string& word) {
  std::string when = date.empty() ? "no-date-" + std::to_string(n) : date;
  // "_" and the date part leave the rest of the 32 characters for the firmware part
  size_t room = STEM_MAX > when.size() + 1 ? STEM_MAX - when.size() - 1 : 0;
  std::string part = firmwarePart.substr(0, std::min(FIRMWARE_PART_MAX, room));
  return part + "_" + when + (word.empty() ? "" : "_" + word);
}

long saveNumber(const Numbered& file) {
  return file.number ? file.number : oldNumber(file.name);
}

long nextSaveNumber(const std::vector<Numbered>& folder) {
  long highest = 0;
  for (const Numbered& f : folder) {
    highest = std::max(highest, saveNumber(f));
  }
  return highest + 1;
}

std::string freeName(const std::string& stem, const std::string& ext, const std::vector<std::string>& taken) {
  for (int n = 1;; n++) {
    std::string name = n == 1 ? stem : stem + "-" + std::to_string(n);
    bool used = false;
    for (const std::string& t : taken) {
      used = used || (t.size() == name.size() + ext.size() && !strcasecmp(t.c_str(), (name + ext).c_str()));
    }
    if (!used) {
      return name;
    }
  }
}

bool newer(const Numbered& a, const Numbered& b) {
  long x = saveNumber(a), y = saveNumber(b);
  return x != y ? x > y : a.name < b.name;
}

long nextNumber(const std::vector<std::string>& names) {
  long highest = 0;
  for (const std::string& n : names) {
    highest = std::max(highest, numberOfName(n));
  }
  return highest + 1;
}

bool newer(const Timed& a, const Timed& b) {
  bool x = a.time > 1600000000, y = b.time > 1600000000;
  if (x != y) {
    return x;
  }
  if (x && a.time != b.time) {
    return a.time > b.time;
  }
  long p = numberOfName(a.name), q = numberOfName(b.name);
  return !x && p != q ? p > q : a.name > b.name;
}

std::string ownName(const std::string& typed) {
  std::string name = safe(typed).substr(0, OWN_NAME_MAX);
  if (!name.empty() && name[0] == '.') {
    name[0] = '-';
  }
  return name;
}

std::string stemOf(const std::string& typed, const std::string& defaultStem) {
  return typed.empty() ? defaultStem : ownName(typed);
}

std::string renameStem(const std::string& current, const std::string& typed, const std::string& ext,
                       const std::vector<std::string>& taken) {
  if (typed.empty() || typed == current) {
    return current;
  }
  std::string own = ownName(typed);
  if (own == current) {
    return current;
  }
  std::vector<std::string> names = taken;
  for (std::string& n : names) {
    if (n.size() > 4 && !strcasecmp(n.c_str() + n.size() - 4, ".txt")) {
      n = n.substr(0, n.size() - 4) + ext;
    }
  }
  return freeName(own, ext, names);
}

long numberOfSaved(const std::string& dir, const std::string& bin) {
  FILE* f = fopen((dir + "/" + bin.substr(0, bin.size() > 4 ? bin.size() - 4 : 0) + ".txt").c_str(), "r");
  long number = 0;
  char line[200];
  while (f && fgets(line, sizeof line, f)) {
    if (!strncmp(line, "number ", 7)) {
      number = atol(line + 7);
    }
  }
  if (f) {
    fclose(f);
  }
  return number;
}
}  // namespace savename
