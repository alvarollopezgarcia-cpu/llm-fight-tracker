# LLM Fight Tracker

A telemetry pipeline that watches how a player fights in a Pokémon Emerald ROM hack, stores every battle, and (in a later phase) asks an LLM to describe the player's style. It is a development and portfolio tool: the official release of the hack does not include it.

## Architecture

```
 ROM (mGBA)  ──reads RAM──►  tracker.lua  ──JSON lines / TCP──►  bridge (Python, WSL)  ──►  SQLite
                             (every frame)    127.0.0.1:8888       decode + store            reports
```

| Part | Role |
|---|---|
| `gen_tracker.py` | Reads the build's `pokeemerald.map` and C headers, writes `build/tracker.lua` (with this build's addresses) and `build/game_data.json` (habit names, trainer ids, map names) |
| `lua/tracker_template.lua` | mGBA 0.10+ script. Polls RAM every frame, emits events on change, queues them while the bridge is offline |
| `bridge/server.py` | Asyncio TCP server. One session per connection, stores events, logs each battle's habits |
| `bridge/store.py` | SQLite schema: raw `events`, derived `battles`, `sessions` |
| `bridge/game_data.py` | Decodes raw profile bytes and ids; computes what one battle added to the profile |
| `bridge/report.py` | Prints recorded battles and the saved profile |
| `bridge/habits.py` | Plain-language meaning of each of the 43 habits (the situation that counts, the behavior that scores) |
| `bridge/facts.py` | Builds the summarizer input: per-battle habits vs the player's usual rate, saved-profile tendencies, trends over time |
| `bridge/summarizers.py` | Summarizer backends: `rules` (free, deterministic) and `claude` (Anthropic API) |
| `bridge/summarize.py` | CLI: summarize the player or one battle; every summary is stored with the exact facts it was made from |

### Phase 1 (this version): RAM polling, no ROM changes

The script reads a handful of symbols each frame:

| Symbol | Used for |
|---|---|
| `gMain.inBattle` | Battle start / end |
| `gBattleTypeFlags`, `gTrainerBattleParameter` | Battle type and opponent |
| `gBattleOutcome`, `gBattleTurnCounter` | Result and length |
| `gSaveBlock1Ptr->location` | Current map |
| `gSaveBlock3Ptr` → Player Profile | The 43 habit counters the ROM's in-game recorder keeps |

The Player Profile is found by scanning SaveBlock3 for its magic number, so it doesn't depend on the save layout. The bridge stores the profile at battle start and end; the difference is exactly what that battle taught the profile.

The ROM is built with `-mabi=apcs-gnu`, which rounds every struct up to 4 bytes: a `ProfileCounter {u8 hits; u8 chances;}` takes 4 bytes in RAM, not 2. The generator mirrors that rule (`STAT_STRIDE`), and a test decodes bytes captured from a real mGBA session.

### Phase 2: telemetry ring buffer in the ROM

Polling sees states, not what happens inside a turn. With `TELEMETRY_ENABLED` set to `TRUE` in `include/config/telemetry.h`, the ROM appends 8-byte events to a 64-entry ring buffer in EWRAM (`gTelemetry`, `src/telemetry.c`); the Lua script reads every entry written since the previous frame and forwards it. The game never waits: if the reader fell more than 64 events behind, the overwritten ones are reported as lost. With `FALSE` (the release setting) every hook compiles away, the buffer doesn't exist, and the generator writes `gTelemetry = nil` so the script skips it.

