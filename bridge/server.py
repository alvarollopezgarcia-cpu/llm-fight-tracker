"""TCP server that receives JSON-line events from the mGBA tracker script.

    python3 -m bridge.server [--host 127.0.0.1] [--port 8888] [--db build/tracker.sqlite]

Each connection from mGBA is one session. Events are stored as they arrive;
duplicates (same session and sequence number) are ignored.
"""

import argparse
import asyncio
import json
import logging

from . import mailbox, planner
from .facts import battle_facts, player_facts
from .game_data import GameData, profile_delta
from .store import DEFAULT_DB, Store
from .summarizers import SummaryError, get_summarizer

log = logging.getLogger("bridge")


class Bridge:
    def __init__(self, store, game_data=None, summarizer=None, mailbox_auto=True,
                 planner_backend="rules", plan_summarizer=None):
        self.store = store
        self.game_data = game_data
        self.summarizer = summarizer      # summarizes each finished battle when set
        self.mailbox_auto = mailbox_auto  # rewrite the mailbox with rules after each battle
        self.writers = {}                 # session id -> stream writer, to send the mailbox
        self.pushed_mailbox = {}          # session id -> mailbox row id that session already has
        self.planner_backend = planner_backend  # League plan on entry: "rules", "claude" or None
        self.plan_summarizer = plan_summarizer  # ClaudeSummarizer for planner_backend "claude"
        self.pushed_plan = {}             # session id -> plan id that session's game has
        self.planning = set()             # sessions with a plan being made right now
        self.last_map = {}                # session id -> map name, to see the player walk in

    def describe(self, event):
        """One human-readable console line per event."""
        kind = event["type"]
        data = self.game_data
        if kind == "battle_start" and data:
            trainer, partner = data.opponents(event.get("battleTypeFlags"), event.get("opponentA"), event.get("opponentB"))
            trainer += f" + {partner}" if partner else ""
            types = ",".join(data.battle_type_names(event.get("battleTypeFlags", 0)))
            return f"battle start vs {trainer} [{types}] on {data.map_name(event.get('mapGroup'), event.get('mapNum'))}"
        if kind == "battle_end" and data:
            return f"battle end: {data.outcome_name(event.get('outcome', 0))} after {event.get('turns')} turns"
        if kind == "map_change" and data:
            return f"map: {data.map_name(event.get('mapGroup'), event.get('mapNum'))}"
        if kind == "telemetry" and data and data.telemetry:
            decoded = data.decode_telemetry(event)
            details = " ".join(f"{k}={v}" for k, v in decoded.items() if k != "kind")
            lost = f" ({event['lost']} events lost before this one)" if event.get("lost") else ""
            return f"  > {decoded['kind']} {details}{lost}"
        return f"{kind} {json.dumps({k: v for k, v in event.items() if k not in ('type', 'profile')})}"

    def handle_line(self, session_id, line):
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            log.warning("session %d: skipped malformed line %r", session_id, line[:80])
            return None
        if "seq" not in event or "type" not in event:
            log.warning("session %d: skipped event without seq/type", session_id)
            return None
        if not self.store.add_event(session_id, event):
            return None
        log.info("session %d #%d %s", session_id, event["seq"], self.describe(event))
        if event["type"] == "hello" and self.game_data and event.get("build") != self.game_data.build:
            log.warning("tracker.lua build %s does not match game_data.json build %s: re-run gen_tracker.py",
                        event.get("build"), self.game_data.build)
        if event["type"] == "battle_end" and self.game_data:
            self.log_battle_habits(session_id, event)
        return event

    def log_battle_habits(self, session_id, event):
        battle = self.store.battle_ended_by(session_id, event["seq"])
        if battle is None:
            return
        delta = profile_delta(self.game_data.decode_profile(battle["profile_before"]),
                              self.game_data.decode_profile(battle["profile_after"]))
        if delta:
            habits = ", ".join(f"{name.removeprefix('PROFILE_')} {d['hits']}/{d['chances']}"
                               for name, d in delta.items() if d)
            log.info("  habits this battle: %s", habits)

    async def summarize_battle(self, session_id, event):
        """Summarize the battle a battle_end event closed and store the summary."""
        battle = self.store.battle_ended_by(session_id, event["seq"])
        if battle is None:
            return None
        facts = battle_facts(battle, self.game_data, self.store.telemetry(battle))
        try:
            # An LLM call takes seconds: run it off the event loop so mGBA's events keep flowing.
            # Only the summarizer runs in the thread; SQLite stays on this one.
            summary = await asyncio.to_thread(self.summarizer.summarize_battle, facts)
        except Exception as error:  # a failed summary must never stop event recording
            log.warning("  summary failed: %s", error)
            return None
        self.store.add_summary("battle", summary, facts, battle_id=battle["id"])
        for line in summary.text.splitlines():
            log.info("  | %s", line)
        return summary

    # -- Mailbox: texts sent back to the game --------------------------------------------------

    @property
    def mailbox_ready(self):
        return bool(self.game_data and self.game_data.mailbox)

    def refresh_mailbox(self):
        """Rewrite the texts with the rules backend after a battle, unless Claude wrote the newest ones.

        Claude texts cost money and are written on purpose (python3 -m bridge.mailbox --backend claude),
        so they stay until the next manual write.
        """
        latest = self.store.latest_mailbox()
        if latest is not None and latest["backend"] != "rules":
            return
        texts = mailbox.generate(self.store, self.game_data, "rules")
        log.info("  mailbox: report and champion line rewritten (rules)")
        return texts

    async def push_mailbox(self, session_id, writer):
        """Send the newest stored texts to one mGBA session if it doesn't have them yet."""
        latest = self.store.latest_mailbox()
        if latest is None or self.pushed_mailbox.get(session_id) == latest["id"]:
            return
        for line in mailbox.mailbox_lines(mailbox.encode_mailbox(latest, self.game_data)):
            writer.write(line.encode())
        await writer.drain()
        self.pushed_mailbox[session_id] = latest["id"]
        log.info("session %d: mailbox #%d sent (%s)", session_id, latest["id"], latest["backend"])

    async def push_mailbox_to_all(self):
        for session_id, writer in list(self.writers.items()):
            try:
                await self.push_mailbox(session_id, writer)
            except ConnectionError:
                pass  # the reading side notices the disconnect and cleans up

    # -- League plan: made once when the player walks into the League --------------------------

    @property
    def planner_ready(self):
        return bool(self.planner_backend and self.mailbox_ready and self.game_data.league
                    and self.game_data.mailbox.get("directive"))

    def entered_league(self, session_id, event):
        """True when this map change takes the player into EverGrandeCity_PokemonLeague_1F."""
        name = self.game_data.map_name(event.get("mapGroup"), event.get("mapNum"))
        previous, self.last_map[session_id] = self.last_map.get(session_id), name
        return name == planner.LEAGUE_ENTRY_MAP and previous != name

    def start_planning(self, session_id):
        """Decide, as the player walks in, whether a new plan is needed. Marks it as being made.

        Walking out and back in without fighting reuses the plan already sent, so a
        Claude plan is paid once per League attempt.
        """
        if session_id in self.planning:
            return False
        sent = self.pushed_plan.get(session_id)
        if sent is not None and not self.store.plan_battles(sent):
            log.info("session %d: League entered again, plan #%d still unused", session_id, sent)
            return False
        self.planning.add(session_id)
        return True

    async def prepare_league_plan(self, session_id, writer, profile_hex=None):
        """Plan the whole League from the profile and history, then send it before the first battle.

        profile_hex is the save's profile read as the player walked in, so the plan matches it.
        """
        try:
            log.info("session %d: League entered, planning (%s)...", session_id, self.planner_backend)
            facts = player_facts(self.store, self.game_data, profile_hex=profile_hex)
            # Claude takes seconds: only the planner runs in the thread; SQLite stays on this one.
            plan = await asyncio.to_thread(planner.build_plan, facts, self.game_data,
                                           self.planner_backend, self.plan_summarizer)
            plan_id = planner.store_plan(self.store, self.game_data, plan, facts)
            for note in plan.notes:
                log.warning("  %s", note)
            log.info("  plan #%d (%s): %s", plan_id, plan.backend, plan.explanation)
            await self.push_plan(session_id, writer)
            await self.push_mailbox(session_id, writer)  # the report now explains the plan
        except ConnectionError:
            pass
        except Exception as error:  # the game falls back to the profile rules on its own
            log.warning("  League plan failed: %s", error)
        finally:
            self.planning.discard(session_id)

    async def push_plan(self, session_id, writer):
        """Send the newest stored plan to one mGBA session if it doesn't have it yet."""
        latest = self.store.latest_plan()
        if latest is None or self.pushed_plan.get(session_id) == latest["id"]:
            return
        writer.write(planner.plan_mailbox_line(latest, self.game_data).encode())
        await writer.drain()
        self.pushed_plan[session_id] = latest["id"]
        log.info("session %d: plan #%d sent (%s)", session_id, latest["id"], latest["backend"])

    def link_plan(self, session_id, event):
        """Record which plan a League battle was fought under, to compare planners later."""
        plan_id = self.pushed_plan.get(session_id)
        if plan_id is None:
            return
        row = self.store.db.execute("SELECT plan FROM plans WHERE id = ?", (plan_id,)).fetchone()
        planned = {entry["trainer_id"] for entry in json.loads(row["plan"])} if row else set()
        if event.get("opponentA") in planned:
            self.store.link_battle_plan(session_id, event["seq"], plan_id)

    async def handle_connection(self, reader, writer):
        session_id = self.store.start_session()
        peer = writer.get_extra_info("peername")
        log.info("session %d: mGBA connected from %s", session_id, peer)
        self.writers[session_id] = writer
        try:
            while line := await reader.readline():
                event = self.handle_line(session_id, line.decode("utf-8", errors="replace").strip())
                if not event or not self.game_data:
                    continue
                if event["type"] == "battle_end":
                    if self.summarizer:
                        await self.summarize_battle(session_id, event)
                    if self.mailbox_ready and self.mailbox_auto:
                        self.refresh_mailbox()
                elif event["type"] == "hello" and self.mailbox_ready and self.mailbox_auto \
                        and self.store.latest_mailbox() is None:
                    self.refresh_mailbox()  # first run: give the game something to show
                if self.mailbox_ready and event["type"] in ("hello", "battle_end"):
                    await self.push_mailbox(session_id, writer)
                if self.planner_ready:
                    if event["type"] == "map_change" and self.entered_league(session_id, event) \
                            and self.start_planning(session_id):
                        # A task, so events keep being read while the plan is made
                        asyncio.create_task(self.prepare_league_plan(session_id, writer, event.get("profile")))
                    elif event["type"] == "hello":
                        await self.push_plan(session_id, writer)  # the game forgets it on reset
                    elif event["type"] == "battle_start":
                        self.link_plan(session_id, event)
        finally:
            log.info("session %d: disconnected", session_id)
            self.writers.pop(session_id, None)
            self.pushed_mailbox.pop(session_id, None)
            self.pushed_plan.pop(session_id, None)
            self.last_map.pop(session_id, None)
            writer.close()


