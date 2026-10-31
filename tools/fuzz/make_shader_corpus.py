#!/usr/bin/env python3
"""Turns stored or dumped shaders into a seed corpus for shader_dxbc_fuzzer.

Sources, any mix of:
- a title's shader storage, <cache>/shaders/shareable/<title_id>.xsh, written
  while the title runs (microcode stored in guest, big endian, byte order);
- a directory the dump_shaders cvar wrote shader_<hash>.ucode.bin.vert / .frag
  into (microcode in host, little endian, byte order).

Each shader becomes one fuzzer input: the 8-byte header of
tests/fuzz/shader_dxbc_fuzzer.cpp, which shader_spirv_fuzzer shares (pixel
or vertex shader, host render targets, conversion for Direct3D 11, register
count 63, all interpolators used, everything else default), then the
microcode as big-endian dwords.
Inputs are named after their contents, so the same shader from several titles
is written once.

Usage: make_shader_corpus.py <source>... <corpus_dir>
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


def header(is_pixel: bool) -> bytes:
    """The fuzzer input header, layout in tests/fuzz/shader_dxbc_fuzzer.cpp."""
    flags = (1 if is_pixel else 0) | 0x80  # Converted for Direct3D 11.
    return bytes([flags, 63, 0, 0xFF, 0xFF, 0, 0, 0])


def read_xsh(path: Path):
    """Yields (is_pixel, big-endian microcode bytes) for each stored shader."""
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
        offset += XSH_ENTRY_HEADER.size
        byte_count = (bits & 0x7FFFFFFF) * 4
        if offset + byte_count > len(data):
            break  # Truncated by a crash of the title; the runtime stops here too.
        yield bool(bits >> 31), data[offset : offset + byte_count]
        offset += byte_count


def read_dump_dir(path: Path):
    """Yields (is_pixel, big-endian microcode bytes) for each dumped shader."""
    for shader_path in sorted(path.glob("shader_*.ucode.bin.*")):
        if shader_path.suffix not in (".vert", ".frag"):
            continue
        data = shader_path.read_bytes()
        dword_count = len(data) // 4
        dwords = struct.unpack(f"<{dword_count}I", data[: dword_count * 4])
        yield shader_path.suffix == ".frag", struct.pack(f">{dword_count}I", *dwords)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sources", type=Path, nargs="+", metavar="source")
    parser.add_argument("corpus_dir", type=Path)
    args = parser.parse_args()

    # PowerShell passes wildcards through unexpanded.
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
            shaders = list(read_dump_dir(source) if source.is_dir() else read_xsh(source))
        except (OSError, ValueError) as error:
            print(f"{source}: skipped, {error}", file=sys.stderr)
            continue
        new = 0
        for is_pixel, ucode in shaders:
            out = header(is_pixel) + ucode
            out_path = args.corpus_dir / hashlib.sha1(out).hexdigest()
            if not out_path.exists():
                out_path.write_bytes(out)
                new += 1
        print(f"{source}: {len(shaders)} shaders, {new} new")
        written += new
    print(f"{written} inputs written to {args.corpus_dir}")
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
