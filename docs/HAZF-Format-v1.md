# HAZF Format v1.0 Specification

**Hyggshi Archive Zip Format**

- Extension: `.hazf`
- Magic Bytes: `HAZF` (0x48 0x41 0x5A 0x46)
- Version: 1.0 (little-endian, Zstandard, CRC32)

HAZF is an independent binary archive format, not directly compatible with traditional ZIP. It is designed for the Hyggshi OS prototype to provide deterministic, simple and verifiable archiving for regular files.

## 1. Design Goals

- **Language:** C++20
- **Build System:** CMake
- **Compression:** Zstandard (zstd) at default level 3
- **Byte Order:** Little-endian for all multi-byte integers
- **Integrity:** CRC32 (IEEE 802.3, polynomial 0xEDB88320) of uncompressed data
- **Limits (prototype):** 8 GiB per file, 1,000,000 entries, 65,535 bytes per path
- **Operations:** `create`, `list`, `extract`, `test`, `info`
- **Security:** Extract refuses to overwrite existing files; path traversal (`..`, absolute) is rejected.

## 2. Overall Archive Structure

```
[ Archive Header : 12 bytes ]
[ Entry 1 Header : 24 bytes ][ Entry 1 Path : path_len bytes ][ Entry 1 Payload : compressed_size bytes ]
[ Entry 2 Header : 24 bytes ][ Entry 2 Path : ...            ][ Entry 2 Payload : ...                     ]
...
[ Entry N Header : 24 bytes ][ Entry N Path ][ Entry N Payload ]
```

All entries are concatenated without padding or global central directory. The archive header declares the entry count; each entry header self-describes its path and payload sizes.

## 3. Archive Header (12 bytes)

| Offset | Size | Field         | Type         | Description |
|--------|------|---------------|--------------|-------------|
| 0      | 4    | `magic`       | `char[4]`    | ASCII `HAZF` |
| 4      | 2    | `version`     | `uint16 LE`  | `1` for v1.0 |
| 6      | 2    | `reserved`    | `uint16 LE`  | `0x0000` (future use) |
| 8      | 4    | `entry_count` | `uint32 LE`  | `0 .. 1,000,000` |

Total = 12 bytes. A reader must verify magic and that `version == 1`. `reserved` must be zero on write and ignored on read (forward compatibility).

## 4. Entry Header (24 bytes)

| Offset | Size | Field             | Type         | Description |
|--------|------|-------------------|--------------|-------------|
| 0      | 2    | `path_len`        | `uint16 LE`  | Length in bytes of UTF-8 path (no NUL) |
| 2      | 2    | `reserved`        | `uint16 LE`  | `0x0000` (future flags) |
| 4      | 8    | `original_size`   | `uint64 LE`  | Uncompressed size (`0 .. 8 GiB`) |
| 12     | 8    | `compressed_size` | `uint64 LE`  | Zstd compressed size |
| 20     | 4    | `crc32`           | `uint32 LE`  | CRC32 of uncompressed data (0 for empty) |

Total = 24 bytes. Followed immediately by `path` and `payload`.

### 4.1 Path

- `path_len` bytes of UTF-8, stored without NUL terminator.
- Use POSIX forward slashes (`/`) as separator, regardless of host OS.
- Must be a relative path, without leading `/`, without `..` components, without backslashes, non-empty, `<= 65535` bytes.
- Normalized via `lexically_normal()` and stripped of leading `./` and `/` on create.
- Example: `assets/logo.png`, `mydir/docs/readme.txt`.
- Extraction validates the path and rejects traversal attempts.

### 4.2 Payload

- `compressed_size` bytes of Zstandard-compressed data.
- For empty files (`original_size == 0`), the payload is the Zstd frame of empty input (typically 9 bytes). Some implementations may store `0` bytes for empty files; readers should accept both. This reference implementation stores the frame.
- Decompression must yield exactly `original_size` bytes and CRC32 must match.

## 5. Integrity

- CRC32 is computed over the uncompressed byte stream using the IEEE polynomial `0xEDB88320`, initial `0xFFFFFFFF`, final XOR `0xFFFFFFFF`.
- Test vector: CRC32 of `123456789` (ASCII) is `0xCBF43926`.
- Empty file CRC32 is `0x00000000`.
- Integrity checks:
  - On `create`: compute CRC before compression and store.
  - On `test` / `extract`: decompress in memory (or streaming) and recompute; mismatch is failure.

## 6. Limits and Errors

