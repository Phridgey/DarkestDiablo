# Darkest Diablo — design

A Hades-style roguelite built in Diablo II: Resurrected. Decisions so far (16 Nov 2026).
The shared whiteboard has the same plan as a picture; this file is the reference.

## The run
- One run spans all five acts. Each act's town is that act's own town:
  Rogue Encampment, Lut Gholein, Kurast Docks, Pandemonium Fortress, Harrogath.
- No open world. Each stop is a short tunnel the player can't get lost in, or an arena
  room with waves of enemies, on that act's tilesets.
- Stops follow the game's own quest order for the act.
- The player reaches the next stop by waypoint (the waypoint has been "scouted ahead").
  The waypoint menu doubles as the fork menu: only the fork's options are unlocked.
- The last kill on a map opens a town portal. The player returns to town to manage loot
  and comes back through the portal.
- At a fork, the player does 2 runs before moving on. The same option can be picked twice.
- Every stop is fought as waves the plugin spawns, even on the game's own maps (decided
  2026-11-19): native monsters come back fresh every game, so quitting would farm them.
- No farming by leaving (decided 2026-11-19): entering a stop starts a run, and it must be
  finished before any other stop can be entered (a fork pick is spent on entry). Leaving and
  coming back, even after quitting the game, resumes at the next wave; it never starts over.
  On a fork, quitting with nothing more to come (the last wave out) gives the pick up: it counts
  as done, with no reward. Main-line stops (Den, Blood Raven, Tristram) are never locked out:
  they resume with their last wave.
- Waves spawn just past the edge of the screen and walk in (decided 2026-11-19): no monster
  appears in view, and the hero is never buried under a fresh pack. Exception: on maps without a
  known open floor (Tristram, the Countess), bosses still appear near the hero, so they never
  fail to spawn.
- Every stop gives +25% magic find the first time it is picked.

## Stop types
| Type | Drops | Magic find | Notes |
|---|---|---|---|
| Quest | Quest rewards | — | Uses the act's own quests |
| MF run | One guaranteed set or unique of the area's level, plus kill drops | D2R rules; bosses and unique monsters feel it more | |
| Crafting hunt | No magic items; runes, gems, crafting materials, white items (socketed and ethereal ones too) | Raises the rune chance instead | |
| XP grind | Normal drops | Normal | More experience (Crypt +40%) |

Magic find also raises socket count, superior ("high quality") chance and ethereal chance
(MF runs and XP grinds).

## Act I
Status (2026-11-20): every stop below is built (Andariel not yet tested). Each is a
waves arena (see "The run"); the numbers are under "Drop and experience numbers".

1. **Den of Evil** — forced quest. A skill point on the first clear (no Akara respec).
2. **Blood Raven** — forced quest. Unlocks the mercenary.
3. Fork (2 picks): **Rakanishu & Treehead** (MF, at the Tree of Inifuss) · **Mausoleum** (crafting
   hunt: a round crypt hall; runes, gems and white items only) · **Crypt** (XP grind: the same
   hall; +40% experience; every pack led by a unique; killing a random one of them wins)
4. **Tristram** — forced. The game's own Tristram map and quest (rescue Cain, unlock identify), but
   its monsters are waves around the player (each pack three times at once), ending with Griswold. Cleared by freeing Cain from his gibbet (2026-11-21); a hero who freed him in an earlier game clears it by killing Griswold.
   The whole zone has +40% magic find.
5. Fork (2 picks): **Pit** (MF: the real Pit level 2; 5 packs led by uniques, a random one is the
   target and drops a guaranteed unique or set) · **Countess** (crafting hunt: Tower Cellar 5; she
   drops 5 runes, Ith to Ral) · **Horadric Malus** (XP grind: a cut-down Barracks, a large hall
   leading into the Smith's forge; +40% experience; cleared when the Smith dies; the Malus is
   Charsi's quest, her imbue)
6. **Andariel** — act boss, built like Sam's Tower zone (decided 2026-11-20). Her lair is
   a copy of Blizzard's Catacombs level 4 map, without its monsters, its stairs (it leads
   nowhere) and its two doors (no door can be shut on her). Packs come out as the hero nears them. Two unique monsters, far apart (the
   south room, and the west end of the hall below her throne), each drop one of her keys: the
   Vial of Maiden's Venom and the Shard of Anguish. Walking into her hall with both takes them and
   brings her out; her death clears the stop. Keys follow the Tower seals' rules: kept in the
   inventory, one of each, gone at the end of the game. Built in `src/patches/125_andariel.py`, `src/maps/andariel.json`, `src/plugin/andariel.cpp`.

