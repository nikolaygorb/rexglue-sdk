#!/usr/bin/env python3
"""Writes seed corpora of well-formed inputs for the fuzzers that have no title data to start from.

Targets (the input layouts are in the fuzzers' sources, tests/fuzz/):
  primitive_processor  draws of every primitive type, auto-indexed and with
                       16-bit and 32-bit index buffers, with and without
                       primitive reset, for the Direct3D 11, Direct3D 12 and a
                       minimal host;
  xma                  contexts with XMA2 packets that hold chains of frames
                       (mono and stereo, a frame split across packets, two
                       input buffers, loops);
  xdbf                 XDBF resources with a title in two languages,
                       achievements and icons;
  stfs                 STFS packages (read-only and not, either root hash
                       table) and SVOD in the single file, enhanced GDF and
                       XSF layouts, each with a directory and files;
  disc_image           GDFX images at each game partition offset the fuzzer
                       allows, with a directory and files.
The frames' bits are random: the fuzzer gets the packet and frame structure
right, FFmpeg rejects most of the audio. Inputs are named after their contents.

Usage: make_synthetic_corpus.py {disc_image,primitive_processor,stfs,xdbf,xma} <corpus_dir>
"""

import argparse
import hashlib
import random
import struct
import sys
from pathlib import Path


def write_inputs(inputs, corpus_dir: Path) -> int:
    corpus_dir.mkdir(parents=True, exist_ok=True)
    written = 0
    for data in inputs:
        path = corpus_dir / hashlib.sha1(data).hexdigest()
        if not path.exists():
            path.write_bytes(data)
            written += 1
    return written


# --- primitive_processor_fuzzer ------------------------------------------------

# xenos::PrimitiveType
POINT_LIST, LINE_LIST, LINE_STRIP, TRIANGLE_LIST, TRIANGLE_FAN, TRIANGLE_STRIP = 1, 2, 3, 4, 5, 6
RECTANGLE_LIST, LINE_LOOP, QUAD_LIST = 8, 0x0C, 0x0D
# xenos::SourceSelect, xenos::Endian
DMA, AUTO_INDEX = 0, 2
ENDIAN_NONE, ENDIAN_8IN16, ENDIAN_8IN32, ENDIAN_16IN32 = 0, 1, 2, 3

# The defaults the fuzzer XORs with: an 8-index 16-bit triangle strip with
# primitive reset at 0xFFFF.
PRIMITIVE_DEFAULTS = (
    TRIANGLE_STRIP | (DMA << 6) | (8 << 16),  # VGT_DRAW_INITIATOR
    8 | (ENDIAN_8IN16 << 30),  # VGT_DMA_SIZE
    1 << 21,  # PA_SU_SC_MODE_CNTL (multi_prim_ib_ena)
    0xFFFF,  # VGT_MULTI_PRIM_IB_RESET_INDX
    0,  # VGT_OUTPUT_PATH_CNTL
    0,  # VGT_HOS_CNTL
)

# Direct3D 11, Direct3D 12, a host without fans, loops, quads and expansion.
PRIMITIVE_HOSTS = (0x79, 0x39, 0x00)


def gpu_swap(value: int, endian: int, bits: int) -> int:
    if endian == ENDIAN_8IN16:
        value = ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF)
    elif endian == ENDIAN_8IN32:
        value = int.from_bytes(value.to_bytes(4, "little"), "big")
    elif endian == ENDIAN_16IN32:
        value = ((value >> 16) & 0xFFFF) | ((value << 16) & 0xFFFFFFFF)
    return value & ((1 << bits) - 1)


