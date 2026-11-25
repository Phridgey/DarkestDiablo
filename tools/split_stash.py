"""Undoes the one-file stash (src/plugin/stashfile.h): gives every save its own 10-page file again, for
going back to a version from before 0.21.0. Close the game first. The next start of a newer version
joins them again. The joined file is kept in camp/stash-backup-split-<time>.

Usage: python tools/split_stash.py "<the mod's save folder>"
"""
import shutil
import struct
import sys
import time
from pathlib import Path

NAME = "SharedStashSoftCoreV2.d2i"


def pages(data):
    out, at = [], 0
    while at < len(data):
        mark, _, version, _, length = struct.unpack_from("<5I", data, at)
        if mark != 0xAA55AA55 or version != 0x69 or length < 68 or at + length > len(data):
            sys.exit("The stash is not a file this tool knows; nothing was changed.")
        out.append(data[at:at + length])
        at += length
    return out


def purse(folder):
    """The gold in a save's stash.txt, taken out of it (it goes back into the file's header)."""
    file = folder / "stash.txt"
    if not file.exists():
        return 0, None
    gold, lines = 0, []
    for line in file.read_text().splitlines():
        if line.startswith("purse "):
            gold = int(line.split()[1])
        elif not line.startswith("pursemoved"):
            lines.append(line)
    return gold, "\n".join(lines) + "\n"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    live = Path(sys.argv[1])
    marker = live / "camp" / "stash-joined.txt"
    if not marker.exists():
        print("The stashes are separate already.")
        return
    joined = live / NAME
    if (live / "camp" / "wipe.txt").exists():
        sys.exit("A deleted save's pages are not emptied yet: start the game once with the newer version, then try again.")
    whole = pages(joined.read_bytes()) if joined.exists() else []
    if joined.exists() and len(whole) < 30:
        sys.exit(f"The stash has {len(whole)} pages instead of 30; nothing was changed.")
    active = 1
    try:
        active = int((live / "camp" / "active.txt").read_text().split()[0])
    except (OSError, ValueError, IndexError):
        pass
    marks = dict((int(w[1]), int(w[2])) for w in (l.split() for l in marker.read_text().splitlines()) if len(w) == 3 and w[0] == "gold")
    backup = live / "camp" / f"stash-backup-split-{int(time.time())}"
    backup.mkdir(parents=True)
    for file in [joined, marker] + list((live / "camp").glob("slot*/stash.txt")):
        if file.exists():
            to = backup / (file.parent.name + "-" + file.name if file.parent.name.startswith("slot") else file.name)
            shutil.copy2(file, to)
    if not whole:
        marker.unlink()
        print("There was no stash to split.")
        return
    for slot in (1, 2, 3):
        mine = whole[(slot - 1) * 10:slot * 10]
        folder = live / "camp" / f"slot{slot}"
        gold, text = purse(folder)
        if text is None or "pursemoved 1" not in (folder / "stash.txt").read_text():
            gold += marks.get(slot, 0)
        tabs = whole[30 + (slot - 1) * 4:30 + slot * 4]
        if any(page[66] or page[67] for page in tabs):  # ascended heroes' pages: 11-14 of the save's own file
            mine = mine + tabs
        first = bytearray(mine[0])
        struct.pack_into("<I", first, 12, gold)
        target = joined.with_name(NAME + ".own") if slot == active else folder / "files" / NAME
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(bytes(first) + b"".join(mine[1:]))
        if text is not None:
            (folder / "stash.txt").write_text(text)
    joined.with_name(NAME + ".own").replace(joined)
    marker.unlink()
    print(f"Every save has its own stash file again. The joined one is in {backup}.")


if __name__ == "__main__":
    main()
