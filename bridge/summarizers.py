"""Summarizer backends: turn facts (bridge/facts.py) into a play-style description.

    rules   free and deterministic; always available
    claude  an LLM summary through the Anthropic API (needs `pip install anthropic`
            and an API key in ANTHROPIC_API_KEY)

Both take the same facts, so their outputs can be compared side by side.
"""

import json
from dataclasses import dataclass

from .habits import tendency


class SummaryError(Exception):
    pass


@dataclass
class Summary:
    text: str
    backend: str
    model: str = None
    input_tokens: int = None
    output_tokens: int = None


OUTCOME_WORDS = {
    "B_OUTCOME_WON": "Won against",
    "B_OUTCOME_LOST": "Lost to",
    "B_OUTCOME_DREW": "Drew with",
    "B_OUTCOME_RAN": "Ran from",
    "B_OUTCOME_FORFEITED": "Forfeited against",
}


def _pct(rate):
    return f"{round(rate * 100)}%"


class RulesSummarizer:
    """Fixed templates and thresholds: no cost, same input gives the same text."""

    name = "rules"
    UNUSUAL = 0.4  # gap between this battle's rate and the usual rate worth pointing out

    def summarize_battle(self, facts):
        verb = OUTCOME_WORDS.get(facts["outcome"], facts["outcome"])
        opponent = facts["trainer"] if facts["trainer"] not in ("WILD", "TRAINER_NONE") else "a wild Pokémon"
        if facts["partner_trainer"]:
            opponent += f" and {facts['partner_trainer']}"
        lines = [f"{verb} {opponent} at {facts['map']} in {facts['turns']} turns."]
        lines += self._telemetry_lines(facts.get("telemetry"))

        if not facts["recorded_by_profile"]:
            lines.append("The in-game profile did not record this battle (wild, excluded, or League).")
            return Summary("\n".join(lines), self.name)
        if not facts["habits"]:
            lines.append("No habit situations came up.")
            return Summary("\n".join(lines), self.name)

        lines.append("Habits shown:")
        for habit in sorted(facts["habits"], key=lambda h: -h["chances"]):
            line = f"- When {habit['situation']}, {habit['behavior']}: {habit['hits']} of {habit['chances']}"
            usual = habit["usual_rate"]
            if usual is not None:
                line += f" (usually {_pct(usual)})"
                if habit["chances"] >= 2 and abs(habit["rate"] - usual) >= self.UNUSUAL:
                    line += " - unusual for this player"
            lines.append(line)
        return Summary("\n".join(lines), self.name)

    @staticmethod
    def _counts(counter):
        return ", ".join(f"{name} x{n}" for name, n in sorted(counter.items(), key=lambda item: -item[1]))

    def _telemetry_lines(self, telemetry):
        if not telemetry:
            return []
        lines = []
        if telemetry["player_moves"]:
            lines.append(f"Moves chosen: {self._counts(telemetry['player_moves'])}.")
        if telemetry["player_move_results"]:
            lines.append(f"Their results: {self._counts(telemetry['player_move_results'])}.")
        if telemetry["opponent_moves"]:
            lines.append(f"Opponent's moves: {self._counts(telemetry['opponent_moves'])}.")
        fainted = telemetry["fainted"]
        lines.append(f"Fainted: {len(fainted['player'])} of the player's, {len(fainted['opponent'])} of the opponent's. "
                     f"Switches: {telemetry['switches']}. "
                     f"Critical hits: {telemetry['crits']['dealt']} dealt, {telemetry['crits']['taken']} taken.")
        if telemetry["items_used"]:
            lines.append(f"Items: {self._counts(telemetry['items_used'])}.")
        if telemetry["level_ups"]:
            lines.append(f"Level-ups: {', '.join(telemetry['level_ups'])}.")
        return lines

    def summarize_player(self, facts):
        profile = facts["profile"]
        if profile is None:
            return Summary("No profile recorded yet: fight a trainer with the tracker running.", self.name)

        outcomes = facts["outcomes"]
        lines = [
            f"Profile: {profile['battles_recorded']} battles recorded by the game "
            f"({profile['battles_in_set_mode']} in Set mode, {profile['battles_in_switch_mode']} in Switch mode).",
            f"Tracked by the bridge: {facts['battles_tracked']} battles "
            f"({outcomes.get('B_OUTCOME_WON', 0)} won, {outcomes.get('B_OUTCOME_LOST', 0)} lost).",
        ]

        tendencies = profile["tendencies"]
        if not tendencies:
            lines.append(f"\nNo habit has {profile['min_samples']} samples yet.")
            return Summary("\n".join(lines), self.name)

        # The most extreme rates are the most predictable: an AI can rely on them.
        predictable = sorted(tendencies, key=lambda h: (-abs(h["rate"] - 0.5), -h["chances"]))[:5]
        lines.append("\nMost predictable habits:")
        for habit in predictable:
            lines.append(f"- When {habit['situation']}, the player {tendency(habit['rate'])} "
                         f"{habit['behavior']} ({habit['hits']}/{habit['chances']}).")

        groups = {}
        for habit in tendencies:
            groups.setdefault(habit["group"], []).append(habit)
        lines.append("\nBy topic:")
        for group, habits in groups.items():
            lines.append(f"{group}:")
            for habit in habits:
                lines.append(f"- {tendency(habit['rate']).capitalize()} {habit['behavior']} when "
                             f"{habit['situation']} ({_pct(habit['rate'])} of {habit['chances']}).")

        leveling = facts.get("leveling")
        if leveling:
            candies = leveling["rare_candies"]
            lines.append(f"\nLeveling: {candies.get('BAG', 0)} Rare Candies by hand, {candies.get('POKENAV', 0)} through "
                         f"the PokéNav; {leveling['levels_from_battle']} levels from battle; "
                         f"{leveling['candy_refused_by_level_cap']} candies refused by the level cap; "
                         f"{len(leveling['whiteouts'])} whiteouts.")
        healing = facts.get("healing")
        if healing:
            heals = healing["heals"]
            lines.append(f"Healing: {heals.get('CENTER', 0)} at a Pokémon Center, {heals.get('POKENAV', 0)} with the "
                         f"PokéNav; party at {healing['average_party_hp_percent_before']}% HP on average before healing, "
                         f"{healing['heals_with_fainted_pokemon']} times with a fainted Pokémon, "
                         f"{healing['heals_at_full_hp']} times already at full HP.")

        if facts["trends"]:
            lines.append("\nChanges over time (first half vs second half of tracked battles):")
            for trend in facts["trends"]:
                lines.append(f"- {trend['habit']}: {_pct(trend['early_rate'])} -> {_pct(trend['late_rate'])}")

        if profile["too_few_samples"]:
            lines.append(f"\nNot enough data yet (<{profile['min_samples']} chances): "
                         + ", ".join(profile["too_few_samples"]))
        return Summary("\n".join(lines), self.name)


