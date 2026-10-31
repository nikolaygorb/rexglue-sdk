#!/usr/bin/env python3
"""Writes seed corpora for lzx_fuzzer and xex_fuzzer from real XEX files.

Targets:
  lzx  prefixes of the LZX stream of normally compressed XEX files, as
       XexModule::ReadImageCompressed passes it to lzx_decompress, in the
       input layout of lzx_fuzzer;
  xex  the real headers of those files with the encryption turned off and only
       the first compressed block, plus small synthetic images, one for each
       kind of compression that needs no LZX stream.

A file that is not a normally compressed XEX2 is skipped with a note. Block
hashes are checked, so a stream that is written is one the module would load.
Only the start of each file is decrypted (pure Python AES is slow). Inputs are
named after their contents. They hold game data: keep them out of the repository.

Usage: make_xex_corpus.py {lzx,xex} <corpus_dir> <file.xex>...
"""

import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path

XEX2_MAGIC = b"XEX2"
HEADER_FILE_FORMAT_INFO = 0x000003FF
HEADER_IMPORT_LIBRARIES = 0x000103FF
ENCRYPTION_NONE, ENCRYPTION_NORMAL = 0, 1
COMPRESSION_NONE, COMPRESSION_BASIC, COMPRESSION_NORMAL = 0, 1, 2
SECURITY_AES_KEY = 0x150
SECURITY_PAGE_DESCRIPTOR_COUNT = 0x180
BLOCK_INFO_SIZE = 24
MAX_PREFIX = 0x20000
# The prefixes of the stream to write and the fuzzer's output pages for each
# (1 to 256): about what it decompresses to, and half of that.
PREFIXES = ((0x1000, 4), (0x1000, 2), (0x4000, 16), (0x4000, 8), (0x10000, 64), (0x20000, 128))


# --- AES-128 CBC decryption, as aes_decrypt_buffer in xex_module.cpp -----------


def make_sboxes():
    sbox = [0] * 256
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4)
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    inverse = [0] * 256
    for i, value in enumerate(sbox):
        inverse[value] = i
    return sbox, inverse


SBOX, INV_SBOX = make_sboxes()


def gf_mul(a, b):
    result = 0
    while b:
        if b & 1:
            result ^= a
        a = ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1
        b >>= 1
    return result


MUL9, MUL11, MUL13, MUL14 = ([gf_mul(i, m) for i in range(256)] for m in (9, 11, 13, 14))


