"""Tests for the generator and the bridge. Run from tools/llm_tracker:

    python3 -m unittest discover tests
"""

import asyncio
import json
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

TRACKER_DIR = Path(__file__).resolve().parent.parent
# The hack's sources: the real repository when the tracker lives in tools/llm_tracker,
# else the trimmed copies in tests/fixtures (the standalone repository).
FIXTURES = Path(__file__).resolve().parent / "fixtures"
REPO_ROOT = FIXTURES if FIXTURES.exists() else TRACKER_DIR.parent.parent
sys.path.insert(0, str(TRACKER_DIR))

import gen_tracker  # noqa: E402
from bridge.facts import battle_facts, healing_facts, leveling_facts, player_facts, telemetry_battle_facts, trends  # noqa: E402
from bridge.game_data import GameData, profile_delta  # noqa: E402
from bridge.habits import HABITS  # noqa: E402
from bridge.server import Bridge  # noqa: E402
from bridge.store import Store  # noqa: E402
from bridge.summarizers import ClaudeSummarizer, RulesSummarizer, SummaryError, estimate_cost  # noqa: E402

PROFILE_HEADER = """
#define PLAYER_PROFILE_MAGIC            0x5050  // "PP"
#define PLAYER_PROFILE_VERSION          1
#define PROFILE_ACE_SLOTS               6       // Mons tracked in the ace table.
enum PlayerProfileStat
{
    // Switching
    PROFILE_SWITCH_T1,
    PROFILE_SWITCH_T2,
    PROFILE_STAT_COUNT,
};
enum PlayerProfileStatusKind { PROFILE_STATUS_PARALYSIS, PROFILE_STATUS_BURN, PROFILE_STATUS_KIND_COUNT, };
enum PlayerProfileFocusTarget { PROFILE_FOCUS_THREAT, PROFILE_FOCUS_TARGET_COUNT, };
"""


TELEMETRY_HEADER = (REPO_ROOT / "include/telemetry.h").read_text(encoding="utf-8")


def make_game_data():
    layout = gen_tracker.profile_layout(PROFILE_HEADER)
    return GameData({
        "build": "test",
        "profile": layout,
        "telemetry": gen_tracker.telemetry_layout(TELEMETRY_HEADER),
        "battle_type_flags": {"1": "BATTLE_TYPE_DOUBLE", "8": "BATTLE_TYPE_TRAINER"},
        "battle_outcomes": {"1": "B_OUTCOME_WON"},
        "trainers": {"5": "TRAINER_ROXANNE_1"},
        "maps": {"0,1": "Route101"},
        "moves": {"89": "EARTHQUAKE", "98": "QUICK_ATTACK"},
        "species": {"219": "MAGCARGO", "445": "GARCHOMP", "323": "CAMERUPT"},
        "items": {"68": "RARE_CANDY", "26": "HYPER_POTION"},
        "battle_actions": {"0": "USE_MOVE", "1": "USE_ITEM", "2": "SWITCH", "3": "RUN"},
    })


def telemetry(kind, arg=0, a=0, b=0, c=0, seq=None):
    kinds = gen_tracker.telemetry_layout(TELEMETRY_HEADER)["event_types"]
    event = {"type": "telemetry", "kind": kinds.index(kind), "arg": arg, "a": a, "b": b, "c": c}
    if seq is not None:
        event["seq"] = seq
    return event


def make_profile_hex(layout, battles, stats):
    raw = bytearray(layout["size"])
    raw[0:2] = layout["magic"].to_bytes(2, "little")
    raw[2] = layout["version"]
    raw[3] = battles
    for index, (hits, chances) in enumerate(stats):
        raw[layout["stats_offset"] + layout["stat_stride"] * index] = hits
        raw[layout["stats_offset"] + layout["stat_stride"] * index + 1] = chances
    return raw.hex()


