#include "catalog.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>

#include "fm1.h"
#include "hal.h"
#include "memory.h"
#include "syncplan.h"

FwList catalog::hub, catalog::sd;
std::string catalog::hubUpdated;

std::string catalog::path(const std::string& p) {
  const char* root = hal::sd();
  return root ? root + p : "";
}

static bool readAll(const std::string& p, std::string& out) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) {
    return false;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n > 0 && !roomFor(n)) {
    fclose(f);
    return false;
  }
  out.resize(n > 0 ? n : 0);
  bool ok = n > 0 && fread(&out[0], 1, n, f) == (size_t)n;
  fclose(f);
  return ok;
}

static std::string baseName(const std::string& p) {
  size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

// the screen's fonts are ASCII: accented letters lose their accents, curly quotes and dashes straighten
static std::string ascii(const char* s) {
  static const char* LATIN =  // U+00C0 to U+017F
      "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"
      "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
  std::string o;
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint32_t c = *p;
    int n = c < 0x80 ? 0 : c >= 0xF0 ? 3
                       : c >= 0xE0   ? 2
                       : c >= 0xC0   ? 1
                                     : 0;
    c &= n == 3 ? 0x07 : n == 2 ? 0x0F
                     : n == 1   ? 0x1F
                                : 0xFF;
    p++;
    for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++) {
      c = c << 6 | (*p++ & 0x3F);
    }
    if (c < 0x80) {
      o += (char)c;
    } else if (c >= 0xC0 && c <= 0x17F) {
      o += LATIN[c - 0xC0];
      if (c == 0xDF)
        o += 's';  // ss
    } else if (c == 0x2018 || c == 0x2019) {
      o += '\'';
    } else if (c == 0x201C || c == 0x201D) {
      o += '"';
    } else if (c == 0x2013 || c == 0x2014) {
      o += '-';
    } else if (c == 0x2026) {
      o += "...";
    } else if (c == 0xA0) {
      o += ' ';
    } else if (c >= 0x1F000 || (c >= 0x2600 && c <= 0x27BF) || c == 0xFE0F) {
      // emoji: left out
    } else {
      o += '?';
    }
  }
  return o;
}

static void readEntry(JsonObjectConst e, Firmware& f) {
  f.name = ascii(e["name"] | "?");
  f.author = ascii(e["author"] | "");
  f.version = e["version"] | "";
  f.date = e["date"] | "";
  f.what = ascii(e["what"] | "");
  f.pkg = e["pkg"] | "";
  f.file = baseName(e["file"] | f.pkg.c_str());
  f.product = e["product"] | "";
  f.sha256 = e["sha256"] | "";
  f.size = e["size"] | 0;
  f.beta = e["beta"] | false;
  f.stock = e["stock"] | false;
  f.keyboard = e["hid"]["keyboard"] | false;
  f.mouse = e["hid"]["mouse"] | false;
  f.chip = e["chip"] | "";
  JsonVariantConst d = e["devices"];
  if (d.is<const char*>()) {
    f.devices.push_back(d.as<const char*>());
  } else {
    for (JsonVariantConst v : d.as<JsonArrayConst>()) {
      f.devices.push_back(v | "");
    }
  }
  JsonVariantConst k = e["kind"];
  if (k.is<const char*>()) {
    f.kinds.push_back(k.as<const char*>());
  } else {
    for (JsonVariantConst v : k.as<JsonArrayConst>()) {
      f.kinds.push_back(v | "");
    }
  }
}

// ------------------------------------------------------------------------------------------- FwList ---
uint32_t FwList::add(const char* s, size_t n) {
  if (!n) {
    return 0;  // (the "" at 0)
  }
  uint32_t at = text.size();
  text.insert(text.end(), s, s + n);
  text.push_back(0);
  return at;
}

uint32_t FwList::kindBit(const std::string& k) {
  for (size_t i = 0; i < kinds.size(); i++) {
    if (kinds[i] == k) {
      return 1u << i;
    }
  }
  if (k.empty() || kinds.size() >= 32) {
    return 0;
  }
  kinds.push_back(k);
  return 1u << (kinds.size() - 1);
}

uint32_t FwList::changes;

void FwList::clear() {
  changes++;
  items.clear();
  text.assign(1, 0);
  kinds.clear();
}

void FwList::swap(FwList& o) {
  changes++;
  items.swap(o.items);
  text.swap(o.text);
  kinds.swap(o.kinds);
}

// big from the start: in the PSRAM (blocks over 4 KB go there), not the internal RAM the SD card and Wi-Fi need. Not on a
// board with no PSRAM (lowMemory): it would take the internal RAM itself, so the list grows as it needs
static void roomy(FwList& l) {
  if (lowMemory) {
    return;
  }
  l.items.reserve(256);
  l.text.reserve(32768);
}

// ------------------------------------------------------------------------------------------ HUB_LIST ---
// the list as the board needs it: a line "fm1buddy hub 2" and when the list was made, then a line a firmware, its
// fields between tabs: name author version date what pkg file product sha256 size flags(b s k m) kinds(a,b) chip
// devices(fm1,smk37). Its line format is in syncplan.h
static const char* const LIST_HEAD = HUB_LIST_HEAD;
#define LIST_FIELDS HUB_LIST_FIELDS

static void field(FILE* f, const std::string& v, char end = '\t') {
  for (char c : v) {
    fputc(c == '\t' || c == '\n' || c == '\r' ? ' ' : c, f);
  }
  fputc(end, f);
}

static void writeLine(FILE* f, const Firmware& e) {
  for (const std::string* v : {&e.name, &e.author, &e.version, &e.date, &e.what, &e.pkg, &e.file, &e.product, &e.sha256}) {
    field(f, *v);
  }
  fprintf(f, "%lu\t%c%c%c%c\t", (unsigned long)e.size, e.beta ? 'b' : '-', e.stock ? 's' : '-', e.keyboard ? 'k' : '-',
          e.mouse ? 'm' : '-');
  std::string kinds;
  for (const std::string& k : e.kinds) {
    kinds += (kinds.empty() ? "" : ",") + k;
  }
  field(f, kinds);
  field(f, e.chip);
  std::string devices;
  for (const std::string& d : e.devices) {
    devices += (devices.empty() ? "" : ",") + d;
  }
  field(f, devices, '\n');
}

using syncplan::readLine;
using syncplan::split;

bool catalog::keepHub = true;
bool catalog::keepSd = true;

