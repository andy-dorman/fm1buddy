// The sync plan: what one sync does to the firmware library, from the Hub list, the sync record, and the files in
// /fm1. Plain logic: no hal, no ArduinoJson, so a program on the computer tests it. The Hub list line format and the
// firmware key rule live here, and catalog.cpp calls them
#pragma once

#include <stdint.h>
#include <stdio.h>

#include <string>

#define HUB_LIST_HEAD "fm1buddy hub 2"
#define HUB_LIST_FIELDS 14  // name author version date what pkg file product sha256 size flags kinds chip devices

namespace syncplan {
// line's fields between tabs (line is cut at them): how many, up to max
int split(std::string& line, const char* v[], int max);
// a line of f into line (without its '\n'): false at the end
bool readLine(FILE* f, std::string& line);

// a file name without its folder, its version and its ".fwsc", in lower case: "ZP12-2.4.4.fwsc" is "zp12"
std::string stem(const std::string& file);
// the product plus the stem: one firmware across all its versions. It holds no tab, no line end, and no ':' (an
// extra source name starts with "extra:")
std::string firmwareKey(const std::string& product, const std::string& file);

// a Hub entry is for the FM-1 when devices lists "fm1", or when devices is empty and the product starts with "FM-1"
bool forFm1(const char* devices, const char* product);
// the same for the SMK-37 ("smk37", "SMK-37")
bool forSmk37(const char* devices, const char* product);

// one file in /fm1
struct SdFile {
  const char* path;  // the whole path: "/fm1/zp12-2.5.fwsc"
  const char* product;
  const char* sha256;  // "" when it isn't known
  uint32_t size;
};

// The actions of one sync, a line each, fields between tabs, to out:
//   download <target> <pkg> <sha256> <size> <name> <version> <product>
//   replace  <target> <pkg> <sha256> <size> <name> <version> <product> <old path>
//   remove   <path>
//   skip     <path>
// false (and nothing written) when the Hub list isn't of the head HUB_LIST_HEAD
bool plan(FILE* hubList, FILE* record, const SdFile* files, size_t count, FILE* out);

// The sync record (SYNC_RECORD): the head, then a line a file: its path, its sha256, its size, and its firmware key (or
// the name of an extra source: "extra:shrub"). Only the files that a sync wrote are in it
// a line at the end of record (open to append; its head first if it is empty)
bool appendRecord(FILE* record, const char* path, const char* sha256, uint32_t size, const char* key);
// the line of path marked as removed, in place (record open to read and write): false if there is no such line
bool markRemoved(FILE* record, const char* path);
// record to out, without the lines marked as removed: false if record holds no head
bool rewriteRecord(FILE* record, FILE* out);
// record (null: there is none) to out with the line of one Hub download last: a line with the same path is replaced, so
// a file has one line. The head is written first, and the lines marked as removed are left out
bool recordDownload(FILE* record, FILE* out, const char* path, const char* sha256, uint32_t size, const char* key);

// The Hub's version of one SD card file. The Hub entries come one at a time, in any order (the Hub list is not in
// memory): the very file (the same sha256) wins; when the SD card file's sha256 is not known, the same file name
// and the same size (once sdSize is set) are the very file too, as in the catalog's scan. Else the entry of the same firmware key (the same product and the
// same name stem) with the newest date. A file with no product is not a package: the Hub does not list it
class HubMatch {
 public:
  enum Result { NOT_LISTED,
                SAME_FILE,
                NEWER };
  // file: the SD card file's name (a folder may come before it); sha256: "" if it isn't known
  HubMatch(const char* file, const char* product, const char* sha256);
  // one Hub entry; line is where its line is in the Hub list
  // size: the Hub entry's size (0: not given)
  void entry(const char* file, const char* product, const char* sha256, const char* date, uint32_t line,
             uint32_t size = 0);
  // the SD card file's size, for the match by name and size when its sha256 is not known (0: not known)
  void sdSize(uint32_t size) { sdBytes = size; }
  Result result() const;
  // the line of the newer version: only for NEWER
  uint32_t line() const { return newest; }

 private:
  std::string name, product, sha256, newestDate, base;
  bool same = false, found = false;
  uint32_t newest = 0, sdBytes = 0;
};

// record (null: there is none) to out, with the line of oldPath moved to newPath: the new sha256 and size, and the same
// firmware key. The other lines stay as they are. A record with no line of oldPath comes out unchanged, so the new file
// of a file that no sync wrote stays outside it. false if a write fails
bool moveRecordLine(FILE* record, FILE* out, const char* oldPath, const char* newPath, const char* sha256,
                    uint32_t size);

// An extra source (Shrub, the sampler) is one file at a fixed address, with no Hub entry. What a sync does with one
// download of it:
//   EXTRA_REPLACE    the file goes into /fm1 and its record line is written (a first file, or another sha256)
//   EXTRA_UNCHANGED  the recorded sha256: the file stays
//   EXTRA_FAILED     the size, sha256, or device check failed: no file is written, report says why
//   EXTRA_USER_FILE  a file of the user (not in the record) has its name: it stays, report says so
//   EXTRA_NOT_FOUND  the source gave no file: report is "shrub: not found"; it is no failure
enum ExtraResult { EXTRA_REPLACE,
                   EXTRA_UNCHANGED,
                   EXTRA_FAILED,
                   EXTRA_USER_FILE,
                   EXTRA_NOT_FOUND };
// the result of the download (the job does the network, the .part file, the sha256, and the package header)
struct ExtraDownload {
  bool found;           // the server gave a file
  uint32_t size;        // its size
  long contentLength;   // the server's Content-Length: -1 if it sent none
  const char* sha256;   // "" if it can't be computed
  const char* product;  // the device in the package header: "" if it names none
};
struct ExtraDecision {
  ExtraResult result;
  std::string report;  // a line for the user (and the reason of a failure for the log); "" when there is nothing to say
};
// name: "extra:shrub" or "extra:sampler" (the record's name for it); file: its name in /fm1 ("SHRUB.fwsc"). files: what
// /fm1 holds (only the file of this source counts). record is only read
ExtraDecision decideExtra(const char* name, const char* file, FILE* record, const SdFile* files, size_t count,
                          const ExtraDownload& got);
}  // namespace syncplan