Act II (plan: `docs/act2.md`; status 2026-11-21: every stop built, not yet tested). Its route map
page opens from the waypoint in Lut Gholein.
1. **Halls of the Dead** (forced) — a copy of three Halls 3 rooms in a line, no stairs and no doors:
   waves on the way to the Horadric Cube's chest, Bloodwitch the Wild by it. Cleared on picking up
   the Cube (a hero who already has one: on opening the chest).
2. Fork: **Sewers & Radament** (XP grind: +40% XP, triple gold; three sewer rooms ending in his lair;
   he always drops his Book of Skill first, and picking it up clears it; the book's 1 skill point
   plus 2 from the plugin on the first clear) · **Stony Tomb** (crafting hunt like the Mausoleum,
   runes up to Shael, a round tomb hall).
3. **Maggot Lair 3** and **Claw Viper Temple 2** (both forced, any order): three lair rooms ending in
   the Staff of Kings' chest (Coldworm the Burrower by it); the temple's altar room (Fangskin; the
   Tainted Sun altar gives the Amulet of the Viper). Each clears on picking up its item (a replay:
   when the chest or altar is opened).
4. Fork: **Arcane Sanctuary** (crafting hunt; one arm, monsters only ahead, the Summoner's death
   clears it) · **Ancient Tunnels** (MF run; four rooms, unique-led packs, a random unique ends it)
   · **Canyon of the Magi** (XP grind; a round sandy hollow).
5. **Duriel**: arrive at the orifice in Tal Rasha's last room; the staff opens the way (the game's
   own event); his death in his lair clears the stop.
Linear maps (Halls, Sewers, Maggot Lair, Arcane, Tunnels) only spawn monsters ahead of the hero.

Acts III-V (plans: `docs/act3.md`, `act4.md`, `act5.md` on the plan branch; status 2026-11-21:
every stop built, not yet tested). Patches `113_act3.py`, `114_act4.py`, `114a_act5.py`; maps in
`src/maps/`; route map pages from `tools/route_pages.py`. Where the build differs from the plan:
- Khalim's three stops are one or two rooms each, without a swamp strip in front (a swamp is an
  outdoor level, which the game places anew every game).
- Outdoor stops borrow a spare level of the same act and copy the real one's look and name:
  Lower Kurast (level 95), Travincal (96), the Hellforge (107), Shenk's foothills (116), the
  Frigid Highlands (113). The Plains of Despair (105) get a fixed spot of their own.
- The Hellforge clears when Hephasto dies (Mephisto's Soulstone comes from a quest the hero may
  not have); the plugin then gives the three runes.
- Rewards the plugin gives on a hero's first clear: Lam Esen +10 stat points, Izual +4 skill
  points, the Hellforge's and the barbarians' runes, a Socket Chisel after the Malus (Charsi),
  after Shenk (Larzuk's second) and once Khalim's three parts are held in Kurast Docks (Hratli),
  the Golden Bird's +40 life when its potion is drunk, Anya's scroll (a choice panel), and a
  level for the Ancients.
- The route map opens on the act of the hero's next open stop, wherever the hero stands (Town
  Portals from Act IV's red portals or the Cow Level land in another act's town).
- Monster levels and loot follow the level progression (004_area_levels.py, 996_monster_levels.py).
- Act I's runes (El to Ral) need levels 3 to 16 instead of 11 to 19, so they work in Act I
  (008_act1_rune_levels.py); later runes keep the game's requirements.

## Healing (decided 2026-11-19)
- No health potions drop or are sold. Each cleared stop earns 3 (by difficulty: light, greater,
  super), put in the hero's stash in town; what doesn't fit stays owed and piles up.
