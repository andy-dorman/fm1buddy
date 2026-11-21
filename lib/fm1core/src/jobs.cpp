#include "jobs.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "datasave.h"
#include "flashbuf.h"
#include "fm1.h"
#include "memory.h"
#include "midi.h"
#include "settings.h"
#include "sloop.h"
#include "syncplan.h"
#include "uboot.h"

namespace jobs {
std::atomic<int> kind{NONE}, step{IDLE};
std::atomic<bool> running{false};
std::atomic<uint32_t> finished{0};
std::atomic<uint32_t> done{0}, total{0};
std::atomic<uint8_t> mapKind[MAP_SECTORS], mapMark[MAP_SECTORS], mapCheck[MAP_SECTORS];
std::atomic<int> mapAt{-1};
std::atomic<bool> mapChecking{false};
std::atomic<uint32_t> mapSeq{0};
bool ok;
std::string error;
bool dataSaveFailed;
std::string dataSaveUid;
bool anotherFm1;
bool noPackage;
bool sectorFailed;
std::vector<hal::Ap> aps;
FwList hub;
std::string hubUpdated;
int device = -1;
sounds::Speaks speaks;
std::string saved;
std::atomic<uint32_t> syncAt{0}, syncOf{0};
char syncName[48];
std::atomic<uint32_t> syncNew{0}, syncUpdated{0}, syncUnchanged{0}, syncRemoved{0}, syncFailed{0};
}  // namespace jobs

using namespace jobs;

static Firmware target;
static std::string ssid, pass, syxFile, setupName;

static void progress(uint32_t d, uint32_t t) {
  done = d;
  total = t;
}

static const char* NAMES[] = {"none", "hub", "download", "scan", "join", "install", "probe", "send", "voice", "keep",
                              "restore", "export", "backup", "prepare", "recover", "sloop backup", "sloop restore",
                              "sloop project", "sloop song", "sloop song in", "sloop samples", "identify", "check update", "firmware update", "sync", "data restore", "update"};
static_assert(sizeof NAMES / sizeof *NAMES == UPDATE + 1, "a name for each job");

// a conversation in SysEx with the FM-1: routing stops meanwhile (its replies aren't for the other devices, and a
// keyboard that doesn't take them in holds up the USB for everyone; nothing else lands in the middle of a message)
static bool talks(Kind k) {
  return k == PROBE || k == SEND || k == VOICE || k == KEEP || k == RESTORE ||
         (k >= SLOOP_BACKUP && k <= SLOOP_SAMPLES);
}

static bool start(Kind k, void (*fn)(void*)) {
  if (running) {
    hal::log("job: %s: busy with %s", NAMES[k], NAMES[(int)kind]);
    return false;
  }
  hal::log("job: %s", NAMES[k]);
  if (talks(k)) {
    midi::thru = false;
  }
  kind = k;
  step = IDLE;
  done = total = 0;
  ok = false;
  error.clear();
  running = true;
  // (the Hub's list and a download use ~4 KB of stack, TLS's handshake included: the rest, more. With no PSRAM
  // every KB counts while Wi-Fi and TLS are up)
  hal::spawn(fn, 0, lowMemory && (k == HUB || k == DOWNLOAD) ? 8192 : 16384);
  return true;
}

static void finish(bool good) {
  if (lowMemory && (kind == HUB || kind == DOWNLOAD || kind == SYNC)) {
    hal::wifiOff();  // (Wi-Fi kept on holds ~30 KB of the internal RAM: joining again takes a few seconds)
  }
  hal::log("job: %s %s%s%s", NAMES[(int)kind], good ? "done" : "failed", good ? "" : ": ", good ? "" : error.c_str());
  ok = good;
  step = IDLE;
  if (talks((Kind)(int)kind)) {
    midi::thru = true;
  }
  running = false;
  finished++;
}

static std::atomic<bool> btPaused{false};
static bool hubAsked = true;

bool jobs::bluetoothPaused() {
  return btPaused.exchange(false);
}

// Wi-Fi for a job: it stays on once it's joined. Only Bluetooth MIDI can't run with it (they don't both fit): while it
// runs (routing on, Bluetooth on), a job the user asked for pauses it (Wi-Fi does that itself), and an automatic job
// doesn't run
static bool wifiFor(bool asked) {
  if (!midi::routing || !hal::bleIsOn() || hal::wifiOn()) {
    return true;
  }
  if (!asked) {
    error = "Bluetooth MIDI is on, so Wi-Fi is off.";
    return false;
  }
  btPaused = true;
  return true;
}

static bool joinSaved(bool asked = true) {
  if (!wifiFor(asked)) {
    return false;
  }
  if (hal::wifiOn()) {
    return true;  // (still on)
  }
  if (!settings.ssid[0]) {
    error = "No Wi-Fi network yet: pick one in Settings.";
    return false;
  }
  step = JOINING;
  hal::log("job: joining %s", settings.ssid);
  bool ok = hal::wifiJoin(settings.ssid, settings.pass, error);
  hal::log("job: %s", ok ? "joined" : "not joined");
  return ok;
}

static void mkdirs() {
  mkdir(catalog::path(FW_DIR).c_str(), 0777);
  mkdir(catalog::path(DATA_DIR).c_str(), 0777);
}

static std::string readText(const std::string& p) {
  char buf[128] = "";
  if (FILE* f = fopen(p.c_str(), "r")) {
    buf[fread(buf, 1, sizeof buf - 1, f)] = 0;
    fclose(f);
  }
  return buf;
}

// the Hub's list onto the SD card (HUB_LIST), with Wi-Fi joined: false (error says why)
static bool getHub() {
  mkdirs();
  step = FETCHING;
  std::string etagPath = catalog::path(HUB_ETAG), cache = catalog::path(HUB_CACHE);
  struct stat st;
  // (the list it's from is asked for again only if it's kept and reads, in this format: else the whole list comes)
  std::string kept;
  std::string etag = stat(catalog::path(HUB_LIST).c_str(), &st) || !catalog::hubWhen(kept) ? "" : readText(etagPath);
  hal::logRam("before the list");
  hal::log("job: getting the list (etag %s)", etag.c_str());
  int code = hal::get(HUB_SITE "catalog.json", cache.c_str(), &etag, progress, error);
  hal::log("job: HTTP %d", code);
  if (code != 200 && code != 304) {
    if (code > 0) {
      error = "The Hub said " + std::to_string(code) + ".";
    }
    return false;
  }
  step = READING;
  bool ok;
  if (code == 304) {  // the same list: the one kept (in memory too, if the app keeps it: catalog::keepHub)
    ok = catalog::keepHub ? catalog::loadHub(hub, hubUpdated) : catalog::hubWhen(hubUpdated);
    if (!ok) {
      error = "The Hub's list on the SD card didn't read.";
    }
  } else if ((ok = catalog::keepHub ? catalog::parseHub(hub, hubUpdated, error) : catalog::writeHub(hubUpdated, error))) {
    if (FILE* f = fopen(etagPath.c_str(), "w")) {
      fputs(etag.c_str(), f);
      fclose(f);
    }
  }
  hal::logRam("the list read");
  return ok;
}

static void hubJob(void*) {
  if (!hal::sd()) {
    error = "No SD card: the list is kept on it.";
    return finish(false);
  }
  if (!joinSaved(hubAsked)) {
    return finish(false);
  }
  finish(getHub());
}

// ------------------------------------------------------------------------------------- fm1buddy's update ---
// the installer page: its version.json says the newest release ({"tag": "v0.1.37", "files": {"fm1buddy-cyd-s3.bin":
// sha256, ...}}), and the release's .bins are next to it (as the installer writes them: all of the flash from 0)
#define UPDATE_SITE "https://konsumer.js.org/fm1buddy/fw/"
#define UPDATE_INFO DATA_DIR "/version.json"
#define UPDATE_BIN DATA_DIR "/update.bin"
std::string jobs::updateTag;
static std::string updateSha;  // the newer release's sha256 of this board's .bin ("" if the page doesn't say)

// "v0.1.37" as numbers; false if it isn't a release's version
static bool versionOf(const std::string& v, int n[3]) {
  return sscanf(v.c_str(), "v%d.%d.%d", &n[0], &n[1], &n[2]) == 3;
}

bool jobs::newer(const std::string& a, const std::string& b) {
  int x[3], y[3];
  if (!versionOf(a, x)) {
    return false;
  }
  if (!versionOf(b, y)) {
    return true;
  }
  return std::lexicographical_compare(y, y + 3, x, x + 3);
}

static void checkUpdateJob(void*) {
  updateTag.clear();
  if (!hal::sd()) {
    error = "No SD card: the update goes on it first.";
    return finish(false);
  }
  if (!joinSaved()) {
    return finish(false);
  }
  mkdirs();
  step = FETCHING;
  std::string p = catalog::path(UPDATE_INFO);
  int code = hal::get(UPDATE_SITE "version.json", p.c_str(), 0, 0, error);
  hal::log("job: version.json: HTTP %d", code);
  if (code != 200) {
    if (code > 0) {
      error = "The installer page said " + std::to_string(code) + ".";
    }
    return finish(false);
  }
  JsonDocument doc;
  FILE* f = fopen(p.c_str(), "r");
  std::string text;
  char buf[256];
  for (size_t n; f && (n = fread(buf, 1, sizeof buf, f)) > 0;) {
    text.append(buf, n);
  }
  if (f) {
    fclose(f);
  }
  std::string tag;
  if (deserializeJson(doc, text) || (tag = doc["tag"] | "").empty()) {
    error = "The installer page's version didn't read.";
    return finish(false);
  }
  // a release from before it listed its files has the 2.8"'s, at least; the others only if it says so
  JsonVariant files = doc["files"];
  bool listed = files.is<JsonObject>();
  bool there = listed ? files[FM1BUDDY_BIN].is<const char*>() : !strcmp(FM1BUDDY_BIN, "fm1buddy-cyd-s3.bin");
  hal::log("job: newest %s (this is %s), %s %s", tag.c_str(), FM1BUDDY_VERSION, FM1BUDDY_BIN,
           there ? "there" : "not there");
  if (there && newer(tag, FM1BUDDY_VERSION)) {
    updateTag = tag;
    updateSha = listed ? (const char*)(files[FM1BUDDY_BIN] | "") : "";
  }
  finish(true);
}

bool jobs::checkUpdate() {
  return start(CHECK_UPDATE, checkUpdateJob);
}

static void updateJob(void*) {
  if (!hal::sd()) {
    error = "No SD card: the update goes on it first.";
    return finish(false);
  }
  if (!joinSaved()) {
    return finish(false);
  }
  mkdirs();
  step = FETCHING;
  std::string p = catalog::path(UPDATE_BIN);
  int code = hal::get(UPDATE_SITE FM1BUDDY_BIN, p.c_str(), 0, progress, error);
  hal::log("job: %s: HTTP %d", FM1BUDDY_BIN, code);
  if (code != 200) {
    if (code > 0) {
      error = "The installer page said " + std::to_string(code) + ".";
    }
    return finish(false);
  }
  step = CHECKING;
  char sum[65];
  if (!updateSha.empty() && (!hal::sha256(p.c_str(), sum) || strcasecmp(sum, updateSha.c_str()))) {
    remove(p.c_str());
    error = "The download isn't what the installer page says it is (sha256).";
    return finish(false);
  }
  step = UPDATING;
  done = total = 0;
  const char* e = hal::selfUpdate(p.c_str(), progress);
  remove(p.c_str());
  if (e) {
    error = e;
    return finish(false);
  }
  finish(true);
}

