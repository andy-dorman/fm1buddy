// The names of the saved files: the default name of a data save and of a backup. Plain logic: no hal, so a program on
// the computer tests it. The caller gives the date as a time value, and the file names of the folder from memory.
#pragma once

#include <time.h>

#include <string>
#include <vector>

namespace savename {
// the kinds of saved file
enum Kind { DATA_SAVE,
            BACKUP,
            VOICE,
            SETUP,
            SLOOP_BACKUP,
            SLOOP_SONG };

// the extension of a saved file of this kind: ".bin", ".bin", ".syx", "" (a setup is a folder), ".json", ".sloopsong"
const char* extension(Kind kind);

// the UTC date of a time value: "2026-11-19" ("" when the clock is not set: 1600000000 or less)
std::string dateText(time_t t);

// the firmware part of a default name. identity: the firmware identity of the FM-1 ("FM-1_915"). displayName and
// product: the display name and the product of the package that the newest install record names. The display name
// is used only when the product is the firmware identity: "Jiant-FM". Else the firmware identity: "FM-1_904". With no
// firmware identity: "FM-1", or "SMK-37" when smk. Each character that a file name cannot hold becomes "-". Longer
// than 20 characters: the first 20
std::string firmwarePart(const std::string& identity, const std::string& displayName, const std::string& product,
                         bool smk = false);

// the default name stem: "Jiant-FM_2026-11-19". With no date (""): "Jiant-FM_no-date-4", with the save number n. With a
// word: "Jiant-FM_2026-11-19_before-recovery". Without the word, the stem holds 32 characters or fewer: the firmware
// part is cut when it must be
std::string defaultStem(const std::string& firmwarePart, const std::string& date, long n, const std::string& word = "");

// a file name, and its "number" line (0: it has none)
struct Numbered {
  std::string name;
  long number;
};

// the save number of the next file: one more than the highest number line, and the highest N of an old name
// ("fm1-3-FM-1_904.bin", "smk-9.bin") in the folder
long nextSaveNumber(const std::vector<Numbered>& folder);

// the save number of a file: its number line, or the N of its old name when it has no number line (0: none)
long saveNumber(const Numbered& file);

// the stem that no file of the folder has: stem, then stem-2, then stem-3. taken: the file names with their extensions.
// Only a name with the extension ext blocks a stem
std::string freeName(const std::string& stem, const std::string& ext, const std::vector<std::string>& taken);

// a comes before b on a list: the higher save number first, and A to Z by file name when they are the same
bool newer(const Numbered& a, const Numbered& b);

// the own name of a file: the text the user typed, as a name stem. Each character that is not a letter, a digit, "-",
// "_", or "." becomes "-", a "." at the start becomes "-" (the lists skip a name that starts with "."), and only the first
// 32 characters stay. It adds no firmware part, no date, and no word. "" gives ""
std::string ownName(const std::string& typed);

// the stem that a job saves with: the own name of the typed text, or defaultStem when the text is empty
std::string stemOf(const std::string& typed, const std::string& defaultStem);

// the stem of a file after a rename. current: its stem now. typed: the text of the user. ext: its extension (".bin").
// taken: the file names of its folder. An empty text, or the text of the current stem, gives the current stem (the
// text is compared before the cleanup, so a current stem of more than 32 characters stays). Else the own name of the
// text, with the free name of the folder. A ".txt" file counts as the file of its stem, so a stem that only a ".txt"
// file holds is taken too
std::string renameStem(const std::string& current, const std::string& typed, const std::string& ext,
                       const std::vector<std::string>& taken);

// the number N of the next name with no date, for a folder with no .txt files: one more than the highest N after
// "no-date-" (a word may follow it) and after an old prefix at the start of a name ("voice-", "setup-", "backup-",
// "song-", "before-restore-", "before-song-") in names (the file names of the folder)
long nextNumber(const std::vector<std::string>& names);

// a file name, and its file time
struct Timed {
  std::string name;
  time_t time;
};

// a comes before b on a list of files with no .txt file: first each file with a good file time (more than
// 1600000000), the newest first; then the other files by the N of their name (see nextNumber; 0: none), the highest
// first. Equal files: by name, in reverse order
bool newer(const Timed& a, const Timed& b);

// the save number of a .bin file in a folder, from the "number" line of its .txt file (0: it has no .txt file, or no
// "number" line). dir is a folder path, bin the .bin file name
long numberOfSaved(const std::string& dir, const std::string& bin);
}  // namespace savename
