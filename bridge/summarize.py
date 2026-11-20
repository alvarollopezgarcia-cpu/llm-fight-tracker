"""Describe the player's fighting style, or one battle, and store the summary.

    python3 -m bridge.summarize                       # whole player, rules backend (free)
    python3 -m bridge.summarize --battle last         # the last finished battle
    python3 -m bridge.summarize --battle 3            # battle id 3 (ids: python3 -m bridge.summarize --list)
    python3 -m bridge.summarize --backend claude      # LLM summary (needs ANTHROPIC_API_KEY)
    python3 -m bridge.summarize --facts               # print the facts the summarizer would read, no summary

Options for the claude backend: --model (default claude-opus-5-5), --effort, --language.
"""

import argparse
import json
import sys

from .facts import MIN_SAMPLES, battle_facts, player_facts
from .game_data import GameData
from .store import DEFAULT_DB, Store
from .summarizers import SummaryError, estimate_cost, get_summarizer


def list_battles(store, game_data):
    for battle in store.battles():
        print(f"{battle['id']:>4}  {game_data.opponents(battle['battle_type'], battle['trainer_a'], battle['trainer_b'])[0]:<32} "
              f"{game_data.outcome_name(battle['outcome'] or 0):<16} "
              f"{game_data.map_name(battle['map_group'], battle['map_num'])}")


def pick_battle(store, which):
    if which == "last":
        battles = store.battles()
        return battles[-1] if battles else None
    return store.battle(int(which))


def run(args):
    store = Store(args.db)
    try:
        game_data = GameData.load()
        if args.list:
            list_battles(store, game_data)
            return 0

        if args.battle:
            battle = pick_battle(store, args.battle)
            if battle is None or battle["end_seq"] is None:
                print(f"no finished battle {args.battle!r}", file=sys.stderr)
                return 1
            scope, facts = "battle", battle_facts(battle, game_data, store.telemetry(battle))
        else:
            scope, facts = "player", player_facts(store, game_data, min_samples=args.min_samples)

        if args.facts:
            print(json.dumps(facts, indent=2, ensure_ascii=False))
            return 0

        options = {}
        if args.backend == "claude":
            options = {"model": args.model, "effort": args.effort, "language": args.language}
        summarizer = get_summarizer(args.backend, **options)
        summary = summarizer.summarize_battle(facts) if scope == "battle" else summarizer.summarize_player(facts)
        store.add_summary(scope, summary, facts, battle_id=facts.get("battle_id"))

        print(summary.text)
        if summary.input_tokens is not None:
            cost = estimate_cost(summary)
            cost_text = f", about ${cost:.4f}" if cost is not None else ""
            print(f"\n[{summary.model}: {summary.input_tokens} in / {summary.output_tokens} out tokens{cost_text}]")
        return 0
    finally:
        store.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", default=str(DEFAULT_DB))
    parser.add_argument("--backend", choices=["rules", "claude"], default="rules")
    parser.add_argument("--battle", help="battle id, or 'last'")
    parser.add_argument("--list", action="store_true", help="list finished battles and their ids")
    parser.add_argument("--facts", action="store_true", help="print the summarizer input instead of a summary")
    parser.add_argument("--min-samples", type=int, default=MIN_SAMPLES)
    parser.add_argument("--model", default="claude-opus-5-5")
    parser.add_argument("--effort", default="low", choices=["low", "medium", "high", "xhigh", "max"])
    parser.add_argument("--language", default="English")
    args = parser.parse_args()

    if args.backend != "claude":
        try:
            sys.exit(run(args))
        except SummaryError as error:
            sys.exit(f"summary failed: {error}")

    try:
        import anthropic
    except ImportError:
        sys.exit("the claude backend needs the SDK: pip install anthropic")
    try:
        sys.exit(run(args))
    except SummaryError as error:
        sys.exit(f"summary failed: {error}")
    except anthropic.AuthenticationError:
        sys.exit("the Anthropic API rejected the key: set ANTHROPIC_API_KEY to a valid key")
    except anthropic.PermissionDeniedError as error:
        sys.exit(f"the API key can't do this (often: no credits left): {error.message}")
    except anthropic.NotFoundError:
        sys.exit(f"unknown model {args.model!r}")
    except anthropic.RateLimitError:
        sys.exit("rate limited: try again in a minute")
    except anthropic.APIStatusError as error:
        sys.exit(f"Anthropic API error {error.status_code}: {error.message}")
    except anthropic.APIConnectionError:
        sys.exit("could not reach the Anthropic API: check the connection")
    except anthropic.AnthropicError as error:  # e.g. no credentials configured at all
        sys.exit(f"Anthropic client error: {error}")


if __name__ == "__main__":
    main()
