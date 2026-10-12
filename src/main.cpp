#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <zstd.h>

#include "hazf/format.hpp"

namespace fs = std::filesystem;
using namespace hazf;

static void print_usage(const char* prog) {
    std::cout << "HAZF - Hyggshi Archive Zip Format v1.0\n"
              << "Usage:\n"
              << "  " << prog << " create <archive.hazf> <file|dir>...\n"
              << "      Create archive from files and directories\n"
              << "  " << prog << " list <archive.hazf>\n"
              << "      List files and original sizes\n"
              << "  " << prog << " extract <archive.hazf> [output_dir]\n"
              << "      Extract archive (refuses to overwrite existing files)\n"
              << "  " << prog << " test <archive.hazf>\n"
              << "      Test archive integrity (decompress in memory and verify CRC32)\n"
              << "  " << prog << " info <archive.hazf>\n"
              << "      Show archive information\n"
              << "  " << prog << " help | --help | -h\n"
              << "      Show this help\n"
              << "\n"
              << "Format: HAZF v1.0, magic \"HAZF\", little-endian, Zstandard compressed,\n"
              << "        header 12 bytes, entry header 24 bytes, CRC32 integrity.\n"
              << "Limits: max 8 GiB per file, max 1,000,000 entries, max path 65535 bytes.\n";
}

struct CollectedEntry {
    fs::path fsPath;      // actual filesystem path
    std::string storedPath; // normalized relative path stored in archive
};

// Collect files from inputs
static std::vector<CollectedEntry> collect_inputs(const std::vector<std::string>& inputs) {
    std::vector<CollectedEntry> out;
    std::unordered_set<std::string> seenStored;

    for (const auto& rawInput : inputs) {
        fs::path inputPath(rawInput);
        if (!fs::exists(inputPath)) {
            throw std::runtime_error("input does not exist: " + rawInput);
        }

        if (fs::is_regular_file(inputPath)) {
            std::string stored;
            if (inputPath.is_absolute()) {
                stored = inputPath.filename().generic_string();
                if (stored.empty() || stored == ".") {
                    throw std::runtime_error("invalid stored path for input: " + rawInput);
                }
                if (stored.find('/') != std::string::npos || stored.find('\\') != std::string::npos) {
                    throw std::runtime_error("invalid filename for input: " + rawInput);
                }
                // Validate no traversal
                stored = normalize_stored_path_string(stored);
            } else {
                stored = normalize_input_path(inputPath);
                if (stored.empty()) {
                    // use filename only if normalization emptied (e.g., input ".")
                    stored = inputPath.filename().generic_string();
                    if (stored.empty() || stored == ".") {
                        throw std::runtime_error("invalid stored path for input: " + rawInput);
                    }
                    stored = normalize_stored_path_string(stored);
                }
            }
            if (!is_valid_stored_path(stored)) {
                throw std::runtime_error("invalid stored path: " + stored);
            }
            if (seenStored.find(stored) != seenStored.end()) {
                throw std::runtime_error("duplicate stored path: " + stored);
            }
            seenStored.insert(stored);
            out.push_back({inputPath, stored});
        } else if (fs::is_directory(inputPath)) {
            std::string baseStored;
            if (inputPath.is_absolute()) {
                baseStored = inputPath.filename().generic_string();
                if (baseStored == "." || baseStored.empty()) {
                    baseStored = "";
                } else {
                    baseStored = normalize_stored_path_string(baseStored);
                }
            } else {
                baseStored = normalize_input_path(inputPath);
            }
            // baseStored may be empty if input is "." or "./"
            // For directory, we want to preserve the directory name itself as prefix.
            // Example: input "mydir" with file "mydir/a.txt" -> stored "mydir/a.txt"
            // If baseStored empty (input "."), store relative without prefix.
            for (auto it = fs::recursive_directory_iterator(inputPath, fs::directory_options::skip_permission_denied);
                 it != fs::recursive_directory_iterator(); ++it) {
                const auto& e = *it;
                // Only regular files; skip directories, symlinks, etc.
                // Follow regular files only
                if (!e.is_regular_file()) {
                    if (e.is_symlink()) {
                        // Optionally skip symlinks with warning
                        std::cerr << "warning: skipping non-regular file: " << e.path().string() << "\n";
                    }
                    continue;
                }
                fs::path filePath = e.path();
                // Compute relative to the input directory
                fs::path rel = fs::relative(filePath, inputPath);
                std::string relStr = rel.generic_string();
                // Normalize relStr
                relStr = normalize_stored_path_string(relStr);
                if (relStr.empty()) continue;

                std::string stored;
                if (baseStored.empty()) {
                    stored = relStr;
                } else {
                    stored = baseStored + "/" + relStr;
                }
                // Final validation and normalize again to catch issues
                stored = normalize_stored_path_string(stored);
                if (!is_valid_stored_path(stored)) {
                    throw std::runtime_error("invalid stored path: " + stored);
                }
                if (seenStored.find(stored) != seenStored.end()) {
                    throw std::runtime_error("duplicate stored path: " + stored);
                }
                seenStored.insert(stored);
                out.push_back({filePath, stored});
            }
            // If directory is empty, we don't store empty dir entry (regular files only)
            // That's acceptable per spec (target: regular files).
        } else {
            std::cerr << "warning: skipping non-regular file: " << rawInput << "\n";
        }
    }

    if (out.size() > kMaxEntries) {
        throw std::runtime_error("too many entries: " + std::to_string(out.size()) + " > 1000000");
    }

    // Sort for deterministic archive
    std::sort(out.begin(), out.end(), [](const CollectedEntry& a, const CollectedEntry& b){
        return a.storedPath < b.storedPath;
    });

    return out;
}

