"""Rune upgrades in the Horadric Cube (decided 2026-11-23): no gems, three of a rune make the next
one up to Dol (Dol to Hel included), two from Hel on (Hel to Io, and so on to Cham to Zod).

The game's recipes take 3 runes up to Lem, 2 from Pul, and a gem from Thul on.
"""
import re

LAST_THREE = 14  # Dol (r14): the last rune that still takes three


def patch(t):
    for row in t.table("cubemain").rows:
        out = row.get("output", "").strip('"')
        first = row.get("input 1", "").strip('"')
        m = re.fullmatch(r"r(\d\d),qty=\d", first)
        if not re.fullmatch(r"r\d\d", out) or not m or int(out[1:]) != int(m.group(1)) + 1:
            continue
        rank = int(m.group(1))
        qty = 3 if rank <= LAST_THREE else 2
        row["input 1"] = f'"r{rank:02d},qty={qty}"' if row["input 1"].startswith('"') else f"r{rank:02d},qty={qty}"
        for k in range(2, 8):
            row[f"input {k}"] = ""
        row["numinputs"] = str(qty)
        name = re.sub(r" \+ 1 [^-]+(?= ->)", "", row["description"])
        row["description"] = re.sub(r"^\d", str(qty), name)
