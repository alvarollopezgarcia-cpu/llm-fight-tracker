"""Tests for the League planner: validation, the bytes the game reads, fallback and the League trigger.

    python3 -m unittest discover tests
"""

import asyncio
import json
import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

TRACKER_DIR = Path(__file__).resolve().parent.parent
FIXTURES = Path(__file__).resolve().parent / "fixtures"
REPO_ROOT = FIXTURES if FIXTURES.exists() else TRACKER_DIR.parent.parent
sys.path.insert(0, str(TRACKER_DIR))

import gen_tracker  # noqa: E402
from bridge import planner  # noqa: E402
from bridge.game_data import GameData  # noqa: E402
from bridge.server import Bridge  # noqa: E402
from bridge.store import Store  # noqa: E402

TELEMETRY_HEADER = (REPO_ROOT / "include/telemetry.h").read_text(encoding="utf-8")
CHARMAP = gen_tracker.parse_charmap((REPO_ROOT / "charmap.txt").read_text(encoding="utf-8"))
WIDTHS = gen_tracker.parse_glyph_widths((REPO_ROOT / "src/fonts.c").read_text(encoding="utf-8"))


def mon(slot, species):
    return {"slot": slot, "species": species, "item": None, "ability": None, "moves": ["TACKLE"]}


LEAGUE = [
    # A doubles trainer: two leads, last slot is the ace
    {"trainer": "TRAINER_SIDNEY", "id": 261, "name": "SIDNEY", "leads": 2, "ace_slots": 1,
     "party": [mon(i, f"SPECIES_{i}") for i in range(6)]},
    # One of the duo: a single trainer with three mons and no ace
    {"trainer": "TRAINER_LLOPEZ_DUO", "id": 855, "name": "LLOPEZ", "leads": 1, "ace_slots": 0,
     "party": [mon(i, f"SPECIES_{i}") for i in range(3)]},
]
LEAGUE_MAP = (16, 10)


def game_data():
    return GameData({
        "build": "test", "profile": {}, "battle_type_flags": {}, "trainers": {"261": "TRAINER_SIDNEY", "855": "TRAINER_LLOPEZ_DUO"},
        "battle_outcomes": {"1": "B_OUTCOME_WON", "2": "B_OUTCOME_LOST"},
        "maps": {"16,10": planner.LEAGUE_ENTRY_MAP, "0,0": "EverGrandeCity"},
        "mailbox": gen_tracker.mailbox_layout(TELEMETRY_HEADER), "charmap": CHARMAP, "glyph_widths": WIDTHS,
        "league": LEAGUE,
    })


def entry(trainer="TRAINER_SIDNEY", **values):
    base = {"trainer": trainer, "lead_slots": [], "switch_prediction": -1, "punish": [], "focus_target": "NONE",
            "aggression": 0, "self_setup": 0, "reason": ""}
    base.update(values)
    return base


class FakeClaude:
    """Stands in for ClaudeSummarizer.ask: returns a fixed answer."""

    def __init__(self, answer):
        self.answer = answer
        self.calls = []

    def ask(self, task, facts, output_format=None):
        self.calls.append((task, facts, output_format))
        if isinstance(self.answer, Exception):
            raise self.answer
        return SimpleNamespace(text=self.answer, model="fake", input_tokens=10, output_tokens=5)


class ValidationTests(unittest.TestCase):
    def setUp(self):
        self.data = game_data()

    def test_good_plan_passes_with_trainer_ids(self):
        trainers = planner.validate_plan({"trainers": [entry(lead_slots=[4, 1], punish=["SETUP", "SETUP"], aggression=2),
                                                       entry("TRAINER_LLOPEZ_DUO", lead_slots=[2])]}, self.data)
        self.assertEqual([t["trainer_id"] for t in trainers], [261, 855])
        self.assertEqual(trainers[0]["punish"], ["SETUP"])

    def test_out_of_bounds_values_are_rejected(self):
        bad = [
            entry(lead_slots=[5, 0]),                    # slot 5 is the ace
            entry(lead_slots=[1]),                       # doubles needs two leads
            entry(lead_slots=[1, 1]),                    # same mon twice
            entry("TRAINER_LLOPEZ_DUO", lead_slots=[3]),  # past the party
            entry(switch_prediction=101),
            entry(aggression=4),
            entry(self_setup=-4),
            entry(punish=["EVERYTHING"]),
            entry(focus_target="RANDOM"),
            entry("TRAINER_NOBODY"),
        ]
        for plan in bad:
            with self.subTest(plan=plan), self.assertRaises(planner.PlanError):
                planner.validate_plan({"trainers": [plan]}, self.data)
        with self.assertRaises(planner.PlanError):
            planner.validate_plan({"trainers": [entry(), entry()]}, self.data)  # twice