class GeneratorTests(unittest.TestCase):
    def test_parses_symbols_from_map(self):
        map_text = (
            " .bss           0x02000000      0x10 build/emerald/src/main.o\n"
            "                0x030066d0                gMain\n"
            "                0x020000cc                gBattleTypeFlags\n"
            "                0x020000d0                gOther\n"
        )
        symbols = gen_tracker.parse_map_symbols(map_text, {"gMain", "gBattleTypeFlags"})
        self.assertEqual(symbols, {"gMain": 0x030066D0, "gBattleTypeFlags": 0x020000CC})

    def test_profile_layout_matches_struct(self):
        layout = gen_tracker.profile_layout(PROFILE_HEADER)
        self.assertEqual(layout["stats"], ["PROFILE_SWITCH_T1", "PROFILE_SWITCH_T2"])
        self.assertEqual(layout["stats_offset"], 8)
        self.assertEqual(layout["stat_stride"], 4)  # -mabi=apcs-gnu pads struct ProfileCounter to 4 bytes
        self.assertEqual(layout["status_offset"], 16)
        self.assertEqual(layout["focus_offset"], 18)
        self.assertEqual(layout["ace_personality_offset"], 20)  # 19 rounded up to 4
        self.assertEqual(layout["ace_turns_offset"], 44)
        self.assertEqual(layout["size"], 52)

    def test_real_header_layout(self):
        header = (REPO_ROOT / "include/player_profile.h").read_text()
        layout = gen_tracker.profile_layout(header)
        self.assertEqual(len(layout["stats"]), 43)
        self.assertEqual(layout["size"], 220)
        self.assertEqual(set(layout["stats"]), set(HABITS), "every habit needs a description in bridge/habits.py")

    def test_decodes_profile_captured_from_mgba(self):
        # First 136 bytes of a real profile read from mGBA: every counter is followed by 2 padding bytes.
        captured = bytes.fromhex(
            "505001767600010821b700003e7900000a120000385300000c4b00000419000004a200001da200000a110000"
            "12c400001bc100001356000021570000000000000145000000000000000000000b5c0000042f000002100000"
            "0f6c0000037900000206000001040000638d00000e120000030800000a5300000b1e00000e1200000a200000"
            "00700000")
        layout = gen_tracker.profile_layout((REPO_ROOT / "include/player_profile.h").read_text())
        data = GameData({"build": "test", "profile": layout, "battle_type_flags": {}, "battle_outcomes": {},
                         "trainers": {}, "maps": {}})
        profile = data.decode_profile((captured + bytes(layout["size"] - len(captured))).hex())
        self.assertEqual(profile["stats"]["PROFILE_SWITCH_T1"], {"hits": 33, "chances": 183})
        self.assertEqual(profile["stats"]["PROFILE_SWITCH_T2"], {"hits": 62, "chances": 121})

    def test_lua_template_is_fully_filled(self):
        template = (TRACKER_DIR / "lua/tracker_template.lua").read_text()
        symbols = {name: 0x02000000 + i for i, name in enumerate(gen_tracker.REQUIRED_SYMBOLS)}
        profile = gen_tracker.profile_layout(PROFILE_HEADER)
        telemetry_layout = gen_tracker.telemetry_layout(TELEMETRY_HEADER)
        mailbox_layout = gen_tracker.mailbox_layout(TELEMETRY_HEADER)
        lua = gen_tracker.render_lua(template, symbols, profile, telemetry_layout, mailbox_layout, "abc")
        self.assertNotIn("{{", lua)
        self.assertIn("gMain = 0x02000000", lua)
        self.assertIn("gTelemetry = nil", lua)  # release build: no buffer
        self.assertIn("gMailbox = nil", lua)
        lua = gen_tracker.render_lua(template, {**symbols, "gTelemetry": 0x02030000, "gMailbox": 0x02031000},
                                     profile, telemetry_layout, mailbox_layout, "abc")
        self.assertIn("gTelemetry = 0x02030000", lua)
        self.assertIn("gMailbox = 0x02031000", lua)
        self.assertIn("champion = {offset = 1004, size = 300}", lua)
        self.assertIn("TELEMETRY_MAGIC = 0x4D4C4554", lua)

    def test_battle_type_flags_parse_shifts(self):
        defines = gen_tracker.parse_defines("#define BATTLE_TYPE_TRAINER            (1 << 3)\n", "BATTLE_TYPE_")
        self.assertEqual(defines, {8: "BATTLE_TYPE_TRAINER"})

    def test_enum_values_handle_aliases_markers_and_macros(self):
        header = """
        enum __attribute__((packed)) Move
        {
            MOVE_NONE = 0,
            MOVE_POUND = 1,
            MOVE_DOUBLESLAP = MOVE_POUND, // old name: same value, first name wins
            MOVES_COUNT_GEN1,             // marker without the prefix: numbers it, names nothing
            MOVE_SKETCH = MOVES_COUNT_GEN1,
            MOVE_TRIPLE_KICK,
            RECURSIVELY(FOREACH_TM(APPEND)) // macro: numbering lost
            MOVE_AFTER_MACRO,
            MOVE_EXPLICIT = 10,
        };"""
        self.assertEqual(gen_tracker.parse_enum_values(header, "Move", "MOVE_"),
                         {0: "MOVE_NONE", 1: "MOVE_POUND", 2: "MOVE_SKETCH", 3: "MOVE_TRIPLE_KICK", 10: "MOVE_EXPLICIT"})

    def test_real_name_tables(self):
        root = REPO_ROOT
        moves = gen_tracker.parse_enum_values((root / "include/constants/moves.h").read_text(encoding="utf-8"), "Move", "MOVE_")
        species = gen_tracker.parse_enum_values((root / "include/constants/species.h").read_text(encoding="utf-8"),
                                                "Species", "SPECIES_")
        items = gen_tracker.parse_enum_values((root / "include/constants/items.h").read_text(encoding="utf-8"), "Item", "ITEM_")
        self.assertEqual((moves[89], moves[166], species[1], items[102]),
                         ("MOVE_EARTHQUAKE", "MOVE_SKETCH", "SPECIES_BULBASAUR", "ITEM_RARE_CANDY"))

    def test_telemetry_layout_matches_header(self):
        layout = gen_tracker.telemetry_layout(TELEMETRY_HEADER)
        self.assertEqual(layout["event_types"][:3], ["NONE", "ACTION", "MOVE_RESULT"])
        self.assertEqual(len(layout["event_types"]), 11)  # NONE + the 10 events
        self.assertEqual(layout["sources"], ["BAG", "POKENAV", "CENTER"])
        self.assertEqual(layout["capacity"] & (layout["capacity"] - 1), 0)


