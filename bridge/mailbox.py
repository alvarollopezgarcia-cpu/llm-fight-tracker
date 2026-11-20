"""The mailbox: texts the bridge sends back into the game.

    python3 -m bridge.mailbox                    # write the texts with the rules backend (free)
    python3 -m bridge.mailbox --backend claude   # write them with Claude (needs ANTHROPIC_API_KEY or `ant auth login`)
    python3 -m bridge.mailbox --show             # print the stored texts as the game will page them

Two texts, both in Spanish because the game is:
    report          shown by the PokéNav INFORME option
    champion line   said by the Champion before the battle

The bridge stores them in SQLite; the running server pushes the newest ones to
the mGBA script, which writes them into the ROM's gMailbox. The text is encoded
with the game's charmap and wrapped by pixel width here, so the ROM only copies bytes.
"""

import argparse
import json
import sys
import unicodedata
from dataclasses import dataclass

from .facts import MIN_SAMPLES, player_facts
from .game_data import GameData
from .store import DEFAULT_DB, Store

CHAMPION_NAME = "LLOPEZ"
MESSAGE_BOX_WIDTH = 208  # pixels of text per line in a standard message box
EOS = 0xFF


# ---------------------------------------------------------------------------
# Encoding: Unicode text -> the game's bytes, wrapped into message box pages
# ---------------------------------------------------------------------------

class GameText:
    """Encodes text with charmap.txt and wraps it with the message box font's glyph widths."""

    def __init__(self, charmap, glyph_widths):
        self.charmap = charmap
        self.widths = glyph_widths
        self.space = charmap[" "]

    def encode_char(self, char):
        if char in self.charmap:
            return self.charmap[char]
        # Not in the font: try without its accent (e.g. "ç" -> "c"), else drop it.
        plain = unicodedata.normalize("NFKD", char).encode("ascii", "ignore").decode()
        return self.charmap.get(plain, []) if len(plain) == 1 else []

    def encode_word(self, word):
        return [byte for char in word for byte in self.encode_char(char)]

    def width(self, encoded):
        return sum(self.widths[byte] if byte < len(self.widths) else 6 for byte in encoded)

    def wrap(self, paragraph, max_width=MESSAGE_BOX_WIDTH):
        """Split one paragraph into lines of encoded bytes that fit the message box."""
        lines, line = [], []
        for word in paragraph.split():
            encoded = self.encode_word(word)
            if not encoded:
                continue
            candidate = line + self.space + encoded if line else encoded
            if line and self.width(candidate) > max_width:
                lines.append(line)
                line = encoded
            else:
                line = candidate
        if line:
            lines.append(line)
        return lines

    def encode(self, text, size):
        """Text with blank-line-separated paragraphs -> EOS-terminated bytes of at most `size`.

        Each paragraph starts a new message box (\\p); its second line goes below the
        first (\\n) and later lines scroll the box up (\\l). Whole lines that don't fit
        are dropped, so the text never ends mid-word.
        """
        newline, scroll, paragraph_break = self.charmap["\\n"], self.charmap["\\l"], self.charmap["\\p"]
        out = []
        for paragraph in [p for p in text.split("\n\n") if p.strip()]:
            for index, line in enumerate(self.wrap(" ".join(paragraph.split()))):
                if index == 0:
                    separator = paragraph_break if out else []
                else:
                    separator = newline if index == 1 else scroll
                if len(out) + len(separator) + len(line) + 1 > size:
                    return bytes(out + [EOS])
                out += separator + line
        return bytes(out + [EOS])


# ---------------------------------------------------------------------------
# Spanish texts from the facts, without an LLM
# ---------------------------------------------------------------------------

