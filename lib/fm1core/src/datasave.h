// The data save decisions: what cardbuddy does with the data saves in /backups/switch. Plain logic: no hal, no uboot,
// so a program on the computer tests it. The caller gives the file names and the facts from memory.
#pragma once

#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#define DATA_SAVES_KEPT 3  // the newest data saves of each firmware identity that stay

namespace datasave {
// one data save in /backups/switch, from its .bin file and its .txt file
struct Save {
  std::string file;         // the .bin file name: "Jiant-FM_2026-11-19.bin", or an old name "fm1-4-FM-1_904.bin"
  std::string firmware;     // the firmware identity, from the "firmware" line of the .txt file: "FM-1_904"
  std::string uid;          // the flash chip's unique ID, from the "flash id" line ("" if it has none)
  std::string sha256;       // from the "sha256" line of the .txt file
  long number = 0;          // the save number, from the "number" line of the .txt file (0: it has none)
  std::string defaultStem;  // the file stem that the job made, from the "default" line ("" if the .txt file has none)
};

// the save number of a data save: the higher, the newer. Its "number" line, or the N of an old name "fm1-N-FW.bin"
// when it has no "number" line (0 if it has neither)
long age(const Save& save);

// the data save has an own name: its "default" line holds another stem than its file name. A data save with no
// "default" line has a default name
bool hasOwnName(const Save& save);

// the data save of one .bin file, from the text of its .txt file (a missing line leaves its value empty)
Save fromText(const std::string& file, const std::string& txt);

// the rows of the list of data saves: the newest first (by the save number, as a number)
std::vector<Save> newestFirst(const std::vector<Save>& saves);

// the data save comes from the FM-1 that has this unique ID. A data save with no unique ID comes from no known FM-1,
// and an empty unique ID is no FM-1 either
bool sameFm1(const Save& save, const std::string& uid);

// the .bin and the .txt file names to remove: for each firmware identity, all but the DATA_SAVES_KEPT newest data saves
// with a default name. A data save with an own name is never removed, and it takes no place among the newest. The data
// save named keep (a .bin file name) is never removed either, and it takes no place among the newest. Another firmware
// identity never loses a file
std::vector<std::string> toRemove(const std::vector<Save>& saves, const std::string& keep = "");

// where the data save, the flash, and the package come from: n bytes from the address at into out (false: it could not
// read them). The caller reads from a file, from a FlashBuf, or from memory
typedef std::function<bool(uint32_t at, uint8_t* out, uint32_t n)> Reader;

// a package of the firmware library: its flash image from flash address 0, and where the image ends
struct Package {
  Reader read;
  uint32_t end;
};

// the facts of one restore. The boot loader gives chipKey, flashId, and flashSize
struct Restore {
  uint32_t fileSize;        // the .bin file of the data save
  std::string fileSha256;   // the SHA-256 of that .bin file, as the job computed it
  std::string savedSha256;  // the SHA-256 in the .txt file
  uint16_t chipKey;
  uint32_t flashId;  // not a reason to refuse: each maker's flash part is the same type
  uint32_t flashSize;
  uint32_t imageEnd;  // the end of the flash image of the matching package (0: no package matched)
};

enum Refusal { REFUSE_NONE,
               REFUSE_FILE_SIZE,     // the .bin file is not 1 MiB
               REFUSE_SHA256,        // the .bin file is not what the .txt file says
               REFUSE_OTHER_BUILD,   // the firmware sectors of the data save differ from the flash
               REFUSE_SMK37_DEVICE,  // the "trimdat" mark is on the device
               REFUSE_SMK37_SAVE,    // the "trimdat" mark is in the data save
               REFUSE_CHIP,          // not the FM-1 chip
               REFUSE_FLASH_SIZE,    // the flash is not 1 MiB
               REFUSE_UNREADABLE };  // a reader could not read what a rule needs

// the .bin file name of the data save with this firmware identity and this unique ID: the newest one ("" if none).
// An empty unique ID matches nothing
std::string matchSave(const std::vector<Save>& saves, const std::string& product, const std::string& uid);

// the index of the first package whose flash image is the same bytes as the data save from the first firmware sector
// to the image end of that package (-1: none)
int matchPackage(const Reader& save, const std::vector<Package>& packages);

// the sectors a restore writes: each one from the image end to the key_mac sector (the sector that holds the image end
// is the first) where the data save differs from the flash. failed (if given) says a reader failed: the list is then
// not whole
std::vector<uint32_t> sectors(const Reader& save, const Reader& flash, uint32_t imageEnd, bool* failed = nullptr);

// where a transfer (a restore on an FM-1 that the data save is not from) goes on: write the data now (the FM-1 runs the
// build of the data save already), install the matching package first, or stop (no package matches)
enum TransferPath { TRANSFER_WRITE,
                    TRANSFER_INSTALL,
                    TRANSFER_NO_PACKAGE };
struct Transfer {
  TransferPath path;
  int package;  // the index of the matching package (-1: none)
};
// the path of a transfer. The same package match as matchPackage, and the same compare of the firmware sectors as the
// refusal REFUSE_OTHER_BUILD. The unique ID is no reason to refuse here
Transfer transfer(const Reader& save, const Reader& flash, const std::vector<Package>& packages);

// why this restore must not start (REFUSE_NONE: it may). A refusal has no force
Refusal refusal(const Restore& r, const Reader& save, const Reader& flash);
// one line for the user, for each refusal
const char* reason(Refusal r);
}  // namespace datasave