class TelemetryTests(unittest.TestCase):
    def setUp(self):
        self.data = make_game_data()

    def test_decode_each_kind(self):
        decode = self.data.decode_telemetry
        self.assertEqual(decode(telemetry("ACTION", arg=0, a=0, b=89, c=3)),
                         {"kind": "ACTION", "side": "player", "action": "USE_MOVE", "turn": 3, "move": "EARTHQUAKE"})
        self.assertEqual(decode(telemetry("MOVE_RESULT", arg=0 | (1 << 4), a=89, b=1 | 0x80, c=3)),
                         {"kind": "MOVE_RESULT", "side": "player", "target_side": "opponent", "move": "EARTHQUAKE",
                          "turn": 3, "result": "SUPER_EFFECTIVE", "crit": True})
        self.assertEqual(decode(telemetry("FAINT", arg=1, a=219, b=0, c=3)),
                         {"kind": "FAINT", "side": "opponent", "species": "MAGCARGO", "turn": 3, "by_side": "player"})
        self.assertEqual(decode(telemetry("RARE_CANDY", arg=1, a=445, b=40, c=5)),
                         {"kind": "RARE_CANDY", "source": "POKENAV", "species": "GARCHOMP", "level": 40, "candies": 5})
        self.assertEqual(decode(telemetry("CAP_BLOCKED", arg=0, a=445, b=40)),
                         {"kind": "CAP_BLOCKED", "source": "BAG", "species": "GARCHOMP", "level_cap": 40})
        self.assertEqual(decode(telemetry("WHITEOUT", a=(0 << 8) | 1))["map"], "Route101")

    def test_healing_facts(self):
        events = [telemetry("HEAL", arg=2, a=1, b=40, c=1),   # Center, a mon fainted
                  telemetry("HEAL", arg=1, a=1, b=100, c=0),  # PokéNav at full HP
                  telemetry("WHITEOUT", a=1)]
        self.assertEqual(self.data.decode_telemetry(events[0]),
                         {"kind": "HEAL", "source": "CENTER", "map": "Route101", "party_hp_percent": 40, "fainted": 1})
        self.assertEqual(healing_facts(events, self.data),
                         {"heals": {"CENTER": 1, "POKENAV": 1}, "average_party_hp_percent_before": 70,
                          "heals_with_fainted_pokemon": 1, "heals_at_full_hp": 1})
        self.assertIsNone(healing_facts([events[2]], self.data))

    def test_battle_telemetry_is_scoped_to_the_battle(self):
        store = Store(":memory:")
        session = store.start_session()
        events = [
            telemetry("RARE_CANDY", arg=0, a=445, b=39, c=1, seq=1),           # before the battle
            {"seq": 2, "type": "battle_start", "opponentA": 5},
            telemetry("ACTION", arg=0, a=0, b=89, c=1, seq=3),
            telemetry("MOVE_RESULT", arg=0 | (1 << 4), a=89, b=1, c=1, seq=4),
            telemetry("MOVE_RESULT", arg=1 | (0 << 4), a=98, b=0 | 0x80, c=1, seq=5),
            telemetry("FAINT", arg=1, a=219, b=0, c=1, seq=6),
            telemetry("LEVEL_UP", arg=0, a=445, b=40, seq=7),
            {"seq": 8, "type": "battle_end", "outcome": 1, "turns": 1},
            telemetry("CAP_BLOCKED", arg=0, a=445, b=40, seq=9),               # after the battle
        ]
        for event in events:
            store.add_event(session, event)
        battle = store.battles()[0]

        facts = telemetry_battle_facts(store.telemetry(battle), self.data)
        self.assertEqual(facts["player_moves"], {"EARTHQUAKE": 1})
        self.assertEqual(facts["player_move_results"], {"SUPER_EFFECTIVE": 1})
        self.assertEqual(facts["opponent_moves"], {"QUICK_ATTACK": 1})
        self.assertEqual(facts["crits"], {"dealt": 0, "taken": 1})
        self.assertEqual(facts["fainted"], {"player": [], "opponent": ["MAGCARGO"]})
        self.assertEqual(facts["level_ups"], ["GARCHOMP to 40"])

        leveling = leveling_facts(store.telemetry(), self.data)
        self.assertEqual(leveling, {"rare_candies": {"BAG": 1}, "levels_from_battle": 1,
                                    "candy_refused_by_level_cap": 1, "whiteouts": []})

        text = RulesSummarizer().summarize_battle(battle_facts(battle, self.data, store.telemetry(battle))).text
        self.assertIn("Moves chosen: EARTHQUAKE x1.", text)
        store.close()


