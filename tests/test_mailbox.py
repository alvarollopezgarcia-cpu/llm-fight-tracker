"""Tests for the mailbox: encoding for the game, the Spanish texts, and sending them to mGBA.

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
from bridge import mailbox  # noqa: E402
from bridge.game_data import GameData  # noqa: E402
from bridge.server import Bridge  # noqa: E402
from bridge.store import Store  # noqa: E402
from bridge.summarizers import ClaudeSummarizer  # noqa: E402

TELEMETRY_HEADER = (REPO_ROOT / "include/telemetry.h").read_text(encoding="utf-8")
CHARMAP = gen_tracker.parse_charmap((REPO_ROOT / "charmap.txt").read_text(encoding="utf-8"))
WIDTHS = gen_tracker.parse_glyph_widths((REPO_ROOT / "src/fonts.c").read_text(encoding="utf-8"))
NEWLINE, SCROLL, PARAGRAPH = CHARMAP["\\n"][0], CHARMAP["\\l"][0], CHARMAP["\\p"][0]


def game_data():
    return GameData({
        "build": "test", "profile": {}, "battle_type_flags": {}, "battle_outcomes": {}, "trainers": {}, "maps": {},
        "mailbox": gen_tracker.mailbox_layout(TELEMETRY_HEADER), "charmap": CHARMAP, "glyph_widths": WIDTHS,
    })


class EncodingTests(unittest.TestCase):
    def setUp(self):
        self.text = mailbox.GameText(CHARMAP, WIDTHS)

    def test_charmap_and_widths_from_the_repo(self):
        self.assertEqual((CHARMAP["a"], CHARMAP["á"], CHARMAP["$"]), ([0xD5], [0x17], [0xFF]))
        self.assertEqual((NEWLINE, SCROLL, PARAGRAPH), (0xFE, 0xFA, 0xFB))
        self.assertEqual(WIDTHS[0xD5], 6)  # 'a' in the message box font

    def test_lines_fit_the_box_and_use_the_right_breaks(self):
        encoded = self.text.encode("palabra " * 40 + "\n\nfin", 1000)
        self.assertEqual(encoded[-1], mailbox.EOS)
        lines, separators, line = [], [], []
        for byte in encoded[:-1]:
            if byte in (NEWLINE, SCROLL, PARAGRAPH):
                lines.append(line)
                separators.append(byte)
                line = []
            else:
                line.append(byte)
        lines.append(line)
        self.assertTrue(all(self.text.width(line) <= mailbox.MESSAGE_BOX_WIDTH for line in lines))
        # first paragraph: line, \n, then \l for the rest; the second paragraph opens with \p
        self.assertEqual(separators[0], NEWLINE)
        self.assertTrue(all(s == SCROLL for s in separators[1:-1]))
        self.assertEqual(separators[-1], PARAGRAPH)

    def test_size_limit_keeps_whole_lines(self):
        encoded = self.text.encode("uno dos tres cuatro cinco seis siete ocho nueve diez " * 10, 60)
        self.assertLessEqual(len(encoded), 60)
        self.assertEqual(encoded[-1], mailbox.EOS)
        self.assertNotIn(CHARMAP[" "][0], encoded[-2:-1])  # ends at a line end, not after a space

    def test_unknown_characters_fall_back_or_drop(self):
        self.assertNotIn("ā", CHARMAP)
        self.assertEqual(self.text.encode_char("ā"), CHARMAP["a"])  # not in the font: accent stripped
        self.assertEqual(self.text.encode_char("😀"), [])           # not in the font


class TextTests(unittest.TestCase):
    FACTS = {
        "profile": {"battles_recorded": 50, "tendencies": [
            {"habit": "HEAL_AT_LOW_HP", "rate": 0.03, "chances": 100},
            {"habit": "SWITCH_T2", "rate": 0.5, "chances": 120},        # 50%: not predictable at all
            {"habit": "LEAD_COUNTERS_ENEMY_LEAD", "rate": 0.9, "chances": 40},
        ]},
        "healing": {"heals": {"CENTER": 1, "POKENAV": 3}, "average_party_hp_percent_before": 60,
                    "heals_with_fainted_pokemon": 0, "heals_at_full_hp": 0},
        "leveling": {"rare_candies": {"POKENAV": 30}, "levels_from_battle": 2, "candy_refused_by_level_cap": 0,
                     "whiteouts": []},
    }

    def test_every_habit_has_spanish_text(self):
        header = (REPO_ROOT / "include/player_profile.h").read_text(encoding="utf-8")
        names = {name.removeprefix("PROFILE_") for name in gen_tracker.profile_layout(header)["stats"]}
        self.assertEqual(names, set(mailbox.HABITS_ES))

    def test_rules_report(self):
        report = mailbox.rules_report(self.FACTS)
        self.assertIn("He analizado 50 combates", report)
        self.assertIn("casi nunca lo usas (3%)", report)
        self.assertNotIn("(50%)", report)
        self.assertIn("1 vez en el Centro y 3 veces con el PokéNav", report)
        self.assertIn("0 a mano y 30 con el PokéNav", report)

    def test_rules_champion_line(self):
        line = mailbox.rules_champion_line(self.FACTS)
        self.assertTrue(line.startswith("LLOPEZ: "))
        self.assertIn("movimiento de curaci", line)
        self.assertIn("Enséñamelo", mailbox.rules_champion_line({"profile": None}))

    def test_claude_writes_both_texts_as_json(self):
        reply = json.dumps({"report": "Juegas con calma.", "champion_line": "Nunca te curas. Hoy lo pagarás."})
        response = SimpleNamespace(stop_reason="end_turn", stop_details=None, model="claude-opus-5-5",
                                   content=[SimpleNamespace(type="text", text=reply)],
                                   usage=SimpleNamespace(input_tokens=3000, output_tokens=200))
        requests = []
        client = SimpleNamespace(beta=SimpleNamespace(messages=SimpleNamespace(
            create=lambda **request: requests.append(request) or response)))
        texts = mailbox.write_with_claude(self.FACTS, ClaudeSummarizer(client=client, language="Spanish"))
        self.assertEqual(texts.champion_line, "LLOPEZ: Nunca te curas. Hoy lo pagarás.")
        self.assertEqual(texts.backend, "claude")
        output_config = requests[0]["output_config"]
        self.assertEqual(output_config["format"]["type"], "json_schema")
        self.assertEqual(output_config["effort"], "low")


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.data = game_data()
        self.store = Store(":memory:")

    def tearDown(self):
        self.store.close()

    def test_wire_lines(self):
        lines = mailbox.mailbox_lines({"report": bytes([0xD5, 0xFF]), "champion": bytes([0xFF])})
        self.assertEqual(lines, ["MAILBOX report d5ff\n", "MAILBOX champion ff\n"])

    def test_auto_refresh_keeps_claude_texts(self):
        bridge = Bridge(self.store, self.data)
        bridge.refresh_mailbox()
        self.assertEqual(self.store.latest_mailbox()["backend"], "rules")
        self.store.add_mailbox(mailbox.MailboxTexts("hola", "LLOPEZ: hola", "claude"), {})
        self.assertIsNone(bridge.refresh_mailbox())  # Claude texts were written on purpose: keep them
        self.assertEqual(self.store.latest_mailbox()["backend"], "claude")

    def test_texts_reach_mgba_on_connect_and_when_they_change(self):
        bridge = Bridge(self.store, self.data)

        async def scenario():
            server = await asyncio.start_server(bridge.handle_connection, "127.0.0.1", 0)
            port = server.sockets[0].getsockname()[1]
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.write(b'{"seq": 1, "type": "hello", "build": "test"}\n')
            await writer.drain()
            first = [await asyncio.wait_for(reader.readline(), 2) for _ in range(2)]

            # A text written while connected (e.g. by python3 -m bridge.mailbox) goes out on the next check.
            self.store.add_mailbox(mailbox.MailboxTexts("nuevo", "LLOPEZ: nuevo", "claude"), {})
            await bridge.push_mailbox_to_all()
            second = [await asyncio.wait_for(reader.readline(), 2) for _ in range(2)]

            writer.close()
            await writer.wait_closed()
            server.close()
            await server.wait_closed()
            return first, second

        first, second = asyncio.run(scenario())
        self.assertTrue(first[0].startswith(b"MAILBOX report "))
        self.assertTrue(first[1].startswith(b"MAILBOX champion "))
        encoded = mailbox.encode_mailbox(self.store.latest_mailbox(), self.data)
        self.assertEqual(second[0], f"MAILBOX report {encoded['report'].hex()}\n".encode())


if __name__ == "__main__":
    unittest.main()