| Event | Fields | Hook |
|---|---|---|
| `ACTION` | player's move / switch-in / item / run, turn | `SetActionsAndBattlersTurnOrder` |
| `MOVE_RESULT` | attacker and target side, move, result (super effective, missed, protected, can't move...), crit | `MoveEndUpdateLastMoves`, both sides, once per target |
| `SWITCH_IN` | species, party slot | battle start (leads) and `Cmd_switchinanim` |
| `FAINT` | species, side of the last attacker | `Cmd_tryfaintmon` |
| `ITEM_IN_BATTLE` | item, species on the field | `HandleAction_UseItem` |
| `RARE_CANDY` | source (bag or the PokéNav LEVEL option), species, new level, candies used | `ItemUseCB_RareCandy`, `Task_PokenavLevelUpNext` |
| `CAP_BLOCKED` | species, level cap | Rare Candy refused by the cap |
| `LEVEL_UP` | species, level (battle experience) | `Cmd_getexp` |
| `WHITEOUT` | map | `DoWhiteOut` |
| `HEAL` | source (Pokémon Center or the PokéNav HEAL charge), map, party HP % and fainted count before healing | nurse script (`callnative Telemetry_LogCenterHeal`), `StartMenuPokeNavHealCallback` |

The generator also reads `moves.h`, `species.h` and `items.h` (resolving aliases like `MOVE_SKETCH = MOVES_COUNT_GEN1`) so the bridge can name everything; facts gain a per-battle `telemetry` block and a player-wide `leveling` block.

### Write-back: the mailbox

The other direction. With telemetry on, the ROM also has `gMailbox`: a magic number and two text slots. The bridge fills them; the game only reads.

| Slot | Shown | ROM side |
|---|---|---|
| report (1000 bytes) | PokéNav > INFORME | `Mailbox_LoadReport` copies it (or a fallback) to `gStringVar4` |
| champion line (300 bytes) | the Champion, after his speech and before the battle | `Mailbox_LoadChampionLine`; nothing is said when the slot is empty |

- **Texts are made in Python, in Spanish** (the game's language): `rules` builds them from the habits with the most evidence (rate far from 50% × chances); `claude` writes both in one call with a JSON schema (structured output).
- **Encoded for the game in Python too.** The generator exports `charmap.txt` and the message box font's glyph widths (`gFontNormalLatinGlyphWidths`), so the bridge wraps by pixel width, inserts `\n`, `\l` and `\p`, drops characters the font lacks, and never cuts a line in half. The ROM just copies bytes.
- **Delivery.** The server sends `MAILBOX <slot> <hex>` lines over the same TCP connection: on connect, after each battle, and within 2 s of `python3 -m bridge.mailbox` storing new texts. The Lua script reads them with `socket:hasdata()`/`receive()` (never blocking), writes the bytes, then the magic, and rewrites them if a reset or save state wipes or rolls them back.
- **Who writes.** After every battle the server rewrites the mailbox with `rules` (free), unless the newest texts came from Claude: those were paid for on purpose and stay until the next manual write.

### Summaries

`facts.py` turns the database into one JSON document; a summarizer turns it into text. Both backends read the same facts, so their output can be compared:

- **`rules`**: thresholds and templates. Free, instant, reproducible; runs after every battle by default.
- **`claude`**: sends the facts to the Anthropic Messages API with a system prompt that explains the hack's context (public trainer docs, unlimited Rare Candy, don't assume skill) and how to read hits/chances. Defaults to `claude-opus-5-5` at low effort, with the server-side refusal fallback enabled. Each call prints its token use and estimated cost; a player summary is a few thousand input tokens.

The in-game profile decays old data (counters halve at 200 chances) so the League AI adapts to recent play. The database keeps every battle's delta, so summaries can still show long-term trends the game has forgotten.

### Design points

- **Lua stays dumb, Python stays smart.** Lua only reads bytes and sends them as hex; all decoding lives in tested Python, generated from the same C headers the ROM is built from.
- **Addresses come from the build.** Symbol addresses move every build, so the script is generated, never edited by hand. The bridge warns if the script and decoding data come from different builds.
- **The game never waits.** Events are queued in Lua (bounded, oldest dropped and counted) while the bridge is down; reconnects are attempted every ~5 seconds.
- **Save states are handled.** A frame counter that goes backwards emits a `rewind` event; the bridge discards the unfinished battle. Every event carries a sequence number and duplicates are ignored.

## Running it

Requires mGBA 0.10+ (Windows) and Python 3 in WSL Ubuntu (standard library only).

1. For telemetry, set `TELEMETRY_ENABLED` to `TRUE` in `include/config/telemetry.h`. Build the ROM, then generate the script for that build (it prints whether telemetry is on):
   ```bash
   cd tools/llm_tracker && python3 gen_tracker.py
   ```
2. Start the bridge (in WSL):
   ```bash
   python3 -m bridge.server
   ```
3. In mGBA: load the ROM, then **Tools → Scripting → File → Load script** and pick `tools/llm_tracker/build/tracker.lua`.
4. Play. Each battle is logged in the bridge's console.
5. See what was recorded:
   ```bash
   python3 -m bridge.report
   ```
6. Summaries:
   ```bash
   python3 -m bridge.summarize                    # the player's style (rules)
   python3 -m bridge.summarize --battle last      # the last battle
   python3 -m bridge.summarize --facts            # the JSON a summarizer reads
   ```
   For LLM summaries, `pip install anthropic`, set `ANTHROPIC_API_KEY`, then add `--backend claude` (options: `--model`, `--effort`, `--language`). `python3 -m bridge.server --summarizer claude` summarizes every battle with the LLM; `--summarizer none` turns per-battle summaries off.

7. Mailbox texts (the running bridge sends them to the game):
   ```bash
   python3 -m bridge.mailbox --show                 # what the game has, paged like the message box
   python3 -m bridge.mailbox --backend claude       # let Claude write them (Spanish)
   ```

Tests: `python3 -m unittest discover tests` (the Claude backend is tested with a fake client, so no key or network is needed)

## Roadmap

1. **Phase 1:** RAM polling and storage (done).
2. **Summaries:** interchangeable backends turn the stats into a play-style description: `rules` and `claude` (done).
3. **Phase 2:** an in-ROM telemetry ring buffer for events polling can't see (moves, switches, faints, items, Rare Candy, level cap) (done).
4. **Write-back:** a mailbox the bridge fills so the game can react: PokéNav INFORME and the Champion's line (done).