class OpponentTests(unittest.TestCase):
    def test_stale_trainer_ids_ignored_in_wild_battles(self):
        data = GameData({"build": "t", "profile": {}, "battle_type_flags": {"8": "BATTLE_TYPE_TRAINER",
                                                                            "32768": "BATTLE_TYPE_TWO_OPPONENTS"},
                         "battle_outcomes": {}, "trainers": {"5": "TRAINER_AUTUMN", "6": "TRAINER_JULIO"}, "maps": {}})
        self.assertEqual(data.opponents(0, 5, 6), ("WILD", None))              # wild after a double trainer battle
        self.assertEqual(data.opponents(8, 5, 6), ("TRAINER_AUTUMN", None))    # single: opponent B is stale
        self.assertEqual(data.opponents(8 | 32768, 5, 6), ("TRAINER_AUTUMN", "TRAINER_JULIO"))


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.data = make_game_data()
        self.layout = self.data.profile

    def test_decode_profile(self):
        profile = self.data.decode_profile(make_profile_hex(self.layout, 3, [(1, 2), (0, 4)]))
        self.assertEqual(profile["battles_recorded"], 3)
        self.assertEqual(profile["stats"]["PROFILE_SWITCH_T2"], {"hits": 0, "chances": 4})

    def test_delta_shows_what_one_battle_added(self):
        before = self.data.decode_profile(make_profile_hex(self.layout, 3, [(1, 2), (0, 4)]))
        after = self.data.decode_profile(make_profile_hex(self.layout, 4, [(2, 3), (0, 4)]))
        self.assertEqual(profile_delta(before, after), {"PROFILE_SWITCH_T1": {"hits": 1, "chances": 1}})

    def test_delta_marks_decayed_stats_unknown(self):
        before = self.data.decode_profile(make_profile_hex(self.layout, 3, [(100, 199), (0, 0)]))
        after = self.data.decode_profile(make_profile_hex(self.layout, 4, [(50, 100), (0, 0)]))
        self.assertEqual(profile_delta(before, after), {"PROFILE_SWITCH_T1": None})


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.store = Store(":memory:")
        self.session = self.store.start_session()

    def tearDown(self):
        self.store.close()

    def test_duplicate_seq_is_ignored(self):
        event = {"seq": 1, "type": "hello", "build": "abc"}
        self.assertTrue(self.store.add_event(self.session, event))
        self.assertFalse(self.store.add_event(self.session, event))

    def test_battle_row_is_built_from_start_and_end(self):
        self.store.add_event(self.session, {"seq": 1, "type": "battle_start", "opponentA": 5, "battleTypeFlags": 8})
        self.store.add_event(self.session, {"seq": 2, "type": "battle_end", "outcome": 1, "turns": 7})
        battles = self.store.battles()
        self.assertEqual(len(battles), 1)
        self.assertEqual((battles[0]["trainer_a"], battles[0]["outcome"], battles[0]["turns"]), (5, 1, 7))

    def test_rewind_discards_unfinished_battle(self):
        self.store.add_event(self.session, {"seq": 1, "type": "battle_start", "opponentA": 5})
        self.store.add_event(self.session, {"seq": 2, "type": "rewind", "fromFrame": 900})
        self.assertEqual(self.store.battles(finished_only=False), [])