bool jobs::update() {
  return !updateTag.empty() && start(UPDATE, updateJob);
}

// target from the Hub into /fm1 as file (Wi-Fi joined already), then its size, its sha256 and its device checked: false
// (error says why; the file is removed)
static bool fetchFile(const std::string& file) {
  step = FETCHING;
  std::string p = catalog::path(std::string(FW_DIR "/") + file);
  size_t slash = file.rfind('/');
  if (slash != std::string::npos) {
    mkdir(catalog::path(FW_DIR "/" + file.substr(0, slash)).c_str(), 0777);  // (old-chip)
  }
  hal::log("job: getting %s", target.pkg.c_str());
  std::string url = target.pkg;  // (a whole URL: the maker's own file)
  int code = hal::get(url.c_str(), p.c_str(), 0, progress, error);
  hal::log("job: HTTP %d", code);
  if (code != 200) {
    if (code > 0) {
      error = "The server said " + std::to_string(code) + ".";
    }
    return false;
  }
  step = CHECKING;
  struct stat st;
  char sum[65];
  if (stat(p.c_str(), &st) || (uint32_t)st.st_size != target.size) {
    error = "The download stopped part way.";
  } else if (!hal::sha256(p.c_str(), sum) || strcasecmp(sum, target.sha256.c_str())) {
    error = "The download isn't what the Hub says it is (sha256).";
  } else if (catalog::product(p.c_str()) != target.product) {
    error = "The package isn't for the device the Hub says.";
  } else {
    return true;
  }
  remove(p.c_str());
  return false;
}

// the SD card there and Wi-Fi joined (one time: it stays on), the folders made: false (error says why)
static bool fetchReady() {
  if (!hal::sd()) {
    error = "No SD card.";
    return false;
  }
  if (!joinSaved()) {
    return false;
  }
  mkdirs();
  return true;
}

// target from the Hub into /fm1: false (error says why)
static bool fetch() {
  return fetchReady() && fetchFile(target.file);
}

// a name in /fm1 for target that no other firmware has (the Hub has a few with the same file name: one mustn't replace
// another). Its own, if that's free or the very same file is there; else with its version ("felucca-1.0.0.fwsc"),
// then -a, -b, ... ("felucca-1.0.0-a.fwsc"). keep: a file it may replace anyway (FW_UPDATE: the old one)
static bool sameFile(const std::string& p) {
  struct stat st;
  char sum[65];
  return !stat(p.c_str(), &st) && (uint32_t)st.st_size == target.size && hal::sha256(p.c_str(), sum) &&
         !strcasecmp(sum, target.sha256.c_str());
}

static std::string freeName(const std::string& file, const std::string& keep = "") {
  size_t slash = file.rfind('/');
  std::string folder = slash == std::string::npos ? "" : file.substr(0, slash + 1);
  std::string base = file.substr(folder.size()), ext;
  if (base.size() > 5 && !strcasecmp(base.c_str() + base.size() - 5, ".fwsc")) {
    ext = base.substr(base.size() - 5);
    base.resize(base.size() - 5);
  }
  std::string v;  // (its version, as a file name can have it)
  for (char c : target.version) {
    v += isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' ? c : '-';
  }
  std::string lower = base, lv = v;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  std::transform(lv.begin(), lv.end(), lv.begin(), ::tolower);
  std::vector<std::string> names{base};
  std::string withV = v.empty() || lower.find(lv) != std::string::npos ? base : base + "-" + v;
  if (withV != base) {
    names.push_back(withV);
  }
  for (char c = 'a'; c <= 'z'; c++) {
    names.push_back(withV + "-" + c);
  }
  for (const std::string& n : names) {
    std::string rel = folder + n + ext, p = catalog::path(FW_DIR "/" + rel);
    struct stat st;
    if (rel == keep || stat(p.c_str(), &st) || sameFile(p)) {
      if (rel != file) {
        hal::log("job: %s is another firmware's on the SD card: %s", file.c_str(), rel.c_str());
      }
      return rel;
    }
  }
  return file;  // (27 others of that name: it's replaced)
}

// the sync record again with the file just downloaded in it (through a .tmp file: the old record stays until the new one
// is written). false if it can't be written
static bool recordDownloaded(const std::string& key) {
  std::string rec = catalog::path(SYNC_RECORD), tmp = rec + ".tmp";
  if (rec.empty()) {
    return false;
  }
  mkdir(catalog::path(DATA_DIR).c_str(), 0777);
  FILE* in = fopen(rec.c_str(), "r");  // (null: there is no record yet)
  FILE* out = fopen(tmp.c_str(), "w");
  std::string path = std::string(FW_DIR "/") + target.file;
  bool ok = out && syncplan::recordDownload(in, out, path.c_str(), target.sha256.c_str(), target.size, key.c_str()) &&
            !fflush(out);
  if (in) {
    fclose(in);
  }
  if (out) {
    fsync(fileno(out));
    ok = !ferror(out) && !fclose(out) && ok;
  }
  if (ok) {
    remove(rec.c_str());
    ok = !rename(tmp.c_str(), rec.c_str());
  }
  if (!ok) {
    remove(tmp.c_str());
  }
  return ok;
}

static void downloadJob(void*) {
  std::string key = syncplan::firmwareKey(target.product, target.file);  // (before freeName: a number may join the name)
  target.file = freeName(target.file);
  bool ok = fetch();
  saved = target.file;
  if (ok) {
    bool recorded = recordDownloaded(key);  // (a failure leaves the file on the card; a later sync takes it for a file of the user)
    hal::log("job: %s %s in the sync record", target.file.c_str(), recorded ? "is" : "can't go");
  }
  finish(ok);
}

static std::string oldFile;  // FW_UPDATE: what the new one replaces

// target from the Hub into /fm1 (Wi-Fi joined already) in place of the file old ("": none) through a .part file: old
// stays until the new one is checked, even with the same name. false (error says why; no .part stays)
static bool replaceFile(const std::string& old) {
  std::string file = freeName(target.file, old), dir = catalog::path(FW_DIR "/");
  target.file = file + ".part";
  if (!fetchFile(target.file)) {
    remove((dir + target.file).c_str());
    return false;
  }
  if (!old.empty()) {
    remove((dir + old).c_str());
  }
  remove((dir + file).c_str());  // (the very same file, if it's there: freeName)
  if (rename((dir + target.file).c_str(), (dir + file).c_str())) {
    error = "Can't name it " + file + " on the SD card.";
    remove((dir + target.file).c_str());
    return false;
  }
  target.file = saved = file;
  if (!old.empty()) {
    hal::log("job: %s replaced by %s", old.c_str(), file.c_str());
  }
  return true;
}

// the sync record line of the file that one update replaced goes to the new file (a file with no line gets none)
static bool recordMoved(const std::string& from, const std::string& to) {
  std::string rec = catalog::path(SYNC_RECORD), tmp = rec + ".tmp";
  FILE* in = fopen(rec.c_str(), "r");
  if (rec.empty() || !in) {
    return false;  // (no record: nothing to move)
  }
  FILE* out = fopen(tmp.c_str(), "w");
  std::string was = std::string(FW_DIR "/") + from, now = std::string(FW_DIR "/") + to;
  bool ok = out && syncplan::moveRecordLine(in, out, was.c_str(), now.c_str(), target.sha256.c_str(), target.size) &&
            !fflush(out);
  fclose(in);
  if (out) {
    fsync(fileno(out));
    ok = !ferror(out) && !fclose(out) && ok;
  }
  if (ok) {
    remove(rec.c_str());
    ok = !rename(tmp.c_str(), rec.c_str());
  }
  if (!ok) {
    remove(tmp.c_str());
  }
  return ok;
}

static void fwUpdateJob(void*) {
  if (!fetchReady()) {
    return finish(false);
  }
  std::string from = oldFile;
  bool ok = replaceFile(from);
  if (ok && !from.empty()) {
    bool moved = recordMoved(from, target.file);  // (a failure leaves the old line: a later sync sees a missing file)
    hal::log("job: sync record line of %s %s", target.file.c_str(), moved ? "moved" : "not moved");
  }
  finish(ok);
}

// ------------------------------------------------------------------------------------------------- sync ---
// The Hub's firmwares into /fm1 as the sync plan says (syncplan.h). The sync record (SYNC_RECORD) holds the files it
// wrote, a line after each file that is checked and a mark after each removal, so a stop or a power loss leaves it true
static std::atomic<bool> stopSync{false};

void jobs::cancel() {
  stopSync = true;
}

static void setName(const char* s) {
  snprintf(syncName, sizeof syncName, "%s", s);
}

// a line for the file just checked, and the mark on the line of a file removed (each written to the card at once)
static bool recordNew(const char* path, const char* sha256, uint32_t size, const char* key) {
  FILE* f = fopen(catalog::path(SYNC_RECORD).c_str(), "a");
  if (!f) {
    return false;
  }
  bool ok = syncplan::appendRecord(f, path, sha256, size, key) && !fflush(f);
  fsync(fileno(f));
  fclose(f);
  return ok;
}

static bool recordGone(const char* path) {
  FILE* f = fopen(catalog::path(SYNC_RECORD).c_str(), "r+");
  if (!f) {
    return false;
  }
  bool ok = syncplan::markRemoved(f, path);
  fsync(fileno(f));
  fclose(f);
  return ok;
}

// the sync record again without the lines of the files that were removed
static void recordRewritten() {
  std::string rec = catalog::path(SYNC_RECORD), tmp = rec + ".tmp";
  FILE* in = fopen(rec.c_str(), "r");
  if (!in) {
    return;
  }
  FILE* out = fopen(tmp.c_str(), "w");
  bool ok = out && syncplan::rewriteRecord(in, out) && !fflush(out);
  fclose(in);
  if (out) {
    fsync(fileno(out));
    fclose(out);
  }
  if (ok) {
    remove(rec.c_str());
    ok = !rename(tmp.c_str(), rec.c_str());
  }
  if (!ok) {
    remove(tmp.c_str());
  }
}