# "Cuando <situación>, <tendencia> <conducta>." in the second person, gender-neutral.
HABITS_ES = {
    "SWITCH_T1": ("un ataque súper eficaz amenaza a tu Pokémon sin noquearlo", "lo cambias"),
    "SWITCH_T2": ("el rival puede noquearte pero tú atacas antes", "cambias de Pokémon"),
    "SWITCH_T3_ACE": ("tu as va a caer antes de poder actuar", "lo retiras"),
    "SWITCH_T3_OTHER": ("un Pokémon que no es tu as va a caer antes de actuar", "lo retiras"),
    "MISREAD_RACE": ("te quedas a atacar bajo amenaza de KO", "calculas mal quién ataca primero"),
    "REVENGE_AFTER_SACK": ("cae un Pokémon que se quedó ante un KO más rápido", "respondes con uno que lo venga o lo aguanta"),
    "SWITCH_TO_IMMUNE": ("cambias con un rival en el campo", "sacas a uno inmune a sus ataques"),
    "SWITCH_TO_RESIST": ("cambias con un rival en el campo", "sacas a uno que resiste sus ataques"),
    "REACTIVE_SWITCH": ("el rival saca a uno que bloquea al tuyo", "cambias al turno siguiente"),
    "SWITCH_T0": ("tu Pokémon no está amenazado", "cambias igualmente"),
    "SWITCH_REPOSITION": ("tienes en el banquillo a uno que amenaza más", "cambias a ese atacante"),
    "RESET_IMMEDIATE": ("te bajan estadísticas o te envenenan gravemente", "cambias en el acto"),
    "RESET_DELAYED": ("te bajan estadísticas o te envenenan gravemente", "cambias tras esperar algún turno"),
    "SWITCH_ABILITY": ("tu Pokémon tiene Regeneración o Cura Natural", "cambias para aprovecharla"),
    "SWITCH_DOUBLE_READ": ("cambias sin estar amenazado", "aciertas el cambio del rival"),
    "FREE_SWITCH_TAKEN": ("el juego te ofrece cambiar tras un KO", "aceptas"),
    "FREE_SWITCH_COUNTER": ("aceptas ese cambio gratis", "sacas a uno que gana al siguiente rival"),
    "SETUP_ENTRY_SOUND": ("entras con un Pokémon que se potencia y es seguro", "te potencias de inmediato"),
    "SETUP_ENTRY_RISKY": ("potenciarte al entrar es arriesgado", "te potencias igualmente"),
    "SETUP_PUNISHED": ("te potencias", "caes antes de aprovecharlo"),
    "SETUP_WHEN_SAFE": ("tu Pokémon está a salvo más adelante", "te potencias"),
    "HEAL_AT_LOW_HP": ("te queda menos del 40% de PS y un movimiento de curación te salvaría", "lo usas"),
    "STATUS_OPEN_VALID": ("entras con un movimiento de estado útil", "lo usas primero"),
    "STATUS_INTO_BAD": ("usas un movimiento de estado", "lo usas contra un rival al que no afecta"),
    "KO_SAFE_PICK": ("varios de tus ataques noquean", "eliges el más seguro"),
    "ACCURACY_GREED": ("tu ataque más fuerte es impreciso y otro fiable casi iguala su daño", "arriesgas con el impreciso"),
    "DRAWBACK_USE": ("tu ataque más fuerte tiene un coste y otro casi iguala su daño", "usas el del coste"),
    "ATTACK_THE_SWITCH": ("el rival está en clara desventaja", "atacas pensando en su cambio"),
    "SPREAD_PREFERENCE": ("hay dos rivales y tienes un ataque a ambos", "lo usas"),
    "PRIORITY_NEEDED": ("eres más lento y un ataque con prioridad noquea", "usas la prioridad"),
    "PRIORITY_REDUNDANT": ("eres más rápido y cualquier ataque noquea", "usas prioridad de todos modos"),
    "PRIORITY_INTO_BLOCK": ("usas un ataque con prioridad", "chocas contra algo que la bloquea"),
    "SUCKER_USE": ("Golpe Bajo noquearía", "lo usas"),
    "PROTECT_SCOUT_SINGLES": ("entras con Protección en combates individuales", "la usas primero para tantear"),
    "PROTECT_SCOUT_DOUBLES": ("entras con Protección en combates dobles", "la usas primero para tantear"),
    "SCOUT_THEN_SWITCH": ("tanteas con Protección y ves una amenaza", "cambias al turno siguiente"),
    "PROTECT_STALL": ("protegerte te da ventaja", "ganas tiempo con Protección"),
    "PROTECT_REPEAT": ("acabas de usar Protección", "la repites"),
    "PROTECT_SPREAD_COMBO": ("tu compañero se puede proteger de tu ataque en área", "combinas ambos"),
    "FOCUS_NEEDED": ("solo juntos podéis noquear a un rival", "atacáis al mismo"),
    "FOCUS_OVERKILL": ("un ataque basta para noquear", "le atacáis los dos igualmente"),
    "REDIRECT_RESPECT": ("el rival usa Señuelo o Polvo Ira", "lo esquivas"),
    "LEAD_COUNTERS_ENEMY_LEAD": ("empieza un combate", "abres con un Pokémon que contrarresta al del rival"),
}


