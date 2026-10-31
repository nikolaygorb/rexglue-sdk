#!/usr/bin/env python3
"""Turns the titles' shader storage into a seed corpus for shader_storage_fuzzer.

Sources: shader storage files, <cache>/shaders/shareable/<title_id>.xsh,
written while a title runs. Wildcards are expanded here (PowerShell passes
them through).

Each input holds a run of up to --shaders consecutive entries of one file (so
inputs stay small and fast): the input header of
tests/fuzz/shader_storage_fuzzer.cpp (right hashes in the shader entries, a
valid .xsh file header, the length of the .xsh part), then the entries as
stored, and an empty .xpso part. The fuzzer adds the file header itself, and
its pipeline descriptions are its own, so the titles' .xpso files are not
used. Inputs are named after their contents.

Usage: make_storage_corpus.py [--shaders N] <xsh>... <corpus_dir>
"""

import argparse
import glob
import hashlib
import struct
import sys
from pathlib import Path

# include/rex/graphics/pipeline/shader/storage.h
XSH_MAGIC = 0x48534558  # 'XESH'
XSH_VERSION = 0x20201219
XSH_ENTRY_HEADER = struct.Struct("<QI")  # ucode_data_hash; dword count:31, type:1

# tests/fuzz/shader_storage_fuzzer.cpp: stamp shader hashes, valid .xsh header.
FLAGS = (1 << 0) | (1 << 2)


def read_entries(path: Path):
    """Yields each whole stored entry (header and microcode) as bytes."""
    data = path.read_bytes()
    if len(data) < 8:
        raise ValueError("too short")
    (magic,) = struct.unpack_from("<I", data, 0)
    (version,) = struct.unpack_from(">I", data, 4)  # version_swapped
    if magic != XSH_MAGIC or version != XSH_VERSION:
        raise ValueError(f"not a version {XSH_VERSION:08X} shader storage file")
    offset = 8
    while offset + XSH_ENTRY_HEADER.size <= len(data):
        _, bits = XSH_ENTRY_HEADER.unpack_from(data, offset)
        end = offset + XSH_ENTRY_HEADER.size + (bits & 0x7FFFFFFF) * 4
        if end > len(data):
            break  # Cut short by a crash of the title; the runtime stops here too.
        yield data[offset:end]
        offset = end


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--shaders", type=int, default=8, help="entries per input (default 8)")
    parser.add_argument("sources", type=Path, nargs="+", metavar="xsh")
    parser.add_argument("corpus_dir", type=Path)
    args = parser.parse_args()

    sources = []
    for source in args.sources:
        if glob.has_magic(str(source)):
            matches = sorted(Path(match) for match in glob.glob(str(source)))
            if not matches:
                print(f"{source}: matches nothing", file=sys.stderr)
            sources += matches
        else:
            sources.append(source)

    args.corpus_dir.mkdir(parents=True, exist_ok=True)
    written = 0
    for source in sources:
        try:
            entries = list(read_entries(source))
        except (OSError, ValueError) as error:
            print(f"{source}: skipped, {error}", file=sys.stderr)
            continue
        new = 0
        for first in range(0, len(entries), args.shaders):
            xsh = b"".join(entries[first : first + args.shaders])
            out = struct.pack("<BI", FLAGS, len(xsh)) + xsh
            out_path = args.corpus_dir / hashlib.sha1(out).hexdigest()
            if not out_path.exists():
                out_path.write_bytes(out)
                new += 1
        print(f"{source}: {len(entries)} shaders, {new} new inputs")
        written += new
    print(f"{written} inputs written to {args.corpus_dir}")
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
