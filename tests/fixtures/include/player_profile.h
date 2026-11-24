#ifndef GUARD_PLAYER_PROFILE_H
#define GUARD_PLAYER_PROFILE_H

// Player Profile: records how the player plays trainer battles so the
// Elite Four and Champion can adapt to their habits.
// See docs/player_profile_habits.pdf for what every habit counts.

#define PLAYER_PROFILE_MAGIC            0x5050  // "PP"
#define PLAYER_PROFILE_VERSION          1

// Config
#define PROFILE_DECAY_THRESHOLD         200     // Halve a habit's counters when its chances reach this value.
#define PROFILE_ACE_SLOTS               6       // Mons tracked in the ace table.
#define PROFILE_ACE_DECAY_BATTLES       10      // Halve the ace table every this many recorded battles.
#define PROFILE_ACE_MIN_TURNS           4       // A mon needs this many tracked turns to count as the ace.
#define PROFILE_RECORD_POSTGAME         FALSE   // If FALSE, battles after FLAG_SYS_GAME_CLEAR are not recorded.

enum PlayerProfileStat
{
    // Switching under threat
    PROFILE_SWITCH_T1,
    PROFILE_SWITCH_T2,
    PROFILE_SWITCH_T3_ACE,
    PROFILE_SWITCH_T3_OTHER,
    // Staying in
    PROFILE_MISREAD_RACE,
    PROFILE_REVENGE_AFTER_SACK,
    // Switch quality
    PROFILE_SWITCH_TO_IMMUNE,
    PROFILE_SWITCH_TO_RESIST,
    PROFILE_REACTIVE_SWITCH,
    // Switching while safe
    PROFILE_SWITCH_T0,
    PROFILE_SWITCH_REPOSITION,
    PROFILE_RESET_IMMEDIATE,
    PROFILE_RESET_DELAYED,
    PROFILE_SWITCH_ABILITY,
    PROFILE_SWITCH_DOUBLE_READ,
    // Battle style
    PROFILE_FREE_SWITCH_TAKEN,
    PROFILE_FREE_SWITCH_COUNTER,
    // Setup
    PROFILE_SETUP_ENTRY_SOUND,
    PROFILE_SETUP_ENTRY_RISKY,
    PROFILE_SETUP_PUNISHED,
    PROFILE_SETUP_WHEN_SAFE,
    // Healing (moves only: most players run Nuzlocke rules, so bag items in battle don't count)
    PROFILE_HEAL_AT_LOW_HP,
    // Status
    PROFILE_STATUS_OPEN_VALID,
    PROFILE_STATUS_INTO_BAD,
    // Move choice
    PROFILE_KO_SAFE_PICK,
    PROFILE_ACCURACY_GREED,
    PROFILE_DRAWBACK_USE,
    PROFILE_ATTACK_THE_SWITCH,
    PROFILE_SPREAD_PREFERENCE,
    // Priority
    PROFILE_PRIORITY_NEEDED,
    PROFILE_PRIORITY_REDUNDANT,
    PROFILE_PRIORITY_INTO_BLOCK,
    PROFILE_SUCKER_USE,
    // Protect
    PROFILE_PROTECT_SCOUT_SINGLES,
    PROFILE_PROTECT_SCOUT_DOUBLES,
    PROFILE_SCOUT_THEN_SWITCH,
    PROFILE_PROTECT_STALL,
    PROFILE_PROTECT_REPEAT,
    PROFILE_PROTECT_SPREAD_COMBO,
    // Focus fire
    PROFILE_FOCUS_NEEDED,
    PROFILE_FOCUS_OVERKILL,
    PROFILE_REDIRECT_RESPECT,
    // Leads
    PROFILE_LEAD_COUNTERS_ENEMY_LEAD,
    PROFILE_STAT_COUNT,
};

enum PlayerProfileStatusKind
{
    PROFILE_STATUS_PARALYSIS,
    PROFILE_STATUS_BURN,
    PROFILE_STATUS_POISON,
    PROFILE_STATUS_SLEEP,
    PROFILE_STATUS_KIND_COUNT,
};

enum PlayerProfileFocusTarget
{
    PROFILE_FOCUS_THREAT,
    PROFILE_FOCUS_EASY_KO,
    PROFILE_FOCUS_SUPPORT,
    PROFILE_FOCUS_FASTEST,
    PROFILE_FOCUS_TARGET_COUNT,
};

struct ProfileCounter
{
    u8 hits;    // times the player did it
    u8 chances; // times the player could have done it
};

struct PlayerBattleProfile
{
    u16 magic;
    u8 version;
    u8 battlesRecorded;
    u8 battlesSet;
    u8 battlesSwitch;
    u8 lastStyle;
    u8 battlesSinceAceDecay;
    struct ProfileCounter stats[PROFILE_STAT_COUNT];
    u8 statusKind[PROFILE_STATUS_KIND_COUNT];
    u8 focusTarget[PROFILE_FOCUS_TARGET_COUNT];
    u32 acePersonality[PROFILE_ACE_SLOTS];
    u8 aceTurns[PROFILE_ACE_SLOTS];
};

// Battle hooks
void PlayerProfile_OnBattleStart(void);
void PlayerProfile_RecordTurn(void);
void PlayerProfile_OnFreeSwitchOffered(void);
void PlayerProfile_OnFreeSwitchTaken(void);
void PlayerProfile_CommitBattle(void);

// Queries
struct PlayerBattleProfile *GetPlayerProfile(void);
void ResetPlayerProfile(void);
s32 GetPlayerProfileBias(enum PlayerProfileStat stat, u32 minSamples);
u32 GetPlayerProfileRate(const enum PlayerProfileStat *stats, u32 count, u32 minSamples);

// Move classes the recorder uses, shared with the profile AI so both judge moves the same way
bool32 Profile_IsSetupMove(enum Move move);
bool32 Profile_IsProtectMove(enum Move move);
bool32 Profile_IsSelfHealingMove(enum Move move);
bool32 Profile_IsStatusMove(enum Move move);
bool32 Profile_IsSpreadMove(enum Move move);

#endif // GUARD_PLAYER_PROFILE_H