// the plan for /fm1 as it is now, from the Hub's list and the sync record, into SYNC_PLAN: false if the list doesn't read
static bool makePlan() {
  FwList none, sd;  // (none: the Hub's list stays on the card)
  catalog::scanSdInto(none, sd);
  std::vector<std::string> paths;
  std::vector<syncplan::SdFile> files;
  paths.reserve(sd.items.size());  // (each in one block: the RAM is small)
  files.reserve(sd.size());
  for (const FwItem& it : sd.items) {
    paths.push_back(std::string(FW_DIR "/") + sd.str(it.file));
  }
  for (size_t i = 0; i < sd.size(); i++) {
    files.push_back({paths[i].c_str(), sd.str(sd[i].product), sd.str(sd[i].sha256), sd[i].size});
  }
  FILE* list = fopen(catalog::path(HUB_LIST).c_str(), "r");
  FILE* rec = fopen(catalog::path(SYNC_RECORD).c_str(), "r");
  FILE* out = fopen(catalog::path(SYNC_PLAN).c_str(), "w");
  hal::logRam("before the plan");
  bool good = list && out && syncplan::plan(list, rec, files.data(), files.size(), out);
  hal::logRam("after the plan");
  for (FILE* f : {list, rec, out}) {
    if (f) {
      fclose(f);
    }
  }
  return good;
}

// one download or replace of the plan: a line of fields (n of them). The failed ones are counted and logged
static void syncGet(const char* const v[], int n) {
  const std::string root = FW_DIR "/";
  target = Firmware();
  target.file = v[1] + root.size();
  target.pkg = v[2];
  target.sha256 = v[3];
  target.size = (uint32_t)strtoul(v[4], 0, 10);
  target.name = v[5];
  target.version = v[6];
  target.product = v[7];
  std::string hubFile = target.file, old = n > 8 ? v[8] + root.size() : "";
  step = FETCHING;
  const std::string reports = saved;  // (replaceFile puts the file's name in saved: a sync keeps its own lines there)
  bool got = replaceFile(old);
  saved = reports;
  if (!got) {
    hal::log("sync: %s: %s", v[5], error.c_str());
    error.clear();
    syncFailed++;
    return;
  }
  // (the old file's line first: the new file can have its path)
  if (!old.empty()) {
    recordGone((root + old).c_str());
  }
  recordNew((root + target.file).c_str(), target.sha256.c_str(), target.size, syncplan::firmwareKey(target.product, hubFile).c_str());
  (old.empty() ? syncNew : syncUpdated)++;
}

// ---------------------------------------------------------------------------------------- extra sources ---
// Shrub and the sampler: one file each at a fixed address, flat in /fm1. syncplan::decideExtra says what to do with a
// download (the sampler's address is the GitLab Pages one of its project: not final, and no file there yet)
struct ExtraSource {
  const char* name;  // in the sync record
  const char* file;  // in /fm1
  const char* url;
};
static const ExtraSource EXTRA_SOURCES[] = {
    {"extra:shrub", "SHRUB.fwsc", "https://shrub.audio/editor/firmware/SHRUB.fwsc"},
    {"extra:sampler", "fm1-sampler.fwsc", "https://aomegasolutions.gitlab.io/fm1-sampler-ios/fm1-sampler.fwsc"},
};

static long extraLength;  // the server's Content-Length of the download now (-1: none), from hal::get's progress

static void extraProgress(uint32_t d, uint32_t t) {
  progress(d, t);
  if (t) {
    extraLength = t;
  }
}

// whether the sync record holds a line (not removed) for path
static bool recordHolds(const std::string& path) {
  FILE* f = fopen(catalog::path(SYNC_RECORD).c_str(), "r");
  bool held = false;
  std::string line;
  while (f && !held && syncplan::readLine(f, line)) {
    held = line.size() > path.size() && !line.compare(0, path.size(), path) && line[path.size()] == '\t';
  }
  if (f) {
    fclose(f);
  }
  return held;
}

// a line for the end screen (saved) and the log
static void extraReport(const std::string& report) {
  hal::log("sync: %s", report.c_str());
  saved += report + "\n";
}

// src from its address into /fm1 through a .part file, only if its sha256 isn't the recorded one
static void syncExtra(const ExtraSource& src) {
  const std::string label = src.name + 6, path = std::string(FW_DIR "/") + src.file;
  const std::string real = catalog::path(path), part = real + ".part";
  setName(label.c_str());
  done = total = 0;
  extraLength = -1;
  hal::log("sync: getting %s", src.url);
  int code = hal::get(src.url, part.c_str(), 0, extraProgress, error);
  hal::log("sync: HTTP %d", code);
  // (-1 is both no connection and a download cut short: the reason tells)
  bool cut = code == -1 && (error.find("part way") != std::string::npos || error.find("Can't write") != std::string::npos ||
                            error.find("Can't rename") != std::string::npos);
  error.clear();
  if (cut) {
    remove(part.c_str());
    syncFailed++;
    return extraReport(label + ": the download stopped part way");
  }
  step = CHECKING;
  struct stat st;
  char sum[65] = "";
  bool got = code == 200 && !stat(part.c_str(), &st);
  uint32_t size = got ? (uint32_t)st.st_size : 0;
  if (got && !hal::sha256(part.c_str(), sum)) {
    sum[0] = 0;
  }
  std::string product = got ? catalog::product(part.c_str()) : "";
  syncplan::SdFile there = {path.c_str(), "", "", 0};
  bool exists = !stat(real.c_str(), &st);
  FILE* rec = fopen(catalog::path(SYNC_RECORD).c_str(), "r");
  syncplan::ExtraDecision d = syncplan::decideExtra(src.name, src.file, rec, &there, exists ? 1 : 0,
                                                    {got, size, extraLength, sum, product.c_str()});
  if (rec) {
    fclose(rec);
  }
  bool hadLine = recordHolds(path);
  step = FETCHING;
  switch (d.result) {
    case syncplan::EXTRA_UNCHANGED:
      hal::log("sync: %s: unchanged", label.c_str());
      syncUnchanged++;
      break;
    case syncplan::EXTRA_FAILED:
      syncFailed++;
      extraReport(d.report);
      break;
    case syncplan::EXTRA_USER_FILE:
    case syncplan::EXTRA_NOT_FOUND:
      extraReport(d.report);
      break;
    case syncplan::EXTRA_REPLACE:
      remove(real.c_str());
      if (rename(part.c_str(), real.c_str())) {
        syncFailed++;
        extraReport(label + ": can't name it " + src.file + " on the SD card");
        break;
      }
      // (the new line before the mark on the old one: a stop between them leaves the file in the record)
      recordNew(path.c_str(), sum, size, src.name);
      if (hadLine) {
        recordGone(path.c_str());
      }
      hal::log("sync: %s: %s", label.c_str(), exists ? "updated" : "new");
      (exists ? syncUpdated : syncNew)++;
      break;
  }
  remove(part.c_str());
}

// the sync after the extra sources: the Hub list, the plan, the downloads. true unless the sync failed (a stop is not
// a failure)
static bool syncHub() {
  if (!getHub()) {
    return false;
  }
  if (stopSync) {
    return true;
  }
  step = READING;
  if (!makePlan()) {
    error = "The Hub's list on the SD card didn't read.";
    return false;
  }
  std::string planPath = catalog::path(SYNC_PLAN), line;
  FILE* plan = fopen(planPath.c_str(), "r");
  if (!plan) {
    error = "Can't read the sync plan on the SD card.";
    return false;
  }
  while (syncplan::readLine(plan, line)) {
    syncOf++;
  }
  rewind(plan);
  bool good = true;
  step = FETCHING;
  done = total = 0;  // (the bytes of each file, from here)
  while (!stopSync && syncplan::readLine(plan, line)) {
    const char* v[12];
    int n = syncplan::split(line, v, 12);
    bool gets = n >= 8;  // (a download or a replace: the Hub's name for it)
    const char* slash = n > 1 ? strrchr(v[1], '/') : 0;
    setName(gets ? v[5] : slash ? slash + 1
                                : "");
    syncAt++;
    if (!strcmp(v[0], "skip")) {
      syncUnchanged++;
    } else if (!strcmp(v[0], "remove") && n == 2) {
      if (remove(catalog::path(v[1]).c_str()) && errno != ENOENT) {
        hal::log("sync: %s: can't remove it", v[1]);
        syncFailed++;
      } else {
        recordGone(v[1]);
        syncRemoved++;
      }
    } else if (gets) {
      if (!hal::wifiOn()) {
        error = "Wi-Fi went away.";
        good = false;
        break;
      }
      syncGet(v, n);
    }
  }
  fclose(plan);
  return good;
}

static void syncJob(void*) {
  if (!fetchReady()) {
    return finish(false);
  }
  syncAt = syncOf = 0;
  step = FETCHING;
  for (const ExtraSource& src : EXTRA_SOURCES) {
    if (!stopSync) {
      syncExtra(src);
    }
  }
  setName("");
  done = total = 0;
  // (one way out for the rest: the record is written again, without the lines of the files that were replaced or removed)
  bool good = stopSync || syncHub();
  recordRewritten();
  setName("");
  finish(good);
}

static std::atomic<uint32_t> askedAt;  // the update loader's last request (hal::millis)

static void installProgress(Fm1Step s, uint32_t bytes, uint32_t) {
  static const Step STEPS[] = {ASKING, VERIFYING, LOADER, WRITING, RESTARTING, RESTARTING};
  step = STEPS[s];
  done = s == STEP_WRITE ? bytes : 0;
  askedAt = hal::millis();
}

// a new install record: /fm1buddy/installs/N.txt, the last 10 kept. name gets "installs/N.txt"
static FILE* newRecord(std::string& name) {
  std::string dir = catalog::path(INSTALLS_DIR);
  mkdir(dir.c_str(), 0777);
  int last = 0;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      last = std::max(last, atoi(e->d_name));
    }
    closedir(d);
  }
  for (int n = last - 9; n > 0 && n > last - 30; n--) {
    remove((dir + "/" + std::to_string(n) + ".txt").c_str());
  }
  name = "installs/" + std::to_string(last + 1) + ".txt";
  FILE* f = fopen((dir + "/" + std::to_string(last + 1) + ".txt").c_str(), "w");
  if (f) {
    setvbuf(f, 0, _IOFBF, 8192);
  }
  return f;
}

static bool installDataSave = true;  // INSTALL: a data save first
static bool dataSaveBefore();        // (with the backup)
static void dataSaveReset();

static void installJob(void*) {
  if (!target.onSd) {
    target.file = freeName(target.file);
    if (!fetch()) {
      return finish(false);
    }
  }
  saved = target.file;
  step = CHECKING;
  Pkg pkg;
  if (const char* e = pkg.open(catalog::path(std::string(FW_DIR "/") + target.file).c_str())) {
    error = e;
    return finish(false);
  }
  if (!target.sha256.empty() && strcasecmp(pkg.sha256, target.sha256.c_str())) {
    error = "The file on the SD card isn't what the Hub says it is (sha256): download it again.";
    return finish(false);
  }
  if (installDataSave && !dataSaveBefore()) {
    return finish(false);
  }
  std::string name;
  FILE* rec = newRecord(name);
  if (rec) {
    fprintf(rec, "fm1buddy install\n");
    fprintf(rec, "file     %s\n", target.file.c_str());
    fprintf(rec, "size     %u (image %u)\n", (unsigned)pkg.fileSize, (unsigned)pkg.size);
    fprintf(rec, "product  %s\n", pkg.product);
    fprintf(rec, "sha256   %s\n", pkg.sha256);
    fprintf(rec, "known    %s\n\n",
            target.sha256.empty() ? "no (not on the Hub, nor one of M-VAVE's): not checked" : "yes: its sha256 checked");
    fflush(rec);
    fsync(fileno(rec));
  }
  // what the update loader writes is the firmware (flash.bin: from the top down, a few header reads besides)
  uint32_t at = 0, n = 0;
  total = pkg.flash(at, n) ? n : pkg.size;
  step = ASKING;
  hal::bleHold(true);  // the radio stays out of it
  const char* e = fm1::install(pkg, installProgress, rec);
  hal::bleHold(false);
  if (rec) {
    fclose(rec);
  }
  if (e) {
    error = e;
    if (rec) {
      error += " (details: " DATA_DIR "/" + name + ")";
    }
  }
  finish(!e);
}

