# Dicewatch

Dicewatch is a Hailstone add-on for tracking loot rolls on EQEmu servers that use the standard RoF2 protocol. Keep several items open at once, each with its own range, timer, standings, and result.

## Install

Use **Add-ons** on Hailstone's login screen to install `fryguy503/Dicewatch`. Dicewatch requires Hailstone 0.7.2-beta or later and ships for Windows x64, Linux x64, and macOS arm64. Open it from the in-game Plugins menu or with `/dicewatch`.

Servers that enforce an add-on allowlist must permit Dicewatch. Hailstone's normal native add-on approval applies.

## Run loot rolls

Select **New round** in Dicewatch or type `/itemroll` followed by a range and a linked item or note. Insert an item link from the game as you would when chatting. Notes can contain spaces.

| Command to start the round | Accepted roll in stock Hailstone |
| --- | --- |
| `/itemroll 500 <item link>` | `/random 500` gives a 0–500 roll |
| `/itemroll 200-1000 <item link>` | `/random 200 1000` gives a 200–1000 roll |
| `/itemroll 2500 Random Note` | `/random 2500` gives a 0–2500 roll for **Random Note** |

Dicewatch opens one round per exact pair of bounds. A 0–1000 round and a 200–1000 round can run together. Let players know which roll to use. Dicewatch receives the same nearby server replies as Hailstone, including Bastion's rolls with its custom chat wording.

- **Ranked** shows eligible rolls and tied leaders. **Roll log** shows every recorded roll and marks duplicates or replaced rolls. **Both** shows the two views together.
- **Mini** keeps the selected item, current leader, and round controls in a compact window. Switch items without interrupting the other rounds.
- **End** closes the selected round. **Auto end** accepts a preset or a custom number of seconds. Set the timer to zero to keep a round open until you end it.
- **Defaults** sets the timer, winning order, and which roll counts when a player rolls more than once. Each new round starts with those defaults unless you override them in the form.
- **History** keeps the latest 24 finished rounds for the current character session. A tie stays a tie until players resolve it. View and round defaults are saved per character.

## Permissions and data

Dicewatch needs `read` to identify the active character, `commands` to register `/dicewatch` and `/itemroll`, and `ui` for the panel. The `packets` capability lets it observe the server's roll replies. It reads only the inbound zone roll reply for RoF2, validates the payload, and leaves it intact. Chat messages cannot put a roll on the board. Dicewatch makes no external requests and keeps roll results in memory.

The add-on limits the number of tracked ranges, stored rolls, and history entries. The panel reports when a round's log reaches its limit. Item-link labels and notes are treated as data.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix build/stage
```

The build uses the vendored Hailstone SDK headers. Install layout and package checks run in GitHub Actions. See the MIT licenses in this repository and each package for terms.
