#ifndef GUARD_TELEMETRY_H
#define GUARD_TELEMETRY_H

#include "config/telemetry.h"

// Ring buffer of game events read by the LLM fight tracker (tools/llm_tracker).
// The game only appends: it never waits for a reader. The reader keeps its own copy of
// `head` and reads the entries written since; if it falls more than TELEMETRY_CAPACITY
// entries behind, the oldest ones were overwritten and it counts them as lost.

#define TELEMETRY_MAGIC     0x4D4C4554 // "TELM"
#define TELEMETRY_CAPACITY  64         // power of two

enum TelemetryEventType
{
    TELEMETRY_NONE,
    TELEMETRY_ACTION,           // arg: battler      a: B_ACTION_*            b: move / species switched in   c: turn
    TELEMETRY_MOVE_RESULT,      // arg: attacker | target << 4   a: move      b: TELEMETRY_RESULT_* | crit    c: turn
    TELEMETRY_SWITCH_IN,        // arg: battler      a: species               b: party index                  c: turn
    TELEMETRY_FAINT,            // arg: battler      a: species               b: last attacker battler        c: turn
    TELEMETRY_ITEM_IN_BATTLE,   // arg: battler      a: item                  b: species on the field         c: turn
    TELEMETRY_RARE_CANDY,       // arg: TELEMETRY_SOURCE_*   a: species       b: new level                    c: candies used
    TELEMETRY_CAP_BLOCKED,      // arg: TELEMETRY_SOURCE_BAG   a: species     b: level cap (a Rare Candy refused by the cap)
    TELEMETRY_LEVEL_UP,         // arg: party slot   a: species               b: new level (from battle experience)
    TELEMETRY_WHITEOUT,         // a: map group << 8 | map number where the player blacked out
    TELEMETRY_HEAL,             // arg: TELEMETRY_SOURCE_CENTER / _POKENAV   a: map   b: party HP % before   c: fainted before
    TELEMETRY_EVENT_COUNT,
};

// Where a Rare Candy or a heal came from
enum TelemetrySource
{
    TELEMETRY_SOURCE_BAG,       // Rare Candy used by hand from the bag
    TELEMETRY_SOURCE_POKENAV,   // RARE_CANDY: the PokéNav LEVEL option, which spends candies up to the level cap
                                // (it reaching the cap shows as b == the cap; it never logs CAP_BLOCKED)
                                // HEAL: the PokéNav HEAL option (one charge, recharged at a Pokémon Center)
    TELEMETRY_SOURCE_CENTER,    // HEAL: the Pokémon Center nurse
};

// MOVE_RESULT outcome, low 7 bits of b; bit 7 set for a critical hit
enum TelemetryMoveResult
{
    TELEMETRY_RESULT_HIT,
    TELEMETRY_RESULT_SUPER_EFFECTIVE,
    TELEMETRY_RESULT_NOT_VERY_EFFECTIVE,
    TELEMETRY_RESULT_NO_EFFECT,
    TELEMETRY_RESULT_MISSED,
    TELEMETRY_RESULT_PROTECTED,
    TELEMETRY_RESULT_FAILED,
    TELEMETRY_RESULT_CANT_MOVE,  // asleep, paralyzed, flinched...
};
#define TELEMETRY_RESULT_CRIT 0x80

// 8 bytes. -mabi=apcs-gnu rounds struct sizes up to 4 bytes, so keep it a multiple of 4.
struct TelemetryEvent
{
    u8 type;
    u8 arg;
    u16 a;
    u16 b;
    u16 c;
};

struct TelemetryBuffer
{
    u32 magic;
    u32 head;   // total events ever written; the next one goes to events[head % TELEMETRY_CAPACITY]
    struct TelemetryEvent events[TELEMETRY_CAPACITY];
};

// Mailbox: the other direction. The tracker's script writes texts here (already in the game's
// character encoding, wrapped for a message box, EOS-terminated), then sets `magic`. The game only
// reads it and never waits: an empty mailbox means a fallback text or nothing.
#define MAILBOX_MAGIC        0x4C49414D // "MAIL"
#define MAILBOX_REPORT_SIZE  1000       // shown with gStringVar4, which holds 1000 bytes
#define MAILBOX_LINE_SIZE    300

struct Mailbox
{
    u32 magic;
    u8 report[MAILBOX_REPORT_SIZE];       // PokéNav INFORME: the player's play style
    u8 championLine[MAILBOX_LINE_SIZE];   // said by the Champion before the battle
};

void Telemetry_Log(u32 type, u32 arg, u32 a, u32 b, u32 c);
void Telemetry_LogBattleStart(void);
void Telemetry_LogTurnActions(void);
void Telemetry_LogMoveResult(u32 attacker, u32 target, u32 move);
void Telemetry_LogHeal(u32 source);
void Telemetry_LogCenterHeal(void); // callnative from the nurse script, so it exists in every build

// callnative from scripts, so they exist in every build
void Mailbox_LoadReport(void);       // the report, or a fallback text, into gStringVar4
void Mailbox_LoadChampionLine(void); // VAR_RESULT = TRUE and the line in gStringVar4 if there is one

// Use this at hook sites: when TELEMETRY_ENABLED is FALSE the call and its arguments compile away.
#define TELEMETRY_LOG(...) do { if (TELEMETRY_ENABLED) Telemetry_Log(__VA_ARGS__); } while (0)
#define TELEMETRY_CALL(call) do { if (TELEMETRY_ENABLED) call; } while (0)

#endif // GUARD_TELEMETRY_H
