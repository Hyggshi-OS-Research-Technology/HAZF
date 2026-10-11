# HAZF — Hyggshi Archive Zip Format

HAZF is an independent binary archive format (not ZIP-compatible) with extension `.hazf` and magic bytes `HAZF`. It is designed for Hyggshi OS Research Technology as a simple, deterministic, verifiable format for regular files.

- **Language:** C++20
- **Build System:** CMake
- **Compression:** Zstandard (zstd)
- **Byte Order:** Little-endian
- **Integrity:** CRC32 (IEEE 802.3)
- **Limits (prototype):** 8 GiB per file, 1,000,000 entries, 65,535 bytes per path
- **Operations:** `create`, `list`, `extract`, `test`, `info`

## Format v1.0

Archive layout:

```
[ Header 12 bytes ][ Entry Header 24 bytes | Path | Zstd Payload ] * N
```

- **Archive Header (12 B):** `magic` 4 (`HAZF`), `version` u16 LE (`1`), `reserved` u16, `entry_count` u32 LE.
- **Entry Header (24 B):** `path_len` u16 LE, `reserved` u16, `original_size` u64 LE, `compressed_size` u64 LE, `crc32` u32 LE.
- Followed by `path_len` bytes UTF-8 relative POSIX path and `compressed_size` bytes Zstd frame.

See [docs/HAZF-Format-v1.md](docs/HAZF-Format-v1.md) for full specification.

## Build

Prerequisites: `cmake`, `pkg-config`, `libzstd-dev`.

```bash
sudo apt update && sudo apt install -y cmake pkg-config libzstd-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hazf --help
```

Install:

```bash
sudo cmake --install build
# or
sudo make -C build install
```

## Usage

```bash
# Create archive from files and directories
hazf create archive.hazf file.txt mydir/ another.bin

# List files and original sizes
hazf list archive.hazf

# Show archive information
hazf info archive.hazf

# Test integrity (decompress in memory, verify CRC32)
hazf test archive.hazf

# Extract (refuses to overwrite existing files)
hazf extract archive.hazf [output_dir]
# default output_dir is current directory "."
```

All path arguments are stored as normalized relative POSIX paths (`/` separator, no leading `/`, no `..`). Regular files only are archived; symlinks, devices and directories without files are skipped. Extraction creates parent directories and will exit non-zero if any destination file already exists.

## Examples

```bash
echo "hello" > hello.txt
mkdir -p assets && echo "world" > assets/world.txt

hazf create demo.hazf hello.txt assets/
hazf list demo.hazf
# HAZF archive: demo.hazf
# Version: 1
# Entries: 2
#   1. hello.txt  6 bytes (compressed 14 bytes, CRC32 0x...)
#   2. assets/world.txt  6 bytes (compressed 14 bytes, CRC32 0x...)

hazf test demo.hazf
# OK: hello.txt (6 bytes, CRC32 0x...)
# OK: assets/world.txt (6 bytes, CRC32 0x...)
# All 2 entries verified successfully.

mkdir out
hazf extract demo.hazf out
ls -R out
```

## Repository Structure

```
HAZF/
├── .github/workflows/ci.yml   # CI: build + smoke tests
├── CMakeLists.txt
├── README.md
├── LICENSE
├── docs/HAZF-Format-v1.md     # Format specification
├── include/hazf/format.hpp    # Header constants, LE helpers, CRC32
├── src/main.cpp               # CLI implementation
├── tests/smoke.sh             # Integration tests
└── packaging/debian/
    ├── control
    ├── rules
    ├── changelog
    └── hazf.xml               # MIME type for .hazf
```

## Debian Packaging

```bash
# Build .deb with CPack
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && cpack -G DEB
```

Or with `debuild`:

```bash
debuild -us -uc
```

The MIME descriptor `packaging/debian/hazf.xml` registers `application/x-hazf` for `.hazf` via `shared-mime-info`:

```xml
<mime-type type="application/x-hazf">
  <glob pattern="*.hazf"/>
  <magic><match value="HAZF" offset="0"/></magic>
</mime-type>
```

## CI

GitHub Actions workflow (`.github/workflows/ci.yml`) builds on `ubuntu-latest`, installs `libzstd-dev`, configures with CMake, builds, runs `tests/smoke.sh`, and verifies packaging.

## License

See [LICENSE](LICENSE).
