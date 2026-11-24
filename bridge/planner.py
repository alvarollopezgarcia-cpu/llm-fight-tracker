"""League planner: one battle plan for the whole Pokémon League, made when the player walks in.

    python3 -m bridge.planner                  # make a plan with the rules backend (free)
    python3 -m bridge.planner --backend claude # let Claude plan (needs `ant auth login` or ANTHROPIC_API_KEY)
    python3 -m bridge.planner --show           # print the newest stored plan

The LLM never plays turns: it is too slow for that and the game can't wait. It plans. For each
League trainer it picks bounded values the C AI understands (include/telemetry.h, struct
AiTrainerDirective): which slots lead, how often to predict switches, which of the player's habits
to punish, which player mon to focus in doubles, and two small style weights. Python validates
every value; an invalid plan is thrown away and the rules plan is used instead. In the game, no plan
at all (no bridge, or a release build) means the profile rules run alone.

The running server makes a plan by itself when the player enters EverGrandeCity_PokemonLeague_1F,
so it is ready before the first Elite Four battle.
"""

import argparse
import json
import struct
import sys
from dataclasses import dataclass, field

from .facts import MIN_SAMPLES, player_facts
from .game_data import GameData
from .store import DEFAULT_DB, Store

LEAGUE_ENTRY_MAP = "EverGrandeCity_PokemonLeague_1F"
BIAS_THRESHOLD = 30  # same as PROFILE_AI_BIAS_THRESHOLD in include/config/ai.h

# Which recorded habits each punish rule answers (the rules plan turns a rule on when one is clear)
PUNISH_HABITS = {
    "SETUP": ["SETUP_ENTRY_SOUND", "SETUP_ENTRY_RISKY", "SETUP_WHEN_SAFE"],
    "HEALING": ["HEAL_AT_LOW_HP"],
    "PROTECT_SCOUT": ["PROTECT_SCOUT_DOUBLES", "PROTECT_SCOUT_SINGLES"],
    "STATUS": ["STATUS_OPEN_VALID"],
    "PRIORITY": ["PRIORITY_NEEDED"],
    "SPREAD": ["SPREAD_PREFERENCE"],
    "FOCUS": ["FOCUS_NEEDED", "FOCUS_OVERKILL"],
}

PUNISH_DESCRIPTIONS = {
    "SETUP": "the player sets up: Taunt before the boost, Haze / Roar / Dragon Tail / Clear Smog after it",
    "HEALING": "the player heals with moves when low: Taunt / Heal Block",
    "PROTECT_SCOUT": "the player opens with Protect: don't attack into it, prefer Feint",
    "PROTECT_REPEAT": "the player rarely protects twice: attack the mon that just protected",
    "STATUS": "the player opens with status moves: Safeguard / Misty Terrain",
    "PRIORITY": "the player finishes with priority: low AI mons protect",
    "SPREAD": "the player uses spread moves: Wide Guard",
    "FOCUS": "the player focuses one target in doubles: the weaker AI mon protects, its partner redirects",
}

FOCUS_DESCRIPTIONS = {
    "NONE": "no preference",
    "LOWEST_HP": "attack the player mon with the lowest HP",
    "FASTEST": "attack the fastest player mon",
    "STRONGEST": "attack the player mon with the highest Attack or Sp. Atk",
}


class PlanError(ValueError):
    pass


@dataclass
class Plan:
    trainers: list            # validated entries, one per League trainer (see validate_plan)
    explanation: str
    backend: str
    model: str = None
    input_tokens: int = None
    output_tokens: int = None
    notes: list = field(default_factory=list)  # why a plan fell back, for the log


# ---------------------------------------------------------------------------
# Validation and encoding
# ---------------------------------------------------------------------------

