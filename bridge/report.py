"""Print what the bridge has recorded so far.

    python3 -m bridge.report [--db build/tracker.sqlite]
"""

import argparse

from .game_data import GameData, profile_delta
from .store import DEFAULT_DB, Store


def format_report(store, game_data):
    lines = ["Events:"]
    lines += [f"  {row['type']:<14} {row['n']}" for row in store.event_counts()]

    battles = store.battles()
    lines.append(f"\nFinished battles: {len(battles)}")
    for battle in battles:
        trainer, _ = game_data.opponents(battle["battle_type"], battle["trainer_a"], battle["trainer_b"])
        outcome = game_data.outcome_name(battle["outcome"] or 0)
        place = game_data.map_name(battle["map_group"], battle["map_num"])
        lines.append(f"  {trainer:<32} {outcome:<16} {battle['turns']:>3} turns  {place}")
        delta = profile_delta(game_data.decode_profile(battle["profile_before"]),
                              game_data.decode_profile(battle["profile_after"]))
        for name, counter in (delta or {}).items():
            if counter:
                lines.append(f"      {name.removeprefix('PROFILE_'):<28} {counter['hits']}/{counter['chances']}")

    if battles and battles[-1]["profile_after"]:
        profile = game_data.decode_profile(battles[-1]["profile_after"])
        lines.append(f"\nSaved profile ({profile['battles_recorded']} battles recorded):")
        for name, counter in profile["stats"].items():
            if counter["chances"]:
                rate = 100 * counter["hits"] // counter["chances"]
                lines.append(f"  {name.removeprefix('PROFILE_'):<30} {counter['hits']:>3}/{counter['chances']:<3} {rate:>3}%")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", default=str(DEFAULT_DB))
    args = parser.parse_args()

    store = Store(args.db)
    try:
        print(format_report(store, GameData.load()))
    finally:
        store.close()


if __name__ == "__main__":
    main()