class EncodingTests(unittest.TestCase):
    def test_bytes_match_struct_ai_directive(self):
        data = game_data()
        directive = data.mailbox["directive"]
        trainers = planner.validate_plan({"trainers": [
            entry(lead_slots=[4, 1], switch_prediction=80, punish=["SETUP", "FOCUS"], focus_target="LOWEST_HP",
                  aggression=-2, self_setup=3),
            entry("TRAINER_LLOPEZ_DUO")]}, data)
        raw = planner.encode_directive(trainers, data, plan_id=7)

        self.assertEqual(len(raw), data.mailbox["directive_size"])
        self.assertEqual(len(raw), 8 + 12 * directive["max_trainers"])
        self.assertEqual(struct.unpack_from("<IHB", raw), (0x4E414C50, 7, 2))
        self.assertEqual(struct.unpack_from("<HHBBBBbb", raw, 8),
                         (261, directive["punish"]["SETUP"] | directive["punish"]["FOCUS"], 4, 1, 80,
                          directive["focus"].index("LOWEST_HP"), -2, 3))
        # No leads and the profile's switch rate: both "keep" (0xFF)
        self.assertEqual(struct.unpack_from("<HHBBB", raw, 20), (855, 0, 0xFF, 0xFF, 0xFF))
        self.assertEqual(raw[32:], bytes(len(raw) - 32))  # unused entries stay zero

    def test_layout_offsets_follow_the_header(self):
        layout = game_data().mailbox
        self.assertEqual(layout["directive_offset"], 4 + layout["report_size"] + layout["line_size"])
        self.assertEqual(layout["directive"]["magic"], 0x4E414C50)


class PlannerTests(unittest.TestCase):
    TENDENCIES = [
        {"habit": "SETUP_ENTRY_SOUND", "rate": 0.9, "hits": 9, "chances": 10},
        {"habit": "PROTECT_REPEAT", "rate": 0.1, "hits": 1, "chances": 10},
        {"habit": "STATUS_OPEN_VALID", "rate": 0.5, "hits": 5, "chances": 10},  # 50%: says nothing
        {"habit": "HEAL_AT_LOW_HP", "rate": 0.1, "hits": 1, "chances": 10},     # rarely heals: nothing to punish
    ]

    def setUp(self):
        self.data = game_data()
        self.facts = {"profile": {"tendencies": self.TENDENCIES}}

    def test_rules_plan_punishes_only_clear_habits(self):
        plan = planner.rules_plan(self.facts, self.data)
        self.assertEqual(plan.backend, "rules")
        for trainer in plan.trainers:
            self.assertEqual(trainer["punish"], ["PROTECT_REPEAT", "SETUP"])
            self.assertEqual(trainer["lead_slots"], [])

    def test_rules_plan_without_a_profile_is_empty(self):
        plan = planner.rules_plan({"profile": None}, self.data)
        self.assertTrue(all(not t["punish"] for t in plan.trainers))

    def test_claude_plan_is_validated_and_used(self):
        answer = json.dumps({"explanation": "Te esperan.", "trainers": [entry(lead_slots=[0, 2], aggression=1)]})
        claude = FakeClaude(answer)
        plan = planner.build_plan(self.facts, self.data, "claude", claude)
        self.assertEqual((plan.backend, plan.model, plan.trainers[0]["lead_slots"]), ("claude", "fake", [0, 2]))
        task, facts, output_format = claude.calls[0]
        self.assertEqual(output_format["type"], "json_schema")
        self.assertEqual([t["trainer"] for t in facts["league"]], ["TRAINER_SIDNEY", "TRAINER_LLOPEZ_DUO"])

    def test_bad_or_failed_claude_plan_falls_back_to_rules(self):
        for answer in [json.dumps({"explanation": "x", "trainers": [entry(lead_slots=[5, 0])]}),  # ace lead
                       "not json", RuntimeError("API down")]:
            with self.subTest(answer=answer):
                plan = planner.build_plan(self.facts, self.data, "claude", FakeClaude(answer))
                self.assertEqual(plan.backend, "rules-fallback")
                self.assertTrue(plan.notes)

    def test_stored_plan_explains_itself_in_the_report(self):
        store = Store(":memory:")
        plan_id, plan = planner.make_plan(store, self.data, "rules")
        row = store.latest_plan()
        self.assertEqual((row["id"], row["backend"]), (plan_id, "rules"))
        self.assertEqual(json.loads(row["plan"]), plan.trainers)
        report = store.latest_mailbox()
        self.assertEqual(report["backend"], "plan-rules")
        self.assertIn(plan.explanation, report["report"])
        line = planner.plan_mailbox_line(row, self.data)
        self.assertTrue(line.startswith("MAILBOX directive ") and line.endswith("\n"))