class ServerTests(unittest.TestCase):
    def test_events_flow_from_socket_to_database(self):
        store = Store(":memory:")
        bridge = Bridge(store, make_game_data())

        async def scenario():
            server = await asyncio.start_server(bridge.handle_connection, "127.0.0.1", 0)
            port = server.sockets[0].getsockname()[1]
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            for event in [
                {"seq": 1, "type": "hello", "build": "test"},
                {"seq": 2, "type": "battle_start", "opponentA": 5, "battleTypeFlags": 9, "mapGroup": 0, "mapNum": 1},
                {"seq": 2, "type": "battle_start", "opponentA": 5},  # duplicate, ignored
                {"seq": 3, "type": "battle_end", "outcome": 1, "turns": 4},
            ]:
                writer.write((json.dumps(event) + "\n").encode())
            writer.write(b"not json\n")
            await writer.drain()
            writer.close()
            await writer.wait_closed()
            for _ in range(50):
                if store.battles():
                    break
                await asyncio.sleep(0.01)
            server.close()
            await server.wait_closed()

        asyncio.run(scenario())
        counts = {row["type"]: row["n"] for row in store.event_counts()}
        self.assertEqual(counts, {"hello": 1, "battle_start": 1, "battle_end": 1})
        self.assertEqual(len(store.battles()), 1)
        store.close()

    def test_finished_battle_is_summarized(self):
        store = Store(":memory:")
        data = make_game_data()
        bridge = Bridge(store, data, RulesSummarizer())
        session = store.start_session()
        before = make_profile_hex(data.profile, 3, [(1, 2), (0, 4)])
        after = make_profile_hex(data.profile, 4, [(2, 3), (0, 4)])

        async def scenario():
            bridge.handle_line(session, json.dumps({"seq": 1, "type": "battle_start", "opponentA": 5, "battleTypeFlags": 8, "profile": before}))
            event = bridge.handle_line(session, json.dumps({"seq": 2, "type": "battle_end", "outcome": 1, "turns": 4,
                                                            "profile": after}))
            return await bridge.summarize_battle(session, event)

        summary = asyncio.run(scenario())
        self.assertIn("Won against TRAINER_ROXANNE_1", summary.text)
        self.assertEqual(len(store.summaries("battle")), 1)
        store.close()


def add_battle(store, session, seq, before, after, outcome=1):
    store.add_event(session, {"seq": seq, "type": "battle_start", "opponentA": 5, "mapGroup": 0, "mapNum": 1,
                              "battleTypeFlags": 8, "profile": before})
    store.add_event(session, {"seq": seq + 1, "type": "battle_end", "outcome": outcome, "turns": 6, "profile": after})