// HUB_LIST open, past its first line (when it was made, into when): 0 if it isn't there, or isn't one
static FILE* openList(std::string& when, uint32_t& at) {
  FILE* f = fopen(catalog::path(HUB_LIST).c_str(), "r");
  if (!f) {
    return 0;
  }
  std::string line;
  const char* v[3];
  if (!readLine(f, line) || split(line, v, 3) != 2 || strcmp(v[0], LIST_HEAD)) {
    fclose(f);
    return 0;
  }
  when = v[1];
  when.erase(when.find_last_not_of(' ') + 1);  // (parseHub leaves room for it)
  at = line.size() + 1;                        // where the next line starts (line: cut at its tabs, not shorter)
  return f;
}

// a list of one, emptied for the next (not FwList::clear: that's a change to a list on screen, FwList::changes)
static void empty(FwList& one) {
  one.items.clear();
  one.text.assign(1, 0);
  one.kinds.clear();
}

// a line of HUB_LIST (from here in it) onto list: false if it isn't a firmware's
static bool addLine(std::string& line, uint32_t here, FwList& list) {
  const char* v[LIST_FIELDS + 1];
  if (split(line, v, LIST_FIELDS + 1) != LIST_FIELDS) {
    return false;
  }
  FwItem it{};
  it.name = list.add(v[0], strlen(v[0]));
  it.author = list.add(v[1], strlen(v[1]));
  it.version = list.add(v[2], strlen(v[2]));
  it.date = list.add(v[3], strlen(v[3]));
  it.file = list.add(v[6], strlen(v[6]));
  it.product = list.add(v[7], strlen(v[7]));
  it.sha256 = list.add(v[8], strlen(v[8]));
  it.line = here;
  it.size = strtoul(v[9], 0, 10);
  it.flags = (strchr(v[10], 'b') ? FwItem::BETA : 0) | (strchr(v[10], 's') ? FwItem::STOCK : 0) |
             (!strcmp(v[12], "old") ? FwItem::CHIP_OLD : 0) | (syncplan::forFm1(v[13], v[7]) ? FwItem::FOR_FM1 : 0) |
             (syncplan::forSmk37(v[13], v[7]) ? FwItem::FOR_SMK37 : 0);
  for (const char* k = v[11]; *k;) {
    const char* c = strchr(k, ',');
    size_t n = c ? c - k : strlen(k);
    it.kinds |= list.kindBit(std::string(k, n));
    k += n + (c ? 1 : 0);
  }
  list.items.push_back(it);
  return true;
}

bool catalog::loadHub(FwList& out, std::string& updated) {
  std::string when, line;
  uint32_t at;
  FILE* f = openList(when, at);
  if (!f) {
    return false;
  }
  FwList list;
  roomy(list);
  for (size_t i = 0; readLine(f, line); i++) {
    uint32_t here = at;
    at += line.size() + 1;
    if (!(i & 15)) {
      hal::idle();  // (the list is long: let the board's idle task run)
    }
    addLine(line, here, list);
  }
  fclose(f);
  out.swap(list);
  updated = when;
  return true;
}

bool catalog::hubWhen(std::string& updated) {
  uint32_t at;
  FILE* f = openList(updated, at);
  if (f) {
    fclose(f);
  }
  return f;
}

bool catalog::eachHub(bool (*fn)(const FwList& one, void* arg), void* arg) {
  std::string when, line;
  uint32_t at;
  FILE* f = openList(when, at);
  if (!f) {
    return false;
  }
  FwList one;
  for (size_t i = 0; readLine(f, line); i++) {
    uint32_t here = at;
    at += line.size() + 1;
    if (!(i & 15)) {
      hal::idle();
    }
    empty(one);
    if (addLine(line, here, one) && !fn(one, arg)) {
      break;
    }
  }
  fclose(f);
  return true;
}

FILE* catalog::openHub() {
  return fopen(path(HUB_LIST).c_str(), "r");
}

bool catalog::hubAt(FILE* f, uint32_t line, FwList& one) {
  std::string text;
  empty(one);
  return f && !fseek(f, line, SEEK_SET) && readLine(f, text) && addLine(text, line, one);
}

bool catalog::hubAt(uint32_t line, FwList& one) {
  FILE* f = openHub();
  bool ok = hubAt(f, line, one);
  if (f) {
    fclose(f);
  }
  return ok;
}

// ----------------------------------------------------------------------------------- the Hub's JSON ---
// catalog.json, read from the SD card a character at a time (ArduinoJson's custom reader), so a firmware at a time
// can be read out of it: memory for one, however long the list is
struct JsonFile {
  FILE* f;
  int back = -1;  // a character put back
  size_t since = 0;
  int read() {
    if (back >= 0) {
      int c = back;
      back = -1;
      return c;
    }
    if (++since >= 0x4000) {
      since = 0;
      hal::idle();  // (the board's idle task, which its watchdog watches)
    }
    return fgetc(f);
  }
  size_t readBytes(char* b, size_t n) {
    size_t k = 0;
    for (int c; k < n && (c = read()) >= 0;) {
      b[k++] = c;
    }
    return k;
  }
  int next() {  // the next character that isn't white space
    int c;
    do {
      c = read();
    } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
    return c;
  }
};

static bool usable(const Firmware& f) {
  return f.pkg.compare(0, 8, "https://") == 0 && f.size && f.sha256.size() == 64 && f.pkg.find("..") == std::string::npos &&
         !f.file.empty() && f.file.find("..") == std::string::npos;
}

