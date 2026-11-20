"""Turn stored battles and profiles into plain facts that every summarizer reads.

The facts are JSON-friendly dicts, so the same input goes to the rules
summarizer and to an LLM, and is stored next to each summary for audit.
"""

from .game_data import profile_delta
from .habits import HABITS, short_name

MIN_SAMPLES = 10    # chances needed before a saved-profile rate is reported as a tendency
TREND_MIN = 4       # chances needed in each half before a trend is reported
RECENT_BATTLES = 20


def habit_entry(name, counter):
    habit = HABITS.get(name)
    entry = {
        "habit": short_name(name),
        "group": habit.group if habit else "Other",
        "situation": habit.situation if habit else "",
        "behavior": habit.behavior if habit else "",
        "hits": counter["hits"],
        "chances": counter["chances"],
    }
    entry["rate"] = round(counter["hits"] / counter["chances"], 2) if counter["chances"] else None
    return entry


def _count(counter, key, amount=1):
    counter[key] = counter.get(key, 0) + amount


def telemetry_battle_facts(events, game_data):
    """What happened inside one battle, from the ROM's telemetry (None when there is none)."""
    if not events or not game_data.telemetry:
        return None
    facts = {"player_moves": {}, "player_move_results": {}, "opponent_moves": {}, "crits": {"dealt": 0, "taken": 0},
             "switches": 0, "items_used": {}, "fainted": {"player": [], "opponent": []}, "level_ups": []}
    for raw in events:
        event = game_data.decode_telemetry(raw)
        kind = event["kind"]
        if kind == "ACTION" and event["action"] == "USE_MOVE":
            _count(facts["player_moves"], event["move"])
        elif kind == "ACTION" and event["action"] == "SWITCH":
            facts["switches"] += 1
        elif kind == "MOVE_RESULT":
            if event["side"] == "player":
                _count(facts["player_move_results"], event["result"])
            else:
                _count(facts["opponent_moves"], event["move"])
            if event["crit"]:
                facts["crits"]["dealt" if event["side"] == "player" else "taken"] += 1
        elif kind == "FAINT":
            facts["fainted"][event["side"]].append(event["species"])
        elif kind == "ITEM_IN_BATTLE":
            _count(facts["items_used"], event["item"])
        elif kind == "LEVEL_UP":
            facts["level_ups"].append(f"{event['species']} to {event['level']}")
    return facts


def leveling_facts(events, game_data):
    """How the player levels: Rare Candies by hand or through the PokéNav, the cap, battle experience."""
    if not events or not game_data.telemetry:
        return None
    facts = {"rare_candies": {}, "levels_from_battle": 0, "candy_refused_by_level_cap": 0, "whiteouts": []}
    for raw in events:
        event = game_data.decode_telemetry(raw)
        if event["kind"] == "RARE_CANDY":
            _count(facts["rare_candies"], event["source"], event["candies"])
        elif event["kind"] == "CAP_BLOCKED":
            facts["candy_refused_by_level_cap"] += 1
        elif event["kind"] == "LEVEL_UP":
            facts["levels_from_battle"] += 1
        elif event["kind"] == "WHITEOUT":
            facts["whiteouts"].append(event["map"])
    return facts


def healing_facts(events, game_data):
    """When the player heals outside battle: Pokémon Center or the PokéNav HEAL charge, and how hurt the party was."""
    if not events or not game_data.telemetry:
        return None
    heals = [game_data.decode_telemetry(raw) for raw in events]
    heals = [event for event in heals if event["kind"] == "HEAL"]
    if not heals:
        return None
    facts = {"heals": {}, "average_party_hp_percent_before": round(sum(h["party_hp_percent"] for h in heals) / len(heals)),
             "heals_with_fainted_pokemon": sum(1 for h in heals if h["fainted"]),
             "heals_at_full_hp": sum(1 for h in heals if h["party_hp_percent"] == 100 and not h["fainted"])}
    for heal in heals:
        _count(facts["heals"], heal["source"])
    return facts