class FactsTests(unittest.TestCase):
    def setUp(self):
        self.data = make_game_data()
        self.store = Store(":memory:")
        self.session = self.store.start_session()

    def tearDown(self):
        self.store.close()

    def hex(self, battles, stats):
        return make_profile_hex(self.data.profile, battles, stats)

    def test_battle_facts_compare_with_usual_rate(self):
        add_battle(self.store, self.session, 1, self.hex(3, [(1, 4), (0, 0)]), self.hex(4, [(3, 6), (0, 0)]))
        facts = battle_facts(self.store.battles()[0], self.data)
        self.assertEqual(facts["trainer"], "TRAINER_ROXANNE_1")
        self.assertTrue(facts["recorded_by_profile"])
        habit = facts["habits"][0]
        self.assertEqual((habit["habit"], habit["hits"], habit["chances"], habit["rate"], habit["usual_rate"]),
                         ("SWITCH_T1", 2, 2, 1.0, 0.25))

    def test_player_facts_split_tendencies_by_sample_size(self):
        add_battle(self.store, self.session, 1, self.hex(9, [(5, 10), (1, 3)]), self.hex(10, [(6, 12), (1, 4)]))
        facts = player_facts(self.store, self.data, min_samples=10)
        self.assertEqual([t["habit"] for t in facts["profile"]["tendencies"]], ["SWITCH_T1"])
        self.assertEqual(facts["profile"]["too_few_samples"], ["SWITCH_T2"])
        self.assertEqual(facts["outcomes"], {"B_OUTCOME_WON": 1})

    def test_trend_detects_a_changed_habit(self):
        # Early battles: always switches at T1. Late battles: never.
        stats, seq = [0, 0], 1
        for hits in (5, 5, 0, 0):
            before = self.hex(seq, [(stats[0], stats[1]), (0, 0)])
            stats = [stats[0] + hits, stats[1] + 5]
            add_battle(self.store, self.session, seq, before, self.hex(seq + 1, [(stats[0], stats[1]), (0, 0)]))
            seq += 2
        trend = trends(self.store.battles(), self.data)
        self.assertEqual(trend, [{"habit": "SWITCH_T1", "early_rate": 1.0, "late_rate": 0.0,
                                  "early_chances": 10, "late_chances": 10}])


class RulesSummarizerTests(unittest.TestCase):
    def test_battle_summary_flags_unusual_behavior(self):
        facts = {"outcome": "B_OUTCOME_LOST", "trainer": "TRAINER_ROXANNE_1", "partner_trainer": None,
                 "map": "Route101", "turns": 9, "recorded_by_profile": True,
                 "habits": [{"habit": "SWITCH_T1", "situation": "s", "behavior": "switched out", "hits": 3,
                             "chances": 3, "rate": 1.0, "usual_rate": 0.2}]}
        text = RulesSummarizer().summarize_battle(facts).text
        self.assertIn("Lost to TRAINER_ROXANNE_1 at Route101 in 9 turns.", text)
        self.assertIn("3 of 3 (usually 20%) - unusual for this player", text)

    def test_player_summary_without_profile(self):
        text = RulesSummarizer().summarize_player({"profile": None}).text
        self.assertIn("No profile recorded yet", text)


class FakeMessages:
    def __init__(self, response):
        self.response = response
        self.requests = []

    def create(self, **request):
        self.requests.append(request)
        return self.response


def fake_client(stop_reason="end_turn", text="Plays safe."):
    response = SimpleNamespace(
        stop_reason=stop_reason, stop_details=None, model="claude-opus-5-5",
        content=[SimpleNamespace(type="thinking", thinking=""), SimpleNamespace(type="text", text=text)],
        usage=SimpleNamespace(input_tokens=2000, output_tokens=400))
    return SimpleNamespace(beta=SimpleNamespace(messages=FakeMessages(response)))


class ClaudeSummarizerTests(unittest.TestCase):
    def test_request_shape_and_result(self):
        client = fake_client()
        summary = ClaudeSummarizer(client=client, language="Spanish").summarize_player({"profile": None})
        request = client.beta.messages.requests[0]
        self.assertEqual(request["model"], "claude-opus-5-5")
        self.assertEqual(request["fallbacks"], "default")
        self.assertEqual(request["betas"], ["server-side-fallback-2026-07-01"])
        self.assertIn("Write in Spanish", request["system"])
        self.assertIn('"profile": null', request["messages"][0]["content"])
        self.assertEqual((summary.text, summary.input_tokens, summary.output_tokens), ("Plays safe.", 2000, 400))
        self.assertAlmostEqual(estimate_cost(summary), 0.016)

    def test_models_without_fallback_skip_it(self):
        client = fake_client()
        ClaudeSummarizer(model="claude-haiku-5-5", client=client).summarize_battle({})
        self.assertNotIn("fallbacks", client.beta.messages.requests[0])

    def test_refusal_raises(self):
        with self.assertRaises(SummaryError):
            ClaudeSummarizer(client=fake_client(stop_reason="refusal")).summarize_battle({})


if __name__ == "__main__":
    unittest.main()
