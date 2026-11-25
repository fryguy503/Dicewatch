# Dicewatch

Dicewatch is a Hailstone add-on for tracking loot rolls on EQEmu servers that use the standard RoF2 protocol. Keep several items open at once, each with its own range, timer, standings, and result.

## Install

Use **Add-ons** on Hailstone's login screen to install `fryguy503/Dicewatch`. Dicewatch requires Hailstone 0.7.2-beta or later and ships for Windows x64, Linux x64, and macOS arm64. Open it with `/dice` or `/dicewatch`, or from the in-game Plugins menu.

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
- **Settings → Round defaults** sets the timer, winning order, and which roll counts when a player rolls more than once. Each new round starts with those defaults unless you override them in the form.
- **Ledger** shows the latest 200 completed rounds for this character across play sessions. View and round defaults are saved per character.

## Loot Ledger

Select a round card in **Ledger** to see its results and award. Use the session filter and page controls to find older rounds.

Right-click a player in the standings or roll log of an ended round and choose **Award** to record them as the recipient. This works in the tracker and in **Ledger**. To enter a different recipient by name or clear an award, open the round in **Ledger** and use **Save** or **Clear**. The ledger displays the award alongside the original roll result.

Choose **Discord summary**, select a play session, and copy the text from the report using your usual Select All and Copy shortcuts. Paste each page into Discord in order. The summary identifies the roll winner or tie, the recorded recipient, and items still awaiting an award.

Dicewatch saves finished rounds when they close manually or by timer. Open rounds belong to the active play session. Older records are retained on disk when they fall outside the 200-round display limit.

## Announcements

Open **Settings → Announcements** to choose what happens when a round opens and when you save an award. Each event has its own mode and channel. Both default to **Off** and are saved per character.

- **Auto-send** submits the announcement to Raid say, Group, Guild, or Say. For rounds started with a linked item, it includes the clickable item link when the tag passes validation and fits in chat. The selected channel must be available and the server must grant Dicewatch `chat` permission.
- **Copy** places the message on your clipboard when the event happens. Copied messages use the item name because Hailstone strips game link delimiters from pasted text. If system clipboard access is unavailable, Dicewatch shows selectable text in the window.
- **Off** leaves the event quiet.

Round announcements include the item or label, exact `/random` command, winning rule, and timer when set at opening. An award announcement follows a successful recipient change on a completed round in the current session. The award text names the recipient and the original roll winner or tie.

## Permissions and data

Dicewatch needs `read` to identify the active character, `commands` to register `/dicewatch`, `/dice`, and `/itemroll`, and `ui` for the panel. The `packets` capability lets it observe the server's roll replies. It reads only the inbound zone roll reply for RoF2, validates the payload, and leaves it intact. Chat messages cannot put a roll on the board. Dicewatch stores finished round data and award names locally in the character's Dicewatch configuration directory. The Discord summary is plain text that you can copy and share.

The `chat` capability is used for Auto-send. If the selected destination is unavailable or chat permission is restricted, the announcement remains available to copy from Dicewatch.

The add-on limits the number of tracked ranges, stored rolls, and history entries. The panel reports when a round's log reaches its limit. Item-link labels and notes are treated as data.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix build/stage
```

The build uses the vendored Hailstone SDK headers. Install layout and package checks run in GitHub Actions. See the MIT licenses in this repository and each package for terms.
