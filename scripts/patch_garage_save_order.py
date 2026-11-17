"""Build the narrow MWM Garage patch for issue #59 without recompiling its TUFA container.

Usage: python scripts/patch_garage_save_order.py INPUT_CLASS OUTPUT_CLASS
Only the verified MWM input is accepted. Other mods must fix their Java source
(changeActiveSection before autoSaveQuiet) and compile with their own toolchain.
"""
import argparse
import hashlib
from pathlib import Path
import struct

STOCK_SHA256 = "9d5a4ae8a109331f31d1ee375696b823e394833938d9c13624ea7aaf46660cc5"
WIDE = {1, 2, 3, 6, 7, 8, 9, 10, 11, 13, 14, 15, 17, 18,
        20, 21, 22, 23, 25, 26, 27, 28, 30, 31, 37, 38, 39, 46}


def patch(data):
    if hashlib.sha256(data).hexdigest() != STOCK_SHA256:
        raise ValueError("Unsupported Garage.class; refusing to modify a different mod/version")
    # Find TREE in the first TUFA blob. Additional concatenated classes stay intact.
    pos = 12
    while data[pos:pos + 4] != b"TREE":
        pos += 8 + struct.unpack_from("<I", data, pos + 4)[0]
    pos += 8
    count = struct.unpack_from("<I", data, pos)[0]
    pos += 4
    matches = []
    # These references were resolved through CONS in the verified input:
    # 1016 = GameLogic.autoSaveQuiet()V; 1019 = changeActiveSection(GameState)V;
    # 260 = Garage.parentState. Both statements are standalone, with no internal jumps.
    expected = [(39, 0), (36, None), (17, 2), (25, 211), (26, 1016), (8, 27),
                (27, 260), (39, 1), (36, None), (17, 2), (25, 211), (26, 1019), (8, 27)]
    for _ in range(count):
        length = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        nodes = []
        for _ in range(length):
            start = pos
            op = data[pos]
            pos += 3  # opcode and original source line
            imm = None
            if op in WIDE:
                imm = struct.unpack_from("<I", data, pos)[0]
                pos += 4
            nodes.append((op, imm, start, pos))
        for i in range(len(nodes) - len(expected) + 1):
            if [(n[0], n[1]) for n in nodes[i:i + 13]] == expected:
                matches.append((nodes[i][2], nodes[i + 6][2], nodes[i + 12][3]))
    if len(matches) != 1:
        raise ValueError("Expected exactly one verified main-menu save/transition pair")
    start, middle, end = matches[0]
    result = data[:start] + data[middle:end] + data[start:middle] + data[end:]
    assert len(result) == len(data)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = patch(args.input.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result)
    print("Wrote Garage save-order patch:", hashlib.sha256(result).hexdigest())
