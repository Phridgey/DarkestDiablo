"""The heroes' permanent potions (docs/potions.md): what the tables and the HUD have to give up for them.

- New heroes no longer start with 4 Minor Healing Potions; the plugin gives every hero its potions.
- Shops sell no potions of any kind, throwing potions included, and none drop (the rest is in
  006_no_health_potions.py). Throwing potions are worth nothing, so a hero can't sell refills.
- The HUD's belt and its pop-up are moved off the screen: the plugin draws the four potion slots
  there. The widget itself stays, because the game's HUD code may look for it.
- A belt's "Belt Size: +4 Slots" line now says what it gives: more charges for every potion. The
  belts' real size is left alone (belts.txt), so a saved hero's belt still loads.
"""

POTIONS = {"hp1", "hp2", "hp3", "hp4", "hp5", "mp1", "mp2", "mp3", "mp4", "mp5", "rvs", "rvl", "yps", "vps", "wms"}
THROWN = {"gps", "gpm", "gpl", "ops", "opm", "opl"}
OFF_SCREEN = 6000


def patch(t):
    for name, codes in (("misc", POTIONS), ("weapons", THROWN)):
        table = t.table(name)
        vendor = [c for c in table.columns if (c.endswith("Min") or c.endswith("Max")) and not c.startswith("TMog")]
        found = set()
        for row in table.rows:
            if row.get("code") in codes:
                found.add(row["code"])
                for c in vendor:
                    row[c] = ""
                row["PermStoreItem"] = "0"
                if name == "weapons":
                    row["cost"] = "1"
        if found != codes:
            raise RuntimeError(f"{name}: missing {sorted(codes - found)}")

    cleared = 0
    for row in t.table("charstats").rows:
        slots = [tuple(row.get(f"item{i}{part}", "") for part in ("", "loc", "count", "quality")) for i in range(1, 11)]
        if not any(slot[0] == "hp1" for slot in slots):
            continue
        kept = [slot for slot in slots if slot[0] != "hp1"]
        kept += [("0", "", "0", "2")] * (10 - len(kept))
        for i, slot in enumerate(kept, 1):
            for part, value in zip(("", "loc", "count", "quality"), slot):
                row[f"item{i}{part}"] = value
        cleared += 1
    if cleared < 7:
        raise RuntimeError(f"charstats: {cleared} heroes started with healing potions; has the game changed?")

    if t.excel.name != "excel":  # the base copy only keeps tables; layouts and strings are shipped once
        return
    hud = t.layout("hudpanelhd")
    for name in ("Belt", "BeltPopupArrow"):
        widget = t.widget(hud, name)
        if widget is None:
            raise RuntimeError(f"hudpanelhd: no widget {name!r}; has the game changed?")
        widget["fields"]["rect"] = dict(widget["fields"]["rect"], y=OFF_SCREEN)
    for entry in t.data("local/lng/strings/ui.json"):
        if entry.get("Key") == "BeltStorageModifierInfo":
            for lang in entry:
                if lang not in ("id", "Key"):
                    entry[lang] = "Maximum Potion Charges: %+d"
            break
    else:
        raise RuntimeError("ui.json: no BeltStorageModifierInfo; has the game changed?")