| Limit | Value | Behavior |
|-------|-------|----------|
| Per-file size | `8 GiB` (`8589934592`) | `create` rejects with error |
| Entry count | `1,000,000` | `create` rejects; reader rejects if header exceeds |
| Path length | `65,535` bytes | `create` rejects; reader rejects `path_len==0` |
| Duplicate stored path | — | `create` rejects |
| Non-regular file (device, FIFO, socket, symlink) | — | Skipped with warning on create (only regular files archived) |
| Archive truncation | — | Reader returns `truncated` error |
| Version mismatch | — | Reader returns `unsupported version` |
| CRC mismatch | — | `test`/`extract` report `FAIL` |
| Overwrite | — | `extract` refuses to overwrite, reports error |

## 7. Little-Endian Encoding

All integer fields are little-endian. Helpers:

```cpp
write_u16_le(buf+0, pathLen);
write_u64_le(buf+4, originalSize);
...
uint32_t n = read_u32_le(buf+8);
```

Host-independent.

## 8. Compression

- Algorithm: Zstandard (https://github.com/facebook/zstd).
- Level: 3 (default).
- One-shot `ZSTD_compress` / `ZSTD_decompress` for prototype simplicity. Future versions may use streaming for > memory files.
- Compression bound via `ZSTD_compressBound`.
- Decompression validates `ZSTD_getFrameContentSize` when possible.

## 9. Filesystem Handling

### 9.1 Create

- Inputs may be files or directories.
- For a file `a/b/c.txt`, stored path is `a/b/c.txt` normalized (relative).
- For a directory `mydir`, recursively walk with `recursive_directory_iterator`; each regular file `mydir/sub/file.txt` stored as `mydir/sub/file.txt` (directory name preserved). If input is `.`, prefix is omitted.
- Paths are deduplicated; sorted lexicographically for determinism.
- Symlinks and non-regular files are skipped.

### 9.2 Extract

- Output directory defaults to `.` if not specified; created if missing.
- For each entry, `dest = output_dir / stored_path`.
- Parent directories are created.
- If `dest` exists, extraction of that entry is skipped and the tool exits non-zero (no overwrite).
- Canonicalization checks that `dest` remains within `output_dir`.

## 10. CLI

```
hazf create <archive.hazf> <file|dir>...
hazf list   <archive.hazf>
hazf extract <archive.hazf> [output_dir]
hazf test   <archive.hazf>
hazf info   <archive.hazf>
```

- `create`: creates archive, overwriting existing archive file if present (but not input files).
- `list`: prints human-readable table containing path, original size, compressed size, CRC32; exits non-zero on corruption.
- `extract`: decompresses to `output_dir`, refusing overwrites.
- `test`: decompresses in memory, verifies CRC32 and sizes, prints `OK` per entry.
- `info`: prints archive metadata (magic, version, counts, total sizes, ratios).

## 11. Example Hex Dump

Empty archive with 0 entries:

```
48 41 5A 46 01 00 00 00 00 00 00 00
|magic |ver |res | entry_count=0 |
```

Archive with 1 entry `a.txt` containing `hello` (5 bytes), CRC `0x3610A686`, Zstd compressed maybe `...`:

```
48 41 5A 46 01 00 00 00 01 00 00 00   # header, 1 entry
05 00 00 00 05 00 00 00 00 00 00 00   # entry: path_len=5, reserved, original=5
?? ?? ?? ?? ?? ?? ?? ?? 86 A6 10 36   # compressed_size (8), crc32 LE
61 2E 74 78 74                        # path "a.txt"
28 B5 2F FD ...                       # payload (zstd frame)
```

## 12. Future Extensions

- `reserved` fields may carry flags (e.g., encryption, storage mode).
- Streaming compression for > RAM files.
- Extended attributes, timestamps, permissions (currently not stored; prototype stores regular file content only).

## 13. Conformance

A conformant v1.0 reader must:

1. Verify magic.
2. Reject `version != 1` or `entry_count > 1_000_000`.
3. For each entry, read exactly `24 + path_len + compressed_size` bytes.
4. Validate `path_len != 0` and `is_valid_stored_path`.
5. Validate `original_size <= 8 GiB`.
6. Decompress and verify `decompressed_size == original_size` and `CRC32 == stored`.
7. Never write outside output directory; never overwrite existing files on extract.

## 14. Reference Implementation

See `include/hazf/format.hpp` for constants, LE helpers, CRC32, header structs, and `src/main.cpp` for CLI. Build with CMake and libzstd.

---
*HAZF v1.0 — 2026-10-11 — Hyggshi OS Research Technology*