static void scanJob(void*) {
  aps.clear();
  wifiFor(true);
  bool good = hal::wifiScan(aps);
  if (!good) {
    error = "Wi-Fi didn't scan.";
  }
  finish(good);
}

static void joinJob(void*) {
  step = JOINING;
  wifiFor(true);
  finish(hal::wifiJoin(ssid.c_str(), pass.c_str(), error));
}

bool jobs::hubList(bool asked) {
  if (running) {
    return false;
  }
  hubAsked = asked;
  return start(HUB, hubJob);
}

bool jobs::download(const Firmware& f) {
  if (running) {
    return false;
  }
  target = f;
  return start(DOWNLOAD, downloadJob);
}

bool jobs::updateFirmware(const Firmware& newer, const std::string& old) {
  if (running) {
    return false;
  }
  target = newer;
  oldFile = old;
  return start(FW_UPDATE, fwUpdateJob);
}

bool jobs::sync() {
  if (running) {
    return false;
  }
  stopSync = false;
  syncAt = syncOf = syncNew = syncUpdated = syncUnchanged = syncRemoved = syncFailed = 0;
  setName("");
  saved.clear();
  return start(SYNC, syncJob);
}

bool jobs::install(const Firmware& f, bool dataSave) {
  if (running) {
    return false;
  }
  target = f;
  installDataSave = dataSave;
  dataSaveReset();
  return start(INSTALL, installJob);
}

static void probeJob(void*) {
  step = ASKING_IT;
  sounds::probe(device, speaks);
  finish(true);
}

static void sendJob(void*) {
  std::vector<uint8_t> data;
  if (!sounds::load(syxFile, data)) {
    error = "Can't read it from the SD card.";
    return finish(false);
  }
  step = SENDING;
  total = data.size();
  if (const char* e = sounds::send(device, data, progress)) {
    error = e;
    return finish(false);
  }
  finish(true);
}

static void voiceJob(void*) {
  step = ASKING_IT;
  std::vector<uint8_t> syx;
  if (const char* e = sounds::readVoice(device, syx)) {
    error = e;
    return finish(false);
  }
  saved = sounds::save(syx, "voice");
  if (saved.empty()) {
    error = "Can't write it to the SD card.";
  }
  finish(!saved.empty());
}

static int phase;  // KEEP, RESTORE: where it's at (sounds.h)

static void keepJob(void*) {
  step = GETTING;
  total = 0;
  if (const char* e = sounds::keep(device, saved, progress, &phase)) {
    error = e;
    return finish(false);
  }
  finish(true);
}

static void restoreJob(void*) {
  step = SENDING;
  total = 0;
  if (const char* e = sounds::restore(device, setupName, progress, &phase)) {
    error = e;
    return finish(false);
  }
  finish(true);
}

// ------------------------------------------------------------------------------------------ backup ---
static int reading;  // BACKUP: which reading of its flash (it's read twice, and they must be the same)

// ------------------------------------------------------------------------------------ the flash map ---
// an SMK-37's flash (its calibration's mark: uboot.h), or a backup of one
static bool smkIn(const FlashBuf& flash) {
  uint8_t m[7];
  return flash.read(SMK_MARK, m, sizeof m) && !memcmp(m, "trimdat", 7);
}

// a reading of the flash, a sector at a time as it comes: into out (if there's one), and onto the map: what's on it (the
// first reading), or whether it's the same as ref says it should be (a reading again: mapBad counts those that aren't)
static FlashBuf* mapOut;
static bool (*mapRef)(uint32_t at, uint8_t* out);  // the sector at at, as it should be
static std::vector<uint8_t> mapSec, mapWant;       // the sector coming, and ref's
static uint32_t mapNext;                           // the next sector to put on the map
static uint32_t mapBad;
static bool mapLost;  // out didn't take a sector (the SD card)

static void mapClear() {
  for (int i = 0; i < MAP_SECTORS; i++) {
    mapKind[i] = MAP_UNREAD;
    mapMark[i] = MARK_NONE;
    mapCheck[i] = CHECK_NONE;
  }
  mapAt = -1;
  mapChecking = false;
  mapSeq++;
}

static uint8_t kindOf(const uint8_t* p, uint32_t s) {
  if (s * SECTOR < WRITE_FROM) {
    return MAP_HEAD;
  }
  if (s * SECTOR >= WRITE_TO) {
    return MAP_KEEP;
  }
  for (uint32_t i = 0; i < SECTOR; i++) {
    if (p[i] != 0xFF) {
      return MAP_DATA;
    }
  }
  return MAP_FREE;
}

// --------------------------------------------------------------------------- the FM-1's own screen ---
// While the loader runs, the FM-1 shows what's going on: a title and the percentage, what it's doing, and the same map
// as here (its sectors in the theme's colors: screenColors). A sector's state there, one of these
enum FmState : uint8_t { FM_UNREAD,
                         FM_TO_WRITE,
                         FM_WRITTEN,
                         FM_DATA,
                         FM_FREE,
                         FM_NEVER,
                         FM_BAD };
static bool fmOn;
static uint32_t fmColors[SCREEN_COLORS];
static const char* fmTitle;
static const char* fmWhat;
static int fmFrom, fmSpan;  // a reading's percentages: from, and how many it takes up
static uint32_t fmShown;    // when it was drawn last (hal::millis)

static uint8_t fmState(int i) {
  if (mapCheck[i] == CHECK_BAD) {
    return FM_BAD;
  }
  switch (mapMark[i]) {
    case MARK_DIFFERS:
      return FM_TO_WRITE;
    case MARK_WRITTEN:
      return FM_WRITTEN;
  }
  switch (mapKind[i]) {
    case MAP_HEAD:
    case MAP_KEEP:
      return FM_NEVER;
    case MAP_DATA:
      return FM_DATA;
    case MAP_FREE:
      return FM_FREE;
  }
  return FM_UNREAD;
}

// its screen on (after uboot::enter or attach): the FM-1's panel, or the SMK-37's
static void fmStart(bool smk, const char* title) {
  screenColors(fmColors);
  fmTitle = title;
  fmWhat = "";
  fmShown = 0;
  fmOn = uboot::screenInit(smk, fmColors, SCREEN_LOADER_COLORS);
}

// the title and how far (percent < 0: none), what it's doing, and the map. At most every 200 ms, unless now
static void fmShow(const char* what, int percent, bool now = false) {
  if (!fmOn || (!now && hal::millis() - fmShown < 200)) {
    return;
  }
  fmShown = hal::millis();
  fmWhat = what;
  char line[24];
  if (percent >= 0) {
    snprintf(line, sizeof line, "%s %d%%", fmTitle, std::min(percent, 100));
  } else {
    snprintf(line, sizeof line, "%s", fmTitle);
  }
  uint8_t states[MAP_SECTORS];
  for (int i = 0; i < MAP_SECTORS; i++) {
    states[i] = fmState(i);
  }
  fmOn = uboot::screenRow(0, fmColors[SCREEN_TEXT], fmColors[0], line) &&
         uboot::screenRow(1, fmColors[SCREEN_DIM], fmColors[0], what) && uboot::screenMap(states);
  if (!fmOn) {
    hal::log("jobs: the FM-1's screen stopped taking it");
  }
}

// a reading of the flash from here on: what it says, and its part of the percentage
static void fmReading(const char* what, int from, int span) {
  fmWhat = what, fmFrom = from, fmSpan = span;
  fmShow(what, from, true);
}

// (uboot::read's progress)
static void mapProgress(uint32_t d, uint32_t t) {
  progress(d, t);
  fmShow(fmWhat, fmFrom + (int)(t ? (uint64_t)d * fmSpan / t : fmSpan), d >= t);
  mapAt = mapNext < MAP_SECTORS && d < t ? (int)mapNext : -1;
  mapSeq++;
}

// sector s has all come (mapSec)
static void mapSector(uint32_t s) {
  const uint8_t* p = mapSec.data();
  if (mapOut && !mapOut->write(s * SECTOR, p, SECTOR)) {
    mapLost = true;
  }
  if (mapRef) {
    bool same = mapRef(s * SECTOR, mapWant.data()) && !memcmp(p, mapWant.data(), SECTOR);
    mapBad += !same;
    if (s < MAP_SECTORS) {
      mapCheck[s] = same ? CHECK_SAME : CHECK_BAD;
    }
  } else if (s < MAP_SECTORS) {
    mapKind[s] = kindOf(p, s);
  }
  mapNext = s + 1;
}

// (uboot::read's pieces)
static void mapGot(uint32_t at, const uint8_t* p, uint32_t n) {
  while (n) {
    uint32_t off = at % SECTOR, k = std::min<uint32_t>(n, SECTOR - off);
    memcpy(mapSec.data() + off, p, k);
    at += k, p += k, n -= k;
    if (at % SECTOR == 0) {
      mapSector(at / SECTOR - 1);
    }
  }
}

// the whole flash into out (if there's one), onto the map: against ref (a reading again: then 0 only if every sector
// is as ref says), or not
static const char* mapRead(const uboot::Chip& chip, FlashBuf* out, bool (*ref)(uint32_t, uint8_t*)) {
  mapOut = out;
  mapRef = ref;
  mapSec.assign(SECTOR, 0);
  mapWant.assign(ref ? SECTOR : 0, 0);
  mapNext = mapBad = 0;
  mapLost = false;
  mapChecking = ref != 0;
  const char* e = uboot::read(chip, mapGot, mapProgress);
  if (!e && mapLost) {
    e = "Can't keep it on the SD card.";
  }
  if (!e && ref && mapBad) {
    hal::log("jobs: %u sectors read differently", (unsigned)mapBad);
    e = "It read differently twice: try again.";
  }
  mapOut = 0;
  mapRef = 0;
  std::vector<uint8_t>().swap(mapSec);
  std::vector<uint8_t>().swap(mapWant);
  mapAt = -1;
  mapSeq++;
  return e;
}

// (a reading again, against a reading kept)
static const FlashBuf* mapAgainst;
static bool againstKept(uint32_t at, uint8_t* out) {
  return mapAgainst->read(at, out, SECTOR);
}

// an SMK-37's calibration sector: kept (never written)
static void mapSmk(const FlashBuf& flash) {
  if (smkIn(flash) && SMK_CAL / SECTOR < MAP_SECTORS) {
    mapKind[SMK_CAL / SECTOR] = MAP_KEEP;
    mapSeq++;
  }
}