def tendency_es(rate):
    if rate >= 0.85:
        return "casi siempre"
    if rate >= 0.60:
        return "normalmente"
    if rate >= 0.35:
        return "a veces"
    if rate >= 0.10:
        return "rara vez"
    return "casi nunca"


def habit_sentence_es(habit):
    situation, behavior = HABITS_ES[habit["habit"]]
    return f"Cuando {situation}, {tendency_es(habit['rate'])} {behavior} ({round(habit['rate'] * 100)}%)."


def most_predictable(facts, count):
    """The habits an opponent could rely on most: far from 50% and seen many times."""
    profile = facts.get("profile") or {}
    habits = [h for h in profile.get("tendencies", [])
              if h["habit"] in HABITS_ES and abs(h["rate"] - 0.5) >= 0.15]  # 35-65% says nothing
    return sorted(habits, key=lambda h: -abs(h["rate"] - 0.5) * h["chances"])[:count]


def times(count):
    return "1 vez" if count == 1 else f"{count} veces"


def rules_report(facts):
    profile = facts.get("profile")
    if not profile:
        return "Todavía no hay combates registrados.\n\nLucha contra algún entrenador con el rastreador conectado."
    paragraphs = [f"He analizado {profile['battles_recorded']} combates tuyos contra entrenadores. "
                  f"Esto es lo que más se repite:"]
    paragraphs += [habit_sentence_es(h) for h in most_predictable(facts, 3)]
    healing, leveling = facts.get("healing"), facts.get("leveling")
    if healing:
        heals = healing["heals"]
        paragraphs.append(f"Te has curado {times(heals.get('CENTER', 0))} en el Centro y "
                          f"{times(heals.get('POKENAV', 0))} con el PokéNav, con el equipo al "
                          f"{healing['average_party_hp_percent_before']}% de PS de media.")
    if leveling and leveling["rare_candies"]:
        candies = leveling["rare_candies"]
        paragraphs.append(f"Caramelos Raros: {candies.get('BAG', 0)} a mano y {candies.get('POKENAV', 0)} con el PokéNav.")
    return "\n\n".join(paragraphs)


def rules_champion_line(facts):
    top = most_predictable(facts, 1)
    if not top:
        return f"{CHAMPION_NAME}: Aún no sé nada de cómo combates. Enséñamelo."
    return (f"{CHAMPION_NAME}: He estudiado tus combates. {habit_sentence_es(top[0])}\n\n"
            f"Veamos si hoy te atreves a cambiar.")


@dataclass
class MailboxTexts:
    report: str
    champion_line: str
    backend: str
    model: str = None
    input_tokens: int = None
    output_tokens: int = None


def write_with_rules(facts):
    return MailboxTexts(rules_report(facts), rules_champion_line(facts), "rules")


