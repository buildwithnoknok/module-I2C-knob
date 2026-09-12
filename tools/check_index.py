#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
check_index.py -- firmware/index.json must not lie about the binary (DEV-32 D7).

Run from the repo root (CI does; you can too: `python3 tools/check_index.py`).
Exits non-zero with a plain message on the first thing that is wrong.

Checks, per Ecosystem/software/firmware-index.md:
  * url     -> resolves to a repo-relative path under firmware/bin/ that exists
  * size    == byte length of that file
  * crc32   == zlib CRC32 of that file, lower-case 8-digit hex, no prefix
  * layout  matches firmware/src/app.ld FLASH ORIGIN:
              0x1400 -> 2, 0x1000 -> 1 (or 0, legacy), anything else (CH32V203
              apps at 0x2000) -> `layout` must be ABSENT until the V203 stage-0 port
  * version == FW_VERSION_MAJOR.MINOR.PATCH from the one source file in
              firmware/src/ that defines them

No dependencies beyond the Python 3 standard library.
"""
import json
import os
import re
import sys
import zlib

INDEX = "firmware/index.json"
APP_LD = "firmware/src/app.ld"
SRC_DIR = "firmware/src"
RAW_PREFIX = "https://raw.githubusercontent.com/buildwithnoknok/"


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def main():
    if not os.path.exists(INDEX):
        fail("%s not found (run from the repo root)" % INDEX)
    with open(INDEX, "r", encoding="utf-8") as f:
        idx = json.load(f)

    for key in ("module", "version", "url", "size", "crc32"):
        if key not in idx:
            fail("%s: required field '%s' is missing" % (INDEX, key))

    # --- url -> repo-relative path -------------------------------------------
    url = idx["url"]
    if not url.startswith(RAW_PREFIX):
        fail("url must start with %s (got %s)" % (RAW_PREFIX, url))
    rest = url[len(RAW_PREFIX):]              # <repo>/main/firmware/bin/x.bin
    parts = rest.split("/", 2)
    if len(parts) != 3 or parts[1] != "main":
        fail("url must be .../<repo>/main/<path> (got %s)" % url)
    url_repo, _, rel_path = parts
    gh_repo = os.environ.get("GITHUB_REPOSITORY", "")   # buildwithnoknok/<repo>
    if gh_repo and gh_repo.split("/")[-1] != url_repo:
        fail("url points at repo '%s' but this is '%s'" % (url_repo, gh_repo))
    if not rel_path.startswith("firmware/bin/"):
        fail("url path must be under firmware/bin/ (got %s)" % rel_path)
    if not os.path.exists(rel_path):
        fail("url names %s but that file is not in the repo" % rel_path)

    with open(rel_path, "rb") as f:
        data = f.read()

    # --- size ----------------------------------------------------------------
    if not isinstance(idx["size"], int) or idx["size"] != len(data):
        fail("size is %r in index but %s is %d bytes"
             % (idx["size"], rel_path, len(data)))

    # --- crc32 ---------------------------------------------------------------
    actual_crc = "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)
    crc = idx["crc32"]
    if not isinstance(crc, str) or not re.fullmatch(r"[0-9a-f]{8}", crc):
        fail("crc32 must be lower-case 8-digit hex with no prefix (got %r)" % (crc,))
    if crc != actual_crc:
        fail("crc32 is %s in index but %s hashes to %s" % (crc, rel_path, actual_crc))

    # --- layout vs app.ld ----------------------------------------------------
    if not os.path.exists(APP_LD):
        fail("%s not found" % APP_LD)
    with open(APP_LD, "r", encoding="utf-8") as f:
        ld = f.read()
    m = re.search(r"FLASH\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*0x([0-9A-Fa-f]+)", ld)
    if not m:
        fail("could not find 'FLASH (rx) : ORIGIN = 0x...' in %s" % APP_LD)
    origin = int(m.group(1), 16)
    expected_layouts = {0x1400: (2,), 0x1000: (1, 0)}.get(origin)
    if expected_layouts is None:
        # CH32V203 (0x2000) or anything unknown: no layout defined yet.
        if "layout" in idx:
            fail("layout is %r in index but app.ld ORIGIN 0x%X has no I2C layout; "
                 "remove the field" % (idx["layout"], origin))
    else:
        if "layout" not in idx:
            fail("layout is missing; app.ld ORIGIN 0x%X means layout %d"
                 % (origin, expected_layouts[0]))
        if idx["layout"] not in expected_layouts:
            fail("layout is %r in index but app.ld ORIGIN 0x%X means layout %d"
                 % (idx["layout"], origin, expected_layouts[0]))

    # --- version vs FW_VERSION_* defines -------------------------------------
    found = {}
    for name in sorted(os.listdir(SRC_DIR)):
        if not name.endswith((".c", ".h")):
            continue
        with open(os.path.join(SRC_DIR, name), "r", encoding="utf-8", errors="replace") as f:
            src = f.read()
        vals = {}
        for part in ("MAJOR", "MINOR", "PATCH"):
            mm = re.search(r"^\s*#define\s+FW_VERSION_%s\s+(\d+)\b" % part, src, re.M)
            if mm:
                vals[part] = int(mm.group(1))
        if len(vals) == 3:
            found[name] = "%d.%d.%d" % (vals["MAJOR"], vals["MINOR"], vals["PATCH"])
        elif vals:
            fail("%s/%s defines only %s of FW_VERSION_MAJOR/MINOR/PATCH"
                 % (SRC_DIR, name, ", ".join(sorted(vals))))
    if not found:
        fail("no file in %s defines FW_VERSION_MAJOR/MINOR/PATCH" % SRC_DIR)
    if len(set(found.values())) != 1:
        fail("conflicting FW_VERSION_* in %s: %s" % (SRC_DIR, found))
    src_file, src_version = next(iter(found.items()))
    if idx["version"] != src_version:
        fail("version is %s in index but %s/%s defines %s"
             % (idx["version"], SRC_DIR, src_file, src_version))

    print("OK: %s v%s  %s  %d bytes  crc32 %s  app.ld ORIGIN 0x%X  layout %s"
          % (idx["module"], idx["version"], rel_path, len(data), actual_crc,
             origin, idx.get("layout", "(absent)")))


if __name__ == "__main__":
    main()