SYSTEM_PROMPT = """You describe how one player fights trainer battles in a Pokémon Emerald ROM hack, using counters the game recorded while they played.

Context about the game:
- Players can read public documentation listing most trainers' teams and movesets, so a good lead or switch-in may come from the docs rather than from reading the battle.
- Rare Candies are unlimited and there is a level cap, so levels say nothing about skill.
- Do not assume the player is skilled. Describe what they do, not what they intended.
- Battle style: Set mode (no free switch after a KO) is the default; some players change to Switch mode.

How to read the data:
- Each habit is hits/chances. A chance is a turn where the situation came up; a hit is a turn where the player did the behavior. The rate is hits/chances.
- Threat tiers: T0 no super-effective move and no KO against the player's Pokémon; T1 a super-effective move but no KO; T2 the enemy can KO but the player moves first; T3 the enemy moves first and KOs.
- Rates from few chances are weak evidence. Say so instead of drawing conclusions from them.
- "usual_rate" in a battle is the player's rate before that battle; compare against it.
- "telemetry" (when present) is what the game logged inside a battle: the player's moves, move results, the opponent's moves, critical hits, switches, items, fainted Pokémon and level-ups. Move, species and item names are game constants (EARTHQUAKE, MAGCARGO).
- "leveling" counts Rare Candies by source: BAG (used by hand) or POKENAV (an option that spends candies to raise the party to the level cap), candies refused by the level cap, levels gained from battle experience, and whiteouts.
- "healing" counts out-of-battle heals by source: CENTER (Pokémon Center nurse) or POKENAV (a single heal charge, recharged at a Pokémon Center), with the party's average HP before healing and how often a Pokémon had fainted.
- Use only the data given. Do not invent battles, Pokémon, or moves.

Write in {language}, in plain prose a non-player can follow, with Pokémon terms kept as they are."""