// HUB_CACHE into the lines of HUB_LIST (out): how many firmwares, -1 if it isn't JSON as it should be
static int hubToLines(JsonFile& in, FILE* out, std::string& updated) {
  JsonDocument fwFilter, stockFilter, skip, all;
  for (const char* k : {"name", "author", "version", "date", "what", "pkg", "file", "product", "sha256", "size",
                        "beta", "stock", "hid", "kind", "chip", "devices"}) {
    fwFilter[k] = true;
  }
  for (const char* k : {"pkg", "product", "sha256", "size"}) {
    stockFilter[k] = true;
  }
  skip.set(false);
  all.set(true);
  int n = 0;
  if (in.next() != '{') {
    return -1;
  }
  for (;;) {
    int c = in.next();
    if (c == '}') {
      return n;
    }
    if (c != '"') {
      return -1;
    }
    std::string key;
    while ((c = in.read()) >= 0 && c != '"') {
      key += (char)c;
    }
    if (in.next() != ':') {
      return -1;
    }
    c = in.next();
    if (key == "firmwares" && c == '[') {
      for (;;) {
        c = in.next();
        if (c == ']') {
          break;
        }
        if (c == ',') {
          continue;
        }
        if (c != '{') {
          return -1;
        }
        in.back = c;
        JsonDocument doc;
        if (deserializeJson(doc, in, DeserializationOption::Filter(fwFilter))) {
          return -1;
        }
        Firmware f;
        readEntry(doc.as<JsonObjectConst>(), f);
        if (f.stock) {
          f.kinds = {"stock"};  // M-VAVE's own, whatever the list's tags say
        }
        if (usable(f)) {
          writeLine(out, f);
          n++;
        }
      }
    } else if (c == '{' || c == '[' || c == '"') {
      in.back = c;
      JsonDocument doc;
      bool keep = key == "updated" || key == "stock";
      if (deserializeJson(doc, in, DeserializationOption::Filter(key == "stock" ? stockFilter : keep ? all
                                                                                                     : skip))) {
        return -1;
      }
      if (key == "updated") {
        updated = doc.as<const char*>() ? doc.as<const char*>() : "";
      } else if (key == "stock") {
        Firmware f;
        readEntry(doc.as<JsonObjectConst>(), f);
        f.name = "M-VAVE stock";
        if (f.product.size() > 5 && f.product.compare(0, 5, "FM-1_") == 0) {
          f.name += " V" + std::to_string(atoi(f.product.c_str() + 5));  // "M-VAVE stock V15"
        }
        f.author = "M-VAVE";
        f.what = "The firmware the FM-1 came with: put it back to how it was.";
        f.kinds = {"stock"};
        f.stock = true;
        if (usable(f)) {
          writeLine(out, f);
          n++;
        }
      }
    } else {  // a number, true, false or null: to the next ',' or '}'
      while (c >= 0 && c != ',' && c != '}') {
        c = in.read();
      }
      in.back = c;
    }
    c = in.next();
    if (c == '}') {
      return n;
    }
    if (c != ',') {
      return -1;
    }
  }
}

bool catalog::parseHub(FwList& out, std::string& updated, std::string& err) {
  if (!writeHub(updated, err)) {
    return false;
  }
  if (!loadHub(out, updated)) {
    err = "The Hub's list on the SD card didn't read.";
    return false;
  }
  return true;
}

bool catalog::writeHub(std::string& updated, std::string& err) {
  FILE* json = fopen(path(HUB_CACHE).c_str(), "r");
  if (!json) {
    err = "The Hub's list isn't on the SD card yet.";
    return false;
  }
  std::string p = path(HUB_LIST), part = p + ".part";
  FILE* f = fopen(part.c_str(), "w");
  if (!f) {
    fclose(json);
    hal::log("hub: can't write %s.part (errno %d)", HUB_LIST, errno);
    err = "Can't write the Hub's list to the SD card.";
    return false;
  }
  fprintf(f, "%s\t", LIST_HEAD);
  long headAt = ftell(f);
  field(f, std::string(32, ' '), '\n');  // (when it was made: written once it's known, below)
  hal::psramFirst(true);                 // (ArduinoJson's small blocks, a firmware's worth at a time)
  JsonFile in{json};
  std::string when;
  int n = hubToLines(in, f, when);
  hal::psramFirst(false);
  fclose(json);
  if (n >= 0) {
    fseek(f, headAt, SEEK_SET);
    fputs(when.substr(0, 32).c_str(), f);
  }
  bool ok = !ferror(f);
  ok = !fclose(f) && ok;
  if (n < 0) {
    remove(part.c_str());
    err = "The Hub's list didn't read.";
    return false;
  }
  if (!ok) {
    hal::log("hub: writing %s.part failed (errno %d)", HUB_LIST, errno);
    remove(part.c_str());
    err = "Can't write the Hub's list to the SD card.";
    return false;
  }
  remove(p.c_str());
  if (rename(part.c_str(), p.c_str())) {
    hal::log("hub: can't rename %s.part (errno %d)", HUB_LIST, errno);
    err = "Can't write the Hub's list to the SD card.";
    return false;
  }
  remove(path(HUB_CACHE).c_str());  // (only the list is kept)
  hal::log("hub: %d firmwares", n);
  updated = when;
  return true;
}

std::string catalog::product(const char* p) {
  std::vector<uint8_t> head(64 * 0x30);  // the most interleave blocks a package has (fm1.cpp)
  FILE* f = fopen(p, "rb");
  if (!f) {
    return "";
  }
  uint32_t n = fread(head.data(), 1, head.size(), f);
  fclose(f);
  char s[24];
  uint32_t blocks;
  return pkgHead(head.data(), n, s, sizeof s, blocks) ? s : "";
}

// M-VAVE's own releases, by their sha256: known on the SD card whatever they're called, and whatever the Hub lists
static const struct {
  const char* sha256;
  const char* name;
  bool open;  // M-VAVE Open (else the stock firmware)
} OFFICIAL[] = {
    {"db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a", "M-VAVE stock V15", false},
    {"d017fb65e70aca97ee29d944470c3515a74581df5c0576bf70de917c7b2b61d5", "M-VAVE stock V16", false},
    {"7e7b6d260163cdf9d716f7fab44f19be600edeffcd390de74370b83c917fd0c6", "M-VAVE Open V14", true},
};

static int official(const char* sha) {
  for (size_t i = 0; i < sizeof OFFICIAL / sizeof OFFICIAL[0]; i++) {
    if (!strcasecmp(sha, OFFICIAL[i].sha256)) {
      return i;
    }
  }
  return -1;
}

Firmware catalog::details(const FwList& l, size_t i) {
  const FwItem& it = l[i];
  Firmware f;
  f.name = l.str(it.name), f.author = l.str(it.author), f.version = l.str(it.version), f.date = l.str(it.date);
  f.file = l.str(it.file), f.product = l.str(it.product), f.sha256 = l.str(it.sha256);
  f.size = it.size;
  f.beta = it.flags & FwItem::BETA;
  f.stock = it.flags & FwItem::STOCK;
  f.onSd = it.flags & FwItem::ON_SD;
  f.hub = it.flags & FwItem::FROM_HUB;
  f.chipOld = it.flags & FwItem::CHIP_OLD;
  f.forFm1 = it.flags & FwItem::FOR_FM1;
  f.forSmk37 = it.flags & FwItem::FOR_SMK37;
  if (&l == &hub && f.onSd) {  // the copy on the SD card: where it is in /fm1 (a folder, say)
    FILE* list = 0;
    FwList one;
    if (openSd(list)) {
      for (size_t i = 0, n = sdShown(); i < n; i++) {
        if (!sdAt(list, i, one)) {
          continue;
        }
        const FwItem& x = one[0];
        if ((x.flags & FwItem::FROM_HUB && x.line == it.line) ||
            (*l.str(it.sha256) && !strcasecmp(one.str(x.sha256), l.str(it.sha256)))) {
          f.file = one.str(x.file);
          break;
        }
      }
      if (list) {
        fclose(list);
      }
    }
  }
  for (size_t k = 0; k < l.kinds.size(); k++) {
    if (it.kinds & 1u << k) {
      f.kinds.push_back(l.kinds[k]);
    }
  }
  if (it.line != FwItem::NO_LINE) {  // the rest from its line
    FILE* h = fopen(path(HUB_LIST).c_str(), "r");
    std::string line;
    const char* v[LIST_FIELDS + 1];
    if (h && !fseek(h, it.line, SEEK_SET) && readLine(h, line) && split(line, v, LIST_FIELDS + 1) == LIST_FIELDS &&
        f.name == v[0]) {
      f.what = v[4];
      f.pkg = v[5];
      f.sha256 = v[8];  // (an SD file it was known by from its name: the install checks it's the same)
      f.keyboard = strchr(v[10], 'k');
      f.mouse = strchr(v[10], 'm');
    } else {
      hal::log("hub: %s isn't where the list said", f.name.c_str());
    }
    if (h) {
      fclose(h);
    }
  } else if (int o = official(f.sha256.c_str()); o >= 0) {
    f.what = OFFICIAL[o].open ? "M-VAVE's open firmware: a groovebox, with a web editor (open.m-vave.com)."
                              : "The firmware the FM-1 came with: put it back to how it was.";
  }
  return f;
}