class LeagueTriggerTests(unittest.TestCase):
    def test_plan_is_sent_on_entering_the_league_and_battles_are_linked(self):
        store = Store(":memory:")
        data = game_data()
        bridge = Bridge(store, data, mailbox_auto=False)

        def send(writer, seq, **event):
            writer.write((json.dumps({"seq": seq, **event}) + "\n").encode())

        async def read_until(reader, prefix):
            while True:
                line = await asyncio.wait_for(reader.readline(), 2)
                if line.startswith(prefix):
                    return line

        async def scenario():
            server = await asyncio.start_server(bridge.handle_connection, "127.0.0.1", 0)
            port = server.sockets[0].getsockname()[1]
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            send(writer, 1, type="hello", build="test")
            send(writer, 2, type="map_change", mapGroup=0, mapNum=0)
            send(writer, 3, type="map_change", mapGroup=LEAGUE_MAP[0], mapNum=LEAGUE_MAP[1])
            await writer.drain()
            directive = await read_until(reader, b"MAILBOX directive ")
            report = await read_until(reader, b"MAILBOX report ")

            # Out and back in without fighting: the same plan stays, nothing new is made
            send(writer, 4, type="map_change", mapGroup=0, mapNum=0)
            send(writer, 5, type="map_change", mapGroup=LEAGUE_MAP[0], mapNum=LEAGUE_MAP[1])
            # A League battle under the plan
            send(writer, 6, type="battle_start", opponentA=261, opponentB=0, battleTypeFlags=0)
            send(writer, 7, type="battle_end", outcome=2, turns=9)
            await writer.drain()
            for _ in range(50):
                if store.plan_battles(1):
                    break
                await asyncio.sleep(0.02)
            writer.close()
            server.close()
            return directive, report

        directive, report = asyncio.run(scenario())
        row = store.latest_plan()
        self.assertEqual(len(store.plans()), 1)
        self.assertEqual(directive.decode(), planner.plan_mailbox_line(row, data))
        self.assertTrue(report.startswith(b"MAILBOX report "))
        results = planner.plan_results(store, data)
        self.assertEqual(results[0]["battles"], [{"trainer": "TRAINER_SIDNEY", "outcome": "B_OUTCOME_LOST", "turns": 9}])
        self.assertEqual(planner.backend_totals(results)["rules"]["player_lost"], 1)


@unittest.skipUnless((REPO_ROOT / "src/data/trainers.party").exists(), "needs the hack's trainers.party")
class LeagueTeamsTests(unittest.TestCase):
    def test_documented_league_teams_parse(self):
        text = (REPO_ROOT / "src/data/trainers.party").read_text(encoding="utf-8")
        teams = gen_tracker.league_teams(text, {})
        self.assertEqual([t["trainer"] for t in teams], gen_tracker.LEAGUE_TRAINERS)
        for team in teams:
            self.assertTrue(team["party"], team["trainer"])
            self.assertTrue(all(m["moves"] for m in team["party"]), team["trainer"])
            self.assertLess(team["ace_slots"], len(team["party"]))


if __name__ == "__main__":
    unittest.main()