def expand_key(key):
    words = [list(key[i : i + 4]) for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        word = list(words[i - 1])
        if i % 4 == 0:
            word = [SBOX[b] for b in word[1:] + word[:1]]
            word[0] ^= rcon
            rcon = gf_mul(rcon, 2)
        words.append([a ^ b for a, b in zip(words[i - 4], word)])
    return [sum(words[i : i + 4], []) for i in range(0, 44, 4)]


def decrypt_block(block, round_keys):
    state = [a ^ b for a, b in zip(block, round_keys[10])]
    for round_index in range(9, -1, -1):
        # Inverse ShiftRows and SubBytes.
        state = [INV_SBOX[state[(i - 4 * (i % 4)) % 16]] for i in range(16)]
        state = [a ^ b for a, b in zip(state, round_keys[round_index])]
        if round_index:
            mixed = []
            for c in range(0, 16, 4):
                s0, s1, s2, s3 = state[c : c + 4]
                mixed += [
                    MUL14[s0] ^ MUL11[s1] ^ MUL13[s2] ^ MUL9[s3],
                    MUL9[s0] ^ MUL14[s1] ^ MUL11[s2] ^ MUL13[s3],
                    MUL13[s0] ^ MUL9[s1] ^ MUL14[s2] ^ MUL11[s3],
                    MUL11[s0] ^ MUL13[s1] ^ MUL9[s2] ^ MUL14[s3],
                ]
            state = mixed
    return state


def aes_cbc_decrypt(key, data):
    round_keys = expand_key(key)
    result = bytearray(len(data) & ~15)
    previous = bytes(16)
    for offset in range(0, len(result), 16):
        block = data[offset : offset + 16]
        plain = decrypt_block(block, round_keys)
        result[offset : offset + 16] = bytes(a ^ b for a, b in zip(plain, previous))
        previous = block
    return bytes(result)


def xex_keys():
    # The keys live in one place, the module that uses them.
    source = (Path(__file__).parents[2] / "src/system/xex_module.cpp").read_text()
    keys = []
    for name in ("xe_xex2_retail_key", "xe_xex2_devkit_key"):
        body = re.search(name + r"\[16\] = \{(.*?)\}", source, re.S).group(1)
        keys.append(bytes(int(value, 16) for value in re.findall(r"0x[0-9A-Fa-f]{2}", body)))
    return keys


# --- Reading a XEX ------------------------------------------------------------


class Xex:
    """A normally compressed XEX2 with its body decrypted as far as needed."""

    def __init__(self, path):
        with open(path, "rb") as file:
            self.head = file.read(0x400000)
        data = self.head
        if data[:4] != XEX2_MAGIC:
            raise ValueError("not XEX2")
        self.header_size, self.security_offset, header_count = struct.unpack_from(">I4xII", data, 8)
        self.format_offset = None
        for i in range(header_count):
            key, value = struct.unpack_from(">II", data, 0x18 + i * 8)
            if key == HEADER_FILE_FORMAT_INFO:
                self.format_offset = value
        if self.format_offset is None:
            raise ValueError("no file format info")
        self.encryption, self.compression = struct.unpack_from(">HH", data, self.format_offset + 4)
        if self.compression != COMPRESSION_NORMAL:
            raise ValueError(f"compression type {self.compression}")
        self.window_size, self.first_block_size = struct.unpack_from(
            ">II", data, self.format_offset + 8
        )
        self.first_block_hash = data[self.format_offset + 16 : self.format_offset + 36]
        self.body = self.decrypted_body()

    def decrypted_body(self):
        size = self.first_block_size
        raw = self.head[self.header_size : self.header_size + MAX_PREFIX * 2 + size]
        if self.encryption == ENCRYPTION_NONE:
            return raw
        aes_key = self.head[self.security_offset + SECURITY_AES_KEY :][:16]
        for key in xex_keys():
            session_key = aes_cbc_decrypt(key, aes_key)
            start = aes_cbc_decrypt(session_key, raw[: size + 16 & ~15])
            if hashlib.sha1(start[:size]).digest() == self.first_block_hash:
                return aes_cbc_decrypt(session_key, raw)
        raise ValueError("no key gives the first block's hash")

    def stream(self):
        """The LZX stream, as far as the decrypted body reaches."""
        result = bytearray()
        offset = 0
        block_size = self.first_block_size
        block_hash = self.first_block_hash
        while block_size and offset + block_size <= len(self.body) and len(result) < MAX_PREFIX:
            block = self.body[offset : offset + block_size]
            if hashlib.sha1(block).digest() != block_hash:
                raise ValueError(f"block at {offset:#x} has the wrong hash")
            next_size = struct.unpack_from(">I", block, 0)[0]
            next_hash = block[4:24]
            p = BLOCK_INFO_SIZE
            while True:
                chunk_size = struct.unpack_from(">H", block, p)[0]
                p += 2
                if not chunk_size:
                    break
                result += block[p : p + chunk_size]
                p += chunk_size
            offset += block_size
            block_size, block_hash = next_size, next_hash
        return bytes(result)

    def first_block_only(self):
        """The file, unencrypted, loading only its first block."""
        header = bytearray(self.head[: self.header_size])
        struct.pack_into(">H", header, self.format_offset + 4, ENCRYPTION_NONE)
        block = bytearray(self.body[: self.first_block_size])
        block[:BLOCK_INFO_SIZE] = bytes(BLOCK_INFO_SIZE)
        header[self.format_offset + 16 : self.format_offset + 36] = hashlib.sha1(block).digest()
        # Enough pages for what one block decompresses to, not the whole image.
        count_offset = self.security_offset + SECURITY_PAGE_DESCRIPTOR_COUNT
        count = struct.unpack_from(">I", header, count_offset)[0]
        struct.pack_into(">I", header, count_offset, min(count, 4))
        return bytes(header) + bytes(block)


# --- lzx_fuzzer ---------------------------------------------------------------


def lzx_inputs(xex):
    stream = xex.stream()
    window_byte = xex.window_size.bit_length() - 1 - 15
    for prefix, pages in PREFIXES:
        if len(stream) >= prefix:
            # The fuzzer's byte order; it adds one to the page count.
            yield bytes([window_byte]) + struct.pack("<H", pages - 1) + stream[:prefix]


# --- xex_fuzzer ---------------------------------------------------------------

LOAD_ADDRESS = 0x82000000


def synthetic_pe():
    """A PE the module takes: an image of 0x2000 bytes with two sections."""
    sections = 2
    optional_size = 0xE0
    dos = bytearray(0x40)
    dos[:2] = b"MZ"
    # is_valid_executable compares the first 4 bytes.
    dos[2:4] = b"\x90\x00"
    struct.pack_into("<I", dos, 0x3C, 0x40)
    file_header = struct.pack("<HHIIIHH", 0x01F2, sections, 0, 0, 0, optional_size, 0x0102)
    optional = bytearray(optional_size)
    struct.pack_into("<H", optional, 0, 0x010B)
    struct.pack_into("<I", optional, 28, LOAD_ADDRESS)
    struct.pack_into("<H", optional, 68, 14)  # IMAGE_SUBSYSTEM_XBOX
    table = b""
    for name, address, size in ((b".text", 0x1000, 0x800), (b".rdata", 0x1800, 0x800)):
        table += name.ljust(8, b"\0") + struct.pack(
            "<IIIIIIHHI", size, address, size, address, 0, 0, 0, 0, 0x60000020
        )
    return (bytes(dos) + b"PE\0\0" + file_header + bytes(optional) + table).ljust(0x2000, b"\0")


def synthetic_xex(file_format, body, flags=1):
    header_size = 0x1000
    security_offset = 0x200
    format_offset = 0x100
    header = bytearray(header_size)
    header[:4] = XEX2_MAGIC
    struct.pack_into(">IIIII", header, 4, flags, header_size, 0, security_offset, 1)
    struct.pack_into(">II", header, 0x18, HEADER_FILE_FORMAT_INFO, format_offset)
    header[format_offset : format_offset + len(file_format)] = file_format
    struct.pack_into(">II", header, security_offset, 0x200, 0x20000)
    struct.pack_into(">I", header, security_offset + 0x110, LOAD_ADDRESS)
    struct.pack_into(">I", header, security_offset + SECURITY_PAGE_DESCRIPTOR_COUNT, 2)
    # A page descriptor's first dword: page count above 4 bits of section type.
    # One page of code, one of data.
    for i, info in enumerate((1, 2)):
        struct.pack_into(">I", header, security_offset + 0x184 + i * 24, (1 << 4) | info)
    return bytes(header) + body


def synthetic_inputs():
    pe = synthetic_pe()
    none = struct.pack(">IHH", 8, ENCRYPTION_NONE, COMPRESSION_NONE)
    yield synthetic_xex(none, pe)
    basic = struct.pack(">IHHII", 16, ENCRYPTION_NONE, COMPRESSION_BASIC, 0x1000, 0x1000)
    yield synthetic_xex(basic, pe[:0x1000])
    two_blocks = struct.pack(">IHHIIII", 24, ENCRYPTION_NONE, COMPRESSION_BASIC, 0x800, 0x100, 0x400, 0)
    yield synthetic_xex(two_blocks, pe[:0xC00])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("target", choices=("lzx", "xex"))
    parser.add_argument("corpus_dir", type=Path)
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()

    args.corpus_dir.mkdir(parents=True, exist_ok=True)
    inputs = [] if args.target == "lzx" else list(synthetic_inputs())
    for path in args.files:
        try:
            xex = Xex(path)
            inputs += lzx_inputs(xex) if args.target == "lzx" else [xex.first_block_only()]
        except (ValueError, struct.error) as error:
            print(f"skipped {path}: {error}")
    written = 0
    for data in inputs:
        output = args.corpus_dir / hashlib.sha1(data).hexdigest()
        if not output.exists():
            output.write_bytes(data)
            written += 1
    print(f"{written} new inputs in {args.corpus_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