// ------------------------------------------------------------------------------------- the SD card ---
// what's known of each file, kept on the card by its name, size and time (so each is read once): lines
// "name size mtime sha256 product" ("-": not a package). Not in memory: an index of it is (CacheIndex, below), and a
// new line goes to the end of the file as soon as it's known. With two lines of one key, the last one is the line
#define SHA_CACHE DATA_DIR "/sd-sha256.txt"

// one lock for the cache file: a scan holds it while it reads the cache, a write of the whole cache holds it
static std::mutex cacheMu;

static std::string cacheKey(const char* name, uint32_t size, uint32_t mtime) {
  char key[300];
  snprintf(key, sizeof key, "%s %lu %lu ", name, (unsigned long)size, (unsigned long)mtime);
  return key;
}

static std::string cacheLine(const std::string& key, const std::string& sha, const std::string& product) {
  return key + sha + " " + (product.empty() ? "-" : product) + "\n";
}

static bool isPkg(const char* name) {
  size_t n = strlen(name);
  return name[0] != '.' && n > 5 && !strcasecmp(name + n - 5, ".fwsc");
}

// the packages in /fm1 and its folders (FOLDER_DEPTH deep): their paths in /fm1 ("zp12.fwsc", "drums/x.fwsc"), sizes and
// times. A read of the card can fail (one did, in /fm1, while the internal RAM was short: the listing ended early), so a
// folder is read again, up to 3 times. whole: false if a last try failed
#define FOLDER_DEPTH 4
#define FOLDERS_MAX 200

static std::atomic<bool> hashing{false}, hashedNew{false};

// ------------------------------------------------------------------- the SD list: a scan, and what reads it ---
// A name on the card of its own for each call (a scan can run while another does, and while a job does)
static std::atomic<unsigned> tempCount{0};

static std::string tempName(const char* what) {
  char name[64];
  snprintf(name, sizeof name, DATA_DIR "/%s-%u.tmp", what, ++tempCount);
  return name;
}

// The packages in /fm1 (as listPkgs above), a line "path size mtime" each, into a file on the card: no list in memory.
// A try of a folder that failed leaves its lines in the file: they are written over with one line that starts with
// '#', and the next try writes after them. count: how many packages the file holds. whole: false if a last try failed,
// or a line did not write
struct Written {
  FILE* out;
  uint32_t* count;
  std::vector<std::string>* folders;
  const std::string* at;
  int depth;
  bool good;
};

static void blankLines(FILE* f, long from, long to) {
  if (to - from < 2) {
    return;
  }
  fseek(f, from, SEEK_SET);
  fputc('#', f);
  for (long i = from + 2; i < to; i++) {
    fputc(' ', f);
  }
  fputc('\n', f);
  fseek(f, to, SEEK_SET);
}

static bool listPkgs(const std::string& dir, const std::string& file, uint32_t& count) {
  count = 0;
  FILE* out = fopen(catalog::path(file).c_str(), "wb");
  if (!out) {
    hal::log("sd: can't write %s (errno %d)", file.c_str(), errno);
    return false;
  }
  std::vector<std::string> folders{""};
  bool whole = true, good = true;
  for (size_t f = 0; f < folders.size(); f++) {
    const std::string at = folders[f];
    const size_t k = folders.size();
    const uint32_t first = count;
    const long from = ftell(out);
    Written w{out, &count, &folders, &at, at.empty() ? 1 : 2 + (int)std::count(at.begin(), at.end(), '/'), true};
    bool ok = false;
    for (int tries = 1; !ok && tries <= 3; tries++) {
      if (tries > 1) {
        blankLines(out, from, ftell(out));  // (what a failed try wrote)
        count = first, folders.resize(k);
      }
      ok = hal::listDir(
          (at.empty() ? dir : dir + "/" + at).c_str(),
          [](const char* name, uint32_t size, uint32_t mtime, bool folder, void* arg) {
            Written& w = *(Written*)arg;
            std::string p = w.at->empty() ? name : *w.at + "/" + name;
            if (folder) {
              if (name[0] != '.' && w.depth <= FOLDER_DEPTH && w.folders->size() < FOLDERS_MAX) {
                w.folders->push_back(p);
              }
            } else if (isPkg(name)) {
              if (fprintf(w.out, "%s\t%lu\t%lu\n", p.c_str(), (unsigned long)size, (unsigned long)mtime) < 0) {
                w.good = false;
              } else {
                ++*w.count;
              }
            }
          },
          &w);
      if (!ok) {
        hal::log("sd: listing %s/%s failed after %u packages, try %d", dir.c_str(), at.c_str(), (unsigned)count, tries);
      }
    }
    whole &= ok;
    good &= w.good;
  }
  good &= !ferror(out);
  good &= !fclose(out);
  return whole && good;
}

// ---- the cache's index: one entry for each cache line (a hash of its key, and where the line starts), in one block,
// in the order of the keys. The lookup reads the line from the card. With two lines of one key, the last one is
// the line of the key
struct CacheEntry {
  uint64_t hash;
  uint32_t at;
};

static uint64_t hashKey(const char* s, size_t n) {  // (FNV-1a)
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ (uint8_t)s[i]) * 1099511628211ull;
  }
  return h;
}

static bool hex(const char* s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!isxdigit((uint8_t)s[i])) {
      return false;
    }
  }
  return true;
}