MAILBOX_PROMPT = """Write two short texts in Spanish (Spain) that the game will show to this player, from the data below.

1. "report": what the PokéNav says when the player opens its INFORME option. Speak to the player in the second person (tú) about how they fight: their two or three most notable habits with numbers, and one piece of advice. At most 550 characters. Separate ideas with a blank line; each block becomes a new message box.
2. "champion_line": what {champion}, the Champion, says right before the final battle. In character: confident, a little teasing, showing they studied the player. Mention one concrete habit from the data. At most 200 characters, one or two sentences. Do not start with the speaker's name.

Both texts:
- Gender-neutral wording about the player (Spanish has gendered adjectives: avoid them).
- Plain letters and punctuation only: no emojis, no quotation marks, no markdown.
- Use only the data given. Rates from few chances are weak evidence: prefer the habits with the most chances."""

MAILBOX_SCHEMA = {
    "type": "object",
    "properties": {"report": {"type": "string"}, "champion_line": {"type": "string"}},
    "required": ["report", "champion_line"],
    "additionalProperties": False,
}


def write_with_claude(facts, summarizer):
    """Ask Claude for both texts at once, as JSON (structured output)."""
    response = summarizer.ask(MAILBOX_PROMPT.format(champion=CHAMPION_NAME), facts,
                              output_format={"type": "json_schema", "schema": MAILBOX_SCHEMA})
    texts = json.loads(response.text)
    return MailboxTexts(texts["report"].strip(), f"{CHAMPION_NAME}: {texts['champion_line'].strip()}", "claude",
                        response.model, response.input_tokens, response.output_tokens)


# ---------------------------------------------------------------------------
# Packaging for the mGBA script
# ---------------------------------------------------------------------------

def encode_mailbox(texts, game_data):
    """{slot: bytes} ready to write into gMailbox."""
    encoder = GameText(game_data.charmap, game_data.glyph_widths)
    layout = game_data.mailbox
    return {
        "report": encoder.encode(texts["report"], layout["report_size"]),
        "champion": encoder.encode(texts["champion_line"], layout["line_size"]),
    }


def mailbox_lines(encoded):
    """The wire format the Lua script reads: one `MAILBOX <slot> <hex>` line per text."""
    return [f"MAILBOX {slot} {data.hex()}\n" for slot, data in encoded.items()]


def preview(data, game_data):
    """Decode game bytes back to readable text, one message box per block (for --show)."""
    reverse = {}
    for char, codes in game_data.charmap.items():
        if len(codes) == 1:
            reverse.setdefault(codes[0], char)
    out = []
    for byte in data:
        if byte == EOS:
            break
        char = reverse.get(byte, "?")
        out.append({"\\n": "\n", "\\l": "\n", "\\p": "\n---\n"}.get(char, char))
    return "".join(out)


def generate(store, game_data, backend, summarizer=None, min_samples=MIN_SAMPLES):
    facts = player_facts(store, game_data, min_samples=min_samples)
    texts = write_with_claude(facts, summarizer) if backend == "claude" else write_with_rules(facts)
    store.add_mailbox(texts, facts)
    return texts


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", default=str(DEFAULT_DB))
    parser.add_argument("--backend", choices=["rules", "claude"], default="rules")
    parser.add_argument("--model", default="claude-opus-5-5")
    parser.add_argument("--show", action="store_true", help="print the newest stored texts as the game pages them")
    args = parser.parse_args()

    game_data = GameData.load()
    if not game_data.mailbox:
        sys.exit("game_data.json has no mailbox layout: re-run gen_tracker.py")
    store = Store(args.db)
    try:
        if not args.show:
            summarizer = None
            if args.backend == "claude":
                from .summarizers import ClaudeSummarizer
                summarizer = ClaudeSummarizer(model=args.model, language="Spanish")
            texts = generate(store, game_data, args.backend, summarizer)
            if texts.input_tokens is not None:
                print(f"[{texts.model}: {texts.input_tokens} in / {texts.output_tokens} out tokens]")
            print("Stored. A running bridge sends it to the game within a few seconds.\n")
        latest = store.latest_mailbox()
        if latest is None:
            sys.exit("no mailbox texts stored yet")
        encoded = encode_mailbox(latest, game_data)
        for slot, data in encoded.items():
            print(f"== {slot} ({len(data)} bytes, {latest['backend']})\n{preview(data, game_data)}\n")
    finally:
        store.close()


if __name__ == "__main__":
    main()
