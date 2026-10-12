#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace hazf {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr std::array<char, 4> kMagic = {'H', 'A', 'Z', 'F'};
constexpr uint16_t kVersion = 1;
constexpr size_t kArchiveHeaderSize = 12;
constexpr size_t kEntryHeaderSize = 24;
constexpr uint64_t kMaxFileSize = 8ULL * 1024ULL * 1024ULL * 1024ULL; // 8 GiB
constexpr uint32_t kMaxEntries = 1'000'000;
constexpr uint32_t kMaxPathLen = 65535;

// ---------------------------------------------------------------------------
// Little-endian helpers
// ---------------------------------------------------------------------------
inline void write_u16_le(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
inline void write_u32_le(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
inline void write_u64_le(uint8_t* dst, uint64_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    dst[4] = static_cast<uint8_t>((v >> 32) & 0xFF);
    dst[5] = static_cast<uint8_t>((v >> 40) & 0xFF);
    dst[6] = static_cast<uint8_t>((v >> 48) & 0xFF);
    dst[7] = static_cast<uint8_t>((v >> 56) & 0xFF);
}
inline uint16_t read_u16_le(const uint8_t* src) {
    return static_cast<uint16_t>(src[0]) |
           (static_cast<uint16_t>(src[1]) << 8);
}
inline uint32_t read_u32_le(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}
inline uint64_t read_u64_le(const uint8_t* src) {
    return static_cast<uint64_t>(src[0]) |
           (static_cast<uint64_t>(src[1]) << 8) |
           (static_cast<uint64_t>(src[2]) << 16) |
           (static_cast<uint64_t>(src[3]) << 24) |
           (static_cast<uint64_t>(src[4]) << 32) |
           (static_cast<uint64_t>(src[5]) << 40) |
           (static_cast<uint64_t>(src[6]) << 48) |
           (static_cast<uint64_t>(src[7]) << 56);
}

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3, polynomial 0xEDB88320)
// ---------------------------------------------------------------------------
inline uint32_t crc32_table_entry(uint32_t i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
        if (c & 1) c = 0xEDB88320U ^ (c >> 1);
        else c >>= 1;
    }
    return c;
}

inline const std::array<uint32_t, 256>& crc32_table() {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) t[i] = crc32_table_entry(i);
        return t;
    }();
    return table;
}

inline uint32_t crc32(const uint8_t* data, size_t len, uint32_t prev = 0) {
    uint32_t crc = ~prev;
    const auto& table = crc32_table();
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

inline uint32_t crc32(const std::vector<uint8_t>& data) {
    if (data.empty()) return 0;
    return crc32(data.data(), data.size(), 0);
}

// overload for string
inline uint32_t crc32_string(const std::string& s) {
    if (s.empty()) return 0;
    return crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size(), 0);
}

// ---------------------------------------------------------------------------
// Archive header (12 bytes)
// Layout (little-endian):
//   0-3 : magic "HAZF" (0x48 0x41 0x5A 0x46)
//   4-5 : version uint16 (1 for v1.0)
//   6-7 : reserved uint16 (0)
//   8-11: entry_count uint32
// ---------------------------------------------------------------------------
struct ArchiveHeader {
    uint16_t version = kVersion;
    uint16_t reserved = 0;
    uint32_t entryCount = 0;

    static constexpr size_t kSize = kArchiveHeaderSize;

    std::array<uint8_t, kSize> serialize() const {
        std::array<uint8_t, kSize> out{};
        out[0] = static_cast<uint8_t>(kMagic[0]);
        out[1] = static_cast<uint8_t>(kMagic[1]);
        out[2] = static_cast<uint8_t>(kMagic[2]);
        out[3] = static_cast<uint8_t>(kMagic[3]);
        write_u16_le(out.data() + 4, version);
        write_u16_le(out.data() + 6, reserved);
        write_u32_le(out.data() + 8, entryCount);
        return out;
    }

    static ArchiveHeader deserialize(const uint8_t* buf) {
        if (buf[0] != static_cast<uint8_t>(kMagic[0]) ||
            buf[1] != static_cast<uint8_t>(kMagic[1]) ||
            buf[2] != static_cast<uint8_t>(kMagic[2]) ||
            buf[3] != static_cast<uint8_t>(kMagic[3])) {
            throw std::runtime_error("invalid HAZF magic");
        }
        ArchiveHeader h;
        h.version = read_u16_le(buf + 4);
        h.reserved = read_u16_le(buf + 6);
        h.entryCount = read_u32_le(buf + 8);
        return h;
    }
};

