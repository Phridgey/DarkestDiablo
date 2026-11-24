"""Monster levels, part 2: in Normal (the only difficulty the mod plays) a monster's level is its own
monstats.txt Level, not the area's level (levels.txt MonLvlEx only counts in Nightmare and Hell;
champions add 2, uniques and superuniques 3). So the route's levels (004_area_levels.py) are put
on the monsters themselves: every wave copy gets its stop's level, and each type a superunique
of a stop is made from gets that stop's level minus 3. Item levels of drops follow, so the
guaranteed set/unique picks (the Pit's target, Treehead) line up with what the monster drops.

Runs late (996), after every patch that makes copies.
"""

# Wave copies: Id prefix -> the stop's level (longest prefix wins).
COPY_LEVELS = {
    "dd_corpsefire": 3, "dd_den_": 3, "dd_tower_": 5,
    "dd_mf_": 7, "dd_rakanishu": 7, "dd_treehead": 7, "dd_craft_": 7, "dd_xp_": 7,
    "dd_tris_": 10, "dd_pit_": 13, "dd_countess": 13, "dd_malus_": 13, "dd_andy_": 17,
    "dd_halls_": 20, "dd_xp_a2_": 23, "dd_craft_a2_": 23, "dd_maggot_": 27, "dd_viper_": 27,
    "dd_craft_a2b_": 31, "dd_mf_a2_": 31, "dd_xp_a2b_": 31,
    "dd_xp_a3a_": 40, "dd_xp_a3b_": 40, "dd_xp_a3c_": 40, "dd_xp_a3d_": 46, "dd_mf_a3_": 46,
    "dd_craft_a3_": 46, "dd_mf_a3b_": 50,
    "dd_xp_a4_": 57, "dd_xp_a4b_": 62, "dd_mf_a4_": 62, "dd_craft_a4_": 62, "dd_craft_a4b_": 67,
    "dd_xp_chaos_": 71,
    "dd_shenk_": 74, "dd_xp_a5_": 78, "dd_mf_a5_": 78, "dd_craft_a5_": 78, "dd_mf_a5b_": 81,
    "dd_mf_a5c_": 84, "dd_craft_a5b_": 84,
}

# The game's own types used for a stop's superuniques or kept bosses: the stop's level minus 3
# for superunique types (they add 3), the stop's level for act bosses (004_area_levels.py).
TYPE_LEVELS = {
    # spawned as they are by the plugin: the Den, Burial Grounds and the Tower, Andariel's packs
    "zombie1": 3, "fallen1": 3, "fallenshaman1": 3,
    "skeleton1": 5, "fallen3": 5, "goatman3": 5, "wraith1": 5, "cr_archer3": 5,
    "zombie3": 17, "bighead2": 17, "vampire5": 17, "bighead1": 17, "fallen4": 17, "arach1": 17,
    "pantherwoman1": 17, "maggotqueen1": 24, "clawviper3": 24,          # Bloodwitch, Coldworm, Fangskin
    "corruptrogue5": 43, "councilmember1": 47, "councilmember2": 47, "councilmember3": 47,
    "mephistospirit": 53,
    "fingermage3": 68, "doomknight3": 68, "megademon3": 68,             # the seal bosses
    "hephasto": 67,
    "overseer1": 71, "minion1": 71,                                     # Shenk, Eldritch
    "reanimatedhorde5": 75, "snowyeti4": 78, "cowking": 81,             # Pindleskin, Frozenstein, the Cow King
    "ancientbarb1": 84, "ancientbarb2": 84, "ancientbarb3": 84,
    "fallenshaman5": 86, "unraveler5": 86, "baalhighpriest": 86, "venomlord": 86, "baalminion1": 86,  # Baal's waves
    "baalthrone": 89,
}


def patch(t):
    stats = t.table("monstats")
    prefixes = sorted(COPY_LEVELS, key=len, reverse=True)
    for row in stats.rows:
        mid = row.get("Id", "")
        if mid in TYPE_LEVELS:
            row["Level"] = str(TYPE_LEVELS[mid])
            continue
        for p in prefixes:
            if mid.startswith(p):
                row["Level"] = str(COPY_LEVELS[p])
                break
