# GM tools (the cheat window for testing)

A window of cheats for testing the mod. Code: `src/plugin/devtools.cpp`.

## Opening it

- In the console (Ctrl + `, needs `enable_console` in `d2rloader.toml`): type `gm` or `/gm`.
- Or press its key: Ctrl + G by default. It can be changed in the game's Controls menu
  ("GM tools window", under "Darkest Diablo").
- The X in its corner, Escape, `gm` or the key again close it.

The game's chat box can't be used: the plugin tools we build on don't let a plugin read what is
typed there.

## What it does

| Part | What happens |
| --- | --- |
| Unlimited life / mana / stamina | Three switches. They stay on from game to game until switched off or the game is closed. |
| Go to town | Sends you to the town of the act you are in and remembers where you were. |
| Return to where I was | Takes you back to that level, at its entrance. This stands in for the portal: a plugin can't make a real one. Going back into an arena starts its waves again. |
| Items | Type an item code (`rin`, `amu`, `lsd`, ...) and press a quality. The item goes into the inventory, or on the ground if it is full. |
| Gold, rejuvenations, identify | One press each. The gold button gives up to 50,000, but never more than the hero can still carry (10,000 per level): a gift that passes that limit empties the purse instead. |
| Events | One button per registered event (see below). |

The same tools from the console, for when the window is in the way:

```
gm life|mana|stamina [0|1]
gm town
gm return
gm item <code> [normal|magic|rare|set|unique] [count]
gm events
gm event <id>
```

## Adding an event

One call, before `DevTools::Load` in `D2RLoaderLoadPlugin` (`darkestdiablo_plugin.cpp`):

```cpp
DevTools::RegisterEvent({ .id = "traveler", .label = "A traveler joins the camp", .console = "dd-traveler" });
```

Give it either a console command (`.console`) or a function (`.run`, called on the UI thread in
a game). The window gets a button for it and `gm event <id>` runs it. Nothing else changes.

## How the switches work

The loader has a cheat that turns no-damage, endless stamina and endless mana on or off together
(`cheat god 0|1`) and cheats for the first two alone (`cheat invincible`, `cheat stamina`), but
none for mana alone. So every change sends `cheat god` for the mana switch, then puts life and
stamina where they should be.