// where the key of a cache line ends ("name size mtime ": the sha256 is next), 0 if the line is not one of a file
static size_t keyEnd(const std::string& line) {
  const char* p = line.c_str();
  const size_t n = line.size();
  for (size_t i = 0; i < n; i++) {
    if (p[i] != ' ') {
      continue;
    }
    size_t j = i + 1, d = j;
    while (isdigit((uint8_t)p[j])) {
      j++;
    }
    if (j == d || p[j] != ' ') {
      continue;
    }
    size_t e = ++j;
    d = e;
    while (isdigit((uint8_t)p[e])) {
      e++;
    }
    if (e == d || p[e] != ' ') {
      continue;
    }
    e++;
    if (n >= e + 64 && hex(p + e, 64) && (n == e + 64 || p[e + 64] == ' ')) {
      return e;
    }
  }
  return 0;
}

struct CacheIndex {
  CacheEntry* entries = 0;
  size_t count = 0;
  FILE* file = 0;       // the cache, open while the index is
  bool failed = false;  // the cache is there, and there was no room to index it
  ~CacheIndex() {
    delete[] entries;
    if (file) {
      fclose(file);
    }
  }
};

// the index of the cache file, with the cache's lock held by the caller. With no room for it (or no file), the
// index holds nothing: each file is one whose sha256 is not known
static void openCacheIndex(CacheIndex& c) {
  c.file = fopen(catalog::path(SHA_CACHE).c_str(), "rb");
  if (!c.file) {
    return;
  }
  std::string line;
  size_t lines = 0;
  for (size_t i = 0; readLine(c.file, line); i++) {
    lines += keyEnd(line) > 0;
    if (!(i & 15)) {
      hal::idle();
    }
  }
  c.entries = lines ? new (std::nothrow) CacheEntry[lines] : 0;
  if (lines && !c.entries) {
    hal::log("sd: no room for the index of %u cache lines: the cache is not read", (unsigned)lines);
    c.failed = true;
    fclose(c.file);
    c.file = 0;
    return;
  }
  rewind(c.file);
  uint32_t at = 0;
  for (size_t i = 0; readLine(c.file, line) && c.count < lines; i++) {
    uint32_t here = at;
    at += line.size() + 1;
    if (size_t e = keyEnd(line)) {
      c.entries[c.count++] = {hashKey(line.data(), e), here};
    }
    if (!(i & 15)) {
      hal::idle();
    }
  }
  std::sort(c.entries, c.entries + c.count, [](const CacheEntry& a, const CacheEntry& b) {
    return a.hash != b.hash ? a.hash < b.hash : a.at < b.at;
  });
}

// a file's line: its sha256 ("" if it isn't known yet) and product (false: not known yet)
static bool cacheFind(CacheIndex& c, const std::string& key, std::string& sha, std::string& product) {
  sha.clear();
  if (!c.file || !c.count) {
    return false;
  }
  const uint64_t h = hashKey(key.data(), key.size());
  const CacheEntry* x = std::upper_bound(c.entries, c.entries + c.count, h,
                                         [](uint64_t h, const CacheEntry& e) { return h < e.hash; });
  std::string line;
  while (x > c.entries && (x - 1)->hash == h) {  // (the last line first)
    --x;
    if (fseek(c.file, x->at, SEEK_SET) || !readLine(c.file, line) || line.compare(0, key.size(), key) ||
        line.size() < key.size() + 64) {
      continue;
    }
    sha = line.substr(key.size(), 64);
    if (line.size() == key.size() + 64 || line[key.size() + 64] != ' ') {
      return false;
    }
    product = line.substr(key.size() + 65);
    product = product == "-" ? "" : product;
    return true;
  }
  return false;
}

// ---- a line of the SD list: name author version date path product sha256 size flags kinds line (the Hub's line in
// HUB_LIST, or NO_LINE), its fields between tabs
#define SD_FIELDS 11
#define SD_HEAD "fm1buddy sd 1"

static void writeSdLine(FILE* f, const FwList& one) {
  const FwItem& it = one[0];
  for (uint32_t at : {it.name, it.author, it.version, it.date, it.file, it.product, it.sha256}) {
    field(f, one.str(at));
  }
  fprintf(f, "%lu\t%u\t", (unsigned long)it.size, (unsigned)it.flags);
  std::string kinds;
  for (size_t k = 0; k < one.kinds.size(); k++) {
    if (it.kinds & 1u << k) {
      kinds += (kinds.empty() ? "" : ",") + one.kinds[k];
    }
  }
  field(f, kinds);
  fprintf(f, "%lu\n", (unsigned long)it.line);
}

// a line of the SD list (from here in it) onto list: false if it isn't a package's
static bool addSdLine(std::string& line, FwList& list) {
  const char* v[SD_FIELDS + 1];
  if (split(line, v, SD_FIELDS + 1) != SD_FIELDS) {
    return false;
  }
  FwItem it{};
  it.name = list.add(v[0], strlen(v[0]));
  it.author = list.add(v[1], strlen(v[1]));
  it.version = list.add(v[2], strlen(v[2]));
  it.date = list.add(v[3], strlen(v[3]));
  it.file = list.add(v[4], strlen(v[4]));
  it.product = list.add(v[5], strlen(v[5]));
  it.sha256 = list.add(v[6], strlen(v[6]));
  it.size = strtoul(v[7], 0, 10);
  it.flags = (uint8_t)strtoul(v[8], 0, 10);
  it.line = strtoul(v[10], 0, 10);
  for (const char* k = v[9]; *k;) {
    const char* c = strchr(k, ',');
    size_t n = c ? c - k : strlen(k);
    it.kinds |= list.kindBit(std::string(k, n));
    k += n + (c ? 1 : 0);
  }
  list.items.push_back(it);
  return true;
}

// item i of from into the list of one to
static void copyItem(const FwList& from, size_t i, FwList& to) {
  FwItem it = from[i];
  it.name = to.add(from.str(it.name));
  it.author = to.add(from.str(it.author));
  it.version = to.add(from.str(it.version));
  it.date = to.add(from.str(it.date));
  it.file = to.add(from.str(it.file));
  it.product = to.add(from.str(it.product));
  it.sha256 = to.add(from.str(it.sha256));
  it.kinds = 0;
  for (size_t k = 0; k < from.kinds.size(); k++) {
    if (from[i].kinds & 1u << k) {
      it.kinds |= to.kindBit(from.kinds[k]);
    }
  }
  to.items.push_back(it);
}

// how a name starts, for the order of the names: lower case, 0 after a short name
static void prefixOf(const char* name, size_t n, char prefix[12]) {
  for (size_t i = 0; i < 12; i++) {
    prefix[i] = i < n ? (char)tolower((uint8_t)name[i]) : 0;
  }
}