// the FM-1 out of its boot loader, back to its firmware. e (an error of the job, or 0): the USB starts again, and error
// says it (and, if the FM-1 stayed in its boot loader, that it must be switched off and on)
static void leaveLoader(const char* e) {
  step = LEAVING;
  uboot::leave();
  for (uint32_t start = hal::millis(); uboot::stuck() && hal::millis() - start < 3000;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  hal::bleHold(false);
  if (e) {
    bool stuck = uboot::stuck();
    hal::usbRestart();  // (whatever the boot loader left behind: the FM-1 is found again)
    error = std::string(e) + (stuck ? " Switch the FM-1 off and on to get it back." : "");
  }
}

// the whole flash of device d into a (read twice, and the same): through its boot loader, and the FM-1 goes back to its
// firmware. title is on the FM-1's own screen, what names the FlashBuf, smkScreen says whose screen it is. False: error
// says why
static bool readFlash(int d, bool smkScreen, const char* title, const char* what, uboot::Chip& chip, FlashBuf& a) {
  step = BOOT_LOADER;
  hal::bleHold(true);
  const char* e = uboot::enter(d, chip);
  if (!e && !a.make(chip.size, what)) {
    e = "No room to read it into (memory, or the SD card).";
  }
  if (!e) {
    fmStart(smkScreen, title);
    step = READING_FLASH;
    reading = 1;
    fmReading("READING", 0, 50);
    e = mapRead(chip, &a, 0);
    mapSmk(a);
  }
  if (!e) {
    reading = 2;
    fmReading("READING IT AGAIN", 50, 50);
    mapAgainst = &a;
    e = mapRead(chip, 0, againstKept);
  }
  fmShow(e ? "FAILED" : "DONE", e ? -1 : 100, true);
  fmOn = false;
  leaveLoader(e);
  return !e;
}

// a flash as dir/fm1-N-FW.bin (its name into saved), and what it was (fm1-N-FW.txt). FW: the firmware it ran
// ("FM-1_904"), when that's known. N comes first (the lists sort by it). head: the first line of the .txt ("" says it's
// a backup). last: the file of the install before, when there is one. With alive: the loader is still running, and
// kept so (the card can take longer than it waits)
static bool saveBackup(const FlashBuf& a, const uboot::Chip& chip, const std::string& was, bool alive,
                       const std::string& fw = "", const char* folder = BACKUPS_DIR, const std::string& head = "",
                       const std::string& last = "") {
  mkdir(catalog::path(BACKUPS_DIR).c_str(), 0777);
  std::string dir = catalog::path(folder);
  mkdir(dir.c_str(), 0777);
  bool smk = smkIn(a);
  const char* prefix = smk ? "smk-" : "fm1-";  // (an SMK-37's has its calibration: uboot.h)
  // N: one past the highest there (with or without a firmware in its name)
  int n = 1;
  if (DIR* dd = opendir(dir.c_str())) {
    while (dirent* e = readdir(dd)) {
      if (!strncasecmp(e->d_name, prefix, 4)) {
        n = std::max(n, atoi(e->d_name + 4) + 1);
      }
    }
    closedir(dd);
  }
  std::string tag;  // the firmware, safe for a file name ("SMK-37 Elite_016" -> "SMK-37-Elite_016")
  for (char c : fw) {
    tag += isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' ? c : '-';
  }
  std::string stem = prefix + std::to_string(n) + (tag.empty() ? "" : "-" + tag);
  std::string base = dir + "/" + stem;
  saved = stem + ".bin";
  char sha[65] = "";
  if (alive) {
    uboot::keepAlive();
  }
  bool ok = a.saveTo((dir + "/" + saved).c_str(), alive ? uboot::keepAlive : 0);
  if (alive) {
    uboot::keepAlive();
  }
  // the hash of the bytes that are in the saved file, read after saveTo closed it. (Not of the FlashBuf: its temporary
  // file on the SD card is still open, and FATFS shows its size to a second open only after a close. That gave the hash
  // of nothing.) A hash that fails is a save that failed.
  ok = ok && hal::sha256((dir + "/" + saved).c_str(), sha);
  if (ok) {
    FILE* f = fopen((base + ".txt").c_str(), "w");
    const char* part = uboot::flashName(chip.flashId);
    std::string first = head.empty() ? std::string("fm1buddy backup: all of the ") + (smk ? "SMK-37" : "FM-1") +
                                           "'s flash, through its boot loader"
                                     : head;
    ok = f && fprintf(f, "%s\nfirmware %s\nchip key %04X\nflash    %06X, %u bytes%s%s\nflash id %s\nsha256   %s\n",
                      first.c_str(), was.empty() ? "?" : was.c_str(), chip.key, (unsigned)chip.flashId,
                      (unsigned)chip.size, part ? ", " : "", part ? part : "", uboot::uidText(chip).c_str(), sha) > 0;
    if (ok && !last.empty()) {
      ok = fprintf(f, "install  %s\n", last.c_str()) > 0;
    }
    ok &= f && !fclose(f);
  }
  if (!ok) {
    error = "Can't write it to the SD card.";
  }
  return ok;
}

static void backupJob(void*) {
  if (!hal::sd()) {
    error = "No SD card: the backup goes on it.";
    return finish(false);
  }
  mkdirs();
  const Fm1Id* id = fm1::known(device);
  std::string was = id ? id->text : "";
  uboot::Chip chip;
  FlashBuf a;
  if (!readFlash(device, id && isSmk(id->model), "BACKUP", "backup", chip, a)) {
    return finish(false);
  }
  finish(saveBackup(a, chip, was, false, was));
}

// IDENTIFY: into its boot loader and straight back, for what it says of its chip (uboot::attach keeps it: lastChip)
static void identifyJob(void*) {
  const Fm1Id* id = fm1::known(device);
  step = BOOT_LOADER;
  hal::bleHold(true);
  uboot::Chip chip;
  const char* e = uboot::enter(device, chip);
  if (!e) {
    fmStart(id && isSmk(id->model), "IDENTIFY");
    char flash[24];
    snprintf(flash, sizeof flash, "FLASH %06X", (unsigned)chip.flashId);
    fmShow(flash, -1, true);
    fmOn = false;
  }
  step = LEAVING;
  uboot::leave();
  for (uint32_t start = hal::millis(); uboot::stuck() && hal::millis() - start < 3000;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  hal::bleHold(false);
  if (e) {
    bool stuck = uboot::stuck();
    hal::usbRestart();
    error = std::string(e) + (stuck ? " Switch the FM-1 off and on to get it back." : "");
    return finish(false);
  }
  finish(true);
}

bool jobs::identify(int d) {
  if (running) {
    return false;
  }
  device = d;
  return start(IDENTIFY, identifyJob);
}

bool jobs::backup(int d) {
  if (running) {
    return false;
  }
  device = d;
  reading = 0;
  mapClear();
  return start(BACKUP, backupJob);
}

// ------------------------------------------------------------------------------------ the data save ---
static void dataSaveReset() {
  dataSaveFailed = false;
  dataSaveUid.clear();
  reading = 0;
  mapClear();
}

// the package file of the newest install record ("" if there is none)
static std::string lastInstall() {
  std::string dir = catalog::path(INSTALLS_DIR);
  int last = 0;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      last = std::max(last, atoi(e->d_name));
    }
    closedir(d);
  }
  std::string name;
  if (FILE* f = last ? fopen((dir + "/" + std::to_string(last) + ".txt").c_str(), "r") : 0) {
    char line[200];
    while (fgets(line, sizeof line, f)) {
      if (!strncmp(line, "file     ", 9)) {
        name = line + 9;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
          name.pop_back();
        }
        break;
      }
    }
    fclose(f);
  }
  return name;
}

// the .txt file of a data save: what it says of the data save (false if there is none)
static bool readSave(const std::string& bin, datasave::Save& save) {
  std::string text = catalog::path(DATA_SAVES_DIR) + "/" + bin.substr(0, bin.size() - 4) + ".txt";
  FILE* f = fopen(text.c_str(), "r");
  if (!f) {
    return false;
  }
  std::string all;
  char chunk[200];
  for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;) {
    all.append(chunk, n);
  }
  fclose(f);
  save = datasave::fromText(bin, all);
  return true;
}

// the data saves on the SD card: each .bin file that has its .txt file
static std::vector<datasave::Save> dataSaves() {
  std::vector<datasave::Save> out;
  std::string dir = catalog::path(DATA_SAVES_DIR);
  DIR* d = opendir(dir.c_str());
  while (d) {
    dirent* e = readdir(d);
    if (!e) {
      break;
    }
    size_t n = strlen(e->d_name);
    if (n <= 4 || strcasecmp(e->d_name + n - 4, ".bin")) {
      continue;
    }
    datasave::Save save;
    if (!readSave(e->d_name, save)) {
      continue;
    }
    out.push_back(save);
  }
  if (d) {
    closedir(d);
  }
  return out;
}

// the data saves that the remove decision gives, off the SD card (keep: the .bin file that stays)
static void removeOldDataSaves(const std::string& keep) {
  for (const std::string& name : datasave::toRemove(dataSaves(), keep)) {
    remove((catalog::path(DATA_SAVES_DIR) + "/" + name).c_str());
  }
}