PLAYER_TASK = """Summarize this player's fighting style from the data below. Use these sections:
1. Play style: two or three sentences.
2. Predictable habits: the strongest tendencies, with their numbers.
3. How an opponent AI could exploit them: concrete counter-play.
4. Changes over time: only if the data shows trends.
Keep it under 300 words."""

BATTLE_TASK = """Summarize this one battle in three to five sentences: the result, the habits the player showed, and anything unusual compared with their usual rates."""

# Models that accept the server-side refusal fallback ("default" form) on the Claude API.
FALLBACK_MODELS = {"claude-opus-5-5", "claude-opus-5", "claude-fable-5-1", "claude-sonnet-5-5"}

# USD per million tokens (input, output), for the cost estimate shown after each call.
PRICES = {
    "claude-opus-5-5": (4.00, 20.00),
    "claude-sonnet-5-5": (2.00, 10.00),
    "claude-haiku-5-5": (0.10, 0.50),
}


def estimate_cost(summary):
    price = PRICES.get(summary.model)
    if price is None or summary.input_tokens is None:
        return None
    return (summary.input_tokens * price[0] + summary.output_tokens * price[1]) / 1_000_000


class ClaudeSummarizer:
    """LLM summaries through the Anthropic Messages API."""

    name = "claude"

    def __init__(self, model="claude-opus-5-5", effort="low", language="English", client=None):
        self.model = model
        self.effort = effort
        self.language = language
        if client is None:
            try:
                import anthropic
            except ImportError as error:
                raise SummaryError("the claude backend needs the SDK: pip install anthropic") from error
            client = anthropic.Anthropic()  # reads ANTHROPIC_API_KEY (or an `ant auth login` profile)
        self.client = client

    def ask(self, task, facts, output_format=None):
        """One request: the task plus the facts as JSON. output_format constrains the reply (e.g. a JSON schema)."""
        output_config = {"effort": self.effort}
        if output_format:
            output_config["format"] = output_format
        request = {
            "model": self.model,
            "max_tokens": 16000,
            "system": SYSTEM_PROMPT.format(language=self.language),
            "output_config": output_config,
            "messages": [{"role": "user", "content": f"{task}\n\n```json\n{json.dumps(facts, indent=1)}\n```"}],
        }
        if self.model in FALLBACK_MODELS:
            # If a safety classifier declines, the API retries on Anthropic's recommended fallback model.
            request["betas"] = ["server-side-fallback-2026-07-01"]
            request["fallbacks"] = "default"
        response = self.client.beta.messages.create(**request)

        if response.stop_reason == "refusal":
            category = response.stop_details.category if response.stop_details else None
            raise SummaryError(f"the model declined to summarize (category: {category})")
        text = "".join(block.text for block in response.content if block.type == "text").strip()
        if response.stop_reason == "max_tokens":
            text += "\n[summary cut off: max_tokens reached]"
        return Summary(text, self.name, response.model,
                       response.usage.input_tokens, response.usage.output_tokens)

    def summarize_battle(self, facts):
        return self.ask(BATTLE_TASK, facts)

    def summarize_player(self, facts):
        return self.ask(PLAYER_TASK, facts)


def get_summarizer(backend, **options):
    if backend == "rules":
        return RulesSummarizer()
    if backend == "claude":
        return ClaudeSummarizer(**options)
    raise SummaryError(f"unknown summarizer backend {backend!r} (use rules or claude)")
