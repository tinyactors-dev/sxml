#!/usr/bin/env python3
"""Makes the DEFLATE test vectors of tests/deflate/ (D333).

    tools/deflate/make-vectors.py

Raw DEFLATE streams (RFC 1951) written by zlib through Python's zlib
module, an implementation independent of sxml's, with the bytes they
inflate to; and malformed streams written by hand, each refused for one
reason. Deterministic: the same zlib version gives the same files
(manifest.tsv records it).

manifest.tsv: name, kind (valid or malformed), the expected status for a
malformed stream, and a description. For a valid vector, <name>.deflate
inflates to <name>.raw, consuming the whole file but its trailer (when the
name ends in -trailer, the stream is followed by 5 bytes that are not
part of it).
"""
import os
import random
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "tests", "deflate")


def raw_deflate(data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY, memory_level=8, flushes=()):
    compressor = zlib.compressobj(level, zlib.DEFLATED, -15, memory_level, strategy)
    output = b""
    position = 0
    for offset, mode in flushes:
        output += compressor.compress(data[position:offset])
        output += compressor.flush(mode)
        position = offset
    output += compressor.compress(data[position:])
    output += compressor.flush(zlib.Z_FINISH)
    return output


def corpus():
    rng = random.Random(1951)
    text = open(os.path.join(HERE, "..", "..", "tests", "exi", "handmade", "large.xml"), "rb").read()
    random_bytes = bytes(rng.randrange(256) for _ in range(70000))
    # Matches as far back as the window allows: a block, 32,000 random
    # bytes, the block again.
    window = bytes(rng.randrange(256) for _ in range(300))
    far = window + bytes(rng.randrange(256) for _ in range(32_000)) + window + window[:7]
    runs = b"a" * 1000 + b"ab" * 500 + b"abc" * 333 + bytes(range(256)) * 4
    lengths = b"".join(bytes([rng.randrange(4)]) * rng.randrange(3, 300) for _ in range(400))
    return {
        "empty": b"",
        "one-byte": b"x",
        "text": text,
        "random": random_bytes,
        "far-matches": far,
        "runs": runs,
        "lengths": lengths,
        "all-bytes": bytes(range(256)),
        "unicode": "Grüße aus Tallinn — 😀 𝄞 日本語 ".encode() * 300,
    }


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in os.listdir(OUT):
        if name.endswith((".deflate", ".raw")):
            os.remove(os.path.join(OUT, name))
    rows = []

    def valid(name, data, stream, description):
        with open(os.path.join(OUT, name + ".deflate"), "wb") as handle:
            handle.write(stream)
        with open(os.path.join(OUT, name + ".raw"), "wb") as handle:
            handle.write(data)
        rows.append((name, "valid", "", description))

    for name, data in corpus().items():
        valid(f"{name}-default", data, raw_deflate(data), "zlib level 6")
        valid(f"{name}-stored", data, raw_deflate(data, level=0), "stored blocks only (level 0)")
        valid(f"{name}-fixed", data, raw_deflate(data, strategy=zlib.Z_FIXED), "fixed Huffman blocks only")
        valid(f"{name}-huffman", data, raw_deflate(data, strategy=zlib.Z_HUFFMAN_ONLY), "literals only")
        valid(f"{name}-rle", data, raw_deflate(data, strategy=zlib.Z_RLE), "distance 1 matches only")
        valid(f"{name}-best", data, raw_deflate(data, level=9, memory_level=9), "zlib level 9")
        valid(f"{name}-fast", data, raw_deflate(data, level=1, memory_level=1), "zlib level 1, small blocks")
        if len(data) > 100:
            third = len(data) // 3
            valid(f"{name}-flushes", data,
                  raw_deflate(data, flushes=((third, zlib.Z_SYNC_FLUSH), (2 * third, zlib.Z_FULL_FLUSH))),
                  "sync and full flushes: empty stored blocks in the middle")
    text = corpus()["text"]
    valid("text-trailer", text, raw_deflate(text) + b"\x00\xff\x13\x37\x42",
          "a stream followed by 5 bytes that are not part of it")

    def malformed(name, stream, status, description):
        with open(os.path.join(OUT, name + ".deflate"), "wb") as handle:
            handle.write(stream)
        rows.append((name, "malformed", status, description))

    good = raw_deflate(text)
    malformed("truncated-middle", good[: len(good) // 2], "SXML_STATUS_UNEXPECTED_END_OF_INPUT", "half a stream")
    malformed("truncated-last-byte", good[:-1], "SXML_STATUS_UNEXPECTED_END_OF_INPUT", "all but the last byte")
    malformed("empty-input", b"", "SXML_STATUS_UNEXPECTED_END_OF_INPUT", "no bytes at all")
    # BFINAL=1, BTYPE=11: reserved.
    malformed("block-type-3", bytes([0b00000111]), "SXML_STATUS_MALFORMED_INPUT", "block type 3 is reserved")
    # BFINAL=1, BTYPE=00, LEN=5, NLEN=5 (not ~LEN).
    malformed("stored-nlen", bytes([0x01, 0x05, 0x00, 0x05, 0x00]) + b"hello", "SXML_STATUS_MALFORMED_INPUT",
              "a stored block whose NLEN is not the complement of LEN")
    malformed("stored-short", bytes([0x01, 0x0A, 0x00, 0xF5, 0xFF]) + b"hello", "SXML_STATUS_UNEXPECTED_END_OF_INPUT",
              "a stored block of 10 bytes with 5")
    # Fixed Huffman block: a distance of 1 before any output. Bits, LSB first:
    # BFINAL 1, BTYPE 01, length code 257 (0000001, 7 bits), distance code 0
    # (00000, 5 bits), then 256 (0000000).
    def bits_to_bytes(bits):
        output = bytearray()
        for index in range(0, len(bits), 8):
            byte = 0
            for offset, bit in enumerate(bits[index:index + 8]):
                byte |= bit << offset
            output.append(byte)
        return bytes(output)

    def huffman_code(code, length):
        # Huffman codes are packed most significant bit first.
        return [(code >> (length - 1 - shift)) & 1 for shift in range(length)]

    bits = [1, 1, 0] + huffman_code(0b0000001, 7) + huffman_code(0, 5) + huffman_code(0, 7)
    malformed("distance-before-start", bits_to_bytes(bits), "SXML_STATUS_MALFORMED_INPUT",
              "a match before any byte was written")
    # Fixed block with literal 'a', then length 3 at distance code 30 (does not exist).
    bits = [1, 1, 0] + huffman_code(0x30 + ord("a"), 8) + huffman_code(0b0000001, 7) + huffman_code(30, 5)
    malformed("distance-code-30", bits_to_bytes(bits), "SXML_STATUS_MALFORMED_INPUT",
              "distance codes 30 and 31 do not exist")
    # Fixed block with literal/length code 286 (11000110, 8 bits): does not exist.
    bits = [1, 1, 0] + huffman_code(0b11000110, 8)
    malformed("length-code-286", bits_to_bytes(bits), "SXML_STATUS_MALFORMED_INPUT",
              "literal/length codes 286 and 287 do not exist")
    # Dynamic block whose code length code lengths over-subscribe: HLIT=0,
    # HDIST=0, HCLEN=4 (8 code length codes... here 4+4), all of length 1.
    bits = [1, 0, 1] + [0] * 5 + [0] * 5 + [0, 0, 0, 0]
    for _ in range(4):
        bits += [1, 0, 0]
    malformed("oversubscribed", bits_to_bytes(bits + [0] * 16), "SXML_STATUS_MALFORMED_INPUT",
              "code length codes that over-subscribe the code")
    # Dynamic block whose first code length is a repeat (code 16) with nothing to repeat.
    # HLIT=0 (257), HDIST=0 (1), HCLEN=15 (19 lengths): give codes 16, 17, 18, 0
    # (the first four in the order) length 2, others 0: complete code of 4.
    bits = [1, 0, 1] + [0] * 5 + [0] * 5 + [1, 1, 1, 1]
    order_lengths = [2, 2, 2, 2] + [0] * 15
    for length in order_lengths:
        bits += [(length >> shift) & 1 for shift in range(3)]
    # Canonical codes (RFC 1951, 3.2.2: equal lengths in symbol order):
    # 0 -> 00, 16 -> 01, 17 -> 10, 18 -> 11. Code 16 first.
    bits += huffman_code(0b01, 2) + [0, 0]
    malformed("repeat-first", bits_to_bytes(bits + [0] * 16), "SXML_STATUS_MALFORMED_INPUT",
              "code 16 (repeat the previous length) with no previous length")
    # A valid stream with its final block's flag cleared: never ends.
    malformed("no-final-block", bytes([0x00, 0x00, 0x00, 0xFF, 0xFF]), "SXML_STATUS_UNEXPECTED_END_OF_INPUT",
              "an empty stored block that is not final, then nothing")
    rng = random.Random(7)
    for index in range(20):
        noise = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 200)))
        rows.append((f"noise-{index:02}", "noise", "", "random bytes: refused or inflated, never out of bounds"))
        with open(os.path.join(OUT, f"noise-{index:02}.deflate"), "wb") as handle:
            handle.write(noise)

    with open(os.path.join(OUT, "manifest.tsv"), "w") as handle:
        handle.write(f"# Generated by tools/deflate/make-vectors.py with zlib {zlib.ZLIB_RUNTIME_VERSION}; do not edit.\n")
        handle.write("# name\tkind\tstatus\tdescription\n")
        for row in rows:
            handle.write("\t".join(row) + "\n")
    print(f"{len(rows)} vectors")


if __name__ == "__main__":
    main()