// the name on the line of the list file at "at" (the field before the first tab)
static bool nameAt(FILE* f, uint32_t at, std::string& name) {
  if (fseek(f, at, SEEK_SET) || !readLine(f, name)) {
    return false;
  }
  name.erase(std::min(name.find('\t'), name.size()));
  return true;
}

// the files of /fm1 in a list on the card, and the index of it in memory
catalog::SdScan catalog::readSd(FwList* hubList) {
  SdScan scan;
  FwList none;
  FwList& hub = hubList ? *hubList : none;
  for (FwItem& h : hub.items) {
    h.flags &= ~FwItem::ON_SD;
  }
  std::string dir = path(FW_DIR);
  if (dir.empty()) {
    scan.complete = true;
    return scan;
  }
  const uint32_t began = hal::millis();
  mkdir(path(DATA_DIR).c_str(), 0777);
  const std::string listing = tempName("scan"), raw = tempName("raw"), file = tempName("list");
  uint32_t found = 0;
  bool whole = listPkgs(dir, listing, found);

  // the files, with what is known of each, one line each (the cache index is in memory, and nothing else of the scan)
  FILE* in = fopen(path(listing).c_str(), "rb");
  FILE* out = fopen(path(raw).c_str(), "wb");
  uint32_t lines = 0, unknown = 0;
  if (in && out) {
    fputs(SD_HEAD "\n", out);
    std::lock_guard<std::mutex> lock(cacheMu);
    CacheIndex cache;
    openCacheIndex(cache);
    FwList one;
    std::string line;
    for (size_t i = 0; readLine(in, line); i++) {
      const char* v[4];
      if (line.empty() || line[0] == '#' || split(line, v, 4) != 3) {
        continue;
      }
      if (!(i & 15)) {
        hal::idle();
      }
      const char* name = v[0];  // (its path in /fm1)
      const uint32_t size = strtoul(v[1], 0, 10), mtime = strtoul(v[2], 0, 10);
      std::string base = baseName(name);
      // what it is: by its sha256 (a Hub firmware, or one of M-VAVE's), not by its name. Until that's known (it's
      // read in the background: hashSd), a Hub firmware by its file name and size (the install checks the sha256)
      std::string sha, prod;
      if (!cacheFind(cache, cacheKey(name, size, mtime), sha, prod)) {
        unknown++;
        prod = product((dir + "/" + name).c_str());
      }
      empty(one);
      FwItem it{};
      it.line = FwItem::NO_LINE;
      it.size = size;
      it.flags = FwItem::ON_SD;
      // the Hub can have the same file more than once (M-VAVE's stock firmware, in another firmware's repo: issue #88):
      // all of them are on the card, and it's named after M-VAVE's own, else the one with its file name, else the first
      size_t h = hub.size();
      int best = -1;
      for (size_t k = 0; k < hub.size(); k++) {
        FwItem& x = hub.items[k];
        bool sameName = !strcasecmp(hub.str(x.file), base.c_str());
        if (sha.empty() ? x.size == it.size && sameName : !strcasecmp(hub.str(x.sha256), sha.c_str())) {
          x.flags |= FwItem::ON_SD;
          int score = (x.flags & FwItem::STOCK ? 2 : 0) + (sameName ? 1 : 0);
          if (score > best) {
            best = score, h = k;
          }
        }
      }
      int o = sha.empty() ? -1 : official(sha.c_str());
      if (h < hub.size() && !(o >= 0 && !(hub.items[h].flags & FwItem::STOCK))) {
        const FwItem& x = hub.items[h];
        it.name = one.add(hub.str(x.name));
        it.author = one.add(hub.str(x.author));
        it.version = one.add(hub.str(x.version));
        it.date = one.add(hub.str(x.date));
        it.line = x.line;
        it.flags |= FwItem::FROM_HUB | (x.flags & (FwItem::BETA | FwItem::STOCK));
        for (size_t k = 0; k < hub.kinds.size(); k++) {
          if (x.kinds & 1u << k) {
            it.kinds |= one.kindBit(hub.kinds[k]);
          }
        }
      } else if (o >= 0) {
        it.name = one.add(OFFICIAL[o].name);
        it.author = one.add("M-VAVE");
        it.kinds = one.kindBit(OFFICIAL[o].open ? "groove" : "stock");
        it.flags |= OFFICIAL[o].open ? 0 : FwItem::STOCK;
      } else {
        it.name = one.add(base.c_str(), base.size() - 5);
      }
      it.file = one.add(name);
      it.product = one.add(prod);
      it.sha256 = one.add(sha);
      one.items.push_back(it);
      writeSdLine(out, one);
      lines++;
    }
  }
  bool good = in && out && !ferror(out);
  if (in) {
    fclose(in);
  }
  if (out) {
    good &= !fclose(out);
  }
  remove(path(listing).c_str());
  if (!good) {
    hal::log("sd: the list of the packages did not write (errno %d)", errno);
  }

  // the index of the list (the cache index is gone): one block, the whole size or smaller while it does not fit
  size_t room = lines;
  while (room && !(scan.index = new (std::nothrow) SdEntry[room])) {
    room /= 2;
  }
  FILE* list = out ? fopen(path(raw).c_str(), "rb") : 0;
  if (list && room) {
    std::string line;
    uint32_t at = 0;
    if (readLine(list, line)) {  // (the head line)
      at = line.size() + 1;
    }
    for (size_t i = 0; scan.shown < room && readLine(list, line); i++) {
      SdEntry& e = scan.index[scan.shown++];
      e.line = at;
      prefixOf(line.c_str(), std::min(line.find('\t'), line.size()), e.prefix);
      at += line.size() + 1;
      if (!(i & 15)) {
        hal::idle();
      }
    }
    // by name (as strcasecmp does): the start of each name, and the names themselves when the starts are the same
    std::string a, b;
    std::sort(scan.index, scan.index + scan.shown, [&](const SdEntry& x, const SdEntry& y) {
      if (int c = memcmp(x.prefix, y.prefix, sizeof x.prefix)) {
        return c < 0;
      }
      if (memchr(x.prefix, 0, sizeof x.prefix) || !nameAt(list, x.line, a) || !nameAt(list, y.line, b)) {
        return x.line < y.line;  // (the same name)
      }
      int c = strcasecmp(a.c_str(), b.c_str());
      return c ? c < 0 : x.line < y.line;
    });
  }
  // the list again, in the order of the names (a page, and the sort of a next scan, read it in that order)
  FILE* sorted = list ? fopen(path(file).c_str(), "wb") : 0;
  const bool listed = sorted;
  if (sorted) {
    fputs(SD_HEAD "\n", sorted);
    std::string line;
    uint32_t at = strlen(SD_HEAD) + 1, i = 0;
    for (; i < scan.shown; i++) {
      if (!(i & 15)) {
        hal::idle();
      }
      if (fseek(list, scan.index[i].line, SEEK_SET) || !readLine(list, line) || fprintf(sorted, "%s\n", line.c_str()) < 0) {
        break;
      }
      scan.index[i].line = at;
      at += line.size() + 1;
    }
    good &= i == scan.shown && !ferror(sorted);
    scan.shown = i;
    good &= !fclose(sorted);
  } else {
    good = false;
    scan.shown = 0;
  }
  if (list) {
    fclose(list);
  }
  remove(path(raw).c_str());
  scan.found = found;
  scan.unknown = unknown;
  scan.complete = whole && good && scan.shown == found;
  scan.file = listed ? file : "";
  hal::log("sd: %u found, %u shown, %u with no sha256%s (%u ms)", (unsigned)scan.found, (unsigned)scan.shown,
           (unsigned)scan.unknown, scan.complete ? "" : ", not complete", (unsigned)(hal::millis() - began));
  return scan;
}

