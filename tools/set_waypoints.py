"""Turn on waypoints in offline character saves, so a run's stops are "scouted ahead".

Usage (with the game CLOSED):
    python tools/set_waypoints.py <slot>[,<slot>...] [character ...] [--difficulty normal|nightmare|hell|all]

    python tools/set_waypoints.py 1            # every Darkest Diablo character: slot 1 (the Den)
    python tools/set_waypoints.py 1,2 Tester   # only the character named Tester

Slots are the waypoint numbers from levels.txt (Act 1: 0-8, Act 2: 9-17, ...). Saves are read
from Saved Games\\Diablo II Resurrected\\mods\\darkestdiablo. Each changed save is backed up
first to the "waypoint backups" folder beside it.

Stopgap until the D2RLoader SDK offers a waypoint service (D2RLoader/PluginSDK issue #9).
"""
import argparse
import os
import shutil
import struct
import sys
import time
from pathlib import Path

SAVES = Path(os.environ.get("USERPROFILE", "~")).expanduser() / "Saved Games" / "Diablo II Resurrected" / "mods" / "darkestdiablo"
DIFFS = {"normal": 0, "nightmare": 1, "hell": 2}


def checksum(data):
    total = 0
    for i, byte in enumerate(data):
        if 12 <= i < 16:  # the checksum field itself counts as zero
            byte = 0
        total = (((total << 1) | (total >> 31)) + byte) & 0xFFFFFFFF
    return total


def set_waypoints(path, slots, diffs):
    data = bytearray(path.read_bytes())
    if struct.unpack_from("<I", data, 0)[0] != 0xAA55AA55:
        raise ValueError("not a D2 save")
    if checksum(data) != struct.unpack_from("<I", data, 12)[0]:
        raise ValueError("checksum doesn't match before editing; save left alone")
    ws = data.find(b"WS")
    if ws < 0:
        raise ValueError("no waypoint section")
    changed = False
    for d in diffs:
        block = ws + 8 + 24 * d  # "WS", 4-byte header, 2-byte size, then 24 bytes per difficulty
        if data[block:block + 2] != b"\x02\x01":
            raise ValueError(f"unexpected waypoint layout at difficulty {d}")
        for slot in slots:
            i, bit = block + 2 + slot // 8, 1 << (slot % 8)
            if not data[i] & bit:
                data[i] |= bit
                changed = True
    if not changed:
        return False
    struct.pack_into("<I", data, 12, checksum(data))
    backup = path.parent / "waypoint backups" / f"{path.stem} {time.strftime('%Y-%m-%d %H%M%S')}.d2s"
    backup.parent.mkdir(exist_ok=True)
    shutil.copy2(path, backup)
    path.write_bytes(data)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("slots")
    ap.add_argument("characters", nargs="*")
    ap.add_argument("--difficulty", default="normal", choices=[*DIFFS, "all"])
    ap.add_argument("--saves", type=Path, default=SAVES)
    a = ap.parse_args()
    slots = [int(s) for s in a.slots.split(",")]
    diffs = list(DIFFS.values()) if a.difficulty == "all" else [DIFFS[a.difficulty]]
    files = [a.saves / f"{c}.d2s" for c in a.characters] or sorted(a.saves.glob("*.d2s"))
    if not files:
        sys.exit(f"no characters found in {a.saves}")
    for f in files:
        try:
            print(f"{f.stem}: {'waypoints turned on' if set_waypoints(f, slots, diffs) else 'already on'}")
        except (OSError, ValueError) as e:
            print(f"{f.stem}: skipped ({e})")


if __name__ == "__main__":
    main()