def validate_plan(raw, game_data):
    """Check every value against the schema the C AI understands. Returns the clean trainer entries.

    Raises PlanError on anything out of range, so a bad LLM answer never reaches the game.
    """
    directive = game_data.mailbox["directive"]
    league = {team["trainer"]: team for team in game_data.league}
    weight = directive["weight_max"]
    clean, seen = [], set()

    if not isinstance(raw, dict) or not isinstance(raw.get("trainers"), list):
        raise PlanError("plan must be an object with a trainers list")
    for entry in raw["trainers"]:
        name = entry.get("trainer")
        if name not in league:
            raise PlanError(f"unknown trainer {name!r}")
        if name in seen:
            raise PlanError(f"{name} appears twice")
        seen.add(name)
        team = league[name]
        candidates = len(team["party"]) - team["ace_slots"]

        leads = entry.get("lead_slots") or []
        if leads:
            if len(leads) != team["leads"]:
                raise PlanError(f"{name} needs {team['leads']} lead slot(s), got {leads}")
            if any(not isinstance(slot, int) or not 0 <= slot < candidates for slot in leads) or len(set(leads)) != len(leads):
                raise PlanError(f"{name}: lead slots {leads} must be distinct, non-ace party slots below {candidates}")

        switch = entry.get("switch_prediction", -1)
        if not isinstance(switch, int) or not (switch == -1 or 0 <= switch <= 100):
            raise PlanError(f"{name}: switch_prediction must be 0..100 or -1, got {switch!r}")

        punish = entry.get("punish") or []
        unknown = [rule for rule in punish if rule not in directive["punish"]]
        if unknown:
            raise PlanError(f"{name}: unknown punish rules {unknown}")

        focus = entry.get("focus_target", "NONE")
        if focus not in directive["focus"]:
            raise PlanError(f"{name}: unknown focus_target {focus!r}")

        values = {}
        for key in ("aggression", "self_setup"):
            value = entry.get(key, 0)
            if not isinstance(value, int) or not -weight <= value <= weight:
                raise PlanError(f"{name}: {key} must be an integer in -{weight}..{weight}, got {value!r}")
            values[key] = value

        clean.append({"trainer": name, "trainer_id": team["id"], "lead_slots": leads, "switch_prediction": switch,
                      "punish": sorted(set(punish)), "focus_target": focus, **values,
                      "reason": str(entry.get("reason", ""))[:300]})
    if len(clean) > directive["max_trainers"]:
        raise PlanError("too many trainers")
    return clean


def encode_directive(trainers, game_data, plan_id):
    """struct AiDirective bytes: u32 magic, u16 planId, u8 count, pad, then 12-byte entries."""
    directive = game_data.mailbox["directive"]
    keep = directive["keep"]
    out = struct.pack("<IHBx", directive["magic"], plan_id & 0xFFFF, len(trainers))
    for entry in trainers:
        leads = entry["lead_slots"] + [keep] * (2 - len(entry["lead_slots"])) if entry["lead_slots"] else [keep, keep]
        mask = sum(directive["punish"][rule] for rule in entry["punish"])
        switch = keep if entry["switch_prediction"] == -1 else entry["switch_prediction"]
        out += struct.pack("<HHBBBBbbxx", entry["trainer_id"], mask, leads[0], leads[1], switch,
                           directive["focus"].index(entry["focus_target"]), entry["aggression"], entry["self_setup"])
    out += bytes(directive["entry_size"] * (directive["max_trainers"] - len(trainers)))
    assert len(out) == game_data.mailbox["directive_size"]
    return out


# ---------------------------------------------------------------------------
# Planners
# ---------------------------------------------------------------------------

def clear_habits(facts):
    """Short habit names the player shows clearly (same thresholds as the C AI)."""
    profile = facts.get("profile") or {}
    return {h["habit"]: h for h in profile.get("tendencies", []) if abs(h["rate"] * 200 - 100) >= BIAS_THRESHOLD}


def rules_plan(facts, game_data):
    """No LLM: punish every clear habit, keep documented leads and the profile's switch prediction."""
    habits = clear_habits(facts)
    punish = [rule for rule, names in PUNISH_HABITS.items() if any(n in habits and habits[n]["rate"] > 0.5 for n in names)]
    if "PROTECT_REPEAT" in habits and habits["PROTECT_REPEAT"]["rate"] < 0.5:
        punish.append("PROTECT_REPEAT")
    raw = {"trainers": [{"trainer": team["trainer"], "punish": punish} for team in game_data.league]}
    trainers = validate_plan(raw, game_data)
    names = ", ".join(sorted(punish)) or "ninguno"
    return Plan(trainers, f"La Liga castigará los hábitos que más repites: {names}.", "rules")


