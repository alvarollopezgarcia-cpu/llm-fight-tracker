"""SQLite storage: every raw event, plus one row per battle derived from them."""

import json
import sqlite3
import time
from pathlib import Path

DEFAULT_DB = Path(__file__).resolve().parent.parent / "build" / "tracker.sqlite"

SCHEMA = """
CREATE TABLE IF NOT EXISTS sessions (
    id          INTEGER PRIMARY KEY,
    started_at  REAL NOT NULL,
    build       TEXT
);
CREATE TABLE IF NOT EXISTS events (
    session_id  INTEGER NOT NULL REFERENCES sessions(id),
    seq         INTEGER NOT NULL,
    frame       INTEGER,
    type        TEXT NOT NULL,
    payload     TEXT NOT NULL,
    received_at REAL NOT NULL,
    PRIMARY KEY (session_id, seq)
);
CREATE TABLE IF NOT EXISTS battles (
    id              INTEGER PRIMARY KEY,
    session_id      INTEGER NOT NULL REFERENCES sessions(id),
    start_seq       INTEGER NOT NULL,
    end_seq         INTEGER,
    trainer_a       INTEGER,
    trainer_b       INTEGER,
    battle_type     INTEGER,
    map_group       INTEGER,
    map_num         INTEGER,
    outcome         INTEGER,
    turns           INTEGER,
    profile_before  TEXT,
    profile_after   TEXT
);
CREATE TABLE IF NOT EXISTS summaries (
    id              INTEGER PRIMARY KEY,
    created_at      REAL NOT NULL,
    scope           TEXT NOT NULL,          -- 'battle' or 'player'
    battle_id       INTEGER REFERENCES battles(id),
    backend         TEXT NOT NULL,
    model           TEXT,
    text            TEXT NOT NULL,
    facts           TEXT NOT NULL,          -- the exact input the summarizer saw
    input_tokens    INTEGER,
    output_tokens   INTEGER
);
CREATE TABLE IF NOT EXISTS mailbox (
    id              INTEGER PRIMARY KEY,
    created_at      REAL NOT NULL,
    backend         TEXT NOT NULL,
    model           TEXT,
    report          TEXT NOT NULL,          -- PokéNav INFORME
    champion_line   TEXT NOT NULL,          -- said by the Champion before the battle
    facts           TEXT NOT NULL,
    input_tokens    INTEGER,
    output_tokens   INTEGER
);
CREATE TABLE IF NOT EXISTS plans (
    id              INTEGER PRIMARY KEY,
    created_at      REAL NOT NULL,
    backend         TEXT NOT NULL,          -- 'rules', 'claude' or 'rules-fallback'
    model           TEXT,
    plan            TEXT NOT NULL,          -- validated trainer entries (JSON)
    explanation     TEXT NOT NULL,
    notes           TEXT,
    facts           TEXT NOT NULL,          -- the exact input the planner saw
    input_tokens    INTEGER,
    output_tokens   INTEGER
);
"""

# Columns added after the first release of a table: (table, column, type)
MIGRATIONS = [("battles", "plan_id", "INTEGER REFERENCES plans(id)")]