void catalog::dropSd(SdScan& scan) {
  if (!scan.file.empty()) {
    remove(path(scan.file).c_str());
  }
  scan.file.clear();
  delete[] scan.index;
  scan.index = 0;
  scan.shown = 0;
}

// the SD list the pages read: the last scan to be taken. With keepSd false, its file is SD_LIST, and its index is here
static std::mutex sdMu;
static catalog::SdScan installed;

// the list of a scan in memory, in the order of the names
static void fillFrom(const catalog::SdScan& scan, FwList& list) {
  FILE* f = scan.file.empty() ? 0 : fopen(catalog::path(scan.file).c_str(), "rb");
  if (!f) {
    return;
  }
  roomy(list);
  std::string line;
  for (uint32_t i = 0; i < scan.shown; i++) {
    if (!(i & 15)) {
      hal::idle();
    }
    if (!fseek(f, scan.index[i].line, SEEK_SET) && readLine(f, line)) {
      addSdLine(line, list);
    }
  }
  fclose(f);
}

// the scan is the SD list. With keepSd, it is in memory, in "into"; else SD_LIST is its file
static void install(catalog::SdScan& scan, FwList* into) {
  using namespace catalog;
  std::lock_guard<std::mutex> lock(sdMu);
  if (keepSd) {
    FwList list;
    fillFrom(scan, list);
    SdScan counts = scan;  // (what the pages ask of the list in memory)
    counts.file.clear(), counts.index = 0;
    dropSd(scan);
    into->swap(list);
    delete[] installed.index;
    installed = counts;
    return;
  }
  if (!scan.file.empty()) {
    remove(path(SD_LIST).c_str());  // (a card of FAT does not rename onto a file)
    if (rename(path(scan.file).c_str(), path(SD_LIST).c_str())) {
      hal::log("sd: can't make %s (errno %d)", SD_LIST, errno);
      dropSd(scan);
      return;
    }
  }
  delete[] installed.index;
  installed = scan;
  if (!scan.file.empty()) {
    installed.file = SD_LIST;
  }
  scan.file.clear();
  scan.index = 0;
  scan.shown = 0;
  FwList::changes++;
}

void catalog::takeSd(SdScan& scan) {
  const uint32_t unknown = scan.unknown;
  install(scan, &sd);
  if (unknown) {
    hashSd();
  }
}

int catalog::scanSdInto(FwList& hub, FwList& sd) {
  SdScan scan = readSd(&hub);
  const int unknown = scan.unknown;
  install(scan, &sd);
  return unknown;
}

uint32_t catalog::sdShown(const SdScan* scan) {
  std::lock_guard<std::mutex> lock(sdMu);
  return scan ? scan->shown : keepSd ? (uint32_t)sd.size()
                                     : installed.shown;
}

uint32_t catalog::sdFound(const SdScan* scan) {
  std::lock_guard<std::mutex> lock(sdMu);
  return (scan ? scan : &installed)->found;
}

uint32_t catalog::sdUnknown(const SdScan* scan) {
  std::lock_guard<std::mutex> lock(sdMu);
  return (scan ? scan : &installed)->unknown;
}

bool catalog::sdComplete(const SdScan* scan) {
  std::lock_guard<std::mutex> lock(sdMu);
  return (scan ? scan : &installed)->complete;
}

bool catalog::openSd(FILE*& f, const SdScan* scan) {
  f = 0;
  if (!scan && keepSd) {
    return true;  // (in memory)
  }
  if (scan && scan->file.empty()) {
    return false;
  }
  f = fopen(path(scan ? scan->file : SD_LIST).c_str(), "rb");
  return f;
}

bool catalog::sdAt(FILE* f, size_t i, FwList& one, const SdScan* scan) {
  empty(one);
  std::unique_lock<std::mutex> lock(sdMu, std::defer_lock);
  if (!scan) {
    lock.lock();
    if (keepSd) {
      if (i >= sd.size()) {
        return false;
      }
      copyItem(sd, i, one);
      return true;
    }
    scan = &installed;
  }
  std::string line;
  return f && i < scan->shown && !fseek(f, scan->index[i].line, SEEK_SET) && readLine(f, line) && addSdLine(line, one);
}

bool catalog::sdAt(size_t i, FwList& one, const SdScan* scan) {
  FILE* f;
  if (!openSd(f, scan)) {
    empty(one);
    return false;
  }
  bool ok = sdAt(f, i, one, scan);
  if (f) {
    fclose(f);
  }
  return ok;
}

int catalog::sdFind(const char* file, const SdScan* scan) {
  FILE* f;
  if (!openSd(f, scan)) {
    return -1;
  }
  FwList one;
  int found = -1;
  for (size_t i = 0, n = sdShown(scan); found < 0 && i < n; i++) {
    found = sdAt(f, i, one, scan) && !strcmp(one.str(one[0].file), file) ? (int)i : -1;
  }
  if (f) {
    fclose(f);
  }
  return found;
}

// a line to the end of the cache (whole: the last line of the file ends with a '\n'), at once
static bool appendCache(const std::string& line) {
  std::lock_guard<std::mutex> lock(cacheMu);
  mkdir(catalog::path(DATA_DIR).c_str(), 0777);
  FILE* c = fopen(catalog::path(SHA_CACHE).c_str(), "ab+");
  if (!c) {
    hal::log("sd: can't write %s (errno %d)", SHA_CACHE, errno);
    return false;
  }
  fseek(c, 0, SEEK_END);
  long n = ftell(c);
  if (n > 0 && !fseek(c, n - 1, SEEK_SET) && fgetc(c) != '\n') {
    fseek(c, 0, SEEK_END);
    fputc('\n', c);
  }
  fseek(c, 0, SEEK_END);
  bool ok = fwrite(line.data(), 1, line.size(), c) == line.size() && !ferror(c);
  if (fclose(c) || !ok) {
    hal::log("sd: writing %s failed (errno %d)", SHA_CACHE, errno);
    return false;
  }
  return true;
}