PLAN_TASK = """You are planning how the Pokémon League will fight this player, before they reach the first Elite Four member. The League trainers are listed in "league" with their documented teams (players can read these docs, so many open with a counter to the documented lead). Their AI already plays every turn; you set its plan for each trainer through these values only:

- lead_slots: party slots to lead with ({doubles} slots for a doubles trainer, 1 for a single trainer), or [] to keep the documented lead. Choose a lead that beats what this player is likely to bring against the documented one. Slots count from 0; never lead with an ace (the last "ace_slots" slots of the party).
- switch_prediction: 0..100, how often the AI should assume the player switches out of a bad matchup, or -1 to use the player's recorded rate.
- punish: which of the player's habits to answer. Options: {punish}.
- focus_target: in doubles, which player mon the AI should gang up on. Options: {focus}.
- aggression: -3..3, a nudge toward attacking moves (positive) or away from them (negative).
- self_setup: -3..3, a nudge toward or away from the AI's own stat-boosting moves.
- reason: one sentence on why, in Spanish.

Base every choice on the data: the player's habits (rates from few chances are weak evidence), their recent battles and trends, and each trainer's documented moves. Plan only for the trainers in "league", each once.

Also write "explanation": 2 to 4 sentences in Spanish (Spain), at most 500 characters, second person and gender-neutral, telling the player what the League has prepared against their style. Plain text, no quotation marks."""


def plan_schema(game_data):
    directive = game_data.mailbox["directive"]
    return {
        "type": "object",
        "properties": {
            "explanation": {"type": "string"},
            "trainers": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "trainer": {"type": "string", "enum": [team["trainer"] for team in game_data.league]},
                        "lead_slots": {"type": "array", "items": {"type": "integer"}},
                        "switch_prediction": {"type": "integer"},
                        "punish": {"type": "array", "items": {"type": "string", "enum": list(directive["punish"])}},
                        "focus_target": {"type": "string", "enum": directive["focus"]},
                        "aggression": {"type": "integer"},
                        "self_setup": {"type": "integer"},
                        "reason": {"type": "string"},
                    },
                    "required": ["trainer", "lead_slots", "switch_prediction", "punish", "focus_target",
                                 "aggression", "self_setup", "reason"],
                    "additionalProperties": False,
                },
            },
        },
        "required": ["explanation", "trainers"],
        "additionalProperties": False,
    }


def planner_facts(facts, game_data):
    """What the LLM reads: the player and the documented League teams."""
    return {
        "player": facts,
        "league": [{"trainer": t["trainer"], "name": t["name"], "leads": t["leads"], "ace_slots": t["ace_slots"],
                    "party": t["party"]} for t in game_data.league],
        "punish_rules": PUNISH_DESCRIPTIONS,
        "focus_targets": FOCUS_DESCRIPTIONS,
    }


def claude_plan(facts, game_data, summarizer):
    directive = game_data.mailbox["directive"]
    task = PLAN_TASK.format(doubles=2, punish=", ".join(directive["punish"]),
                            focus=", ".join(directive["focus"]))
    response = summarizer.ask(task, planner_facts(facts, game_data),
                              output_format={"type": "json_schema", "schema": plan_schema(game_data)})
    raw = json.loads(response.text)
    trainers = validate_plan(raw, game_data)
    return Plan(trainers, str(raw.get("explanation", "")).strip(), "claude", response.model,
                response.input_tokens, response.output_tokens)


def build_plan(facts, game_data, backend, summarizer=None):
    """A validated plan. A failed or invalid LLM plan falls back to the rules plan.

    Touches no database, so the server can run it in a thread while the LLM thinks.
    """
    notes = []
    if backend == "claude":
        try:
            return claude_plan(facts, game_data, summarizer)
        except Exception as error:  # an LLM failure must never leave the League without a plan
            notes.append(f"claude plan rejected, using rules: {error}")
    plan = rules_plan(facts, game_data)
    if notes:
        plan.backend = "rules-fallback"
    plan.notes = notes
    return plan


def plan_report(plan):
    """The PokéNav INFORME text while the plan is active: what the League prepared."""
    return f"La Liga Pokémon ha estudiado tus combates y ha preparado un plan.\n\n{plan.explanation}"


def store_plan(store, game_data, plan, facts, champion_line=None):
    """Store the plan and put its explanation in the PokéNav report. Returns the plan id."""
    from .mailbox import MailboxTexts, rules_champion_line  # mailbox imports nothing from here
    plan_id = store.add_plan(plan, planner_facts(facts, game_data))
    if game_data.mailbox:
        latest = store.latest_mailbox()
        line = champion_line or (latest["champion_line"] if latest else rules_champion_line(facts))
        store.add_mailbox(MailboxTexts(plan_report(plan), line, f"plan-{plan.backend}"), facts)
    return plan_id