// ---------------------------------------------------------------------------
// Command: create
// ---------------------------------------------------------------------------
static int cmd_create(const std::vector<std::string>& args) {
    // args: [archive, inputs...]
    if (args.size() < 2) {
        std::cerr << "error: create requires <archive.hazf> and at least one input\n";
        std::cerr << "usage: hazf create <archive.hazf> <file|dir>...\n";
        return 1;
    }
    std::string archivePathStr = args[0];
    std::vector<std::string> inputs(args.begin() + 1, args.end());

    std::vector<CollectedEntry> entries;
    try {
        entries = collect_inputs(inputs);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    if (entries.empty()) {
        std::cerr << "error: no regular files found to archive\n";
        return 1;
    }

    // Validate archive path writable
    // Do not allow archive to be inside inputs to avoid recursion? Not needed.

    struct PreparedEntry {
        std::string storedPath;
        std::vector<uint8_t> compressed;
        uint64_t originalSize = 0;
        uint32_t crc = 0;
    };
    std::vector<PreparedEntry> prepared;
    prepared.reserve(entries.size());

    std::cout << "Creating archive: " << archivePathStr << " (" << entries.size() << " entries)\n";

    for (const auto& ce : entries) {
        // Check file size
        uint64_t fsize = 0;
        try {
            fsize = fs::file_size(ce.fsPath);
        } catch (const std::exception& e) {
            std::cerr << "error: cannot get size of " << ce.fsPath << ": " << e.what() << "\n";
            return 1;
        }
        if (fsize > kMaxFileSize) {
            std::cerr << "error: file too large (>8 GiB): " << ce.fsPath << " (" << fsize << " bytes)\n";
            return 1;
        }
        if (ce.storedPath.size() > kMaxPathLen) {
            std::cerr << "error: path too long: " << ce.storedPath << "\n";
            return 1;
        }

        // Read file
        std::vector<uint8_t> data;
        data.resize(static_cast<size_t>(fsize));
        if (fsize > 0) {
            std::ifstream in(ce.fsPath, std::ios::binary);
            if (!in) {
                std::cerr << "error: cannot open for reading: " << ce.fsPath << "\n";
                return 1;
            }
            in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fsize));
            if (static_cast<uint64_t>(in.gcount()) != fsize) {
                std::cerr << "error: short read: " << ce.fsPath << "\n";
                return 1;
            }
        }

        // CRC32 of uncompressed
        uint32_t crc = 0;
        if (!data.empty()) crc = crc32(data.data(), data.size());
        else crc = 0;

        // Compress with Zstd
        size_t bound = ZSTD_compressBound(data.size());
        std::vector<uint8_t> comp(bound);
        size_t cSize = 0;
        if (data.empty()) {
            // ZSTD handles empty as 0? But compressBound for 0 still gives something; compress empty yields small frame
            // We still compress empty to produce valid zstd frame; alternatively store 0 compressed bytes for empty?
            // To keep decompression symmetric, we compress empty input via ZSTD_compress
            cSize = ZSTD_compress(comp.data(), comp.size(), data.data(), 0, 3);
            if (ZSTD_isError(cSize)) {
                std::cerr << "error: ZSTD compress failed for " << ce.fsPath << ": " << ZSTD_getErrorName(cSize) << "\n";
                return 1;
            }
            comp.resize(cSize);
        } else {
            cSize = ZSTD_compress(comp.data(), comp.size(), data.data(), data.size(), 3);
            if (ZSTD_isError(cSize)) {
                std::cerr << "error: ZSTD compress failed for " << ce.fsPath << ": " << ZSTD_getErrorName(cSize) << "\n";
                return 1;
            }
            comp.resize(cSize);
        }

        // For empty original, compressed may be ~9 bytes (zstd frame). That's okay.
        // Alternative we could store zero compressed size for empty files, but we choose to store actual frame.

        PreparedEntry pe;
        pe.storedPath = ce.storedPath;
        pe.compressed = std::move(comp);
        pe.originalSize = fsize;
        pe.crc = crc;
        prepared.push_back(std::move(pe));

        std::cout << "  adding: " << ce.storedPath << " (" << fsize << " -> " << prepared.back().compressed.size() << " bytes)\n";
    }

    // Write archive
    std::ofstream out(archivePathStr, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "error: cannot open archive for writing: " << archivePathStr << "\n";
        return 1;
    }

    // Header
    ArchiveHeader ah;
    ah.version = kVersion;
    ah.reserved = 0;
    ah.entryCount = static_cast<uint32_t>(prepared.size());
    auto ahBytes = ah.serialize();
    out.write(reinterpret_cast<const char*>(ahBytes.data()), ahBytes.size());
    if (!out) {
        std::cerr << "error: failed to write archive header\n";
        return 1;
    }

    // Entries
    for (const auto& pe : prepared) {
        EntryHeader eh;
        eh.pathLen = static_cast<uint16_t>(pe.storedPath.size());
        eh.reserved = 0;
        eh.originalSize = pe.originalSize;
        eh.compressedSize = static_cast<uint64_t>(pe.compressed.size());
        eh.crc32 = pe.crc;
        auto ehBytes = eh.serialize();
        out.write(reinterpret_cast<const char*>(ehBytes.data()), ehBytes.size());
        if (!out) {
            std::cerr << "error: failed to write entry header for " << pe.storedPath << "\n";
            return 1;
        }
        if (!pe.storedPath.empty()) {
            out.write(pe.storedPath.data(), pe.storedPath.size());
            if (!out) {
                std::cerr << "error: failed to write path for " << pe.storedPath << "\n";
                return 1;
            }
        }
        if (!pe.compressed.empty()) {
            out.write(reinterpret_cast<const char*>(pe.compressed.data()), pe.compressed.size());
            if (!out) {
                std::cerr << "error: failed to write payload for " << pe.storedPath << "\n";
                return 1;
            }
        }
    }

    out.close();
    if (!out) {
        std::cerr << "error: failed to close archive\n";
        return 1;
    }

    auto archiveSize = fs::file_size(archivePathStr);
    std::cout << "Archive created successfully: " << archivePathStr << " (" << archiveSize << " bytes, " << prepared.size() << " entries)\n";
    return 0;
}

