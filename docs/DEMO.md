# Demo

A 90-second recording that shows the whole loop. Setup: a ROM built with `TELEMETRY_ENABLED` set to `TRUE`, `gen_tracker.py` run for that build, the bridge running, and `build/tracker.lua` loaded in mGBA.

Screen layout: mGBA on the left, the bridge's terminal on the right, mGBA's scripting console below if it fits.

| Time | On screen | What it shows |
|---|---|---|
| 0:00-0:10 | Load the script; the console prints `connected to bridge`, the bridge prints the session and `mailbox sent` | The game talks to Python over TCP |
| 0:10-0:40 | A trainer battle; the bridge scrolls `> ACTION`, `> MOVE_RESULT ... SUPER_EFFECTIVE`, `> FAINT`; at the end, `habits this battle` and the rules summary | Telemetry from inside the turn, decoded live |
| 0:40-0:55 | `python3 -m bridge.summarize --backend claude --battle last --language Spanish`; the summary and its token cost appear | LLM summary from stored facts |
| 0:55-1:10 | `python3 -m bridge.mailbox --backend claude`; within 2 s the bridge prints `mailbox sent (claude)`; open Start → PokéNav → INFORME | The LLM's text, encoded for the game, inside the game |
| 1:10-1:30 | A save state just before the Champion: his speech, then the generated line about the player's habits, then the battle | Write-back driving game content |

A GIF for the top of the README, from the recording (ffmpeg):

```bash
ffmpeg -ss 55 -t 15 -i demo.mp4 -vf "fps=12,scale=720:-1:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse" demo.gif
```
