#include "syncplan.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <vector>

#include "catalog.h"

int syncplan::split(std::string& line, const char* v[], int max) {
  int n = 0;
  char* p = &line[0];
  v[n++] = p;
  for (; *p && n < max; p++) {
    if (*p == '\t') {
      *p = 0;
      v[n++] = p + 1;
    }
  }
  return n;
}

bool syncplan::readLine(FILE* f, std::string& line) {
  line.clear();
  char buf[512];
  while (fgets(buf, sizeof buf, f)) {
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n') {
      line.append(buf, n - 1);
      return true;
    }
    line.append(buf, n);
  }
  return !line.empty();
}

static std::string baseName(const std::string& p) {
  size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

std::string syncplan::stem(const std::string& file) {
  std::string s;
  for (char c : baseName(file)) {
    s += (char)tolower((unsigned char)c);
  }
  if (s.size() > 5 && !s.compare(s.size() - 5, 5, ".fwsc")) {
    s.resize(s.size() - 5);
  }
  // up to a separator followed by a number ("-2.4.4", "_v1.2")
  for (size_t k = 1; k + 1 < s.size(); k++) {
    char n = s[k + 1] == 'v' && k + 2 < s.size() ? s[k + 2] : s[k + 1];
    if (strchr("-_. ", s[k]) && isdigit((unsigned char)n)) {
      return s.substr(0, k);
    }
  }
  return s;
}

// a part of a key: lower case, and a character that the record or the extra source names use becomes '_'
static void keyPart(std::string& key, const std::string& s) {
  for (char c : s) {
    key += c == ':' || c == '\t' || c == '\n' || c == '\r' ? '_' : (char)tolower((unsigned char)c);
  }
}

std::string syncplan::firmwareKey(const std::string& product, const std::string& file) {
  std::string key;
  keyPart(key, product);
  key += '|';
  keyPart(key, stem(file));
  return key;
}

// a word in a list between commas
static bool listed(const char* list, const char* word) {
  size_t n = strlen(word);
  for (const char* k = list; *k;) {
    const char* c = strchr(k, ',');
    size_t len = c ? c - k : strlen(k);
    if (len == n && !strncasecmp(k, word, n)) {
      return true;
    }
    k += len + (c ? 1 : 0);
  }
  return false;
}

bool syncplan::forFm1(const char* devices, const char* product) {
  return *devices ? listed(devices, "fm1") : !strncasecmp(product, "FM-1", 4);
}

bool syncplan::forSmk37(const char* devices, const char* product) {
  return *devices ? listed(devices, "smk37") : !strncasecmp(product, "SMK-37", 6);
}

// ------------------------------------------------------------------------------------- the sync record ---
// A line that a sync has removed the file of starts with this, in place of the '/' of its path: the same length, so
// the mark changes the record in place, and no path in /fm1 starts with it
static const char removedMark = '#';
static const char recordHead[] = "fm1buddy sync 1";

bool syncplan::appendRecord(FILE* record, const char* path, const char* sha256, uint32_t size, const char* key) {
  if (!record || fseek(record, 0, SEEK_END)) {
    return false;
  }
  if (!ftell(record) && fprintf(record, "%s\n", recordHead) < 0) {
    return false;
  }
  return fprintf(record, "%s\t%s\t%u\t%s\n", path, sha256, (unsigned)size, key) > 0;
}

// the stream from at on: written again by chunks. A write in the middle of a memory stream can put a 0 after the last
// byte written (macOS), so the mark and the rest of the record go in one run, each chunk read before the write that
// would spoil it
static bool writeFrom(FILE* f, long at, bool mark) {
  char cur[512], next[512];
  if (fseek(f, at, SEEK_SET)) {
    return false;
  }
  size_t n = fread(cur, 1, sizeof cur, f);
  while (n) {
    if (mark) {
      cur[0] = removedMark;
      mark = false;
    }
    if (fseek(f, at + n, SEEK_SET)) {
      return false;
    }
    size_t more = fread(next, 1, sizeof next, f);
    if (fseek(f, at, SEEK_SET) || fwrite(cur, 1, n, f) != n) {
      return false;
    }
    at += n;
    memcpy(cur, next, more);
    n = more;
  }
  return !fflush(f);
}

bool syncplan::markRemoved(FILE* record, const char* path) {
  if (!record || fseek(record, 0, SEEK_SET)) {
    return false;
  }
  std::string line;
  size_t n = strlen(path);
  for (;;) {
    long at = ftell(record);
    if (!readLine(record, line)) {
      return false;
    }
    if (!line.compare(0, n, path) && line.size() > n && line[n] == '\t') {
      return writeFrom(record, at, true);
    }
  }
}

bool syncplan::rewriteRecord(FILE* record, FILE* out) {
  if (!record || !out) {
    return false;
  }
  std::string line;
  bool head = false;
  while (readLine(record, line)) {
    if (line.empty() || line[0] == removedMark || (!head && line != recordHead)) {
      continue;
    }
    head = true;
    fprintf(out, "%s\n", line.c_str());
  }
  return head;
}

bool syncplan::recordDownload(FILE* record, FILE* out, const char* path, const char* sha256, uint32_t size,
                              const char* key) {
  if (!out || fprintf(out, "%s\n", recordHead) < 0) {
    return false;
  }
  std::string line;
  bool head = false;
  size_t n = strlen(path);
  while (record && readLine(record, line)) {
    if (!head) {
      head = line == recordHead;  // (a line before the head isn't a file's)
      continue;
    }
    bool same = !strncasecmp(line.c_str(), path, n) && line.size() > n && line[n] == '\t';
    if (line.empty() || line[0] == removedMark || same) {
      continue;
    }
    if (fprintf(out, "%s\n", line.c_str()) < 0) {
      return false;
    }
  }
  return fprintf(out, "%s\t%s\t%u\t%s\n", path, sha256, (unsigned)size, key) > 0;
}

// ------------------------------------------------------------------------------------- one file's Hub version ---
syncplan::HubMatch::HubMatch(const char* file, const char* product, const char* sha256)
    : name(stem(file)), product(product), sha256(sha256) {
  const char* slash = strrchr(file, '/');
  base = slash ? slash + 1 : file;
}

void syncplan::HubMatch::entry(const char* file, const char* product, const char* sha256, const char* date,
                               uint32_t line, uint32_t size) {
  if (this->product.empty()) {
    return;  // (not a package)
  }
  if (this->sha256.empty() ? sdBytes && size == sdBytes && !strcasecmp(file, base.c_str())
                           : !strcasecmp(sha256, this->sha256.c_str())) {
    same = true;
  }
  // (not "the only one for that product": a product is shared by other firmwares, issue #88)
  if (strcasecmp(product, this->product.c_str()) || stem(file) != name) {
    return;
  }
  // (two of the same name: a release and a beta, say: the newest)
  if (!found || strcmp(date, newestDate.c_str()) > 0) {
    found = true;
    newestDate = date;
    newest = line;
  }
}

syncplan::HubMatch::Result syncplan::HubMatch::result() const {
  return same ? SAME_FILE : found ? NEWER
                                  : NOT_LISTED;
}

bool syncplan::moveRecordLine(FILE* record, FILE* out, const char* oldPath, const char* newPath, const char* sha256,
                              uint32_t size) {
  if (!out) {
    return false;
  }
  std::string line;
  size_t n = strlen(oldPath), m = strlen(newPath);
  while (record && readLine(record, line)) {
    bool old = !strncasecmp(line.c_str(), oldPath, n) && line.size() > n && line[n] == '\t';
    const char* v[4];
    std::string copy = line;
    if (old && split(copy, v, 4) == 4) {
      if (fprintf(out, "%s\t%s\t%u\t%s\n", newPath, sha256, (unsigned)size, v[3]) < 0) {
        return false;
      }
      continue;
    }
    if (!strncasecmp(line.c_str(), newPath, m) && line.size() > m && line[m] == '\t') {
      continue;  // (an older line of the new file's path: a file has one line)
    }
    if (fprintf(out, "%s\n", line.c_str()) < 0) {
      return false;
    }
  }
  return true;
}

// --------------------------------------------------------------------------------------- the extra sources ---
syncplan::ExtraDecision syncplan::decideExtra(const char* name, const char* file, FILE* record, const SdFile* files,
                                              size_t count, const ExtraDownload& got) {
  const std::string path = std::string(FW_DIR "/") + file, label = name + (strncmp(name, "extra:", 6) ? 0 : 6);
  bool present = false;
  for (size_t i = 0; i < count; i++) {
    present = present || !strcasecmp(files[i].path, path.c_str());
  }
  // the sync record: the last line of this source's file (a replace is cut short between its lines)
  bool recorded = false;
  std::string recordedSha, rec;
  while (record && readLine(record, rec)) {
    const char* r[5];
    if (rec[0] != removedMark && split(rec, r, 5) == 4 && !strcasecmp(r[0], path.c_str()) && !strcmp(r[3], name)) {
      recorded = true;
      recordedSha = r[1];
    }
  }
  if (present && !recorded) {
    return {EXTRA_USER_FILE, label + ": skipped, a file of yours has the name " + file};
  }
  if (!got.found) {
    return {EXTRA_NOT_FOUND, label + ": not found"};
  }
  const char* why = 0;
  if (!got.size) {
    why = "the download is empty";
  } else if (got.contentLength >= 0 && (long)got.size != got.contentLength) {
    why = "the download isn't the size the server said";
  } else if (!*got.sha256) {
    why = "the sha256 can't be computed";
  } else if (!forFm1("", got.product)) {
    why = "the package isn't for the FM-1";
  }
  if (why) {
    return {EXTRA_FAILED, label + ": " + why};
  }
  bool same = recorded && present && !strcasecmp(recordedSha.c_str(), got.sha256);
  return {same ? EXTRA_UNCHANGED : EXTRA_REPLACE, ""};
}

// ------------------------------------------------------------------------------------------- the plan ---
// one firmware key: 16 bytes, whatever the Hub list holds (the strings stay on the SD card). A board with no PSRAM has
// only a few KB in one block, so the keys must be small and in one allocation
struct Key {
  enum : uint8_t { FM1 = 1,
                   HANDLED = 2 };
  uint64_t hash;
  uint32_t line;       // where the newest FM-1 entry's line is in the Hub list (when FM1)
  uint32_t date : 30;  // its date as a number: "2026-11-17" is 20261117
  uint32_t flags : 2;
};

// the digits of a date as a number: dates in order give numbers in order
static uint32_t dateNumber(const char* date) {
  uint32_t n = 0;
  for (; *date; date++) {
    if (isdigit((unsigned char)*date)) {
      n = n * 10 + (*date - '0');
    }
  }
  return n & 0x3FFFFFFF;
}

static uint64_t hashOf(const std::string& key) {
  uint64_t h = 1469598103934665603ull;  // (FNV-1a)
  for (char c : key) {
    h = (h ^ (uint8_t)c) * 1099511628211ull;
  }
  return h;
}

static Key* find(std::vector<Key>& keys, uint64_t hash) {
  for (Key& k : keys) {
    if (k.hash == hash) {
      return &k;
    }
  }
  return 0;
}

// the Hub list line at an offset, cut at its tabs
static bool lineAt(FILE* hubList, uint32_t at, std::string& line, const char* v[]) {
  return !fseek(hubList, at, SEEK_SET) && syncplan::readLine(hubList, line) &&
         syncplan::split(line, v, HUB_LIST_FIELDS + 1) == HUB_LIST_FIELDS;
}

static std::string target(const char* file, const char* chip) {
  return std::string(FW_DIR) + (strcmp(chip, "old") ? "/" : "/old-chip/") + file;
}

// download or replace: the file to get, and (a replace) the file it takes the place of
static void get(FILE* out, const char* kind, const char* const v[], const char* old) {
  fprintf(out, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s", kind, target(v[6], v[12]).c_str(), v[5], v[8], v[9], v[0], v[2],
          v[7]);
  if (old) {
    fprintf(out, "\t%s", old);
  }
  fputc('\n', out);
}

bool syncplan::plan(FILE* hubList, FILE* record, const SdFile* files, size_t count, FILE* out) {
  std::string line;
  const char* v[HUB_LIST_FIELDS + 1];
  if (!hubList || !readLine(hubList, line) || split(line, v, 3) != 2 || strcmp(v[0], HUB_LIST_HEAD)) {
    return false;
  }
  // each firmware key once: of the FM-1's entries, the newest
  // (the lines are counted first: the keys are one block, never a bigger one after a smaller)
  std::vector<Key> keys;
  long first = ftell(hubList);
  size_t lines = 0;
  while (readLine(hubList, line)) {
    lines++;
  }
  keys.reserve(lines);
  if (fseek(hubList, first, SEEK_SET)) {
    return false;
  }
  for (;;) {
    uint32_t here = (uint32_t)ftell(hubList);
    if (!readLine(hubList, line)) {
      break;
    }
    if (split(line, v, HUB_LIST_FIELDS + 1) != HUB_LIST_FIELDS) {
      continue;
    }
    uint64_t hash = hashOf(firmwareKey(v[7], v[6]));
    Key* k = find(keys, hash);
    if (!k) {
      keys.push_back(Key{hash, 0, 0, 0});
      k = &keys.back();
    }
    if (forFm1(v[13], v[7]) && (!(k->flags & Key::FM1) || dateNumber(v[3]) > k->date)) {
      k->flags |= Key::FM1;
      k->line = here;
      k->date = dateNumber(v[3]);
    }
  }
  // the sync record: each recorded file
  std::vector<bool> recorded(count, false);
  std::string rec;
  while (record && readLine(record, rec)) {
    const char* r[5];
    if (rec[0] == removedMark || split(rec, r, 5) != 4) {
      continue;  // (the head, a line that isn't a file's, or a file that a sync removed)
    }
    size_t at = 0;
    while (at < count && strcmp(files[at].path, r[0])) {
      at++;
    }
    if (at < count) {
      recorded[at] = true;
    }
    if (!strncmp(r[3], "extra:", 6)) {
      continue;  // (an extra source: the sync never takes it away)
    }
    Key* k = find(keys, hashOf(r[3]));
    if (!k) {
      if (at < count) {
        fprintf(out, "remove\t%s\n", r[0]);  // (no entry of any device holds its key)
      }
    } else if ((k->flags & Key::FM1) && !(k->flags & Key::HANDLED) && at < count) {
      std::string text;
      if (!lineAt(hubList, k->line, text, v)) {
        continue;
      }
      if (!strcasecmp(r[1], v[8])) {
        fprintf(out, "skip\t%s\n", r[0]);
      } else {
        get(out, "replace", v, r[0]);
      }
      k->flags |= Key::HANDLED;
    }
  }
  // a file of the user holds the key of a Hub firmware: the firmware stays as the user has it
  for (size_t i = 0; i < count; i++) {
    if (recorded[i] || !*files[i].product) {
      continue;
    }
    Key* k = find(keys, hashOf(firmwareKey(files[i].product, files[i].path)));
    if (k && (k->flags & Key::FM1) && !(k->flags & Key::HANDLED)) {
      fprintf(out, "skip\t%s\n", files[i].path);
      k->flags |= Key::HANDLED;
    }
  }
  // the rest of the FM-1's firmwares are not in the library
  for (Key& k : keys) {
    std::string text;
    if ((k.flags & Key::FM1) && !(k.flags & Key::HANDLED) && lineAt(hubList, k.line, text, v)) {
      get(out, "download", v, 0);
    }
  }
  return true;
}