def primitive_input(host, prim_type, source, is_32bit, endian, reset, reset_index, indices):
    count = len(indices)
    registers = (
        prim_type | (source << 6) | (int(is_32bit) << 11) | (count << 16),
        count | (endian << 30),
        int(reset) << 21,
        reset_index,
        0,
        0,
    )
    data = bytearray([host])
    for value, default in zip(registers, PRIMITIVE_DEFAULTS):
        data += struct.pack("<I", value ^ default)
    if source == DMA:
        bits = 32 if is_32bit else 16
        for index in indices:
            # The guest index is GpuSwap of what is in memory, read little endian.
            raw = gpu_swap(index, endian, bits)
            data += raw.to_bytes(bits // 8, "little")
    return bytes(data)


def primitive_inputs():
    types = (POINT_LIST, LINE_LIST, LINE_STRIP, TRIANGLE_LIST, TRIANGLE_FAN, TRIANGLE_STRIP,
             RECTANGLE_LIST, LINE_LOOP, QUAD_LIST)
    for host in PRIMITIVE_HOSTS:
        for prim_type in types:
            # Auto-indexed.
            yield primitive_input(host, prim_type, AUTO_INDEX, False, ENDIAN_NONE, False, 0,
                                  list(range(12)))
            # 16-bit, swapped, reset at 0xFFFF in the middle.
            indices = list(range(5)) + [0xFFFF] + list(range(5, 12))
            yield primitive_input(host, prim_type, DMA, False, ENDIAN_8IN16, True, 0xFFFF, indices)
            # 16-bit with another reset index, 0xFFFF as a vertex.
            indices = [0, 1, 2, 0x1234, 3, 4, 0xFFFF, 5, 6]
            yield primitive_input(host, prim_type, DMA, False, ENDIAN_8IN16, True, 0x1234, indices)
            # 32-bit, swapped, reset at 0xFFFFFF.
            indices = list(range(6)) + [0xFFFFFFFF] + list(range(6, 13))
            yield primitive_input(host, prim_type, DMA, True, ENDIAN_8IN32, True, 0xFFFFFF, indices)
            # 32-bit, no reset, upper bits set (ignored by the GPU).
            indices = [i | 0x7F000000 for i in range(13)]
            yield primitive_input(host, prim_type, DMA, True, ENDIAN_16IN32, False, 0, indices)


# --- xma_fuzzer -----------------------------------------------------------------

BYTES_PER_PACKET = 2048
BITS_PER_PACKET = BYTES_PER_PACKET * 8
BITS_PER_PACKET_HEADER = 32


def xma_context_dwords(**fields):
    """The XMA_CONTEXT_DATA dwords (include/rex/audio/xma/context.h bitfields)."""
    f = {
        "input_buffer_0_packet_count": 0, "loop_count": 0, "input_buffer_0_valid": 0,
        "input_buffer_1_valid": 0, "output_buffer_block_count": 0,
        "input_buffer_1_packet_count": 0, "loop_subframe_end": 0, "loop_subframe_skip": 0,
        "subframe_decode_count": 0, "sample_rate": 0, "is_stereo": 0, "output_buffer_valid": 0,
        "input_buffer_read_offset": 0, "loop_start": 0, "loop_end": 0, "current_buffer": 0,
    }
    f.update(fields)
    dwords = [0] * 16
    dwords[0] = (f["input_buffer_0_packet_count"] | (f["loop_count"] << 12) |
                 (f["input_buffer_0_valid"] << 20) | (f["input_buffer_1_valid"] << 21) |
                 (f["output_buffer_block_count"] << 22))
    dwords[1] = (f["input_buffer_1_packet_count"] | (f["loop_subframe_end"] << 12) |
                 (f["loop_subframe_skip"] << 17) | (f["subframe_decode_count"] << 20) |
                 (f["sample_rate"] << 27) | (f["is_stereo"] << 29) |
                 (f["output_buffer_valid"] << 31))
    dwords[2] = f["input_buffer_read_offset"]
    dwords[3] = f["loop_start"]
    dwords[4] = f["loop_end"] | (f["current_buffer"] << 31)
    return dwords


# The default the fuzzer XORs with: 2 packets of input buffer 0 into 8 output
# blocks at 48 kHz.
XMA_DEFAULT = xma_context_dwords(input_buffer_0_packet_count=2, input_buffer_0_valid=1,
                                 output_buffer_block_count=8, sample_rate=3,
                                 output_buffer_valid=1, input_buffer_read_offset=32)


class BitWriter:
    def __init__(self, size_bits):
        self.bits = bytearray(size_bits // 8)

    def write(self, offset, num_bits, value):
        for i in range(num_bits):
            if (value >> (num_bits - 1 - i)) & 1:
                bit = offset + i
                self.bits[bit >> 3] |= 0x80 >> (bit & 7)


def xma_stream(rng, frame_sizes, packet_count):
    """Packets holding the frames back to back after the first packet header.

    Each frame: a 15-bit size, random bits, and a last bit that is 1 when
    another frame follows in the packet. Frames that cross a packet boundary
    continue after the next packet's header.
    """
    # Lay the frames out in the payload bits (packet headers excluded).
    payload_bits = packet_count * (BITS_PER_PACKET - BITS_PER_PACKET_HEADER)
    payload = BitWriter(payload_bits + 64)
    starts = []
    offset = 0
    for size in frame_sizes:
        if offset + size > payload_bits:
            break
        starts.append(offset)
        payload.write(offset, 15, size)
        for bit in range(offset + 15, offset + size - 1, 8):
            payload.write(bit, min(8, offset + size - 1 - bit), rng.getrandbits(8))
        offset += size
    # The "another frame follows" bit of each frame but the last.
    for i, start in enumerate(starts):
        size = frame_sizes[i]
        payload.write(start + size - 1, 1, int(i + 1 < len(starts)))
    data = bytearray()
    per_packet = BITS_PER_PACKET - BITS_PER_PACKET_HEADER
    for packet in range(packet_count):
        first = packet * per_packet
        in_packet = [s for s in starts if first <= s < first + per_packet]
        frame_count = len(in_packet)
        # First frame offset in bits from the packet start, minus the header;
        # all ones when no frame starts here.
        field = (in_packet[0] - first) if in_packet else 0x7FFF
        header = bytes([
            ((frame_count & 0x3F) << 2) | ((field >> 13) & 0x3),
            (field >> 5) & 0xFF,
            ((field & 0x1F) << 3) | 1,  # XMA2 packet metadata
            0,  # skip count
        ])
        body = bytes(payload.bits[first // 8:(first + per_packet) // 8])
        data += header + body
    return bytes(data)


def xma_input(flags, context, packets):
    data = bytearray([flags])
    for value, default in zip(context, XMA_DEFAULT):
        data += struct.pack(">I", value ^ default)
    return bytes(data + packets)


def xma_inputs():
    rng = random.Random(0x584D41)
    for stereo in (0, 1):
        sizes = [rng.randrange(800, 3000) for _ in range(12)]
        stream = xma_stream(rng, sizes, 2)
        context = xma_context_dwords(input_buffer_0_packet_count=2, input_buffer_0_valid=1,
                                     output_buffer_block_count=8, sample_rate=3,
                                     is_stereo=stereo, output_buffer_valid=1,
                                     input_buffer_read_offset=32)
        yield xma_input(0x07 | 0x10, context, stream)
        # Two input buffers, refilled and drained between kicks.
        context = xma_context_dwords(input_buffer_0_packet_count=1, input_buffer_0_valid=1,
                                     input_buffer_1_packet_count=1, input_buffer_1_valid=1,
                                     output_buffer_block_count=16, subframe_decode_count=2,
                                     sample_rate=2, is_stereo=stereo, output_buffer_valid=1,
                                     input_buffer_read_offset=32)
        yield xma_input(0x07 | 0x08 | 0x10, context, stream)
        # A loop over the first frames.
        context = xma_context_dwords(input_buffer_0_packet_count=2, input_buffer_0_valid=1,
                                     output_buffer_block_count=8, sample_rate=3,
                                     is_stereo=stereo, output_buffer_valid=1,
                                     input_buffer_read_offset=32, loop_count=3,
                                     loop_start=32, loop_end=32 + sizes[0] + sizes[1],
                                     loop_subframe_end=1, loop_subframe_skip=2)
        yield xma_input(0x07 | 0x10, context, stream)
    # A frame whose 15-bit header crosses into the next packet.
    per_packet = BITS_PER_PACKET - BITS_PER_PACKET_HEADER
    sizes = [per_packet - 7, 2000, 1500]
    stream = xma_stream(rng, sizes, 2)
    context = xma_context_dwords(input_buffer_0_packet_count=2, input_buffer_0_valid=1,
                                 output_buffer_block_count=8, sample_rate=3,
                                 output_buffer_valid=1, input_buffer_read_offset=32)
    yield xma_input(0x03 | 0x10, context, stream)


# --- xdbf_fuzzer ----------------------------------------------------------------

SECTION_METADATA, SECTION_IMAGE, SECTION_STRING_TABLE = 1, 2, 3
ID_TITLE, ID_XSTC, ID_XACH = 0x8000, 0x58535443, 0x58414348
LANGUAGE_ENGLISH, LANGUAGE_GERMAN = 1, 3


def xdbf(entries, free_count=2):
    """entries: (section, id, bytes); content blocks in the given order."""
    content = bytearray()
    table = bytearray()
    for section, entry_id, block in entries:
        table += struct.pack(">HQII", section, entry_id, len(content), len(block))
        content += block
    entry_count = len(entries) + 2  # Spare entries, as the tools write them.
    table += bytes(18 * (entry_count - len(entries)))
    free = struct.pack(">II", len(content), 0x1000) + bytes(8 * (free_count - 1))
    header = struct.pack(">4sIIIII", b"XDBF", 0x10000, entry_count, len(entries), free_count, 1)
    return header + bytes(table) + free + bytes(content)


def string_table(strings):
    body = bytearray()
    for string_id, text in strings:
        encoded = text.encode("utf-8")
        body += struct.pack(">HH", string_id, len(encoded)) + encoded
    return struct.pack(">4sIIH", b"XSTR", 1, len(body) + 2, len(strings)) + bytes(body)


def achievements(count):
    body = bytearray()
    for i in range(count):
        body += struct.pack(">HHHHIHHIIIII", i + 1, 0x100 + i, 0x200 + i, 0x300 + i, 0x10 + i,
                            10 * (i + 1), 0, 0x8, 0, 0, 0, 0)
    return struct.pack(">4sIIH", b"XACH", 1, len(body) + 2, count) + bytes(body)


def xdbf_inputs():
    count = 3
    english = [(ID_TITLE, "Fuzz Title")]
    german = [(ID_TITLE, "Fuzz Titel")]
    for i in range(count):
        english += [(0x100 + i, f"Achievement {i}"), (0x200 + i, f"Do thing {i}"),
                    (0x300 + i, "Secret")]
        german += [(0x100 + i, f"Erfolg {i}"), (0x200 + i, f"Tu Ding {i}")]
    png = b"\x89PNG\r\n\x1a\n" + bytes(24)
    entries = [
        (SECTION_METADATA, ID_XSTC, struct.pack(">4sIII", b"XSTC", 1, 4, LANGUAGE_ENGLISH)),
        (SECTION_METADATA, ID_XACH, achievements(count)),
        (SECTION_STRING_TABLE, LANGUAGE_ENGLISH, string_table(english)),
        (SECTION_STRING_TABLE, LANGUAGE_GERMAN, string_table(german)),
        (SECTION_IMAGE, ID_TITLE, png),
    ] + [(SECTION_IMAGE, 0x10 + i, png) for i in range(count)]
    resource = xdbf(entries)
    for language in (LANGUAGE_ENGLISH, LANGUAGE_GERMAN, 2):
        yield bytes([language]) + resource
    # No achievements, title only.
    yield bytes([LANGUAGE_ENGLISH]) + xdbf(entries[:1] + entries[2:3] + entries[4:5])


# --- stfs_fuzzer and disc_image_fuzzer -----------------------------------------

STFS_HEADER_SIZE = 0x971A
STFS_DATA_OFFSET = 0xA000
STFS_BLOCK_SIZE = 0x1000
STFS_END_OF_CHAIN = 0xFFFFFF
SVOD_BLOCK_SIZE = 0x800
MEDIA_MAGIC = b"MICROSOFT*XBOX*MEDIA"
ATTRIBUTE_DIRECTORY = 0x10


def le24(value):
    return struct.pack("<I", value)[:3]


def stfs_input(flags, descriptor, data_file_count, content, first_fragment_size=0):
    return (
        bytes([flags])
        + descriptor
        + struct.pack(">IBH", STFS_HEADER_SIZE, data_file_count, first_fragment_size >> 4)
        + content
    )


def stfs_dir_entry(name, start_block, length, block_count, directory=False, parent=0xFFFF):
    return (
        name.ljust(40, b"\0")
        + bytes([len(name) | (0x80 if directory else 0)])
        + le24(block_count)
        + le24(block_count)
        + le24(start_block)
        + struct.pack(">HIHHHH", parent, length, 0x5021, 0x6000, 0x5021, 0x6000)
    )


def stfs_package(read_only, root_active_index):
    # Block 0: the directory; 1-2: default.xex; 3: a file in a directory. All
    # within the first level 0 hash table, which comes first.
    tables = 1 if read_only else 2
    chains = {0: STFS_END_OF_CHAIN, 1: 2, 2: STFS_END_OF_CHAIN, 3: STFS_END_OF_CHAIN}
    table = bytearray(STFS_BLOCK_SIZE)
    for block, next_block in chains.items():
        struct.pack_into(">I", table, block * 0x18 + 0x14, (2 << 30) | next_block)
    directory = (
        stfs_dir_entry(b"sub", 0, 0, 0, directory=True)
        + stfs_dir_entry(b"default.xex", 1, 5000, 2)
        + stfs_dir_entry(b"a.bin", 3, 100, 1, parent=0)
    ).ljust(STFS_BLOCK_SIZE, b"\0")
    content = bytes(table) * tables + directory + bytes(range(256)) * 48
    descriptor = (
        bytes([0x24, 0, (1 if read_only else 0) | (2 if root_active_index else 0)])
        + struct.pack("<H", 1)
        + le24(0)
        + bytes(0x14)
        + struct.pack(">II", 4, 0)
    )
    return stfs_input(0, descriptor, 0, content)


def gdf_entry(name, sector, length, attributes=0, left=0, right=0):
    entry = struct.pack("<HHIIBB", left, right, sector, length, attributes, len(name)) + name
    return entry.ljust((len(entry) + 3) & ~3, b"\0")


def gdf_directory(*entries):
    # Each entry but the last has the next as its right node.
    result = b""
    for i, (name, sector, length, attributes) in enumerate(entries):
        right = 0
        if i + 1 < len(entries):
            right = (len(result) + ((14 + len(name) + 3) & ~3)) // 4
        result += gdf_entry(name, sector, length, attributes, right=right)
    return result


def svod_descriptor(enhanced):
    return (
        bytes([0x24, 0, 0, 0])
        + bytes(0x14)
        + bytes([0x40 if enhanced else 0])
        + le24(0x100)
        + le24(0)
        + bytes(5)
    )


def svod_image(size, magic_offset, block_address):
    # Root directory in the block after the magic, a subdirectory, files after.
    image = bytearray(size)
    root_block = max((magic_offset - block_address(0)) // SVOD_BLOCK_SIZE + 1, 1)
    magic = MEDIA_MAGIC + struct.pack("<IIII", root_block, SVOD_BLOCK_SIZE, 0x5021, 0x6000)
    image[magic_offset : magic_offset + len(magic)] = magic
    root = gdf_directory(
        (b"default.xex", root_block + 2, 0x900, 0),
        (b"sub", root_block + 1, SVOD_BLOCK_SIZE, ATTRIBUTE_DIRECTORY),
    )
    sub = gdf_directory((b"a.bin", root_block + 4, 0x100, 0))
    for block, data in ((root_block, root), (root_block + 1, sub)):
        address = block_address(block)
        image[address : address + len(data)] = data
    return image


def stfs_inputs():
    inputs = [stfs_package(True, False), stfs_package(False, False), stfs_package(False, True)]
    svod = 1 << 2

    # One file: the magic at 0xD000, after the header and two hash blocks. The
    # device looks at 0x12000 first, so the file reaches that far.
    single = svod_image(0x12100, 0xD000, lambda block: block * SVOD_BLOCK_SIZE + 0xD000)
    inputs.append(stfs_input(svod, svod_descriptor(False), 1, bytes(single[STFS_DATA_OFFSET:])))

    # Enhanced GDF in two fragments: the magic at 0x2000 of the first.
    enhanced = svod_image(0x6000, 0x2000, lambda block: (block + 2) * SVOD_BLOCK_SIZE + 0x2000)
    inputs.append(
        stfs_input(svod, svod_descriptor(True), 2, bytes(enhanced) + bytes(0x100), len(enhanced))
    )

    # XSF: the magic at 0x12000, "XSF" at 0x2000.
    xsf = svod_image(0x14000, 0x12000, lambda block: block * SVOD_BLOCK_SIZE + 0x2000)
    xsf[0x2000:0x2003] = b"XSF"
    inputs.append(stfs_input(svod, svod_descriptor(False), 2, bytes(xsf) + bytes(0x100), len(xsf)))
    return inputs


def disc_image_inputs():
    sector_size = 0x800
    inputs = []
    root = gdf_directory(
        (b"default.xex", 35, 0x1000, 0),
        (b"sub", 34, sector_size, ATTRIBUTE_DIRECTORY),
    ).ljust(sector_size, b"\0")
    sub = gdf_directory((b"a.bin", 37, 0x100, 0)).ljust(sector_size, b"\0")
    data = bytes(range(256)) * 24
    for game_offset_index in range(3):
        descriptor = struct.pack("<II", 33, sector_size).ljust(sector_size - len(MEDIA_MAGIC), b"\0")
        inputs.append(bytes([game_offset_index]) + descriptor + root + sub + data)
    return inputs


TARGETS = {
    "primitive_processor": primitive_inputs,
    "xma": xma_inputs,
    "xdbf": xdbf_inputs,
    "stfs": stfs_inputs,
    "disc_image": disc_image_inputs,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("target", choices=sorted(TARGETS))
    parser.add_argument("corpus_dir", type=Path)
    args = parser.parse_args()
    written = write_inputs(TARGETS[args.target](), args.corpus_dir)
    print(f"{written} new inputs in {args.corpus_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
