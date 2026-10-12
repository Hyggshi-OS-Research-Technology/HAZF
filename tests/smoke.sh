#!/usr/bin/env bash
set -euo pipefail

# HAZF smoke test — validates create, list, info, test, extract, overwrite refusal
# Usage: ./tests/smoke.sh or with HAZF_BIN env var pointing to binary

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Locate hazf binary
if [[ -n "${HAZF_BIN:-}" && -x "${HAZF_BIN}" ]]; then
    HAZF="${HAZF_BIN}"
elif [[ -x "${REPO_ROOT}/build/hazf" ]]; then
    HAZF="${REPO_ROOT}/build/hazf"
elif [[ -x "${REPO_ROOT}/build/Release/hazf" ]]; then
    HAZF="${REPO_ROOT}/build/Release/hazf"
elif command -v hazf >/dev/null 2>&1; then
    HAZF="hazf"
else
    # Try building
    echo "[smoke] hazf binary not found, attempting build..."
    cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${REPO_ROOT}/build" -j
    HAZF="${REPO_ROOT}/build/hazf"
fi

if [[ ! -x "${HAZF}" ]]; then
    echo "[smoke] FAIL: hazf binary not found at ${HAZF}"
    exit 1
fi

echo "[smoke] Using hazf: ${HAZF}"
"${HAZF}" --help >/dev/null || "${HAZF}" help >/dev/null || true

TMP="$(mktemp -d)"
echo "[smoke] TMP: ${TMP}"
cleanup() {
    rm -rf "${TMP}"
}
trap cleanup EXIT

# Prepare test files
mkdir -p "${TMP}/input/subdir"
echo "hello hazf" > "${TMP}/input/file1.txt"
echo "second file content" > "${TMP}/input/subdir/file2.txt"
printf "" > "${TMP}/input/empty.txt"
# binary content
head -c 1024 /dev/urandom > "${TMP}/input/subdir/random.bin" || dd if=/dev/urandom of="${TMP}/input/subdir/random.bin" bs=1024 count=1 2>/dev/null

ARCHIVE="${TMP}/test.hazf"
OUTDIR="${TMP}/out"

echo "[smoke] Test: create"
"${HAZF}" create "${ARCHIVE}" "${TMP}/input/file1.txt" "${TMP}/input/subdir" "${TMP}/input/empty.txt"
if [[ ! -f "${ARCHIVE}" ]]; then
    echo "[smoke] FAIL: archive not created"
    exit 1
fi
# Check magic
MAGIC="$(head -c 4 "${ARCHIVE}" || true)"
if [[ "${MAGIC}" != "HAZF" ]]; then
    echo "[smoke] FAIL: magic mismatch: got '${MAGIC}'"
    hexdump -C "${ARCHIVE}" | head -n 5 || od -An -tx1 "${ARCHIVE}" | head -n 5
    exit 1
fi
echo "[smoke] create OK, magic verified"

echo "[smoke] Test: list"
LIST_OUT="$("${HAZF}" list "${ARCHIVE}")"
echo "${LIST_OUT}"
# Should contain filenames and sizes
if ! echo "${LIST_OUT}" | grep -q "file1.txt"; then
    echo "[smoke] FAIL: list missing file1.txt"
    exit 1
fi
if ! echo "${LIST_OUT}" | grep -q "file2.txt"; then
    echo "[smoke] FAIL: list missing file2.txt"
    exit 1
fi
if ! echo "${LIST_OUT}" | grep -q "empty.txt"; then
    echo "[smoke] FAIL: list missing empty.txt"
    exit 1
fi
if ! echo "${LIST_OUT}" | grep -q "bytes"; then
    echo "[smoke] FAIL: list missing size info"
    exit 1
fi
echo "[smoke] list OK"

echo "[smoke] Test: info"
INFO_OUT="$("${HAZF}" info "${ARCHIVE}")"
echo "${INFO_OUT}"
if ! echo "${INFO_OUT}" | grep -q "HAZF"; then
    echo "[smoke] FAIL: info missing HAZF"
    exit 1
fi
if ! echo "${INFO_OUT}" | grep -q "Entries"; then
    echo "[smoke] FAIL: info missing Entries"
    exit 1
fi
echo "[smoke] info OK"

echo "[smoke] Test: test (integrity)"
"${HAZF}" test "${ARCHIVE}"
echo "[smoke] test OK"