// INSTALL, before the install: the FM-1's whole flash to /backups/switch, and the older ones off. False: nothing was
// written to the FM-1, error says why, and dataSaveFailed says it was the data save. An SMK-37 gets none. An FM-1 with
// no identity, or in its update loader, is a data save failure: the user answers "Install anyway?"
static bool dataSaveBefore() {
  int d = fm1::current();
  const Fm1Id* known = fm1::known(d);
  if (known && !known->ota() && isSmk(known->model)) {
    return true;  // an SMK-37 has no data to save
  }
  if (!known || known->ota()) {
    dataSaveFailed = true;
    error = known ? "The FM-1 is in its update loader: the data save needs its firmware."
                  : "The FM-1 gave no firmware identity: the data save needs it.";
    return false;
  }
  Fm1Id id = *known;  // (known is lost when the FM-1 leaves its firmware)
  dataSaveFailed = true;
  if (!hal::sd()) {
    error = "No SD card: the data save goes on it.";
    return false;
  }
  mkdirs();
  uboot::Chip chip;
  FlashBuf a;
  if (!readFlash(d, false, "DATA SAVE", "data save", chip, a)) {
    return false;
  }
  if (!smkIn(a)) {  // (an SMK-37 under the name of an FM-1: the calibration is its own)
    step = SAVING;
    std::string kept = saved;
    std::string head = "fm1buddy data save: all of the FM-1's flash, before an install, through its boot loader";
    bool good = saveBackup(a, chip, id.text, false, id.text, DATA_SAVES_DIR, head, lastInstall());
    std::string own = saved;
    saved = kept;
    if (!good) {
      return false;
    }
    dataSaveUid = uboot::uidText(chip);
    removeOldDataSaves("");
    hal::log("jobs: data save %s", own.c_str());
  }
  dataSaveFailed = false;
  // the FM-1 starts its firmware again: the install looks for it one time only
  step = ASKING;
  done = 0;
  for (uint32_t start = hal::millis(); fm1::current() < 0 && hal::millis() - start < 60000;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return true;
}

// ---------------------------------------------------------------------------------------- recovery ---
#define RECOVER_KEY "recover"
#define RECOVER_CAL_KEY "recovercal"
static char recoverFrom[160];  // what goes on it: "/fm1/x.fwsc" or "/backups/fm1-3.bin", kept across the restart
static uint8_t recoverCal;     // 1: the backup's calibration goes on it too (jobs.h calLost), kept across the restart
bool jobs::calLost;

void jobs::giveCalibration() {
  recoverCal = 1;
  hal::save(RECOVER_CAL_KEY, &recoverCal, 1);
}

static bool isBackup(const std::string& p) {
  return p.size() > 4 && !strcasecmp(p.c_str() + p.size() - 4, ".bin");
}

static void prepareJob(void*) {
  if (!hal::sd()) {
    error = "No SD card.";
    return finish(false);
  }
  mkdirs();
  hal::save(RECOVER_KEY, recoverFrom, sizeof recoverFrom);
  hal::save(RECOVER_CAL_KEY, &recoverCal, 1);
  finish(true);
  hal::recoveryModeSet(true);  // (the board restarts)
}

bool jobs::prepare(const std::string& from) {
  if (running || from.size() >= sizeof recoverFrom) {
    return false;
  }
  snprintf(recoverFrom, sizeof recoverFrom, "%s", from.c_str());
  recoverCal = 0;  // (a new choice: only its own calibration)
  return start(PREPARE, prepareJob);
}

std::string jobs::recovering() {
  if (!recoverFrom[0] && !hal::load(RECOVER_KEY, recoverFrom, sizeof recoverFrom)) {
    recoverFrom[0] = 0;
  }
  return recoverFrom;
}

// what goes on it, as the flash would be from 0 (its first WRITE_FROM bytes aren't written), and if it's for an
// SMK-37: 0 or an error
static const char* image(const std::string& from, FlashBuf& out, bool& smk) {
  std::string p = catalog::path(from);
  std::vector<uint8_t> buf(SECTOR);
  if (isBackup(from)) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) {
      return "Can't open the backup on the SD card.";
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size != 1024 * 1024) {
      fclose(f);
      return "That backup isn't all of a flash (1 MB).";
    }
    if (!out.make(WRITE_TO, "recover")) {  // (its key_mac, past WRITE_TO, stays as the chip has it)
      fclose(f);
      return "No room for it (memory, or the SD card).";
    }
    bool ok = true;
    for (uint32_t at = 0; ok && at < WRITE_TO; at += SECTOR) {
      uint32_t k = std::min<uint32_t>(SECTOR, WRITE_TO - at);
      ok = fread(buf.data(), 1, k, f) == k && out.write(at, buf.data(), k);
    }
    fclose(f);
    if (!ok) {
      return "The SD card didn't read the whole backup.";
    }
    smk = smkIn(out);
    return 0;
  }
  Pkg pkg;
  if (const char* e = pkg.open(p.c_str())) {
    return e;
  }
  uint32_t at, n;
  if (!pkg.flash(at, n)) {
    return "That package has no flash image in it (or it's damaged).";
  }
  smk = isSmk(pkg.product);
  if (strncmp(pkg.product, "FM-1_", 5) && !smk) {
    return "That package isn't for the FM-1 or the SMK-37.";
  }
  if (n <= WRITE_FROM || n > (smk ? SMK_CAL : WRITE_TO)) {
    return "Its flash image is a size no firmware's is.";
  }
  if (!out.make(n, "recover")) {
    return "No room for it (memory, or the SD card).";
  }
  for (uint32_t i = 0; i < n; i += SECTOR) {
    uint32_t k = std::min<uint32_t>(SECTOR, n - i);
    if (!pkg.read(at + i, buf.data(), k) || !out.write(i, buf.data(), k)) {
      return "The SD card didn't read the whole package.";
    }
  }
  return 0;
}

// recovery: what's on it (now), what goes on it (want), and how
static struct {
  const FlashBuf* now;
  const FlashBuf* want;
  bool keepCal;  // an SMK-37's calibration sector stays as it is
} rec;

// the sector at a as it should be once it's recovered: what goes on it from WRITE_FROM to the image's end, what's on it
// now everywhere else (and the SMK-37's calibration, unless it's being given one)
static bool expectSector(uint32_t a, uint8_t* out) {
  if (!rec.now->read(a, out, SECTOR)) {
    return false;
  }
  uint32_t end = rec.want->size();
  if (a + SECTOR <= WRITE_FROM || a >= end || (rec.keepCal && a == SMK_CAL)) {
    return true;
  }
  uint32_t from = std::max<uint32_t>(a, WRITE_FROM), to = std::min<uint32_t>(a + SECTOR, end);
  return rec.want->read(from, out + (from - a), to - from);
}

static void recoverJob(void*) {
  std::string from = recovering();
  if (!hal::load(RECOVER_CAL_KEY, &recoverCal, 1)) {
    recoverCal = 0;
  }
  bool cal = recoverCal == 1;
  calLost = false;
  FlashBuf want, now;
  bool forSmk = false;
  step = GETTING_READY;
  const char* e = !hal::sd() ? "No SD card." : from.empty() ? "Nothing was chosen to go on it."
                                                            : image(from, want, forSmk);
  if (e) {
    error = e;
    return finish(false);
  }
  hal::log("recover: %s, %u bytes%s", from.c_str(), (unsigned)want.size(), want.inMemory() ? "" : " (on the SD card)");
  step = KEYING;
  hal::keyStart();
  while (hal::keyState() != hal::KEY_HOST) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  step = BOOT_LOADER;
  uboot::Chip chip;
  e = uboot::attach(chip);
  if (e) {  // its firmware came instead (it was on all along, or the key was missed): say so
    for (int d = 0; d < MIDI_USB; d++) {
      if (fm1::candidate(d)) {
        e = "It started its firmware, not its boot loader: it had power all along. Tap Try again, and unplug it as well "
            "as switching it off.";
      }
    }
  }
  if (!e && (chip.key != FM1_KEY || !uboot::fm1Flash(chip.flashId) || chip.size < WRITE_TO)) {
    e = "That isn't the FM-1's chip: nothing was written.";
  }
  if (!e && !now.make(chip.size, "now")) {
    e = "No room to read it into (memory, or the SD card): nothing was written.";
  }
  if (!e) {
    fmStart(forSmk, "RECOVERY");
    step = READING_FLASH;
    reading = 1;
    fmReading("READING", 0, 30);
    e = mapRead(chip, &now, 0);
    mapSmk(now);
  }
  // the flash as it was, first: whatever happens next, it's on the SD card
  if (!e) {
    fmShow("SAVING A BACKUP", 30, true);
    step = SAVING;
    e = saveBackup(now, chip, "before recovery", true) ? 0 : error.c_str();
  }
  // an FM-1 and an SMK-37 look the same to here: what's on its flash says which it is. The SMK-37's calibration is
  // its own (never written): another SMK-37's backup goes on it with its own calibration kept
  bool smk = !e && smkIn(now);
  // its calibration sector blank, and an SMK-37's backup with one going on: an SMK-37 whose calibration was lost, or an
  // FM-1. Only when that's said (prepare withCal) does the backup's go on it; one it has is never written
  bool giveCal = false;
  std::vector<uint8_t> ex(SECTOR), nw(SECTOR);  // a sector: as it should be, as it is
  if (!e && !smk && forSmk && isBackup(from) && smkIn(want)) {
    bool blank = now.read(SMK_CAL, nw.data(), SECTOR) &&
                 std::all_of(nw.begin(), nw.end(), [](uint8_t b) { return b == 0xFF; });
    hal::log("recover: no calibration on it (its sector %s)", blank ? "blank" : "not blank");
    if (blank && cal) {
      smk = giveCal = true;
    } else if (blank) {
      calLost = true;
      e = "No calibration: an FM-1, or an SMK-37 that lost it. Nothing was written.";
    }
  }
  if (!e) {
    hal::log("recover: it's %s, the image is for %s%s", smk ? "an SMK-37" : "an FM-1", forSmk ? "an SMK-37" : "an FM-1",
             giveCal ? " (with the backup's calibration)" : "");
    if (smk != forSmk) {
      e = smk ? "This is an SMK-37, and that's for the FM-1: nothing was written."
              : "This is an FM-1 (no SMK-37 calibration on it), and that's for the SMK-37: nothing was written. An "
                "SMK-37 whose calibration is gone takes another SMK-37's backup.";
    }
  }
  std::vector<uint32_t> sectors;
  rec = {&now, &want, smk && !giveCal};  // (an SMK-37's calibration: never written)
  if (!e) {
    for (uint32_t a = WRITE_FROM; !e && a < want.size(); a += SECTOR) {
      if (!expectSector(a, ex.data()) || !now.read(a, nw.data(), SECTOR)) {
        e = "Can't read it back from the SD card: nothing was written.";
      } else if (memcmp(ex.data(), nw.data(), SECTOR)) {
        sectors.push_back(a);
      }
    }
  }
  if (!e) {
    hal::log("recover: %u sectors differ", (unsigned)sectors.size());
    bool head = false;
    for (uint32_t a = 0; a < WRITE_FROM && a < want.size(); a += SECTOR) {
      uint32_t k = std::min<uint32_t>(SECTOR, want.size() - a);
      head |= want.read(a, ex.data(), k) && now.read(a, nw.data(), k) && memcmp(ex.data(), nw.data(), k);
    }
    if (head) {
      hal::log("recover: its head differs from the image's (it isn't written)");
    }
    for (uint32_t a : sectors) {
      if (a / SECTOR < MAP_SECTORS) {
        mapMark[a / SECTOR] = MARK_DIFFERS;
      }
    }
    mapSeq++;
    step = WRITING_FLASH;
    total = sectors.size();
    done = 0;
    for (uint32_t a : sectors) {
      mapAt = a / SECTOR;
      mapSeq++;
      char what[24];
      snprintf(what, sizeof what, "WRITING %u/%u", (unsigned)done + 1, (unsigned)sectors.size());
      fmShow(what, 30 + (int)(40 * done / sectors.size()), true);
      if (!expectSector(a, ex.data())) {
        e = "Can't read it back from the SD card.";
        break;
      }
      for (int t = 0; t < 3; t++) {
        e = uboot::writeSector(a, ex.data());
        if (!e) {
          break;
        }
        hal::log("recover: sector %06X: %s", (unsigned)a, e);
      }
      if (e) {
        break;
      }
      if (a / SECTOR < MAP_SECTORS) {
        mapMark[a / SECTOR] = MARK_WRITTEN;
        mapKind[a / SECTOR] = kindOf(ex.data(), a / SECTOR);  // (what's on it now)
      }
      mapAt = -1;
      mapSeq++;
      done = done + 1;
    }
  }
  if (!e) {
    step = CHECKING_FLASH;
    reading = 2;
    fmReading("CHECKING", 70, 30);
    e = mapRead(chip, 0, expectSector);
    if (e && mapBad) {
      e = "It reads back differently from what was written.";
    }
  }
  fmShow(e ? "FAILED" : "DONE", e ? -1 : 100, true);
  fmOn = false;
  step = LEAVING;
  uboot::leave();
  if (e) {
    hal::usbRestart();  // (whatever the boot loader left behind)
    error = std::string(e) + (sectors.empty() ? "" : " It may not start now: switch it off, and try again.");
    return finish(false);
  }
  error = sectors.empty() ? "It already had that on it."
          : giveCal       ? "Done, with the backup's calibration (another keyboard's: keys and pads may be a little off)."
                          : "";
  hal::save(RECOVER_KEY, "", 1);  // (done: not again)
  recoverCal = 0;
  hal::save(RECOVER_CAL_KEY, &recoverCal, 1);
  finish(true);
}