// The cache again with one line for each file of the listing, and none for the files that are gone: line by line into a
// file of its own, then it takes the place of the cache. false: not written (the cache stays as it was)
static bool rewriteCache(const std::string& listing) {
  const std::string temp = tempName("sha");
  FILE* in = fopen(catalog::path(listing).c_str(), "rb");
  FILE* out = fopen(catalog::path(temp).c_str(), "wb");
  bool ok = in && out;
  {
    CacheIndex cache;
    {
      std::lock_guard<std::mutex> lock(cacheMu);
      openCacheIndex(cache);
    }
    ok = ok && !cache.failed;
    std::string line, sha, prod;
    for (size_t i = 0; ok && readLine(in, line); i++) {
      const char* v[4];
      if (line.empty() || line[0] == '#' || split(line, v, 4) != 3) {
        continue;
      }
      if (!(i & 15)) {
        hal::idle();
      }
      std::string key = cacheKey(v[0], strtoul(v[1], 0, 10), strtoul(v[2], 0, 10));
      cacheFind(cache, key, sha, prod);
      if (!sha.empty()) {
        std::string text = cacheLine(key, sha, prod);
        ok = fputs(text.c_str(), out) >= 0;
      }
    }
  }  // (the index closes the old cache)
  if (in) {
    fclose(in);
  }
  if (out) {
    ok = ok && !ferror(out) && !fclose(out);
  }
  if (ok) {
    std::lock_guard<std::mutex> lock(cacheMu);
    remove(catalog::path(SHA_CACHE).c_str());  // (a rename on the card does not replace a file)
    ok = !rename(catalog::path(temp).c_str(), catalog::path(SHA_CACHE).c_str());
  }
  if (!ok) {
    hal::log("sd: can't write the new %s (errno %d)", SHA_CACHE, errno);
    remove(catalog::path(temp).c_str());
  }
  return ok;
}

// the sha256 (and product) of the files the cache doesn't know yet (seconds each), on a thread of their own; each line
// goes to the end of the cache as it's known (so a restart meanwhile keeps what's done), and after a whole listing the
// cache is written again with only the files that are there
static void hashTask(void*) {
  uint32_t t = hal::millis();
  const std::string dir = catalog::path(FW_DIR), listing = tempName("hash");
  uint32_t found = 0;
  int done = 0;
  bool whole = false, ran = !dir.empty();
  {
    CacheIndex cache;
    if (ran) {
      mkdir(catalog::path(DATA_DIR).c_str(), 0777);
      whole = listPkgs(dir, listing, found);
      std::lock_guard<std::mutex> lock(cacheMu);
      openCacheIndex(cache);
      ran = !cache.failed;
    }
    FILE* in = ran ? fopen(catalog::path(listing).c_str(), "rb") : 0;
    std::string line;
    for (size_t i = 0; in && readLine(in, line); i++) {
      const char* v[4];
      if (line.empty() || line[0] == '#' || split(line, v, 4) != 3) {
        continue;
      }
      const std::string full = dir + "/" + v[0];
      const std::string key = cacheKey(v[0], strtoul(v[1], 0, 10), strtoul(v[2], 0, 10));
      std::string sha, prod;
      hal::idle();  // (many files to read, one after the other: the idle task runs, or the watchdog restarts the board)
      if (cacheFind(cache, key, sha, prod)) {
        continue;
      }
      if (sha.empty()) {
        char hex[65];
        if (!hal::sha256(full.c_str(), hex)) {
          hal::log("sd: can't read %s (errno %d)", full.c_str(), errno);
          continue;
        }
        sha = hex;
      }
      prod = catalog::product(full.c_str());
      if (appendCache(cacheLine(key, sha, prod))) {
        done++;
      }
    }
    if (in) {
      fclose(in);
    }
  }  // (the index closes the cache)
  bool ok = true;
  if (ran && whole) {
    ok = rewriteCache(listing);  // (one line for each key: the old line of a key that got a new one, and the line of a gone file, go)
  }
  if (!dir.empty()) {
    remove(catalog::path(listing).c_str());
  }
  if (!ran && !dir.empty()) {
    hal::log("sd: no room for the SHA task: no cache line changed");
  }
  hal::log("sd: %d read (%u ms), %u files%s", done, (unsigned)(hal::millis() - t), (unsigned)found,
           ok ? "" : ": cache not written");
  hashedNew = hashedNew || done;
  hashing = false;
}

void catalog::hashSd() {
  if (!path(FW_DIR).empty() && !hashing.exchange(true)) {
    hal::spawn(hashTask, 0, 8192);
  }
}

bool catalog::sdHashed() {
  return hashedNew.exchange(false);
}

std::string catalog::stem(const std::string& file) {
  return syncplan::stem(file);
}

int catalog::hubVersionOf(const FwList& hub, const FwList& sd, size_t i) {
  if (i >= sd.size()) {
    return -1;
  }
  const FwItem& f = sd[i];
  const char* prod = sd.str(f.product);
  if (!*prod) {
    return -1;  // (not a package)
  }
  std::string name = stem(sd.str(f.file));
  for (size_t h = 0; f.flags & FwItem::FROM_HUB && h < hub.size(); h++) {
    if (hub[h].line == f.line) {
      return (int)h;  // the very file, as the list named it (scanSdInto)
    }
  }
  int same = -1;
  for (size_t h = 0; h < hub.size(); h++) {
    const FwItem& x = hub[h];
    if (*sd.str(f.sha256) && !strcasecmp(hub.str(x.sha256), sd.str(f.sha256))) {
      return (int)h;  // the very file
    }
    if (strcasecmp(hub.str(x.product), prod)) {
      continue;
    }
    // (two of the same name: a release and a beta, say: the newest)
    if (stem(hub.str(x.file)) == name && (same < 0 || strcmp(hub.str(x.date), hub.str(hub[same].date)) > 0)) {
      same = (int)h;
    }
  }
  return same;  // (not "the only one for that device": a device ID is shared by other firmwares, issue #88)
}

bool catalog::matchHub(syncplan::HubMatch& m) {
  return eachHub([](const FwList& one, void* arg) {
    const FwItem& x = one[0];
    ((syncplan::HubMatch*)arg)->entry(one.str(x.file), one.str(x.product), one.str(x.sha256), one.str(x.date), x.line, x.size);
    return true;
  },
                 &m);
}

void catalog::scanSd() {
  SdScan scan = readSd(&hub);
  takeSd(scan);
}
