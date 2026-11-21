// SLOOP (isod89/sloop-fm1), through its editor protocol (web/EDITOR_PROTOCOL.md: SysEx F0 7D 46 4C <cmd> ... F7, one
// request at a time, each answered once): backups in SLOOP's own file, so they open in its web editor and its
// backups open here; restoring one; loading a project from one; and .wav files into its user sample slots.
//
// Backups need SLOOP 2.3 (protocol 6) or later. They're kept on the SD card in /sloop, as <name>.json
// ({"format": "sloop-backup", "version": 1, "firmware", "date", "objects": [{"id", "len", "crc", "data"}]}); the
// sample folders are in /sloop/samples (a folder of .wav files, or one .wav, makes a slot: each file a key zone, its
// root note from its name, bass_C2.wav). Songs (SLOOP 2.7's "Export this song": the working loop, sections A-D and
// their order on the SONG screen) are in /sloop/songs, as SLOOP's own .sloopsong files ({"format": "sloop-song",
// "version": 1, "name", "firmware", "date", "order", "objects"}): they open in its editor, and its exports load here.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#define SLOOP_DIR "/sloop"
#define SLOOP_SAMPLES_DIR "/sloop/samples"
#define SLOOP_SONGS_DIR "/sloop/songs"

namespace sloop {
// what INFO says
struct Info {
  std::string version;  // "FELUCCA SLOOP 2.8"
  int pcount = 0;       // its track parameters (which project formats it reads)
  int proto = 0;        // its protocol version: 6 and up have backups
  bool sloop() const;   // it's SLOOP, or Kitten Synths (SLOOP with more): not Felucca itself, or Clipper
};
bool parseInfo(const uint8_t* args, uint32_t n, Info& out);  // INFO's reply, after its cmd

// a backup
struct Object {
  int id;
  std::vector<uint8_t> data;
};
struct Backup {
  std::string firmware, date;
  std::vector<Object> objects;
  const Object* get(int id) const;
};
std::string objectName(int id);      // "project A", "settings", "sample slot USR1"
std::vector<std::string> backups();  // its files, the newest first
// a backup file, checked (each object's length and CRC); false and why if it isn't a good one
bool read(const std::string& name, Backup& out, std::string& why);
// a one-line summary: "projects A B, presets, 2 sample slots"
std::string summary(const Backup& b);
// a song (.sloopsong): the working loop (id 0) and the sections A-D in it (2..5; one not in it is empty), and their
// order: the 36 bytes of the settings at 48 (SLOOP's arr_config_t), or none
struct Song : Backup {
  std::string name;
  std::vector<uint8_t> order;
};
std::vector<std::string> songs();  // its files, the newest first
bool readSong(const std::string& name, Song& out, std::string& why);
std::string songSummary(const Song& s);  // "sections A B, and their order"

// the sample sources in /sloop/samples: folders of .wav files, and .wav files on their own
std::vector<std::string> sampleSources();

// a sample slot, as SMP_INFO tells it
struct Slot {
  int zones;  // 0: empty
  std::string name;
  int kib;
};

typedef void (*Progress)(uint32_t done, uint32_t total);
// these block (a job's thread); each returns 0, or what went wrong
const char* info(int d, Info& out);
const char* slots(int d, std::vector<Slot>& out);
// all of it to a new backup file named stem (stem-2 ... when it is taken; its name into saved)
const char* backup(int d, const Info& info, std::string& saved, Progress p, const std::string& stem);
// a backup back onto it: its banks, projects, the working project, the settings, then the samples (backed up first,
// into a backup file named beforeStem: its name in savedFirst)
const char* restore(int d, const std::string& name, std::string& savedFirst, Progress p,
                    const std::string& beforeStem);
// one project of a backup (id 0: the working one, 2..5: A..D) onto it, as project to (the same ids)
const char* loadProject(int d, const std::string& name, int from, int to, Progress p);
// the song on it to a new song file named stem (stem-2 ... when it is taken; its name into saved). The name inside the
// file is the stem that it saved as
const char* saveSong(int d, const Info& info, std::string& saved, Progress p, const std::string& stem);
// a song file onto it (stopped): the sections, the loop, then the order into its settings (the rest of them kept). The
// song it had is saved first, into a song file named beforeStem: its name in savedFirst
const char* loadSong(int d, const std::string& name, std::string& savedFirst, Progress p,
                     const std::string& beforeStem);
// a sample source (sampleSources) into slot (0..3)
const char* loadSamples(int d, const std::string& source, int slot, Progress p);
}  // namespace sloop
