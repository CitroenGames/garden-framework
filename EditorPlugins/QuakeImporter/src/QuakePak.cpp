#include "QuakePak.hpp"
#include <algorithm>
#include <cstring>

namespace QuakeImporter {

namespace {
    struct PakHeader { char magic[4]; int32_t dirofs; int32_t dirlen; };
    struct RawEntry  { char name[56]; int32_t offset; int32_t size; };
    static_assert(sizeof(RawEntry) == 64, "Quake PAK entry must be 64 bytes");
}

bool PakArchive::open(const std::string& path)
{
    close();
    m_path = path;
    m_file.open(path, std::ios::binary);
    if (!m_file.is_open()) return false;

    PakHeader header{};
    m_file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (m_file.gcount() != (std::streamsize)sizeof(header))
    {
        close();
        return false;
    }

    if (std::memcmp(header.magic, "PACK", 4) != 0)
    {
        close();
        return false;
    }

    if (header.dirlen < 0 || header.dirofs < 0 || (header.dirlen % (int32_t)sizeof(RawEntry)) != 0)
    {
        close();
        return false;
    }

    const size_t count = (size_t)(header.dirlen / (int32_t)sizeof(RawEntry));
    m_entries.clear();
    m_entries.reserve(count);

    m_file.seekg(header.dirofs, std::ios::beg);
    for (size_t i = 0; i < count; ++i)
    {
        RawEntry raw{};
        m_file.read(reinterpret_cast<char*>(&raw), sizeof(raw));
        if (m_file.gcount() != (std::streamsize)sizeof(raw))
        {
            close();
            return false;
        }
        // Name is NUL-terminated within the 56-byte field.
        char buf[57]; std::memcpy(buf, raw.name, 56); buf[56] = 0;
        PakEntry e;
        e.name   = buf;
        e.offset = (uint32_t)raw.offset;
        e.size   = (uint32_t)raw.size;
        m_entries.push_back(std::move(e));
    }

    // Deterministic order for UI.
    std::sort(m_entries.begin(), m_entries.end(),
              [](const PakEntry& a, const PakEntry& b) { return a.name < b.name; });
    return true;
}

void PakArchive::close()
{
    if (m_file.is_open()) m_file.close();
    m_entries.clear();
    m_path.clear();
}

bool PakArchive::readEntry(const PakEntry& e, std::vector<uint8_t>& out)
{
    if (!m_file.is_open()) return false;
    out.resize(e.size);
    m_file.seekg(e.offset, std::ios::beg);
    m_file.read(reinterpret_cast<char*>(out.data()), e.size);
    return m_file.gcount() == (std::streamsize)e.size;
}

static bool endsWith(const std::string& s, const char* suf)
{
    size_t n = std::strlen(suf);
    return s.size() >= n && std::equal(s.end() - n, s.end(), suf,
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
}

EntryKind classify(const std::string& name)
{
    if (endsWith(name, ".mdl")) return EntryKind::Model;
    if (endsWith(name, ".bsp")) return EntryKind::Level;
    if (endsWith(name, ".wav")) return EntryKind::Sound;
    if (endsWith(name, ".lmp") || endsWith(name, ".spr") || endsWith(name, ".wad"))
        return EntryKind::Texture;
    if (endsWith(name, ".qc")  || endsWith(name, ".rc") || endsWith(name, ".cfg"))
        return EntryKind::Script;
    return EntryKind::Other;
}

const char* kindName(EntryKind k)
{
    switch (k)
    {
        case EntryKind::Model:   return "Models";
        case EntryKind::Texture: return "Textures";
        case EntryKind::Level:   return "Levels";
        case EntryKind::Sound:   return "Sounds";
        case EntryKind::Script:  return "Scripts";
        default:                 return "Other";
    }
}

} // namespace QuakeImporter