def make_plan(store, game_data, backend, summarizer=None, min_samples=MIN_SAMPLES):
    facts = player_facts(store, game_data, min_samples=min_samples)
    plan = build_plan(facts, game_data, backend, summarizer)
    return store_plan(store, game_data, plan, facts), plan


def plan_mailbox_line(plan_row, game_data):
    """The wire line that writes a stored plan into the game."""
    trainers = json.loads(plan_row["plan"])
    return f"MAILBOX directive {encode_directive(trainers, game_data, plan_row['id']).hex()}\n"


# ---------------------------------------------------------------------------
# Measurement: how League battles went under each plan
# ---------------------------------------------------------------------------

def plan_results(store, game_data):
    """Per plan: backend and each League battle fought under it (trainer, outcome, turns)."""
    results = []
    for row in store.plans():
        battles = [{"trainer": game_data.trainer_name(b["trainer_a"] or 0),
                    "outcome": game_data.outcome_name(b["outcome"] or 0), "turns": b["turns"]}
                   for b in store.plan_battles(row["id"])]
        results.append({"plan": row["id"], "backend": row["backend"], "battles": battles})
    return results


def backend_totals(results):
    """Player wins and losses against the League per planner backend: rules vs LLM."""
    totals = {}
    for result in results:
        backend = "claude" if result["backend"] == "claude" else "rules"
        total = totals.setdefault(backend, {"plans": 0, "battles": 0, "player_won": 0, "player_lost": 0})
        total["plans"] += 1
        for battle in result["battles"]:
            total["battles"] += 1
            total["player_won"] += battle["outcome"] == "B_OUTCOME_WON"
            total["player_lost"] += battle["outcome"] == "B_OUTCOME_LOST"
    return totals


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", default=str(DEFAULT_DB))
    parser.add_argument("--backend", choices=["rules", "claude"], default="rules")
    parser.add_argument("--model", default="claude-opus-5-5")
    parser.add_argument("--show", action="store_true", help="print the newest stored plan")
    parser.add_argument("--results", action="store_true", help="League battles per plan, and rules vs Claude totals")
    args = parser.parse_args()

    game_data = GameData.load()
    if not game_data.league or not (game_data.mailbox or {}).get("directive"):
        sys.exit("game_data.json has no League teams or plan layout: re-run gen_tracker.py")
    store = Store(args.db)
    try:
        if args.results:
            results = plan_results(store, game_data)
            for result in results:
                print(f"Plan #{result['plan']} ({result['backend']}): {len(result['battles'])} League battles")
                for battle in result["battles"]:
                    print(f"    {battle['trainer']:<20} {battle['outcome']:<18} {battle['turns']} turns")
            for backend, total in backend_totals(results).items():
                print(f"{backend}: {total['plans']} plans, {total['battles']} battles, "
                      f"player won {total['player_won']}, player lost {total['player_lost']}")
            return
        if not args.show:
            summarizer = None
            if args.backend == "claude":
                from .summarizers import ClaudeSummarizer
                summarizer = ClaudeSummarizer(model=args.model, language="Spanish")
            plan_id, plan = make_plan(store, game_data, args.backend, summarizer)
            for note in plan.notes:
                print(note)
            if plan.input_tokens is not None:
                print(f"[{plan.model}: {plan.input_tokens} in / {plan.output_tokens} out tokens]")
            print(f"Stored plan #{plan_id}. A running bridge sends it to the game within a few seconds.\n")
        row = store.latest_plan()
        if row is None:
            sys.exit("no plan stored yet")
        print(f"Plan #{row['id']} ({row['backend']})\n{row['explanation']}\n")
        for entry in json.loads(row["plan"]):
            leads = entry["lead_slots"] or "documented"
            switch = "profile" if entry["switch_prediction"] == -1 else f"{entry['switch_prediction']}%"
            print(f"{entry['trainer']:<20} leads {leads}  switch prediction {switch}  focus {entry['focus_target']}"
                  f"  aggression {entry['aggression']:+d}  self setup {entry['self_setup']:+d}")
            print(f"{'':<20} punish {', '.join(entry['punish']) or '-'}")
            if entry["reason"]:
                print(f"{'':<20} {entry['reason']}")
    finally:
        store.close()


if __name__ == "__main__":
    main()