- Akara doesn't heal (the plugin undoes it).

## Drop and experience numbers (decided so far)
Every multiplier and drop rate we have agreed on, in one place. "Built" means it is in the game;
"planned" means agreed but not made yet.

| What | Number | Status |
|---|---|---|
| Magic find, first pick of each stop | +25% magic find | planned |
| Crypt (XP grind) | +40% experience from kills; drops as in the base game; every pack led by a random unique of its type (no minions); killing one of those uniques, picked at random and not marked, clears the stop | built |
| Tristram, the whole zone | +40% magic find (on top of the hero's; done through the zone's loot tables) | built |
| Griswold | 6 white weapon or armor drops (never magic; sockets and ethereal possible, superior not); a monster drops 6 items at most, so none of his own loot | built |
| Horadric Malus (XP grind) | +40% experience from the waves' kills; drops as in the base game; cleared when the Smith dies | built |
| XP grinds: gold (Crypt, Malus) | Every gold pile is worth 3 times as much (+200%); how often gold drops is unchanged (`src/patches/115_xp_gold.py`) | built |
| Countess (crafting hunt) | She drops 5 runes, Ith to Ral, at the game's own odds between them (Ith 22%, Tal 47%, Ral 31%), then 1 of her usual item picks; the game's own Tower Cellar 5 map, with waves of its monster types, then her; cleared when she dies | built |
| Pit (MF run) | Like Rakanishu & Treehead: every monster's drops 1.4 times as likely to be magic or better; 5 packs of the Pit's monsters, each led by a unique; one unique, picked at random, drops a guaranteed unique or set, and killing it clears the stop | built |
| MF runs (Rakanishu & Treehead, Pit) | Every drop 1.4 times as likely to be magic or better; charms 3 times as likely (+200%) | built |
| Den of Evil, first clear | 1 skill point (no respec) | built |
| Health potions | None drop and shops sell none; every cleared stop earns 3 (light in Normal, greater in Nightmare, super in Hell), put in the hero's stash in town (the bag if full); what doesn't fit stays owed and piles up | built |
| Mana potions | Drop half as often as in the base game (`src/patches/007_fewer_mana_potions.py`); everything else in the same loot tables is unchanged | built |
| Akara | No longer heals: the plugin writes life and mana back when talking to her fills them (a full rejuvenation drunk next to her is undone too) | built |
| Rakanishu & Treehead (MF run) | Treehead: one guaranteed unique or set (a set if no unique fits), plus his normal table drops | built |
| Mausoleum: rune chance per kill | 5% at 0 magic find, 10% at 250, then on up in a straight line (+1% per 50 MF), 100% at most | built |
| Mausoleum: white item chance per kill | 12% (weapon or armor near the area's level, never magic; can roll sockets and ethereal like any normal drop) | built |
| Mausoleum: gems, potions, gold | 1 pick per kill (2 for champions) from chipped/flawed gems, a healing potion and gold; 40 in 74 picks drop nothing (10 in 44 for champions) | built |

Rune rules:
- Highest rune per act (decided 2026-11-20, replacing every earlier Act I limit): Act I Ral,
  Act II Shael, Act III Lem, Act IV Ohm. The Mausoleum drops El to Ral (the game's "Runes 4"),
  the Countess Ith to Ral.
- Keep the game's own odds between runes (its rune tables), even where they look odd: the
  newest runes of a table drop most often (in the Mausoleum, Tal 25%, Eth 17.5%, Ral 16.7%,
  Tir 12.5%, Ith 11.7%, Nef 8.3%, El 5%, Eld 3.3%). Only the top rune and the overall rune chance are ours to set.

The game's player count (`/players`) raises every drop chance above, as it does in the base
game: the numbers are for one player.

## The camp (between characters)
- The camp is the run: the circle of characters by the fire, plus the stash.
- Hardcore rules: death is final for the character, and everything it wears and carries is lost.
  The camp carries on.
- Recruiting: one of each class in the campfire circle. The first character can be any class.
  After that, each return to town has a chance that a traveler joins the camp and offers up to
  3 locked classes (decided 2026-11-17). The first offer after a death includes the fallen
  hero's class. A recruit starts at level 1 with nothing.
- Three save slots; each is its own camp with its own stash.
- In town, the bonfire leads to the Campfire. How it is built, and what the engine forced:
  `docs/campfire.md`.
- The next character to play is picked from the campfire circle, with the stash in view to plan.
- Stashed items keep their level and attribute requirements. That is the balance.
- The stash starts small. Optional challenges (e.g. "bonus for spears", "can't use shields")
  unlock more tabs and stash features.
- **Atlas:** kiss and curse bonuses that keep less-progressed characters relevant. Points come
  from reaching a new furthest point; they can be re-allocated freely; profiles can be saved
  (pushing vs farming). No out-of-run skill tree.
- **Blaze of glory:** for a character that won't go further. One boss fight, no fleeing, uber
  difficulty for its level, huge rewards. Win: the character retires and brings its whole
  inventory home, boss loot included. When it can be tried: not decided.

## Technical approach
- Data edits (tables, maps) for everything they can do.
- A D2RLoader plugin (our code, built with the D2RLoader Plugin SDK) for what data can't: fork/waypoint control, run and camp state, first-pick bonuses,
  magic find effects on sockets/superior/ethereal and runes, death and retirement rules,
  class limit, challenges, Atlas.
- The plugin keeps its camp files in the `camp` folder of the mod's save folder
  (`Saved Games\Diablo II Resurrected\mods\darkestdiablo`). Back it up with the saves.

## Open questions
- Is Andariel (and each act boss) a forced final stop?
- Examples of kiss and curse bonuses.
- When can a character attempt a blaze of glory?

## Arena stops (decided 2026-11-17, updated)
- Waves come on a timer: wave 1 shortly after arrival, then one every 10 s, whatever is still
  alive. The stop is cleared when the last wave has spawned and everything is dead, the boss
  included, in any kill order (players may go for the boss first).
- The fight happens where the player can see it (e.g. Burial Grounds: the plaza by the hanging
  tree), never in a far corner.
- The player needs time to buff and prep: where possible, arrive in an entry room and walk
  through a gate or stairs into the arena; that starts the waves. No short timers on arrival.
- Burial Grounds packs: +50% over the first version.
- The player arrives by waypoint into one room, and the fight stays in that room.
- Example, Den of Evil: 5 waves of about 10 monsters, then a final wave with Corpsefire.
- The last kill drops a Town Portal scroll. That is the way home.
- The player can bail out of a stop early (too spooky); the goal is never being stranded.

## Tuning notes (later)
- Den arena: the room's open edge shows black nothingness; cap it with walls or a closing room.
- Den arena: the wave feel is right, but too dense for the first quest, when the player has no
  area damage yet. Lighter early waves, heavier later in the run.

## Route map (the waypoint "map")
- The town waypoint opens a map of the act's route (see the sketch: Den, Burial, fork x2,
  Tristram, fork x2, Catacombs). The player picks the next stop there.
- First version: a list panel, opened by the town waypoint (or `dd-map`). Waypoints outside
  town keep Blizzard's list, so the player is never stranded. Next: draw it as a tree.

## Fork 1 MF run: Rakanishu and Treehead (decided 2026-11-17, moved to fork 1 the same day)
- The MF option at the fork after Blood Raven, replacing "Blood Raven again" (Rakanishu+Treehead /
  Mausoleum / Crypt). The fork after Tristram keeps the Pit as its MF run.
- Place: Treehead's usual spot in the Dark Wood, the Tree of Inifuss (Blizzard's `funktree`
  map piece), on a custom map far smaller than the Dark Wood so players can't wander off.
- Fight: waves of Fallen and Fallen Shamans, then Rakanishu (superunique row 3) with his pack;
  then waves of Dark Spearwomen ("lancers", cr_lancer1), then Brutes (brute2) and Quill Rats
  (quillrat1), then Treehead Woodfist (superunique row 4).
- Open question: how the player gets prep time before the waves (Burial uses an entry room with
  stairs; the forest has no door to pair stairs with).
