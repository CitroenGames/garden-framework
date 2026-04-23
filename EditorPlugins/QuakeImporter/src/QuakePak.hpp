#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>

// Quake 1 PAK file format. Header + flat directory, no compression. Spec:
//   struct Header   { char magic[4] = "PACK"; int32 dirofs; int32 dirlen; };
//   struct Entry    { char name[56]; int32 offset; int32 size; };  // 64 bytes
//   entry_count  = dirlen / sizeof(Entry)
//
// This reader opens a .pak, reads the directory, and exposes slice reads
// without loading the entire file into memory.

namespace QuakeImporter {

struct PakEntry
{
    std::string name;    // e.g. "progs/player.mdl" (null-terminated, forward slashes)
    uint32_t    offset;  // byte offset into the .pak file
    uint32_t    size;    // file size in bytes
};

class PakArchive
{
public:
    bool open(const std::string& path);
    void close();
    bool isOpen() const { return m_file.is_open(); }

    const std::vector<PakEntry>& entries() const { return m_entries; }
    const std::string&           path()    const { return m_path; }

    // Read bytes for a single entry. Returns true on success; `out` is resized.
    bool readEntry(const PakEntry& e, std::vector<uint8_t>& out);

private:
    std::string           m_path;
    std::ifstream         m_file;
    std::vector<PakEntry> m_entries;
};

// Category of a PAK entry derived from its extension — used to group in UI.
enum class EntryKind : uint8_t { Other, Model, Texture, Level, Sound, Script };

EntryKind classify(const std::string& name);
const char* kindName(EntryKind k);

} // namespace QuakeImporter