// Helper to open and validate archive header, returning entry count and leaving stream positioned after header
static ArchiveHeader open_and_read_header(const std::string& archivePath, std::ifstream& in, uint64_t& archiveFileSize) {
    if (!fs::exists(archivePath)) {
        throw std::runtime_error("archive not found: " + archivePath);
    }
    archiveFileSize = fs::file_size(archivePath);
    if (archiveFileSize < kArchiveHeaderSize) {
        throw std::runtime_error("archive too small (truncated header): " + archivePath);
    }
    in.open(archivePath, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open archive for reading: " + archivePath);
    std::array<uint8_t, kArchiveHeaderSize> buf{};
    read_exact(in, buf.data(), buf.size());
    ArchiveHeader ah = ArchiveHeader::deserialize(buf.data());
    if (ah.version != kVersion) {
        throw std::runtime_error("unsupported HAZF version: " + std::to_string(ah.version) + " (expected 1)");
    }
    if (ah.entryCount > kMaxEntries) {
        throw std::runtime_error("archive entry count exceeds limit: " + std::to_string(ah.entryCount));
    }
    return ah;
}

// ---------------------------------------------------------------------------
// Command: list
// ---------------------------------------------------------------------------
static int cmd_list(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "error: list requires <archive.hazf>\n";
        std::cerr << "usage: hazf list <archive.hazf>\n";
        return 1;
    }
    std::string archivePath = args[0];
    std::ifstream in;
    uint64_t archiveSize = 0;
    ArchiveHeader ah;
    try {
        ah = open_and_read_header(archivePath, in, archiveSize);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "HAZF archive: " << archivePath << "\n";
    std::cout << "Version: " << ah.version << "\n";
    std::cout << "Entries: " << ah.entryCount << "\n";
    std::cout << "Archive size: " << archiveSize << " bytes\n";
    if (ah.entryCount == 0) {
        std::cout << "(empty archive)\n";
        return 0;
    }

    // For pretty alignment, but also machine-parseable
    std::cout << "------------------------------------------------------------\n";
    std::cout << " #  Path                                    Original  Compressed  CRC32\n";
    std::cout << "------------------------------------------------------------\n";

    uint64_t totalOrig = 0;
    uint64_t totalComp = 0;

    for (uint32_t i = 0; i < ah.entryCount; ++i) {
        // Read entry header
        std::array<uint8_t, kEntryHeaderSize> ehBuf{};
        try {
            read_exact(in, ehBuf.data(), ehBuf.size());
        } catch (const std::exception& e) {
            std::cerr << "error: truncated entry header #" << (i+1) << ": " << e.what() << "\n";
            return 1;
        }
        EntryHeader eh;
        try {
            eh = EntryHeader::deserialize(ehBuf.data());
        } catch (const std::exception& e) {
            std::cerr << "error: invalid entry header #" << (i+1) << ": " << e.what() << "\n";
            return 1;
        }

        if (eh.originalSize > kMaxFileSize) {
            std::cerr << "error: entry #" << (i+1) << " original size exceeds limit\n";
            return 1;
        }
        if (eh.pathLen == 0) {
            std::cerr << "error: entry #" << (i+1) << " has empty path\n";
            return 1;
        }

        std::string path;
        path.resize(eh.pathLen);
        if (eh.pathLen > 0) {
            try {
                read_exact(in, reinterpret_cast<uint8_t*>(path.data()), eh.pathLen);
            } catch (const std::exception& e) {
                std::cerr << "error: truncated path for entry #" << (i+1) << ": " << e.what() << "\n";
                return 1;
            }
        }

        // Validate path
        if (!is_valid_stored_path(path)) {
            std::cerr << "warning: entry #" << (i+1) << " has suspicious path: " << path << "\n";
        }

        // Skip payload: seek forward with truncation check
        if (eh.compressedSize > 0) {
            std::streampos curPos = in.tellg();
            if (curPos == std::streampos(-1)) {
                std::cerr << "error: cannot determine position for entry #" << (i+1) << "\n";
                return 1;
            }
            uint64_t cur = static_cast<uint64_t>(curPos);
            if (cur + eh.compressedSize > archiveSize) {
                std::cerr << "error: truncated payload for entry #" << (i+1) << " (" << path << "): need " << eh.compressedSize << " bytes but only " << (archiveSize - cur) << " remain\n";
                return 1;
            }
            in.seekg(static_cast<std::streamoff>(eh.compressedSize), std::ios::cur);
            if (!in) {
                std::cerr << "error: truncated payload for entry #" << (i+1) << " (" << path << ")\n";
                return 1;
            }
        }

        totalOrig += eh.originalSize;
        totalComp += eh.compressedSize;

        // Print entry line - contains path and sizes for grep-ability
        // Format: "  1. path/to/file.txt  1234 bytes (compressed 567 bytes, CRC32 0x12345678)"
        // Also align column but keep tab for machine parse?
        char crcBuf[16];
        std::snprintf(crcBuf, sizeof(crcBuf), "%08x", eh.crc32);
        std::cout << " " << (i+1) << ". " << path
                  << "  " << eh.originalSize << " bytes"
                  << " (compressed " << eh.compressedSize << " bytes, CRC32 0x" << crcBuf << ")\n";
        // Also emit simple tab line for easy parsing: path<TAB>size
        // To help automated checks, we could optionally but not needed.
    }

    // Check for trailing data?
    // If there is extra data beyond entries, warn.

    std::cout << "------------------------------------------------------------\n";
    std::cout << "Total uncompressed: " << totalOrig << " bytes\n";
    std::cout << "Total compressed:   " << totalComp << " bytes\n";
    if (totalOrig > 0) {
        double ratio = 100.0 * (1.0 - static_cast<double>(totalComp)/static_cast<double>(totalOrig));
        std::cout << "Compression ratio:  " << ratio << "% saved\n";
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Command: info
// ---------------------------------------------------------------------------
static int cmd_info(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "error: info requires <archive.hazf>\n";
        std::cerr << "usage: hazf info <archive.hazf>\n";
        return 1;
    }
    std::string archivePath = args[0];
    std::ifstream in;
    uint64_t archiveSize = 0;
    ArchiveHeader ah;
    try {
        ah = open_and_read_header(archivePath, in, archiveSize);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "HAZF Archive Information\n";
    std::cout << "------------------------\n";
    std::cout << "File: " << archivePath << "\n";
    std::cout << "Magic: HAZF\n";
    std::cout << "Version: " << ah.version << " (v1.0)\n";
    std::cout << "Entries: " << ah.entryCount << "\n";
    std::cout << "Archive size: " << archiveSize << " bytes\n";
    std::cout << "Header size: " << kArchiveHeaderSize << " bytes\n";
    std::cout << "Entry header size: " << kEntryHeaderSize << " bytes\n";
    std::cout << "Byte order: little-endian\n";
    std::cout << "Compression: Zstandard\n";
    std::cout << "Integrity: CRC32 (IEEE)\n";
    std::cout << "Limits: 8 GiB per file, 1,000,000 entries\n";

    if (ah.entryCount > 0) {
        uint64_t totalOrig = 0, totalComp = 0;
        // Iterate to compute totals without full decompression
        for (uint32_t i = 0; i < ah.entryCount; ++i) {
            std::array<uint8_t, kEntryHeaderSize> ehBuf{};
            try {
                read_exact(in, ehBuf.data(), ehBuf.size());
            } catch (const std::exception& e) {
                std::cerr << "error: truncated entry header #" << (i+1) << ": " << e.what() << "\n";
                return 1;
            }
            EntryHeader eh = EntryHeader::deserialize(ehBuf.data());
            // skip path with truncation check
            if (eh.pathLen > 0) {
                std::streampos curPos = in.tellg();
                if (curPos == std::streampos(-1)) { std::cerr << "error: cannot determine position\n"; return 1; }
                uint64_t cur = static_cast<uint64_t>(curPos);
                if (cur + eh.pathLen > archiveSize) { std::cerr << "error: truncated path\n"; return 1; }
                in.seekg(eh.pathLen, std::ios::cur);
                if (!in) { std::cerr << "error: truncated path\n"; return 1; }
            }
            // skip payload with truncation check
            if (eh.compressedSize > 0) {
                std::streampos curPos = in.tellg();
                if (curPos == std::streampos(-1)) { std::cerr << "error: cannot determine position\n"; return 1; }
                uint64_t cur = static_cast<uint64_t>(curPos);
                if (cur + eh.compressedSize > archiveSize) { std::cerr << "error: truncated payload\n"; return 1; }
                in.seekg(static_cast<std::streamoff>(eh.compressedSize), std::ios::cur);
                if (!in) { std::cerr << "error: truncated payload\n"; return 1; }
            }
            totalOrig += eh.originalSize;
            totalComp += eh.compressedSize;
        }
        std::cout << "Total uncompressed size: " << totalOrig << " bytes\n";
        std::cout << "Total compressed size:   " << totalComp << " bytes\n";
        if (totalOrig > 0) {
            double ratio = (static_cast<double>(totalComp) / static_cast<double>(totalOrig)) * 100.0;
            std::cout << "Overall compression: " << ratio << "% of original\n";
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Command: test
// ---------------------------------------------------------------------------
static int cmd_test(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "error: test requires <archive.hazf>\n";
        std::cerr << "usage: hazf test <archive.hazf>\n";
        return 1;
    }
    std::string archivePath = args[0];
    std::ifstream in;
    uint64_t archiveSize = 0;
    ArchiveHeader ah;
    try {
        ah = open_and_read_header(archivePath, in, archiveSize);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Testing archive: " << archivePath << " (" << ah.entryCount << " entries)\n";
    bool allOk = true;
    for (uint32_t i = 0; i < ah.entryCount; ++i) {
        std::array<uint8_t, kEntryHeaderSize> ehBuf{};
        try {
            read_exact(in, ehBuf.data(), ehBuf.size());
        } catch (const std::exception& e) {
            std::cerr << "FAIL: truncated entry header #" << (i+1) << ": " << e.what() << "\n";
            allOk = false;
            break;
        }
        EntryHeader eh = EntryHeader::deserialize(ehBuf.data());

        std::string path;
        path.resize(eh.pathLen);
        if (eh.pathLen > 0) {
            try {
                read_exact(in, reinterpret_cast<uint8_t*>(path.data()), eh.pathLen);
            } catch (const std::exception& e) {
                std::cerr << "FAIL: truncated path for entry #" << (i+1) << ": " << e.what() << "\n";
                allOk = false;
                break;
            }
        }

        std::vector<uint8_t> comp;
        comp.resize(static_cast<size_t>(eh.compressedSize));
        if (eh.compressedSize > 0) {
            try {
                read_exact(in, comp.data(), comp.size());
            } catch (const std::exception& e) {
                std::cerr << "FAIL: truncated payload for " << path << ": " << e.what() << "\n";
                allOk = false;
                break;
            }
        }

        // Validate sizes
        if (eh.originalSize > kMaxFileSize) {
            std::cerr << "FAIL: " << path << " original size exceeds limit\n";
            allOk = false;
            continue;
        }

        // Decompress
        std::vector<uint8_t> decompressed;
        decompressed.resize(static_cast<size_t>(eh.originalSize));
        size_t dSize = 0;
        if (eh.originalSize == 0) {
            // For empty file, we expect decompressed size 0. But we still have a zstd frame for empty.
            // Decompress if compressed payload exists.
            if (eh.compressedSize == 0) {
                dSize = 0;
            } else {
                // ZSTD_decompress with dst size 0 will fail if src is non-empty; but empty original should decompress to 0
                // Use ZSTD_getFrameContentSize to check
                unsigned long long fcs = ZSTD_getFrameContentSize(comp.data(), comp.size());
                if (fcs == 0 || fcs == ZSTD_CONTENTSIZE_UNKNOWN) {
                    // empty
                    dSize = 0;
                    // Optionally verify that decompressing to empty buffer succeeds via streaming?
                    // Try decompress with nullptr?
                    // We'll just treat as OK if frame content size is 0.
                    // But to verify CRC, decompress via ZSTD_decompress with small buffer?
                    // We'll do a trial decompress to a 1-byte buffer to check magic
                    if (comp.size() > 0) {
                        size_t res = ZSTD_decompress(nullptr, 0, comp.data(), comp.size());
                        if (ZSTD_isError(res)) {
                            // Some zstd versions require non-null dst for empty; try with 1 byte
                            uint8_t dummy = 0;
                            res = ZSTD_decompress(&dummy, 0, comp.data(), comp.size());
                            if (ZSTD_isError(res) && res != 0) {
                                std::cerr << "FAIL: " << path << " ZSTD decompress error: " << ZSTD_getErrorName(res) << "\n";
                                allOk = false;
                                continue;
                            }
                        }
                    }
                } else {
                    std::cerr << "FAIL: " << path << " mismatch: originalSize 0 but frame content size " << fcs << "\n";
                    allOk = false;
                    continue;
                }
            }
        } else {
            dSize = ZSTD_decompress(decompressed.data(), decompressed.size(), comp.data(), comp.size());
            if (ZSTD_isError(dSize)) {
                std::cerr << "FAIL: " << path << " ZSTD decompress error: " << ZSTD_getErrorName(dSize) << "\n";
                allOk = false;
                continue;
            }
            if (dSize != eh.originalSize) {
                std::cerr << "FAIL: " << path << " decompressed size mismatch: got " << dSize << " expected " << eh.originalSize << "\n";
                allOk = false;
                continue;
            }
        }

        // CRC check
        uint32_t calcCrc = 0;
        if (eh.originalSize > 0) {
            calcCrc = crc32(decompressed.data(), decompressed.size());
        } else {
            calcCrc = 0;
        }
        if (calcCrc != eh.crc32) {
            std::cerr << "FAIL: " << path << " CRC32 mismatch: stored 0x"
                      << std::hex << eh.crc32 << " calculated 0x" << calcCrc << std::dec << "\n";
            allOk = false;
            continue;
        }

        std::cout << "OK: " << path << " (" << eh.originalSize << " bytes, CRC32 0x"
                  << std::hex << eh.crc32 << std::dec << ")\n";
    }

    if (allOk) {
        std::cout << "All " << ah.entryCount << " entries verified successfully.\n";
        return 0;
    } else {
        std::cerr << "Archive test FAILED.\n";
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Command: extract
// ---------------------------------------------------------------------------
static int cmd_extract(const std::vector<std::string>& args) {
    if (args.size() < 1 || args.size() > 2) {
        std::cerr << "error: extract requires <archive.hazf> [output_dir]\n";
        std::cerr << "usage: hazf extract <archive.hazf> [output_dir]\n";
        return 1;
    }
    std::string archivePath = args[0];
    std::string outDirStr = (args.size() == 2) ? args[1] : ".";
    fs::path outDir(outDirStr);

    // Ensure outDir exists or create
    try {
        if (!fs::exists(outDir)) {
            fs::create_directories(outDir);
        } else if (!fs::is_directory(outDir)) {
            std::cerr << "error: output path is not a directory: " << outDirStr << "\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: cannot create output directory: " << e.what() << "\n";
        return 1;
    }

    std::ifstream in;
    uint64_t archiveSize = 0;
    ArchiveHeader ah;
    try {
        ah = open_and_read_header(archivePath, in, archiveSize);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Extracting archive: " << archivePath << " -> " << outDirStr << " (" << ah.entryCount << " entries)\n";
    bool hadError = false;
    bool hadOverwriteRefusal = false;

    // Resolve canonical outDir for traversal check (lexically)
    fs::path outDirCanonical;
    try {
        outDirCanonical = fs::weakly_canonical(outDir);
    } catch (...) {
        outDirCanonical = fs::absolute(outDir).lexically_normal();
    }

    for (uint32_t i = 0; i < ah.entryCount; ++i) {
        std::array<uint8_t, kEntryHeaderSize> ehBuf{};
        try {
            read_exact(in, ehBuf.data(), ehBuf.size());
        } catch (const std::exception& e) {
            std::cerr << "error: truncated entry header #" << (i+1) << ": " << e.what() << "\n";
            hadError = true;
            break;
        }
        EntryHeader eh = EntryHeader::deserialize(ehBuf.data());

        std::string path;
        path.resize(eh.pathLen);
        if (eh.pathLen > 0) {
            try {
                read_exact(in, reinterpret_cast<uint8_t*>(path.data()), eh.pathLen);
            } catch (const std::exception& e) {
                std::cerr << "error: truncated path for entry #" << (i+1) << ": " << e.what() << "\n";
                hadError = true;
                break;
            }
        }

        std::vector<uint8_t> comp;
        comp.resize(static_cast<size_t>(eh.compressedSize));
        if (eh.compressedSize > 0) {
            try {
                read_exact(in, comp.data(), comp.size());
            } catch (const std::exception& e) {
                std::cerr << "error: truncated payload for " << path << ": " << e.what() << "\n";
                hadError = true;
                break;
            }
        }

        // Validate stored path
        if (!is_valid_stored_path(path)) {
            std::cerr << "error: skipping invalid path entry #" << (i+1) << ": " << path << "\n";
            hadError = true;
            continue;
        }

        fs::path destPath = outDir / fs::path(path);
        // Security: ensure destPath is within outDir
        fs::path destCanonical;
        try {
            // Lexically normalize without requiring existence
            destCanonical = fs::absolute(destPath).lexically_normal();
            // Check prefix
            // Compare string prefix
            std::string outStr = outDirCanonical.generic_string();
            std::string destStr = destCanonical.generic_string();
            // Ensure outDir ends without slash for compare, dest is either equal or has '/' after prefix
            if (destStr.size() < outStr.size() || destStr.compare(0, outStr.size(), outStr) != 0 ||
                (destStr.size() > outStr.size() && destStr[outStr.size()] != '/')) {
                // Allow exact match if file is at outDir itself? Should not happen because path not empty.
                if (destStr != outStr) {
                    std::cerr << "error: path traversal detected, skipping: " << path << " -> " << destPath << "\n";
                    hadError = true;
                    continue;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "warning: cannot canonicalize path " << path << ": " << e.what() << "\n";
        }

        // Refuse to overwrite
        if (fs::exists(destPath)) {
            std::cerr << "error: refusing to overwrite existing file: " << destPath << "\n";
            hadOverwriteRefusal = true;
            hadError = true;
            continue;
        }

        // Parent dirs
        try {
            fs::create_directories(destPath.parent_path());
        } catch (const std::exception& e) {
            std::cerr << "error: cannot create parent directories for " << destPath << ": " << e.what() << "\n";
            hadError = true;
            continue;
        }

        // Decompress
        std::vector<uint8_t> decompressed;
        decompressed.resize(static_cast<size_t>(eh.originalSize));
        bool decompressOk = true;
        if (eh.originalSize == 0) {
            if (eh.compressedSize == 0) {
                // empty file, no payload
            } else {
                // Verify empty frame decompresses correctly (as in test)
                unsigned long long fcs = ZSTD_getFrameContentSize(comp.data(), comp.size());
                if (fcs != 0 && fcs != ZSTD_CONTENTSIZE_UNKNOWN) {
                    std::cerr << "error: " << path << " originalSize 0 but frame size " << fcs << "\n";
                    decompressOk = false;
                } else {
                    // try decompress to validate
                    size_t res = ZSTD_decompress(nullptr, 0, comp.data(), comp.size());
                    if (ZSTD_isError(res)) {
                        uint8_t dummy = 0;
                        res = ZSTD_decompress(&dummy, 0, comp.data(), comp.size());
                        if (ZSTD_isError(res)) {
                            std::cerr << "error: " << path << " decompress failed: " << ZSTD_getErrorName(res) << "\n";
                            decompressOk = false;
                        }
                    }
                }
            }
        } else {
            size_t dSize = ZSTD_decompress(decompressed.data(), decompressed.size(), comp.data(), comp.size());
            if (ZSTD_isError(dSize)) {
                std::cerr << "error: " << path << " decompress failed: " << ZSTD_getErrorName(dSize) << "\n";
                decompressOk = false;
            } else if (dSize != eh.originalSize) {
                std::cerr << "error: " << path << " size mismatch: decompressed " << dSize << " expected " << eh.originalSize << "\n";
                decompressOk = false;
            } else {
                uint32_t calcCrc = crc32(decompressed.data(), decompressed.size());
                if (calcCrc != eh.crc32) {
                    std::cerr << "error: " << path << " CRC mismatch: stored 0x" << std::hex << eh.crc32
                              << " calc 0x" << calcCrc << std::dec << "\n";
                    decompressOk = false;
                }
            }
        }

        if (!decompressOk) {
            hadError = true;
            continue;
        }

        // For non-empty, CRC already checked. For empty, CRC should be 0.
        if (eh.originalSize == 0 && eh.crc32 != 0) {
            std::cerr << "error: " << path << " CRC mismatch for empty file\n";
            hadError = true;
            continue;
        }

        // Write file
        std::ofstream out(destPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "error: cannot open for writing: " << destPath << "\n";
            hadError = true;
            continue;
        }
        if (!decompressed.empty()) {
            out.write(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
            if (!out) {
                std::cerr << "error: write failed for " << destPath << "\n";
                hadError = true;
                continue;
            }
        }
        out.close();
        std::cout << "  extracted: " << path << " -> " << destPath.string() << " (" << eh.originalSize << " bytes)\n";
    }

    if (hadError) {
        if (hadOverwriteRefusal) {
            std::cerr << "Extraction completed with errors (refused to overwrite existing files).\n";
        } else {
            std::cerr << "Extraction completed with errors.\n";
        }
        return 1;
    }
    std::cout << "Extraction completed successfully.\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    std::string cmd = argv[1];
    // Normalize help variants
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_usage(argv[0]);
        return 0;
    }

    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);

    try {
        if (cmd == "create") {
            return cmd_create(args);
        } else if (cmd == "list") {
            return cmd_list(args);
        } else if (cmd == "extract") {
            return cmd_extract(args);
        } else if (cmd == "test") {
            return cmd_test(args);
        } else if (cmd == "info") {
            return cmd_info(args);
        } else {
            std::cerr << "error: unknown command: " << cmd << "\n";
            print_usage(argv[0]);
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "fatal error: " << e.what() << "\n";
        return 1;
    }
}
