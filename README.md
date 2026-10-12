# HAZF — Hyggshi Archive Zip Format

HAZF is an independent binary archive format (not ZIP-compatible) with extension `.hazf` and magic bytes `HAZF`. It is designed for Hyggshi OS Research Technology as a simple, deterministic, verifiable format for regular files.

- **Language:** C++20
- **Build System:** CMake
- **Compression:** Zstandard (zstd)
- **Byte Order:** Little-endian
- **Integrity:** CRC32 (IEEE 802.3)
- **Limits (prototype):** 8 GiB per file, 1,000,000 entries, 65,535 bytes per path
- **Operations:** `create`, `list`, `extract`, `test`, `info`
- **GUI:** GTK4 (latest) with polished icons, animations and Adwaita-inspired theme

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

Prerequisites (CLI): `cmake`, `pkg-config`, `libzstd-dev`.

Prerequisites (GUI): additionally `libgtk-4-dev` (≥4.10) and optionally `libadwaita-1-dev` for extra polish.

```bash
# CLI only
sudo apt update && sudo apt install -y cmake pkg-config libzstd-dev

# With GUI (GTK4 latest)
sudo apt update && sudo apt install -y cmake pkg-config libzstd-dev libgtk-4-dev libadwaita-1-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hazf --help
./build/hazf-gui --help  # if GTK4 found
```

Install:

```bash
sudo cmake --install build
# or
sudo make -C build install
```

CMake options:
- `-DHAZF_BUILD_GUI=ON/OFF` — enable/disable GTK GUI (default ON, auto-skipped if GTK4 not found)
- `-DHAZF_BUILD_TESTS=ON/OFF`

## CLI Usage

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

## GUI Usage (GTK4)

Launch the polished graphical front-end:

```bash
hazf-gui                    # open empty window
hazf-gui archive.hazf       # open with archive (via Open dialog)
# or double-click a .hazf file in file manager (MIME registered)
```

**Features:**
- **Create:** Drag & drop files/folders onto dashed area (animated hover, scale & glow), browse, clear, archive path chooser, live progress, success toast. Icons: `folder-open-symbolic`, `package-x-generic-symbolic`, per-file type icons (text, image, archive) with hover transitions.
- **Browse:** Open `.hazf`, searchable ColumnView/ListBox with file icon, path, original/compressed size, CRC32 badge, animated rows.
- **Extract:** Archive + destination choosers, extract with integrity check, refuses overwrite (error dialog), progress bar with gradient and pulse.
- **Verify:** Info display (magic, version, counts, totals) + Test button that decompresses in memory, shows per-file OK/FAIL pills with CRC.

**Polished Icons & Effects:**
- App icon: custom SVG with gradient (`#3584e4→#1c71d8`), stacked papers, folder tab, ZSTD badge, CRC check, drop shadow, shine — properly finished, not placeholder.
- System symbolic icons from Adwaita (latest GTK4 icon theme): `folder-symbolic`, `text-x-generic-symbolic`, `image-x-generic-symbolic`, `package-x-generic-symbolic`, `view-list-symbolic`, `system-run-symbolic`, `emblem-ok-symbolic`, `dialog-error-symbolic`, etc., rendered at correct pixel sizes with CSS hover scaling.
- CSS animations: `cubic-bezier(0.25,0.46,0.45,0.94)` for stack transitions (220 ms slide), drop-area scale, button translateY, progress shimmer, row hover shadow, scrollbar pill, header bar blur.

**Web Preview (for development without GTK):**
```bash
python3 gui/web/server.py --port 8000  # serves polished web replica at http://localhost:8000
# then open http://localhost:8000 - identical UI in browser, calls hazf CLI via /api/*
```

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
├── .github/workflows/ci.yml          # CI: build + smoke + GUI
├── CMakeLists.txt                    # CLI + optional GTK GUI
├── README.md
├── LICENSE
├── docs/HAZF-Format-v1.md
├── include/hazf/format.hpp
├── src/main.cpp                      # CLI
├── gui/
│   ├── src/gui_main.cpp              # GTK4 GUI (latest, polished)
│   ├── resources/style.css           # Adwaita-inspired polished CSS
│   ├── data/com.hyggshi.hazf.desktop
│   └── data/icons/hicolor/scalable/apps/com.hyggshi.hazf.svg  # polished app icon
│   └── web/                          # Web preview (browser replica)
│       ├── index.html
│       └── server.py
├── tests/smoke.sh
└── packaging/debian/
    ├── control
    ├── rules
    ├── changelog
    └── hazf.xml
```

## Debian Packaging

```bash
# Build .deb with CPack (includes hazf + hazf-gui + icons + MIME)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && cpack -G DEB
```

Or with `debuild`:

```bash
debuild -us -uc
```

The MIME descriptor `packaging/debian/hazf.xml` registers `application/x-hazf` for `.hazf` via `shared-mime-info`. The `.desktop` file registers the GUI for file manager double-click.

## CI

GitHub Actions workflow (`.github/workflows/ci.yml`) builds on `ubuntu-latest`, installs `libzstd-dev` + `libgtk-4-dev` + `libadwaita-1-dev`, configures with CMake (`-DHAZF_BUILD_GUI=ON`), builds `hazf` and `hazf-gui`, runs `tests/smoke.sh`, and verifies packaging.

## License

See [LICENSE](LICENSE).
