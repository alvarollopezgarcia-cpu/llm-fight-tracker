"""Plain-language meaning of each Player Profile habit.

Each habit is a (hits, chances) counter kept by the ROM (src/player_profile.c).
`situation` is what makes a turn count as a chance; `behavior` is what the player
did to score a hit. Both read as: "When <situation>, the player <tendency> <behavior>."
Definitions follow docs/player_profile_habits.pdf.
"""

from collections import namedtuple

Habit = namedtuple("Habit", "group situation behavior")

HABITS = {
    # Switching under threat (T1: enemy has a super-effective move; T2: enemy KOs but the player
    # moves first; T3: enemy moves first and KOs)
    "PROFILE_SWITCH_T1": Habit("Switching under threat",
                               "its Pokémon faced a super-effective move but no KO (T1)", "switched out"),
    "PROFILE_SWITCH_T2": Habit("Switching under threat",
                               "the enemy could KO its Pokémon but it moved first (T2)", "switched out"),
    "PROFILE_SWITCH_T3_ACE": Habit("Switching under threat",
                                   "its ace was about to be KO'd before acting (T3)", "switched the ace out"),
    "PROFILE_SWITCH_T3_OTHER": Habit("Switching under threat",
                                     "a non-ace Pokémon was about to be KO'd before acting (T3)", "switched it out"),
    # Staying in
    "PROFILE_MISREAD_RACE": Habit("Staying in",
                                  "it stayed in and attacked under a KO threat",
                                  "picked a move that only KOs if it had moved first (misread the speed race)"),
    "PROFILE_REVENGE_AFTER_SACK": Habit("Staying in",
                                        "a Pokémon fainted after staying in against a faster KO",
                                        "followed up with a revenge killer or an immune switch-in, or crippled the attacker first"),
    # Switch quality
    "PROFILE_SWITCH_TO_IMMUNE": Habit("Switch quality",
                                      "it switched with an enemy on the field",
                                      "brought in a Pokémon immune to the enemy's attacks"),
    "PROFILE_SWITCH_TO_RESIST": Habit("Switch quality",
                                      "it switched with an enemy on the field",
                                      "brought in a Pokémon that resists the enemy's attacks"),
    "PROFILE_REACTIVE_SWITCH": Habit("Switch quality",
                                     "the enemy brought in a Pokémon that walls its current one",
                                     "switched out the next turn"),
    # Switching while safe (T0: no super-effective move and no KO against it)
    "PROFILE_SWITCH_T0": Habit("Switching while safe",
                               "its Pokémon was not threatened (T0)", "switched anyway"),
    "PROFILE_SWITCH_REPOSITION": Habit("Switching while safe",
                                       "it was safe but a benched Pokémon threatened the enemy more",
                                       "switched to that better attacker"),
    "PROFILE_RESET_IMMEDIATE": Habit("Switching while safe",
                                     "its Pokémon got a condition a switch clears (stat drops, Toxic, confusion, Taunt, Encore, Leech Seed...)",
                                     "switched out right away"),
    "PROFILE_RESET_DELAYED": Habit("Switching while safe",
                                   "its Pokémon got a condition a switch clears",
                                   "switched out after waiting at least a turn"),
    "PROFILE_SWITCH_ABILITY": Habit("Switching while safe",
                                    "its Pokémon had Regenerator while hurt or Natural Cure while statused",
                                    "switched out to use the ability"),
    "PROFILE_SWITCH_DOUBLE_READ": Habit("Switching while safe",
                                        "it switched while not threatened",
                                        "predicted the AI's own switch and brought in a Pokémon that beats it"),
    # Battle style
    "PROFILE_FREE_SWITCH_TAKEN": Habit("Battle style",
                                       "the game offered a free switch after a KO (Switch mode)", "took it"),
    "PROFILE_FREE_SWITCH_COUNTER": Habit("Battle style",
                                         "it took the free switch",
                                         "brought in a Pokémon that beats the enemy's next Pokémon"),
    # Setup
    "PROFILE_SETUP_ENTRY_SOUND": Habit("Setup",
                                       "a Pokémon with a boosting move came in and setting up was safe",
                                       "set up immediately"),
    "PROFILE_SETUP_ENTRY_RISKY": Habit("Setup",
                                       "a Pokémon with a boosting move came in and setting up was risky",
                                       "set up anyway"),
    "PROFILE_SETUP_PUNISHED": Habit("Setup",
                                    "it set up", "lost the Pokémon before it could use the boost"),
    "PROFILE_SETUP_WHEN_SAFE": Habit("Setup",
                                     "a Pokémon with a boosting move was safe later in its stay", "set up"),
    # Healing
    # Moves only: items in battle aren't tracked, since most players follow Nuzlocke rules
    "PROFILE_HEAL_AT_LOW_HP": Habit("Healing",
                                    "its Pokémon was below 40% HP, knew a healing move, and healing would keep it alive",
                                    "used the healing move"),
    # Status
    "PROFILE_STATUS_OPEN_VALID": Habit("Status",
                                       "a Pokémon with a status move came in against a valid target",
                                       "opened with the status move"),
    "PROFILE_STATUS_INTO_BAD": Habit("Status",
                                     "it used a status move",
                                     "used it into a target where it fails (immunity, ability or item)"),
    # Move choice
    "PROFILE_KO_SAFE_PICK": Habit("Move choice",
                                  "two or more of its moves could KO",
                                  "picked the most accurate one with no drawback"),
    "PROFILE_ACCURACY_GREED": Habit("Move choice",
                                    "its strongest move was inaccurate and a reliable move was nearly as good",
                                    "picked the inaccurate move"),
    "PROFILE_DRAWBACK_USE": Habit("Move choice",
                                  "its strongest move had a drawback and another move did most of the damage",
                                  "picked the drawback move"),
    "PROFILE_ATTACK_THE_SWITCH": Habit("Move choice",
                                       "the enemy was in a clearly bad matchup",
                                       "used a move aimed at the enemy's best switch-in"),
    "PROFILE_SPREAD_PREFERENCE": Habit("Move choice",
                                       "both enemies were up and it had a spread move", "used the spread move"),
    # Priority
    "PROFILE_PRIORITY_NEEDED": Habit("Priority",
                                     "it was slower and a priority move would KO", "used priority"),
    "PROFILE_PRIORITY_REDUNDANT": Habit("Priority",
                                        "it was faster and both priority and a normal move would KO",
                                        "used priority anyway"),
    "PROFILE_PRIORITY_INTO_BLOCK": Habit("Priority",
                                         "it used a priority move",
                                         "used it into something that blocks it (Psychic Terrain, Dazzling, immunity)"),
    "PROFILE_SUCKER_USE": Habit("Priority",
                                "Sucker Punch or Thunderclap would KO", "used it"),
    # Protect
    "PROFILE_PROTECT_SCOUT_SINGLES": Habit("Protect",
                                           "a Pokémon with Protect came in (singles)", "opened with Protect"),
    "PROFILE_PROTECT_SCOUT_DOUBLES": Habit("Protect",
                                           "a Pokémon with Protect came in (doubles)", "opened with Protect"),
    "PROFILE_SCOUT_THEN_SWITCH": Habit("Protect",
                                       "it scouted with Protect and the enemy revealed a KO threat or set up",
                                       "switched the next turn"),
    "PROFILE_PROTECT_STALL": Habit("Protect",
                                   "Protect would gain something (enemy poisoned or seeded, own Leftovers, enemy Trick Room or Tailwind ending...)",
                                   "stalled with Protect"),
    "PROFILE_PROTECT_REPEAT": Habit("Protect",
                                    "it had just used Protect", "used it again"),
    "PROFILE_PROTECT_SPREAD_COMBO": Habit("Protect",
                                          "one Pokémon had an ally-hitting spread move and the ally knew Protect",
                                          "used the spread move while the ally protected"),
    # Focus fire (doubles)
    "PROFILE_FOCUS_NEEDED": Habit("Focus fire",
                                  "only both attacks together could KO an enemy", "focused that enemy"),
    "PROFILE_FOCUS_OVERKILL": Habit("Focus fire",
                                    "one attack alone already KO'd an enemy", "doubled into that enemy anyway"),
    "PROFILE_REDIRECT_RESPECT": Habit("Focus fire",
                                      "an enemy Follow Me or Rage Powder was active", "got around the redirection"),
    # Leads
    "PROFILE_LEAD_COUNTERS_ENEMY_LEAD": Habit("Leads",
                                              "a trainer battle started",
                                              "led with a Pokémon that counters the documented enemy lead"),
}


def short_name(name):
    return name.removeprefix("PROFILE_")


def tendency(rate):
    """Frequency word for a hit rate between 0 and 1."""
    if rate >= 0.85:
        return "almost always"
    if rate >= 0.60:
        return "usually"
    if rate >= 0.35:
        return "sometimes"
    if rate >= 0.10:
        return "rarely"
    return "almost never"