class Store:
    def __init__(self, path=DEFAULT_DB):
        if path != ":memory:":
            Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path)
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        for table, column, kind in MIGRATIONS:
            columns = {row["name"] for row in self.db.execute(f"PRAGMA table_info({table})")}
            if column not in columns:
                self.db.execute(f"ALTER TABLE {table} ADD COLUMN {column} {kind}")
        self.db.commit()

    def close(self):
        self.db.close()

    def start_session(self):
        cursor = self.db.execute("INSERT INTO sessions (started_at) VALUES (?)", (time.time(),))
        self.db.commit()
        return cursor.lastrowid

    def add_event(self, session_id, event):
        """Store one event. Returns False if this (session, seq) was already stored."""
        cursor = self.db.execute(
            "INSERT OR IGNORE INTO events (session_id, seq, frame, type, payload, received_at) VALUES (?, ?, ?, ?, ?, ?)",
            (session_id, event["seq"], event.get("frame"), event["type"], json.dumps(event), time.time()),
        )
        if cursor.rowcount == 0:
            return False

        kind = event["type"]
        if kind == "hello":
            self.db.execute("UPDATE sessions SET build = ? WHERE id = ?", (event.get("build"), session_id))
        elif kind == "battle_start":
            self.db.execute(
                "INSERT INTO battles (session_id, start_seq, trainer_a, trainer_b, battle_type, map_group, map_num, profile_before)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (session_id, event["seq"], event.get("opponentA"), event.get("opponentB"), event.get("battleTypeFlags"),
                 event.get("mapGroup"), event.get("mapNum"), event.get("profile")),
            )
        elif kind == "battle_end":
            self.db.execute(
                "UPDATE battles SET end_seq = ?, outcome = ?, turns = ?, profile_after = ?"
                " WHERE id = (SELECT id FROM battles WHERE session_id = ? AND end_seq IS NULL ORDER BY start_seq DESC LIMIT 1)",
                (event["seq"], event.get("outcome"), event.get("turns"), event.get("profile"), session_id),
            )
        elif kind == "rewind":
            # A save state was loaded: a battle that was open never finished as recorded.
            self.db.execute("DELETE FROM battles WHERE session_id = ? AND end_seq IS NULL", (session_id,))
        self.db.commit()
        return True

    def battles(self, finished_only=True):
        query = "SELECT * FROM battles"
        if finished_only:
            query += " WHERE end_seq IS NOT NULL"
        return self.db.execute(query + " ORDER BY session_id, start_seq").fetchall()

    def battle_ended_by(self, session_id, end_seq):
        return self.db.execute("SELECT * FROM battles WHERE session_id = ? AND end_seq = ?", (session_id, end_seq)).fetchone()

    def telemetry(self, battle=None):
        """Raw telemetry events in order: those inside one battle, or all of them."""
        query, args = "SELECT payload FROM events WHERE type = 'telemetry'", ()
        if battle is not None:
            query += " AND session_id = ? AND seq > ? AND seq < ?"
            args = (battle["session_id"], battle["start_seq"], battle["end_seq"])
        rows = self.db.execute(query + " ORDER BY session_id, seq", args).fetchall()
        return [json.loads(row["payload"]) for row in rows]

    def battle(self, battle_id):
        return self.db.execute("SELECT * FROM battles WHERE id = ?", (battle_id,)).fetchone()

    def add_summary(self, scope, summary, facts, battle_id=None):
        cursor = self.db.execute(
            "INSERT INTO summaries (created_at, scope, battle_id, backend, model, text, facts, input_tokens, output_tokens)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (time.time(), scope, battle_id, summary.backend, summary.model, summary.text, json.dumps(facts),
             summary.input_tokens, summary.output_tokens),
        )
        self.db.commit()
        return cursor.lastrowid

    def summaries(self, scope=None):
        query, args = "SELECT * FROM summaries", ()
        if scope:
            query, args = query + " WHERE scope = ?", (scope,)
        return self.db.execute(query + " ORDER BY id", args).fetchall()

    def add_mailbox(self, texts, facts):
        cursor = self.db.execute(
            "INSERT INTO mailbox (created_at, backend, model, report, champion_line, facts, input_tokens, output_tokens)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (time.time(), texts.backend, texts.model, texts.report, texts.champion_line, json.dumps(facts),
             texts.input_tokens, texts.output_tokens),
        )
        self.db.commit()
        return cursor.lastrowid

    def latest_mailbox(self):
        return self.db.execute("SELECT * FROM mailbox ORDER BY id DESC LIMIT 1").fetchone()

    def add_plan(self, plan, facts):
        cursor = self.db.execute(
            "INSERT INTO plans (created_at, backend, model, plan, explanation, notes, facts, input_tokens, output_tokens)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (time.time(), plan.backend, plan.model, json.dumps(plan.trainers), plan.explanation,
             json.dumps(plan.notes), json.dumps(facts), plan.input_tokens, plan.output_tokens),
        )
        self.db.commit()
        return cursor.lastrowid

    def latest_plan(self):
        return self.db.execute("SELECT * FROM plans ORDER BY id DESC LIMIT 1").fetchone()

    def plans(self):
        return self.db.execute("SELECT * FROM plans ORDER BY id").fetchall()

    def link_battle_plan(self, session_id, start_seq, plan_id):
        """Mark the battle that started at this event as fought under a plan."""
        self.db.execute("UPDATE battles SET plan_id = ? WHERE session_id = ? AND start_seq = ?",
                        (plan_id, session_id, start_seq))
        self.db.commit()

    def plan_battles(self, plan_id):
        return self.db.execute("SELECT * FROM battles WHERE plan_id = ? AND end_seq IS NOT NULL ORDER BY session_id, start_seq",
                               (plan_id,)).fetchall()

    def event_counts(self):
        return self.db.execute("SELECT type, COUNT(*) AS n FROM events GROUP BY type ORDER BY n DESC").fetchall()