echo "[smoke] Test: extract"
mkdir -p "${OUTDIR}"
"${HAZF}" extract "${ARCHIVE}" "${OUTDIR}"
# Find extracted files (stored paths preserve input structure)
# For file1.txt input was "${TMP}/input/file1.txt" - stored as "tmp/.../input/file1.txt" or just file? Our implementation preserves full input normalized.
# So we search recursively for file1.txt
if ! find "${OUTDIR}" -name "file1.txt" -type f | grep -q .; then
    echo "[smoke] FAIL: extracted file1.txt not found"
    find "${OUTDIR}" -type f | head -n 20
    exit 1
fi
EXTRACTED_FILE1="$(find "${OUTDIR}" -name "file1.txt" -type f | head -n1)"
if ! diff -q "${TMP}/input/file1.txt" "${EXTRACTED_FILE1}"; then
    echo "[smoke] FAIL: file1.txt content mismatch"
    diff "${TMP}/input/file1.txt" "${EXTRACTED_FILE1}" || true
    exit 1
fi

if ! find "${OUTDIR}" -name "file2.txt" -type f | grep -q .; then
    echo "[smoke] FAIL: extracted file2.txt not found"
    exit 1
fi
EXTRACTED_FILE2="$(find "${OUTDIR}" -name "file2.txt" -type f | head -n1)"
if ! diff -q "${TMP}/input/subdir/file2.txt" "${EXTRACTED_FILE2}"; then
    echo "[smoke] FAIL: file2.txt content mismatch"
    exit 1
fi

# Check empty file exists and is empty
if ! find "${OUTDIR}" -name "empty.txt" -type f | grep -q .; then
    echo "[smoke] FAIL: empty.txt not found"
    exit 1
fi
EXTRACTED_EMPTY="$(find "${OUTDIR}" -name "empty.txt" -type f | head -n1)"
if [[ -s "${EXTRACTED_EMPTY}" ]]; then
    echo "[smoke] FAIL: empty.txt should be empty"
    exit 1
fi

# Check random binary
if ! find "${OUTDIR}" -name "random.bin" -type f | grep -q .; then
    echo "[smoke] FAIL: random.bin not found"
    exit 1
fi
EXTRACTED_RAND="$(find "${OUTDIR}" -name "random.bin" -type f | head -n1)"
if ! cmp -s "${TMP}/input/subdir/random.bin" "${EXTRACTED_RAND}"; then
    echo "[smoke] FAIL: random.bin mismatch"
    exit 1
fi

echo "[smoke] extract OK"

echo "[smoke] Test: refuse overwrite"
set +e
"${HAZF}" extract "${ARCHIVE}" "${OUTDIR}" 2>"${TMP}/overwrite.log"
RET=$?
set -e
if [[ ${RET} -eq 0 ]]; then
    echo "[smoke] FAIL: extract should have refused to overwrite (expected non-zero)"
    cat "${TMP}/overwrite.log" || true
    exit 1
fi
if ! grep -qi "refusing to overwrite\|refuse" "${TMP}/overwrite.log"; then
    echo "[smoke] FAIL: overwrite refusal message not found"
    cat "${TMP}/overwrite.log" || true
    exit 1
fi
echo "[smoke] overwrite refusal OK"

echo "[smoke] Test: directory create (input dir)"
mkdir -p "${TMP}/dirtest/a/b"
echo "dir file" > "${TMP}/dirtest/a/b/nested.txt"
ARCHIVE2="${TMP}/dirtest.hazf"
"${HAZF}" create "${ARCHIVE2}" "${TMP}/dirtest"
"${HAZF}" test "${ARCHIVE2}"
mkdir -p "${TMP}/out2"
"${HAZF}" extract "${ARCHIVE2}" "${TMP}/out2"
if ! find "${TMP}/out2" -name "nested.txt" | grep -q .; then
    echo "[smoke] FAIL: nested.txt not found after dir create"
    find "${TMP}/out2" -type f | head -n 20
    exit 1
fi
echo "[smoke] dir create/extract OK"

echo "[smoke] Test: info and test on empty archive edge (if supported)"
# Create archive with single file then info again
"${HAZF}" info "${ARCHIVE2}" >/dev/null
"${HAZF}" list "${ARCHIVE2}" >/dev/null

echo "[smoke] All smoke tests passed."