MAILBOX_POLL_SECONDS = 2


async def watch_mailbox(bridge):
    """Pick up texts written by `python3 -m bridge.mailbox` while the server runs."""
    while True:
        await asyncio.sleep(MAILBOX_POLL_SECONDS)
        await bridge.push_mailbox_to_all()


async def serve(host, port, bridge):
    server = await asyncio.start_server(bridge.handle_connection, host, port)
    log.info("listening on %s:%d", host, port)
    watcher = asyncio.create_task(watch_mailbox(bridge)) if bridge.mailbox_ready else None
    try:
        async with server:
            await server.serve_forever()
    finally:
        if watcher:
            watcher.cancel()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--db", default=str(DEFAULT_DB))
    parser.add_argument("--summarizer", choices=["rules", "claude", "none"], default="rules",
                        help="summarize each finished battle (claude needs ANTHROPIC_API_KEY and costs per battle)")
    parser.add_argument("--model", default="claude-opus-5-5", help="model for --summarizer claude")
    parser.add_argument("--no-mailbox-auto", action="store_true",
                        help="don't rewrite the mailbox (rules) after each battle; only send what bridge.mailbox stores")
    parser.add_argument("--planner", choices=["rules", "claude", "none"], default="rules",
                        help="plan the League when the player walks into it (claude costs one call per attempt)")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
    try:
        game_data = GameData.load()
    except FileNotFoundError:
        log.warning("build/game_data.json not found: run gen_tracker.py; events will be stored undecoded")
        game_data = None

    summarizer = None
    if args.summarizer != "none":
        options = {"model": args.model} if args.summarizer == "claude" else {}
        try:
            summarizer = get_summarizer(args.summarizer, **options)
        except SummaryError as error:
            parser.exit(1, f"{error}\n")

    plan_summarizer = None
    if args.planner == "claude":
        try:
            from .summarizers import ClaudeSummarizer
            plan_summarizer = ClaudeSummarizer(model=args.model, language="Spanish")
        except SummaryError as error:
            parser.exit(1, f"{error}\n")

    store = Store(args.db)
    try:
        bridge = Bridge(store, game_data, summarizer, mailbox_auto=not args.no_mailbox_auto,
                        planner_backend=None if args.planner == "none" else args.planner,
                        plan_summarizer=plan_summarizer)
        asyncio.run(serve(args.host, args.port, bridge))
    except KeyboardInterrupt:
        pass
    finally:
        store.close()


if __name__ == "__main__":
    main()