// ---------------------------------------------------------------------------
// Entry header (24 bytes)
// Layout (little-endian):
//   0-1  : path_len uint16 (bytes of UTF-8 path, no NUL)
//   2-3  : reserved uint16 (0, future flags)
//   4-11 : original_size uint64 (uncompressed)
//   12-19: compressed_size uint64
//   20-23: crc32 uint32 (of uncompressed data)
// Total 2+2+8+8+4 = 24
// Followed by: path bytes (path_len) + payload (compressed_size)
// ---------------------------------------------------------------------------
struct EntryHeader {
    uint16_t pathLen = 0;
    uint16_t reserved = 0;
    uint64_t originalSize = 0;
    uint64_t compressedSize = 0;
    uint32_t crc32 = 0;

    static constexpr size_t kSize = kEntryHeaderSize;

    std::array<uint8_t, kSize> serialize() const {
        std::array<uint8_t, kSize> out{};
        write_u16_le(out.data() + 0, pathLen);
        write_u16_le(out.data() + 2, reserved);
        write_u64_le(out.data() + 4, originalSize);
        write_u64_le(out.data() + 12, compressedSize);
        write_u32_le(out.data() + 20, crc32);
        return out;
    }

    static EntryHeader deserialize(const uint8_t* buf) {
        EntryHeader h;
        h.pathLen = read_u16_le(buf + 0);
        h.reserved = read_u16_le(buf + 2);
        h.originalSize = read_u64_le(buf + 4);
        h.compressedSize = read_u64_le(buf + 12);
        h.crc32 = read_u32_le(buf + 20);
        return h;
    }
};

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------
inline void write_exact(std::ostream& os, const uint8_t* data, size_t len) {
    os.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    if (!os) throw std::runtime_error("write failed");
}

inline void read_exact(std::istream& is, uint8_t* data, size_t len) {
    is.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(len));
    if (static_cast<size_t>(is.gcount()) != len || !is) {
        throw std::runtime_error("read failed: unexpected EOF or truncated file");
    }
}

// ---------------------------------------------------------------------------
// Path utilities
// ---------------------------------------------------------------------------
inline std::string to_generic_string(const std::filesystem::path& p) {
    return p.generic_string();
}

// Normalize stored path: convert to generic, strip leading slashes and "./",
// lexically_normalize. Returns empty if path is ".".
// Throws if path contains ".." traversal that would escape.
inline std::string normalize_stored_path_string(const std::string& raw) {
    if (raw.empty()) return "";
    std::filesystem::path p(raw);
    // Use generic string for consistent separators
    p = p.lexically_normal();
    std::string s = p.generic_string();

    // Remove leading "./"
    while (s.rfind("./", 0) == 0) {
        s.erase(0, 2);
    }
    // Remove leading "/"
    while (!s.empty() && s[0] == '/') {
        s.erase(0, 1);
    }
    // Remove trailing "/"
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    if (s == "." || s.empty()) return "";
    // Validate no ".." components
    std::filesystem::path check(s);
    for (auto& part : check) {
        if (part == "..") {
            throw std::runtime_error("stored path must not contain '..': " + s);
        }
    }
    // No backslash allowed (should be normalized)
    if (s.find('\\') != std::string::npos) {
        throw std::runtime_error("stored path must not contain backslash: " + s);
    }
    return s;
}

inline std::string normalize_input_path(const std::filesystem::path& input) {
    // For file input: the stored path is the normalized input string.
    // e.g., "a/b/c.txt" -> "a/b/c.txt", "./a/b" -> "a/b", "/abs/path" -> "abs/path"
    std::string s = input.generic_string();
    s = normalize_stored_path_string(s);
    return s;
}

// Validate stored path for security on extract
inline bool is_valid_stored_path(const std::string& s) {
    if (s.empty()) return false;
    if (s.size() > kMaxPathLen) return false;
    if (s[0] == '/' || s[0] == '\\') return false;
    // Check for absolute or traversal
    std::filesystem::path p(s);
    if (p.is_absolute()) return false;
    for (auto& part : p) {
        if (part == "..") return false;
        std::string partStr = part.string();
        if (partStr.find('\\') != std::string::npos) return false;
        if (partStr.empty()) return false;
    }
    // No backslash in raw string
    if (s.find('\\') != std::string::npos) return false;
    return true;
}

} // namespace hazf
