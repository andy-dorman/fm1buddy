// The firmwares: mutant.cloud's list (mutant.cloud/catalog.json: the FM-1, SMK-37 and MKE-P37, kept on the SD card)
// and the .fwsc files in the SD card's /fm1 folder.
//
// Both lists stay small in memory however long they get: a list keeps what a row shows (FwList), and the rest of a
// firmware (what it is, where it comes from) stays on the SD card until it's opened (catalog::details)
#pragma once

#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "syncplan.h"

#define HUB_SITE "https://mutant.cloud/"
#define FW_DIR "/fm1"
#define DATA_DIR "/fm1buddy"
#define HUB_CACHE DATA_DIR "/hub.json"  // the Hub's list as it comes, until parseHub has read it
#define HUB_ETAG DATA_DIR "/hub.etag"
#define HUB_LIST DATA_DIR "/hub.txt"         // the list as the board needs it (parseHub writes it): a line a firmware
#define INSTALLS_DIR DATA_DIR "/installs"    // a record of each install
#define SYNC_RECORD DATA_DIR "/sync.txt"     // the files a sync wrote (syncplan.h)
#define SYNC_PLAN DATA_DIR "/sync-plan.txt"  // what the last sync set out to do
#define SD_LIST DATA_DIR "/sd-list.txt"      // the SD list on the card (keepSd off): a head line, then a line a package

// a whole firmware: the one opened, the one being saved or installed
struct Firmware {
  std::string name, author, version, date, what;
  std::string pkg;      // a whole URL: the maker's own file ("https://github.com/.../zp12-2.4.4.fwsc")
  std::string file;     // its file name: "zp12-2.4.4.fwsc"
  std::string product;  // the device it's for: "FM-1_974", "SMK-37 Elite_961"
  std::string sha256;
  std::vector<std::string> kinds;  // "synth", "groove", "drums", ...
  uint32_t size = 0;
  bool beta = false, stock = false, keyboard = false, mouse = false;
  std::string chip;                                        // "old", "new", "any" or "": the chip the package is for
  std::vector<std::string> devices;                        // "fm1", "smk37": what the Hub says it is for
  bool chipOld = false, forFm1 = false, forSmk37 = false;  // from a Hub list line (see FwItem)
  bool onSd = false;                                       // a Hub firmware that is in /fm1 too
  bool hub = false;                                        // on the SD card: its details came from the Hub's list
};

// a firmware in a list: what its row shows. Its strings are in the list's text
struct FwItem {
  enum : uint8_t { BETA = 1,
                   STOCK = 2,
                   ON_SD = 4,
                   FROM_HUB = 8,
                   CHIP_OLD = 16,  // chip: old
                   FOR_FM1 = 32,
                   FOR_SMK37 = 64 };
  static const uint32_t NO_LINE = 0xFFFFFFFF;
  uint32_t name, author, version, date, file, product, sha256;  // sha256: "" until it's known
  uint32_t line;                                                // where its line is in HUB_LIST (the Hub's; an SD file the Hub knows: that one's), or NO_LINE
  uint32_t size;
  uint32_t kinds;  // bits: the list's kinds
  uint8_t flags;
};

struct FwList {
  FwList() { text.push_back(0); }  // (offset 0: "")
  std::vector<FwItem> items;
  std::vector<char> text;          // the items' strings, one after the other, each with its 0
  std::vector<std::string> kinds;  // the kinds its items have (up to 32)

  size_t size() const { return items.size(); }
  bool empty() const { return items.empty(); }
  const char* str(uint32_t at) const { return text.data() + at; }
  const FwItem& operator[](size_t i) const { return items[i]; }
  uint32_t add(const char* s, size_t n);  // a string into text: where it is
  uint32_t add(const std::string& s) { return add(s.data(), s.size()); }
  uint32_t kindBit(const std::string& k);  // the bit of a kind (made if it's new)
  void clear();
  void swap(FwList& o);
  static uint32_t changes;  // a swap or a clear: a list on screen is made again
};