bool jobs::recover() {
  if (running) {
    return false;
  }
  reading = 0;
  mapClear();
  return start(RECOVER, recoverJob);
}

// ------------------------------------------------------------------------------- the data restore ---
static FILE* restoreBin;             // DATA_RESTORE: the .bin file of the data save
static const FlashBuf* restoreNow;   // the flash as it was read
static uint32_t restoreFrom;         // the first sector that it writes

static bool binRead(uint32_t at, uint8_t* out, uint32_t n) {
  if (!restoreBin || fseek(restoreBin, at, SEEK_SET) || fread(out, 1, n, restoreBin) != n) {
    return false;
  }
  sdPace(n);
  return true;
}

// the sector at a once the data is back: what the data save holds from restoreFrom to key_mac, what is on it now
// everywhere else
static bool restoreExpect(uint32_t a, uint8_t* out) {
  return a >= restoreFrom && a < WRITE_TO ? binRead(a, out, SECTOR) : restoreNow->read(a, out, SECTOR);
}

static std::string restoreFile;  // DATA_RESTORE: the data save's .bin file name, in DATA_SAVES_DIR
static bool restoreSaveFirst;    // DATA_RESTORE: a data save of the current data comes before the first write
static bool restoreTransfer;     // DATA_RESTORE: the data save may be from another FM-1 (a transfer)

// the firmware of /fm1 whose flash image is the firmware of the data save: its image end (0: none). With flash (a
// transfer), the data save and the flash decide the path, and file gets the name of the package in /fm1
static uint32_t matchingImageEnd(const datasave::Reader* flash = 0, datasave::TransferPath* path = 0,
                                 std::string* file = 0) {
  for (size_t i = 0; i < catalog::sd.size(); i++) {
    if (strncmp(catalog::sd.str(catalog::sd[i].product), "FM-1_", 5)) {
      continue;
    }
    Pkg pkg;  // (one at a time: a board with no PSRAM has no room for two)
    uint32_t at = 0, n = 0;
    if (pkg.open(catalog::path(std::string(FW_DIR "/") + catalog::sd.str(catalog::sd[i].file)).c_str()) ||
        !pkg.flash(at, n)) {
      continue;
    }
    datasave::Package one{[&pkg, at](uint32_t a, uint8_t* out, uint32_t len) { return pkg.read(at + a, out, len); }, n};
    if (flash) {
      datasave::Transfer t = datasave::transfer(binRead, *flash, {one});
      if (t.path == datasave::TRANSFER_NO_PACKAGE) {
        continue;
      }
      *path = t.path;
      *file = catalog::sd.str(catalog::sd[i].file);
      return n;
    }
    if (datasave::matchPackage(binRead, {one}) == 0) {
      return n;
    }
  }
  return 0;
}

