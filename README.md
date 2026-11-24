# LLM Fight Tracker

[![tests](https://github.com/alvarollopezgarcia-cpu/llm-fight-tracker/actions/workflows/tests.yml/badge.svg)](https://github.com/alvarollopezgarcia-cpu/llm-fight-tracker/actions/workflows/tests.yml)
&nbsp;[Leer en español](README.es.md)

**A telemetry pipeline that learns how a player fights in a Game Boy Advance game, then makes the final bosses fight back: the enemy AI adapts to the player's recorded habits, and an LLM plans the whole Pokémon League's strategy against them before it starts.**

<!-- Demo: replace with the GIF and the video link (see docs/DEMO.md) -->
> 🎬 **Demo video:** _coming soon_

I built it for my [Pokémon Emerald ROM hack, EmeraldFF](https://github.com/alvarollopezgarcia-cpu/emeraldFF-romhack), a C codebase based on [pokeemerald-expansion](https://github.com/rh-hideout/pokeemerald-expansion). It's a development and portfolio tool: the hack's public release is built with it switched off and contains none of it.

---

## What it does

1. **Records** every trainer battle: who, where, the result, and 43 behavioral habits the game computes turn by turn (does the player switch out when threatened? set up when it's risky? lead with a counter to the documented enemy team?).
2. **Logs what polling can't see**: moves and their results, KOs, switches, Rare Candy use, level-cap hits, Pokémon Center visits. The game writes them to an in-memory ring buffer that never blocks it.
3. **Stores and analyzes** it all in SQLite: per-battle deltas, long-term trends, and summaries written by either a free rules engine or Claude (Anthropic API).
4. **Writes back into the game**: a Spanish report appears in the in-game PokéNav menu, and the final boss greets the player with a line about their actual habits.
5. **Adapts the enemy AI** (`AI_FLAG_PLAYER_PROFILE`, in C): the Elite Four and Champion punish the player's clear habits with small score nudges (Taunt a player who sets up, Wide Guard against spread moves, predict switches at the player's own rate) and swap their documented lead when the player usually counters it. The AI only uses information the base game already gives it.
6. **Plans the League with an LLM**: when the player walks into the Pokémon League, Claude reads the profile, the battle history and the documented enemy teams, and returns a bounded plan for every trainer (leads, switch prediction, which habits to punish, focus target, aggression). Python validates every value and falls back to a rules plan on any error; the game applies it as weights. [An example plan.](docs/examples/league_plan_claude.txt)

```
 GBA ROM (C)                mGBA (Lua)                    Bridge (Python)                    Claude API
┌──────────────────┐  RAM  ┌──────────────┐  JSON / TCP  ┌───────────────────────┐  HTTPS  ┌──────────┐
│ habit recorder   │──────►│ tracker.lua  │─────────────►│ asyncio server        │────────►│ summary, │
│ telemetry ring   │       │ polls every  │              │ SQLite store          │◄────────│ texts    │
│ buffer (64 × 8B) │       │ frame, never │◄─────────────│ facts → rules/Claude  │         └──────────┘
│ mailbox (texts,  │◄──────│ blocks       │   MAILBOX    │ text encoder (charmap,│
│ League plan)     │ write └──────────────┘   lines      │ glyph widths)         │
│ profile AI flag  │                                     │ League planner        │
└──────────────────┘                                     └───────────────────────┘
```

## Engineering highlights

- **A struct-padding bug found in production data.** The first live session decoded "impossible" numbers: three lead checks in one battle, a doubles-only habit in a singles battle. The raw bytes showed every counter followed by two zero bytes. The ROM is compiled with `-mabi=apcs-gnu`, which rounds every struct up to 4 bytes, so a `{u8 hits; u8 chances;}` takes 4, not 2. The fix mirrors that rule in the layout generator; a regression test decodes the exact bytes captured from that session, and the newer C structs carry `STATIC_ASSERT`s on their size.
- **The game never waits for the tooling.** Telemetry is a 64-entry ring buffer: the game only appends and bumps a counter; the reader keeps its own cursor and counts anything overwritten as lost. The Lua script queues events when the bridge is down and reconnects on its own. Save states and resets are detected (the frame counter goes backwards), and partial battles are discarded.
- **Zero cost in the release build.** One config flag (`TELEMETRY_ENABLED`) removes every hook at compile time: the hook macros wrap `if (FALSE)`, the buffers don't exist, the menu option is hidden, and the generator emits `nil` addresses so the script skips them.
- **Generated, not hand-written, glue.** Symbol addresses move with every build, so `gen_tracker.py` reads the linker map and the C headers (enums with aliases and macro-generated members, `#define`s, the charmap, font glyph widths) and generates both the Lua script and the bridge's decoding tables. Lua stays dumb (copies bytes); Python stays smart (decodes, tested).
- **Text that fits a 2004 message box.** Texts for the game are encoded with the game's own character map and wrapped by pixel width using the font's glyph table, with the engine's control codes for new lines, scrolling and new boxes.
- **The LLM plans; it never plays turns.** An LLM call takes seconds and a battle turn can't wait, so the model makes one strategic plan per League attempt, on entering the building. Its output is a JSON schema of bounded values (lead slots, a 0-100 chance, -3..3 weights, named rules), validated in Python against the documented teams (slots in range, distinct, never the ace) and encoded as a 104-byte C struct the AI reads. A bad answer or an API failure falls back to a deterministic rules plan, and with no bridge at all the AI runs on the recorded profile alone. Each League battle is linked to the plan it was fought under, so rules plans and LLM plans can be compared on results.
- **LLM cost under control.** A deterministic rules backend is the default and runs after every battle for free. Claude runs on demand (about $0.02 per battle summary with Claude Opus 5.5 at low effort). Both backends read the same facts, and every summary is stored with the exact input it saw. Claude returns both game texts in one call through a JSON schema (structured output). Texts it wrote are never overwritten by the automatic rules refresh.

## Repository layout

| Path | What it is |
|---|---|
| `gen_tracker.py` | Reads a build's `pokeemerald.map` and the C headers; writes the Lua script and `game_data.json` |
| `lua/tracker_template.lua` | mGBA script: per-frame RAM polling, ring buffer reader, TCP client, mailbox writer |
| `bridge/` | Python: asyncio server, SQLite store, decoding, facts, summarizers (rules / Claude), mailbox, League planner |
| `tests/` | 51 unit and integration tests (sockets, SQLite, a fake Anthropic client); `fixtures/` holds trimmed copies of the game files they read |
| `rom/` | The C code I wrote in the game: the habit recorder, the profile-based AI, telemetry and mailbox, 27 in-engine battle tests, plus `HOOKS.md` with every place it touches the engine |
| `docs/examples/` | A real League plan written by Claude |
| `docs/ARCHITECTURE.md` | Full technical write-up: protocol, data layout, event table, design decisions |

About 2,900 lines of Python and Lua, 2,900 lines of C, and 1,500 lines of tests (Python and in-engine battle tests).

## Running the tests

```bash
python -m unittest discover tests
```

Standard library only (Python 3.10+). They run on every push through GitHub Actions.

## Running it for real

It needs the hack's source (to build the ROM), [mGBA](https://mgba.io) 0.10+ and Python 3. The steps are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#running-it). In short:

```bash
python3 gen_tracker.py --repo path/to/the/hack   # after building the ROM
python3 -m bridge.server --planner claude       # then load build/tracker.lua in mGBA
python3 -m bridge.summarize --backend claude     # optional: needs the anthropic package and a key
```

## What's not here

No ROM, save files or game assets. Pokémon is © Nintendo / Game Freak / Creatures; this repository contains only my own code and short excerpts of the open-source decompilation's headers used as test fixtures.

## Stack

C (GBA, `arm-none-eabi-gcc`), Lua 5.4 (mGBA scripting API), Python 3 (asyncio, sqlite3, unittest), Anthropic Claude API (structured outputs), GitHub Actions.