def battle_facts(battle, game_data, telemetry_events=None):
    """One finished battle: who, where, result, the habits it added, and its telemetry."""
    before = game_data.decode_profile(battle["profile_before"])
    after = game_data.decode_profile(battle["profile_after"])
    delta = profile_delta(before, after) or {}

    habits = []
    for name, counter in delta.items():
        if counter is None:
            continue  # decayed during the merge: this battle's share is unknown
        entry = habit_entry(name, counter)
        usual = before["stats"].get(name) if before else None
        entry["usual_rate"] = round(usual["hits"] / usual["chances"], 2) if usual and usual["chances"] else None
        habits.append(entry)

    trainer, partner = game_data.opponents(battle["battle_type"], battle["trainer_a"], battle["trainer_b"])
    return {
        "battle_id": battle["id"],
        "trainer": trainer,
        "partner_trainer": partner,
        "battle_type": game_data.battle_type_names(battle["battle_type"] or 0),
        "map": game_data.map_name(battle["map_group"], battle["map_num"]),
        "outcome": game_data.outcome_name(battle["outcome"] or 0),
        "turns": battle["turns"],
        "recorded_by_profile": bool(before and after and after["battles_recorded"] != before["battles_recorded"]),
        "habits": habits,
        "telemetry": telemetry_battle_facts(telemetry_events, game_data),
    }


def _sum_deltas(battles, game_data):
    totals = {}
    for battle in battles:
        delta = profile_delta(game_data.decode_profile(battle["profile_before"]),
                              game_data.decode_profile(battle["profile_after"])) or {}
        for name, counter in delta.items():
            if counter:
                total = totals.setdefault(name, {"hits": 0, "chances": 0})
                total["hits"] += counter["hits"]
                total["chances"] += counter["chances"]
    return totals


def trends(battles, game_data, min_chances=TREND_MIN):
    """Habits whose rate moved between the first and second half of the recorded battles.

    Uses the per-battle deltas stored in the database, so it still works after
    the in-game profile has decayed the old data away.
    """
    if len(battles) < 4:
        return []
    half = len(battles) // 2
    early, late = _sum_deltas(battles[:half], game_data), _sum_deltas(battles[half:], game_data)
    result = []
    for name in early.keys() & late.keys():
        a, b = early[name], late[name]
        if a["chances"] < min_chances or b["chances"] < min_chances:
            continue
        early_rate, late_rate = a["hits"] / a["chances"], b["hits"] / b["chances"]
        if abs(late_rate - early_rate) >= 0.2:
            result.append({"habit": short_name(name), "early_rate": round(early_rate, 2),
                           "late_rate": round(late_rate, 2), "early_chances": a["chances"],
                           "late_chances": b["chances"]})
    return sorted(result, key=lambda t: -abs(t["late_rate"] - t["early_rate"]))


def player_facts(store, game_data, min_samples=MIN_SAMPLES, recent=RECENT_BATTLES):
    """The whole picture: record, saved profile tendencies, recent battles and trends."""
    battles = store.battles()
    outcomes = {}
    for battle in battles:
        name = game_data.outcome_name(battle["outcome"] or 0)
        outcomes[name] = outcomes.get(name, 0) + 1

    profile = None
    for battle in reversed(battles):
        if battle["profile_after"]:
            profile = game_data.decode_profile(battle["profile_after"])
            break

    tendencies, thin = [], []
    if profile:
        for name, counter in profile["stats"].items():
            if counter["chances"] >= min_samples:
                tendencies.append(habit_entry(name, counter))
            elif counter["chances"]:
                thin.append(short_name(name))

    all_telemetry = store.telemetry()
    return {
        "battles_tracked": len(battles),
        "outcomes": outcomes,
        "profile": None if profile is None else {
            "battles_recorded": profile["battles_recorded"],
            "battles_in_set_mode": profile["battles_set"],
            "battles_in_switch_mode": profile["battles_switch"],
            "min_samples": min_samples,
            "tendencies": tendencies,
            "too_few_samples": thin,
            "status_kinds_used": profile["status_kinds"],
            "focus_targets": profile["focus_targets"],
        },
        "recent_battles": [battle_facts(b, game_data, store.telemetry(b)) for b in battles[-recent:]],
        "trends": trends(battles, game_data),
        "leveling": leveling_facts(all_telemetry, game_data),
        "healing": healing_facts(all_telemetry, game_data),
    }