// A transfer to an FM-1 that runs another build: out of the boot loader, the package installed (no data save: the
// transfer made it before), the FM-1 found again, its boot loader entered again, and its flash read again into now.
// The job thread cannot call jobs::install: start refuses a job while one runs. loader says whether the boot loader
// is open when this returns
static const char* transferInstall(const std::string& file, uboot::Chip& chip, FlashBuf& now, bool& loader) {
  fmOn = false;
  leaveLoader(0);
  loader = false;
  // the FM-1 starts its firmware again: the install looks for it one time only
  for (uint32_t start = hal::millis(); fm1::current() < 0 && hal::millis() - start < 60000;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  Pkg pkg;
  if (const char* e = pkg.open(catalog::path(std::string(FW_DIR "/") + file).c_str())) {
    return e;
  }
  uint32_t at = 0, n = 0;
  total = pkg.flash(at, n) ? n : pkg.size;
  done = 0;
  step = ASKING;
  hal::bleHold(true);
  const char* e = fm1::install(pkg, installProgress, 0);
  hal::bleHold(false);
  if (e) {
    return e;
  }
  for (uint32_t start = hal::millis(); fm1::current() < 0 && hal::millis() - start < 60000;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (fm1::current() < 0) {
    return "The FM-1 didn't come back after the install: switch it off and on.";
  }
  step = BOOT_LOADER;
  hal::bleHold(true);
  loader = true;
  if ((e = uboot::enter(fm1::current(), chip))) {
    return e;
  }
  fmStart(false, "DATA RESTORE");
  step = READING_FLASH;
  reading = 1;
  fmReading("READING", 0, 30);
  e = mapRead(chip, &now, 0);
  mapSmk(now);
  return e;
}

static void restoreDataJob(void*) {
  if (!hal::sd()) {
    error = "No SD card.";
    return finish(false);
  }
  step = GETTING_READY;
  std::string path = catalog::path(DATA_SAVES_DIR) + "/" + restoreFile;
  datasave::Restore r = {0, "", "", 0, 0, 0, 0};
  datasave::Save save;
  struct stat st;
  char sha[65] = "";
  restoreBin = fopen(path.c_str(), "rb");
  if (!restoreBin || stat(path.c_str(), &st) || !readSave(restoreFile, save)) {
    if (restoreBin) {
      fclose(restoreBin);
      restoreBin = 0;
    }
    error = "Can't read that data save on the SD card: nothing was written.";
    return finish(false);
  }
  r.fileSize = st.st_size;
  r.savedSha256 = save.sha256;
  if (r.fileSize == 1024 * 1024 && hal::sha256(path.c_str(), sha)) {
    r.fileSha256 = sha;
  }
  FlashBuf now;
  const char* e = 0;
  uboot::Chip chip;
  const Fm1Id* known = fm1::known(device);
  const std::string current = known ? known->text : "";  // (known is lost when the FM-1 leaves its firmware)
  step = BOOT_LOADER;
  hal::bleHold(true);
  e = uboot::enter(device, chip);
  std::vector<uint32_t> sectors;
  bool refused = false;
  bool inLoader = true;  // (a transfer that installs leaves it, and may fail outside it)
  // the unique ID comes first: another FM-1 gets no data save and no write, unless the user asked for a transfer
  if (!e && !restoreTransfer && !datasave::sameFm1(save, uboot::uidText(chip))) {
    anotherFm1 = true;
    e = "That data save is from another FM-1: nothing was written.";
    refused = true;
  }
  if (!e) {
    r.chipKey = chip.key;
    r.flashId = chip.flashId;
    r.flashSize = chip.size;
    if (chip.key != FM1_KEY || chip.size != 1024 * 1024) {  // (nothing to read: the refusal says it)
      e = datasave::reason(datasave::refusal(r, binRead, [](uint32_t, uint8_t*, uint32_t) { return false; }));
      refused = true;
    } else if (!now.make(chip.size, "now")) {
      e = "No room to read it into (memory, or the SD card): nothing was written.";
    }
  }
  datasave::Reader flash = [&now](uint32_t at, uint8_t* out, uint32_t n) { return now.read(at, out, n); };
  if (!e) {
    fmStart(false, "DATA RESTORE");
    step = READING_FLASH;
    reading = 1;
    fmReading("READING", 0, 30);
    e = mapRead(chip, &now, 0);
    mapSmk(now);
  }
  // every refusal comes before the first write
  datasave::TransferPath route = datasave::TRANSFER_WRITE;
  std::string package;  // a transfer: the file in /fm1 that gets installed
  uint32_t imageEnd = 0;  // the image end of the matching package (0: none)
  if (!e) {
    if (r.fileSize == 1024 * 1024) {
      imageEnd = restoreTransfer ? matchingImageEnd(&flash, &route, &package) : matchingImageEnd();
    }
    // a flash that runs another build gets the firmware compare after the install, not before
    r.imageEnd = route == datasave::TRANSFER_INSTALL ? 0 : imageEnd;
    datasave::Refusal why = datasave::refusal(r, binRead, flash);
    if (why != datasave::REFUSE_NONE) {
      e = datasave::reason(why);
      refused = true;
    } else if (!imageEnd) {
      noPackage = true;
      e = "No firmware in /fm1 is the firmware of that data save: nothing was written.";
      refused = true;
    }
  }
  restoreNow = &now;
  restoreFrom = imageEnd - imageEnd % SECTOR;
  if (!e && route == datasave::TRANSFER_WRITE) {
    bool lost = false;
    sectors = datasave::sectors(binRead, flash, imageEnd, &lost);
    if (lost) {
      e = "Can't read it back from the SD card: nothing was written.";
      refused = true;
    }
  }
  // the refusals are over: the data save of the current data comes from the same read, before the first write
  if (!e && restoreSaveFirst) {
    step = SAVING;
    fmShow("SAVING", 30, true);
    std::string head = std::string("fm1buddy data save: all of the FM-1's flash, before a ") +
                       (restoreTransfer ? "transfer" : "restore") + ", through its boot loader";
    if (!saveBackup(now, chip, current, true, current, DATA_SAVES_DIR, head, lastInstall())) {
      e = "Can't make a data save of the current data: nothing was written.";
      refused = true;
    }
  }
  if (!e && route == datasave::TRANSFER_INSTALL) {
    e = transferInstall(package, chip, now, inLoader);
    if (!e) {
      r.chipKey = chip.key;
      r.flashId = chip.flashId;
      r.flashSize = chip.size;
      r.imageEnd = imageEnd;
      datasave::Refusal why = datasave::refusal(r, binRead, flash);
      bool lost = false;
      if (why != datasave::REFUSE_NONE) {
        e = datasave::reason(why);
      } else {
        sectors = datasave::sectors(binRead, flash, imageEnd, &lost);
        e = lost ? "Can't read it back from the SD card: nothing was written." : 0;
      }
    }
    refused = e != 0;  // (a failure up to here leaves the data as the install left it: no write)
  }
  if (!e) {
    hal::log("data restore: %s, %u sectors differ", restoreFile.c_str(), (unsigned)sectors.size());
    for (uint32_t a : sectors) {
      mapMark[a / SECTOR] = MARK_DIFFERS;
    }
    mapSeq++;
    step = WRITING_FLASH;
    total = sectors.size();
    done = 0;
    std::vector<uint8_t> data(SECTOR);
    for (uint32_t a : sectors) {
      mapAt = a / SECTOR;
      mapSeq++;
      char what[24];
      snprintf(what, sizeof what, "WRITING %u/%u", (unsigned)done + 1, (unsigned)sectors.size());
      fmShow(what, 30 + (int)(40 * done / sectors.size()), true);
      if (!binRead(a, data.data(), SECTOR)) {
        e = "Can't read it back from the SD card.";
        break;
      }
      for (int t = 0; t < 3; t++) {
        e = uboot::writeSector(a, data.data());
        if (!e) {
          break;
        }
        hal::log("data restore: sector %06X: %s", (unsigned)a, e);
      }
      if (e) {
        sectorFailed = true;
        break;
      }
      mapMark[a / SECTOR] = MARK_WRITTEN;
      mapKind[a / SECTOR] = kindOf(data.data(), a / SECTOR);
      mapAt = -1;
      mapSeq++;
      done = done + 1;
    }
  }
  if (!e) {
    step = CHECKING_FLASH;
    reading = 2;
    fmReading("CHECKING", 70, 30);
    e = mapRead(chip, 0, restoreExpect);
    if (e && mapBad) {
      e = "It reads back differently from what was written.";
    }
  }
  fmShow(e ? "FAILED" : "DONE", e ? -1 : 100, true);
  fmOn = false;
  if (inLoader) {
    leaveLoader(e);
  } else {  // (the install failed: the FM-1 is not in its boot loader)
    hal::bleHold(false);
    if (e) {
      hal::usbRestart();
      error = e;
    }
  }
  restoreNow = 0;
  fclose(restoreBin);
  restoreBin = 0;
  if (e) {
    error += refused || sectors.empty() ? "" : " It may not start now: switch it off, and try again.";
    return finish(false);
  }
  if (restoreSaveFirst) {  // (a restore with no data save of its own removes none)
    removeOldDataSaves(restoreFile);
  }
  error = sectors.empty() ? "It already had that data." : "";
  finish(true);
}

bool jobs::restoreData(int d, const std::string& file, bool dataSaveFirst, bool transfer) {
  if (running) {
    return false;
  }
  device = d;
  restoreFile = file;
  restoreTransfer = transfer;
  restoreSaveFirst = dataSaveFirst || transfer;  // (a transfer always makes a data save of the target first)
  anotherFm1 = noPackage = sectorFailed = false;
  saved.clear();
  reading = 0;
  mapClear();
  return start(DATA_RESTORE, restoreDataJob);
}

std::vector<datasave::Save> jobs::dataSaveList() {
  return datasave::newestFirst(dataSaves());
}

std::string jobs::dataSaveFor(const std::string& product, const std::string& uid) {
  return datasave::matchSave(dataSaves(), product, uid);
}

static std::string exportApp;

static void exportJob(void*) {
  step = WRITING_FILES;
  const char* e = sounds::exportSetup(setupName, exportApp.c_str(), saved);
  if (e) {
    error = e;
  }
  finish(!e);
}

bool jobs::exportSetup(const std::string& setup, const char* app) {
  if (running) {
    return false;
  }
  setupName = setup;
  exportApp = app;
  return start(EXPORT, exportJob);
}

bool jobs::keep(int d) {
  if (running) {
    return false;
  }
  device = d;
  phase = 0;
  return start(KEEP, keepJob);
}

bool jobs::restore(int d, const std::string& setup) {
  if (running) {
    return false;
  }
  device = d;
  setupName = setup;
  phase = 0;
  return start(RESTORE, restoreJob);
}

bool jobs::probe(int d) {
  if (running) {
    return false;
  }
  device = d;
  return start(PROBE, probeJob);
}

bool jobs::send(int d, const std::string& file) {
  if (running) {
    return false;
  }
  device = d;
  syxFile = file;
  return start(SEND, sendJob);
}

bool jobs::voice(int d) {
  if (running) {
    return false;
  }
  device = d;
  return start(VOICE, voiceJob);
}

bool jobs::scan() {
  return start(SCAN, scanJob);
}

bool jobs::join(const char* s, const char* p) {
  if (running) {
    return false;
  }
  ssid = s;
  pass = p;
  return start(JOIN, joinJob);
}

// ------------------------------------------------------------------------------------------- SLOOP ---
static std::string doing;  // WORKING: what ("Backing up")
static std::string sloopFile;
static int sloopFrom, sloopTo;

static void sloopJob(void*) {
  step = WORKING;
  const char* e = 0;
  saved.clear();
  if (kind == SLOOP_BACKUP) {
    doing = "Backing it up";
    sloop::Info info;
    e = sloop::info(device, info);
    if (!e) {
      e = sloop::backup(device, info, saved, progress);
    }
  } else if (kind == SLOOP_RESTORE) {
    doing = "Backing it up, then restoring";
    e = sloop::restore(device, sloopFile, saved, progress);
  } else if (kind == SLOOP_PROJECT) {
    doing = "Loading the project";
    e = sloop::loadProject(device, sloopFile, sloopFrom, sloopTo, progress);
  } else if (kind == SLOOP_SONG) {
    doing = "Saving the song";
    sloop::Info info;
    e = sloop::info(device, info);
    if (!e) {
      e = sloop::saveSong(device, info, saved, progress);
    }
  } else if (kind == SLOOP_SONG_IN) {
    doing = "Saving its song, then loading";
    e = sloop::loadSong(device, sloopFile, saved, progress);
  } else {
    doing = "Loading the samples";
    e = sloop::loadSamples(device, sloopFile, sloopTo, progress);
  }
  if (e) {
    error = e;
  }
  finish(!e);
}

static bool sloopStart(Kind k, int d, const std::string& file, int from, int to) {
  if (running) {
    return false;
  }
  device = d;
  sloopFile = file;
  sloopFrom = from;
  sloopTo = to;
  return start(k, sloopJob);
}

bool jobs::sloopBackup(int d) {
  return sloopStart(SLOOP_BACKUP, d, "", 0, 0);
}

bool jobs::sloopRestore(int d, const std::string& file) {
  return sloopStart(SLOOP_RESTORE, d, file, 0, 0);
}

bool jobs::sloopProject(int d, const std::string& file, int from, int to) {
  return sloopStart(SLOOP_PROJECT, d, file, from, to);
}

bool jobs::sloopSong(int d) {
  return sloopStart(SLOOP_SONG, d, "", 0, 0);
}

bool jobs::sloopSongIn(int d, const std::string& file) {
  return sloopStart(SLOOP_SONG_IN, d, file, 0, 0);
}

bool jobs::sloopSamples(int d, const std::string& source, int slot) {
  return sloopStart(SLOOP_SAMPLES, d, source, 0, slot);
}

static std::string syncStatus() {
  return "Syncing " + std::to_string(syncAt) + " of " + std::to_string(syncOf);
}

std::string jobs::status() {
  switch ((int)step) {
    case JOINING:
      return "Joining " + std::string(kind == JOIN ? ssid : settings.ssid) + "...";
    case FETCHING: {
      if (kind == HUB || (kind == SYNC && !syncOf)) {
        return "Getting the Hub's list...";
      }
      if (kind == SYNC) {
        return syncStatus();
      }
      if (kind == CHECK_UPDATE) {
        return "Checking for an update...";
      }
      if (kind == INSTALL) {
        uint32_t t = total, d = done;
        return "Downloading" + (t ? " " + std::to_string((unsigned long long)d * 100 / t) + "%" : std::string("..."));
      }
      uint32_t t = total, d = done;
      return t ? "Downloading " + std::to_string((unsigned long long)d * 100 / t) + "%" : "Downloading...";
    }
    case CHECKING:
      return kind == SYNC && syncOf ? syncStatus() : "Checking it...";
    case UPDATING: {
      uint32_t t = total, d = done;
      return "Installing " + std::to_string(t ? (unsigned long long)d * 100 / t : 0) + "%: keep it powered";
    }
    case ASKING:
      return "Looking for the FM-1...";
    case VERIFYING:
      return "The FM-1 is checking the package...";
    case LOADER:
      return "The FM-1 is starting its update loader...";
    case WRITING: {
      // what a loader reads besides the firmware differs (header parts, other files): never past 99% until it's
      // finished, and when it goes quiet after the data, it's checking or storing it, not stuck
      uint32_t t = total, d = done;
      if (d && hal::millis() - askedAt > 2000) {
        return "The FM-1 is finishing the update: keep it plugged in";
      }
      unsigned long long pct = t ? (unsigned long long)d * 100 / t : 0;
      return "Writing " + std::to_string(pct > 99 ? 99 : pct) + "%: keep the FM-1 plugged in";
    }
    case RESTARTING:
      return "The FM-1 is restarting...";
    case READING:
      return "Reading the list...";
    case SENDING: {
      uint32_t t = total, d = done;
      std::string pct = std::to_string(t ? (unsigned long long)d * 100 / t : 0) + "%...";
      if (kind == RESTORE) {
        return phase == 0 ? "Making the kits..." : (phase == 1 ? "Sending the kits " : "Sending the song ") + pct;
      }
      return "Sending " + pct;
    }
    case ASKING_IT:
      return "Asking it...";
    case WRITING_FILES:
      return "Writing the files...";
    case BOOT_LOADER:
      return kind == INSTALL ? "Saving its data: into its boot loader..." : "Into its boot loader...";
    case READING_FLASH: {
      uint32_t t = total, d = done;
      return std::string(kind == INSTALL ? "Saving its data: " : "") + (reading == 1 ? "Reading it " : "Reading it again ") +
             std::to_string(t ? (unsigned long long)d * 100 / t : 0) + "%...";
    }
    case LEAVING:
      return kind == INSTALL ? "Saving its data: back to its firmware..." : "Back to its firmware...";
    case GETTING_READY:
      return "Getting it ready...";
    case KEYING:
      switch (hal::keyState()) {
        case hal::KEY_OFF:
          return "Unplug the FM-1, and switch it off.";
        case hal::KEY_ANSWERED:
          return "It answered! Its boot loader is starting...";
        case hal::KEY_HOST:
          return "Its boot loader is starting...";
      }
      return "Now plug the FM-1 back in, and switch it on.";
    case SAVING:
      return kind == INSTALL ? "Saving its data to the SD card..." : "Saving its flash as it was...";
    case WRITING_FLASH: {
      uint32_t t = total, d = done;
      return "Writing " + std::to_string(t ? (unsigned long long)d * 100 / t : 0) + "%: keep it plugged in and on";
    }
    case CHECKING_FLASH: {
      uint32_t t = total, d = done;
      return "Checking it " + std::to_string(t ? (unsigned long long)d * 100 / t : 0) + "%...";
    }
    case WORKING: {
      uint32_t t = total, d = done;
      return doing + (t ? " " + std::to_string((unsigned long long)d * 100 / t) + "%..." : std::string("..."));
    }
    case GETTING: {
      uint32_t t = total, d = done;
      return (phase ? "Getting the kits " : "Getting the song ") + std::to_string(t ? (unsigned long long)d * 100 / t : 0) +
             "%...";
    }
  }
  return kind == SCAN && running ? "Looking for networks..." : "";
}