namespace catalog {
extern FwList hub, sd;
extern std::string hubUpdated;  // when the Hub's list was made: "2026-10-22 05:01 UTC"

// a path on the SD card ("/fm1/x.fwsc" -> "/sdcard/fm1/x.fwsc"), "" without a card
std::string path(const std::string& p);
// the Hub's list as it came (HUB_CACHE), a firmware at a time, into HUB_LIST (HUB_CACHE removed), then that read:
// false (err says why) if it isn't there or doesn't read
bool parseHub(FwList& out, std::string& updated, std::string& err);
// the list from HUB_LIST, in a moment: false if it isn't there
bool loadHub(FwList& out, std::string& updated);
// The app's choice: the Hub's list kept in memory (hub, and jobs::hub as the Hub job reads it), or not. All of it is
// ~100 KB: a board that can't spare that (no PSRAM) sets it false, and reads HUB_LIST as it needs it, with these
extern bool keepHub;
// the Hub's list as it came (HUB_CACHE) into HUB_LIST (HUB_CACHE removed), not kept: false (err says why) if it doesn't
bool writeHub(std::string& updated, std::string& err);
// when HUB_LIST was made: false if it isn't there
bool hubWhen(std::string& updated);
// HUB_LIST, a firmware at a time, without keeping it: fn gets each in a list of its own (one[0], its line in
// one[0].line); false from fn stops it. False if there's no list
bool eachHub(bool (*fn)(const FwList& one, void* arg), void* arg);
// one firmware of HUB_LIST, by where its line starts (FwItem::line), into one (one[0]): false if it isn't there
bool hubAt(uint32_t line, FwList& one);
// the same from HUB_LIST already open (openHub: 0 if it isn't there; fclose it), for many at once: opening a file on
// an SD card on SPI is slow
FILE* openHub();
bool hubAt(FILE* f, uint32_t line, FwList& one);
// the files in /fm1, with what the Hub's list knows about them; marks the Hub's that are there. Quick: it reads no
// whole file (a sha256 not known yet is read in the background: hashSd). It is readSd, then takeSd
void scanSd();
// the same into sd (kept in memory when keepSd is true: the caller's list; on the card when it is false), with the
// Hub's list hub (marked where it's on the card): from another thread. Returns how many files have no sha256 yet
int scanSdInto(FwList& hub, FwList& sd);

// ---------------------------------------------------------------------------------------------- the SD list ---
// The list of the firmware library as the "Firmware: SD card" page shows it (a package of /fm1 on each line, in the
// order of the names). With keepSd true it is in memory (catalog::sd). With keepSd false it is on the card (SD_LIST),
// and memory holds one SdEntry for each package: the page, and each job, read a package at a time (sdAt)
extern bool keepSd;

// where a package line is in a list file, and how its name starts (lower case, 0 after a short name): for the sort
struct SdEntry {
  uint32_t line;
  char prefix[12];
};

// The result of one scan of /fm1. A scan holds a file on the card with its own name and one block of SdEntry (shown
// of them). Free both with takeSd (install) or dropSd (no one installs it)
struct SdScan {
  uint32_t found = 0;     // the packages in /fm1
  uint32_t shown = 0;     // the packages the scan holds (fewer than found when memory is short)
  uint32_t unknown = 0;   // the packages with no sha256 known yet
  bool complete = false;  // false when the listing of /fm1 ended early, or shown is less than found
  std::string file;       // its list on the card ("" if it has none)
  SdEntry* index = 0;     // shown entries, in the order of the names
};
// /fm1 into a new SdScan, with the Hub's list hub (marked where it's on the card; 0: none). It installs nothing, and
// it runs on any thread
SdScan readSd(FwList* hub = 0);
// the scan is the SD list the pages read (the UI thread only): its file is SD_LIST (keepSd false), or the list in
// memory (keepSd true). Starts hashSd when a sha256 isn't known. The scan is spent
void takeSd(SdScan& scan);
// the scan of no one: its file is deleted, its block freed
void dropSd(SdScan& scan);
// What the SD list holds, from one scan (scan), or the installed one (0)
uint32_t sdShown(const SdScan* scan = 0);
uint32_t sdFound(const SdScan* scan = 0);
uint32_t sdUnknown(const SdScan* scan = 0);
bool sdComplete(const SdScan* scan = 0);
// package i (0 is the first by name) into one (one[0]): false if it isn't there
bool sdAt(size_t i, FwList& one, const SdScan* scan = 0);
// the same from the list already open (openSd), for many at once: opening a file on an SD card on SPI is slow
bool openSd(FILE*& f, const SdScan* scan = 0);  // false if it can't be read; f stays 0 for a list in memory
bool sdAt(FILE* f, size_t i, FwList& one, const SdScan* scan = 0);
// the place of the package with this file name in /fm1 ("zp12.fwsc", "drums/x.fwsc"): -1 if it isn't there
int sdFind(const char* file, const SdScan* scan = 0);
// read the sha256 of the files that don't have one yet, on a thread of its own (once at a time)
void hashSd();
// true once after hashSd learned something: time to scanSd again
bool sdHashed();
// all of item i of list l (what it is, where it comes from: read from HUB_LIST)
Firmware details(const FwList& l, size_t i);
// a firmware's name from its file: without the version, lower case ("ZP12-2.4.4.fwsc" -> "zp12")
std::string stem(const std::string& file);
// the Hub's firmware that SD card firmware i is a version of: the very file, else the same device ID and the same name
// (stem); -1 if none
int hubVersionOf(const FwList& hub, const FwList& sd, size_t i);
// the same for a firmware, with the Hub's list read from the SD card one entry at a time (no list in memory: for
// keepHub off). m gives the answer (HubMatch for the firmware); false if there is no Hub list on the card
bool matchHub(syncplan::HubMatch& m);
// the device a package is for, from its own header ("FM-1_974"), "" if it isn't a package
std::string product(const char* path);
}  // namespace catalog
