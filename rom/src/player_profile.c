#include "global.h"
#include "battle.h"
#include "battle_ai_main.h"
#include "battle_ai_util.h"
#include "battle_main.h"
#include "battle_script_commands.h"
#include "battle_setup.h"
#include "battle_util.h"
#include "event_data.h"
#include "item.h"
#include "player_profile.h"
#include "pokemon.h"
#include "constants/abilities.h"
#include "constants/battle_move_effects.h"
#include "constants/hold_effects.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/opponents.h"

// Player Profile recording.
// Every turn of a recorded trainer battle, the player's choices are compared
// against the board and tallied per habit. Tallies live in RAM and are merged
// into the save when the battle ends, so a battle the player resets out of
// never counts. Nothing here consumes RNG, so battles play out identically.

enum ThreatTier
{
    THREAT_T0, // safe
    THREAT_T1, // bad matchup
    THREAT_T2, // enemy KOs, player acts first
    THREAT_T3, // enemy acts first and KOs
};

struct ProfileDamage
{
    u16 median;
    u16 minimum;
    uq4_12_t effectiveness;
};

struct ProfileThreat
{
    enum ThreatTier tier;
    enum BattlerId threatBattler;
    bool32 winningRace;
    u32 totalBestDamage; // sum of every enemy's best hit on this mon
};

struct ProfileBattlerState
{
    u8 partyIndex;
    u8 firstAction:1;
    u8 justEntered:1;
    u8 protectedLastTurn:1;
    u8 scoutPending:1;
    u8 sackPending:1;
    u8 sackStatused:1;
    u8 setupPending:2;
    u8 resetTurns;
    u8 setupPartyIndex;
    u8 sackPartyIndex;
    u8 sackThreat;
};

struct ProfileTally
{
    struct ProfileCounter stats[PROFILE_STAT_COUNT];
    u8 statusKind[PROFILE_STATUS_KIND_COUNT];
    u8 focusTarget[PROFILE_FOCUS_TARGET_COUNT];
};

struct ProfileBattleState
{
    bool8 active;
    bool8 leadRecorded;
    u8 acePartyIndex;
    u8 turnsOnField[PARTY_SIZE];
    struct ProfileBattlerState battlers[MAX_BATTLERS_COUNT];
    struct ProfileTally tally;
    enum Ability abilities[MAX_BATTLERS_COUNT];
    enum HoldEffect holdEffects[MAX_BATTLERS_COUNT];
    struct ProfileDamage dmg[MAX_BATTLERS_COUNT][MAX_BATTLERS_COUNT][MAX_MON_MOVES]; // attacker, target, moveIndex
};

static EWRAM_DATA struct ProfileBattleState sProfile = {0};

static const u16 sLeagueTrainers[] =
{
    TRAINER_SIDNEY,
    TRAINER_PHOEBE,
    TRAINER_GLACIA,
    TRAINER_DRAKE,
    TRAINER_WALLACE,
    TRAINER_LLOPEZ_DUO,
    TRAINER_CANETE_DUO,
};

// Habits shaped by the Set / Switch battle style; halved when the style changes.
static const enum PlayerProfileStat sStyleDependentStats[] =
{
    PROFILE_SWITCH_T1,
    PROFILE_SWITCH_T2,
    PROFILE_SWITCH_T3_ACE,
    PROFILE_SWITCH_T3_OTHER,
    PROFILE_SWITCH_TO_IMMUNE,
    PROFILE_SWITCH_TO_RESIST,
    PROFILE_REACTIVE_SWITCH,
    PROFILE_SWITCH_T0,
    PROFILE_SWITCH_REPOSITION,
    PROFILE_RESET_IMMEDIATE,
    PROFILE_RESET_DELAYED,
    PROFILE_SWITCH_ABILITY,
    PROFILE_SWITCH_DOUBLE_READ,
    PROFILE_FREE_SWITCH_TAKEN,
    PROFILE_FREE_SWITCH_COUNTER,
};

// ---------------------------------------------------------------------------
// Saved profile
// ---------------------------------------------------------------------------

void ResetPlayerProfile(void)
{
    struct PlayerBattleProfile *profile = &gSaveBlock3Ptr->playerProfile;

    memset(profile, 0, sizeof(*profile));
    profile->magic = PLAYER_PROFILE_MAGIC;
    profile->version = PLAYER_PROFILE_VERSION;
}

struct PlayerBattleProfile *GetPlayerProfile(void)
{
    struct PlayerBattleProfile *profile = &gSaveBlock3Ptr->playerProfile;

    if (profile->magic != PLAYER_PROFILE_MAGIC || profile->version != PLAYER_PROFILE_VERSION)
        ResetPlayerProfile();
    return profile;
}

s32 GetPlayerProfileBias(enum PlayerProfileStat stat, u32 minSamples)
{
    const struct ProfileCounter *counter = &GetPlayerProfile()->stats[stat];

    if (counter->chances == 0 || counter->chances < minSamples)
        return 0;
    return (counter->hits * 200) / counter->chances - 100;
}

// Combined hit rate (0..100) of several habits, or UINT32_MAX until they add up to minSamples chances.
u32 GetPlayerProfileRate(const enum PlayerProfileStat *stats, u32 count, u32 minSamples)
{
    u32 hits = 0, chances = 0;

    for (u32 i = 0; i < count; i++)
    {
        hits += GetPlayerProfile()->stats[stats[i]].hits;
        chances += GetPlayerProfile()->stats[stats[i]].chances;
    }
    if (chances == 0 || chances < minSamples)
        return UINT32_MAX;
    return hits * 100 / chances;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void AddHabit(enum PlayerProfileStat stat, bool32 hit)
{
    struct ProfileCounter *counter = &sProfile.tally.stats[stat];

    if (counter->chances == UINT8_MAX)
        return;
    counter->chances++;
    if (hit)
        counter->hits++;
}

static void AddCount(u8 *counts, u32 index)
{
    if (counts[index] < UINT8_MAX)
        counts[index]++;
}

static bool32 IsLeagueTrainer(u16 trainerId)
{
    for (u32 i = 0; i < ARRAY_COUNT(sLeagueTrainers); i++)
    {
        if (sLeagueTrainers[i] == trainerId)
            return TRUE;
    }
    return FALSE;
}

static bool32 ShouldRecordBattle(void)
{
    u32 excludedTypes = BATTLE_TYPE_LINK | BATTLE_TYPE_FRONTIER | BATTLE_TYPE_EREADER_TRAINER | BATTLE_TYPE_TRAINER_HILL
                      | BATTLE_TYPE_SECRET_BASE | BATTLE_TYPE_FIRST_BATTLE | BATTLE_TYPE_INGAME_PARTNER | BATTLE_TYPE_SAFARI;

    // Test battles are played back as recorded battles.
    if (!TESTING)
        excludedTypes |= BATTLE_TYPE_RECORDED | BATTLE_TYPE_RECORDED_LINK;

    if (!(gBattleTypeFlags & BATTLE_TYPE_TRAINER))
        return FALSE;
    if (gBattleTypeFlags & excludedTypes)
        return FALSE;
    if (!PROFILE_RECORD_POSTGAME && FlagGet(FLAG_SYS_GAME_CLEAR))
        return FALSE;
    if (IsLeagueTrainer(TRAINER_BATTLE_PARAM.opponentA))
        return FALSE;
    if ((gBattleTypeFlags & BATTLE_TYPE_TWO_OPPONENTS) && IsLeagueTrainer(TRAINER_BATTLE_PARAM.opponentB))
        return FALSE;
    return TRUE;
}

static bool32 IsMoveUsableBy(enum BattlerId battler, u32 moveIndex)
{
    enum Move move = gBattleMons[battler].moves[moveIndex];
    return move != MOVE_NONE && move != MOVE_UNAVAILABLE && gBattleMons[battler].pp[moveIndex] != 0;
}

static bool32 IsDamaging(enum Move move)
{
    return move != MOVE_NONE && move != MOVE_UNAVAILABLE && GetMovePower(move) != 0;
}

static u32 GetChosenMoveIndex(enum BattlerId battler, enum Move move)
{
    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        if (gBattleMons[battler].moves[i] == move)
            return i;
    }
    return MAX_MON_MOVES;
}

static u32 GetMoveDamage(enum BattlerId battlerAtk, enum BattlerId battlerDef, enum Move move)
{
    u32 moveIndex = GetChosenMoveIndex(battlerAtk, move);
    if (moveIndex == MAX_MON_MOVES)
        return 0;
    return sProfile.dmg[battlerAtk][battlerDef][moveIndex].median;
}

static bool32 IsEnemyBattler(enum BattlerId battler)
{
    return battler < gBattlersCount && !IsOnPlayerSide(battler) && IsBattlerAlive(battler);
}

static bool32 DoesAbilityAbsorbType(enum Ability ability, enum Type type)
{
    switch (ability)
    {
    case ABILITY_VOLT_ABSORB:
    case ABILITY_LIGHTNING_ROD:
    case ABILITY_MOTOR_DRIVE:
        return type == TYPE_ELECTRIC;
    case ABILITY_WATER_ABSORB:
    case ABILITY_STORM_DRAIN:
    case ABILITY_DRY_SKIN:
        return type == TYPE_WATER;
    case ABILITY_FLASH_FIRE:
    case ABILITY_WELL_BAKED_BODY:
        return type == TYPE_FIRE;
    case ABILITY_SAP_SIPPER:
        return type == TYPE_GRASS;
    case ABILITY_EARTH_EATER:
        return type == TYPE_GROUND;
    default:
        return FALSE;
    }
}

// Type-only matchup of a move against a party mon (used for mons not on the field).
static uq4_12_t GetMoveVsSpecies(enum Move move, enum Species species, enum Ability ability)
{
    if (DoesAbilityAbsorbType(ability, GetMoveType(move)))
        return UQ_4_12(0.0);
    return CalcPartyMonTypeEffectivenessMultiplier(move, species, ability);
}

static uq4_12_t GetMoveVsPartyMon(enum Move move, struct Pokemon *mon)
{
    return GetMoveVsSpecies(move, GetMonData(mon, MON_DATA_SPECIES), GetMonAbility(mon));
}

static bool32 IsMonUsable(struct Pokemon *mon)
{
    return GetMonData(mon, MON_DATA_SPECIES) != SPECIES_NONE
        && !GetMonData(mon, MON_DATA_IS_EGG)
        && GetMonData(mon, MON_DATA_HP) != 0;
}

static bool32 IsPartySlotOnField(enum BattlerId battler, u32 partyIndex)
{
    if (gBattlerPartyIndexes[battler] == partyIndex)
        return TRUE;
    if (IsDoubleBattle() && gBattlerPartyIndexes[GetPartnerBattler(battler)] == partyIndex)
        return TRUE;
    return FALSE;
}

// Does party mon A win the matchup against party mon B on types?
static bool32 DoesPartyMonBeat(struct Pokemon *monA, struct Pokemon *monB)
{
    bool32 bHasDamagingMove = FALSE;
    bool32 bAllResisted = TRUE;

    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        enum Move move = GetMonData(monA, MON_DATA_MOVE1 + i);
        if (IsDamaging(move) && GetMoveVsPartyMon(move, monB) >= UQ_4_12(2.0))
            return TRUE;
    }

    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        enum Move move = GetMonData(monB, MON_DATA_MOVE1 + i);
        if (!IsDamaging(move))
            continue;
        bHasDamagingMove = TRUE;
        if (GetMoveVsPartyMon(move, monA) > UQ_4_12(0.5))
            bAllResisted = FALSE;
    }
    return bHasDamagingMove && bAllResisted;
}

// ---------------------------------------------------------------------------
// Damage and speed
// ---------------------------------------------------------------------------

// Deterministic version of AI_CalcDamage: real abilities and items, no crits, no RNG.
static struct ProfileDamage CalcProfileDamage(enum BattlerId battlerAtk, enum BattlerId battlerDef, enum Move move)
{
    struct ProfileDamage result = {0};
    struct DamageContext ctx = {0};

    if (move == MOVE_NONE || move == MOVE_UNAVAILABLE)
        return result;

    gBattleStruct->magnitudeBasePower = 70;
    gBattleStruct->presentBasePower = 80;

    ctx.aiCalc = TRUE;
    ctx.battlerAtk = battlerAtk;
    ctx.battlerDef = battlerDef;
    ctx.move = ctx.chosenMove = ctx.baseMove = move;
    ctx.randomFactor = FALSE;
    ctx.updateFlags = FALSE;
    ctx.weather = AI_GetWeather();
    ctx.terrain = gFieldTimers.terrain;
    for (enum BattlerId battler = 0; battler < gBattlersCount; battler++)
    {
        ctx.abilities[battler] = sProfile.abilities[battler];
        ctx.holdEffects[battler] = sProfile.holdEffects[battler];
    }

    SetDynamicMoveCategory(battlerAtk, battlerDef, move);
    SetTypeBeforeUsingMove(move, battlerAtk, ctx.abilities[battlerAtk], ctx.holdEffects[battlerAtk]);
    ctx.moveType = GetBattleMoveType(move);
    ctx.typeEffectivenessModifier = result.effectiveness = CalcTypeEffectivenessMultiplier(&ctx);

    if (GetMovePower(move) != 0 && !IsDamageMoveUnusable(&ctx))
    {
        s32 median, minimum;
        s32 fixedDamage = DoFixedDamageMoveCalc(&ctx);

        if (fixedDamage != INT32_MAX)
        {
            median = minimum = fixedDamage;
        }
        else
        {
            s32 damage = CalculateMoveDamageVars(&ctx);
            median = ApplyModifiersAfterDmgRoll(&ctx, damage * DMG_ROLL_PERCENTAGE / 100);
            minimum = ApplyModifiersAfterDmgRoll(&ctx, damage * MIN_ROLL_PERCENTAGE / 100);
        }

        u32 strikeCount = GetMoveStrikeCount(move);
        if (strikeCount > 1)
        {
            median *= strikeCount;
            minimum *= strikeCount;
        }
        else if (IsMultiHitMove(move))
        {
            median *= 3;
            minimum *= 2;
        }

        result.median = min(max(median, 1), UINT16_MAX);
        result.minimum = min(max(minimum, 1), UINT16_MAX);
    }

    gBattleStruct->dynamicMoveType = TYPE_NONE;
    gBattleStruct->dynamicMoveCategory = DAMAGE_CATEGORY_NONE;
    gBattleStruct->battlerState[battlerAtk].ateBoost = FALSE;
    gSpecialStatuses[battlerAtk].gemBoost = FALSE;
    return result;
}

static void CalcAllProfileDamage(void)
{
    memset(sProfile.dmg, 0, sizeof(sProfile.dmg));
    for (enum BattlerId battlerAtk = 0; battlerAtk < gBattlersCount; battlerAtk++)
    {
        if (!IsBattlerAlive(battlerAtk))
            continue;
        for (enum BattlerId battlerDef = 0; battlerDef < gBattlersCount; battlerDef++)
        {
            if (battlerDef == battlerAtk || !IsBattlerAlive(battlerDef))
                continue;
            for (u32 i = 0; i < MAX_MON_MOVES; i++)
            {
                if (IsMoveUsableBy(battlerAtk, i))
                    sProfile.dmg[battlerAtk][battlerDef][i] = CalcProfileDamage(battlerAtk, battlerDef, gBattleMons[battlerAtk].moves[i]);
            }
        }
    }
}

static u32 GetProfileSpeed(enum BattlerId battler, s32 extraStages)
{
    s8 savedStage = gBattleMons[battler].statStages[STAT_SPEED];
    s32 stage = savedStage + extraStages;

    gBattleMons[battler].statStages[STAT_SPEED] = min(max(stage, 0), MAX_STAT_STAGE);
    u32 speed = GetBattlerTotalSpeedStat(battler, sProfile.abilities[battler], sProfile.holdEffects[battler]);
    gBattleMons[battler].statStages[STAT_SPEED] = savedStage;
    return speed;
}

static s32 GetProfilePriority(enum BattlerId battler, enum Move move)
{
    if (move == MOVE_NONE || move == MOVE_UNAVAILABLE)
        return 0;
    return GetBattleMovePriority(battler, sProfile.abilities[battler], move);
}

// 1 if battlerA acts first, -1 if battlerB acts first, 0 on a speed tie.
static s32 CompareTurnOrder(enum BattlerId battlerA, enum Move moveA, u32 speedA, enum BattlerId battlerB, enum Move moveB, u32 speedB)
{
    s32 priorityA = GetProfilePriority(battlerA, moveA);
    s32 priorityB = GetProfilePriority(battlerB, moveB);

    if (priorityA != priorityB)
        return priorityA > priorityB ? 1 : -1;
    if (speedA == speedB)
        return 0;
    if ((speedA > speedB) != ((gFieldStatuses & STATUS_FIELD_TRICK_ROOM) != 0))
        return 1;
    return -1;
}

static u32 GetBestDamage(enum BattlerId battlerAtk, enum BattlerId battlerDef, enum Move *bestMove)
{
    u32 best = 0;

    if (bestMove != NULL)
        *bestMove = MOVE_NONE;
    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        if (!IsMoveUsableBy(battlerAtk, i))
            continue;
        if (sProfile.dmg[battlerAtk][battlerDef][i].median > best)
        {
            best = sProfile.dmg[battlerAtk][battlerDef][i].median;
            if (bestMove != NULL)
                *bestMove = gBattleMons[battlerAtk].moves[i];
        }
    }
    return best;
}

static u32 GetBestPriorityDamage(enum BattlerId battlerAtk, enum BattlerId battlerDef)
{
    u32 best = 0;

    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        enum Move move = gBattleMons[battlerAtk].moves[i];
        if (!IsMoveUsableBy(battlerAtk, i) || !IsDamaging(move) || GetProfilePriority(battlerAtk, move) <= 0)
            continue;
        best = max(best, sProfile.dmg[battlerAtk][battlerDef][i].median);
    }
    return best;
}

static bool32 CanKO(enum BattlerId battlerAtk, enum BattlerId battlerDef)
{
    return GetBestDamage(battlerAtk, battlerDef, NULL) >= gBattleMons[battlerDef].hp;
}

static bool32 AreAllDamagingMovesResisted(enum BattlerId battlerAtk, enum BattlerId battlerDef, uq4_12_t limit)
{
    bool32 hasDamagingMove = FALSE;

    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        if (!IsMoveUsableBy(battlerAtk, i) || !IsDamaging(gBattleMons[battlerAtk].moves[i]))
            continue;
        hasDamagingMove = TRUE;
        if (sProfile.dmg[battlerAtk][battlerDef][i].effectiveness > limit)
            return FALSE;
    }
    return hasDamagingMove;
}

// ---------------------------------------------------------------------------
// Move classification
// ---------------------------------------------------------------------------

static bool32 IsSetupMove(enum Move move)
{
    if (GetMoveCategory(move) != DAMAGE_CATEGORY_STATUS)
        return FALSE;

    switch (GetMoveEffect(move))
    {
    case EFFECT_BELLY_DRUM:
    case EFFECT_CLANGOROUS_SOUL:
    case EFFECT_NO_RETREAT:
    case EFFECT_GROWTH:
        return TRUE;
    default:
        return IsStatRaisingMove(move) && GetMoveTarget(move) == TARGET_USER;
    }
}

static s32 GetMoveSpeedBoost(enum Move move)
{
    for (u32 i = 0; i < GetMoveAdditionalEffectCount(move); i++)
    {
        const struct AdditionalEffect *effect = GetMoveAdditionalEffectById(move, i);
        if (effect->moveEffect == STAT_CHANGE_EFFECT_PLUS)
            return effect->speed;
    }
    return 0;
}

static bool32 IsProtectMove(enum Move move)
{
    if (GetMoveEffect(move) != EFFECT_PROTECT)
        return FALSE;

    switch (GetMoveProtectMethod(move))
    {
    case PROTECT_NORMAL:
    case PROTECT_SPIKY_SHIELD:
    case PROTECT_KINGS_SHIELD:
    case PROTECT_BANEFUL_BUNKER:
    case PROTECT_BURNING_BULWARK:
    case PROTECT_OBSTRUCT:
    case PROTECT_SILK_TRAP:
        return TRUE;
    default:
        return FALSE;
    }
}

static bool32 IsStatusMove(enum Move move)
{
    return GetMoveCategory(move) == DAMAGE_CATEGORY_STATUS && IsNonVolatileStatusMove(move);
}

static bool32 IsSelfHealingMove(enum Move move)
{
    return IsHealingMove(move) && GetMoveTarget(move) == TARGET_USER;
}

static bool32 IsProfileSpreadMove(enum Move move)
{
    enum MoveTarget target = GetMoveTarget(move);
    return target == TARGET_BOTH || target == TARGET_FOES_AND_ALLY;
}

bool32 Profile_IsSetupMove(enum Move move)
{
    return IsSetupMove(move);
}

bool32 Profile_IsProtectMove(enum Move move)
{
    return IsProtectMove(move);
}

bool32 Profile_IsSelfHealingMove(enum Move move)
{
    return IsSelfHealingMove(move);
}

bool32 Profile_IsStatusMove(enum Move move)
{
    return IsStatusMove(move);
}

bool32 Profile_IsSpreadMove(enum Move move)
{
    return IsProfileSpreadMove(move);
}

static bool32 KnowsMove(enum BattlerId battler, bool32 (*predicate)(enum Move))
{
    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        if (IsMoveUsableBy(battler, i) && predicate(gBattleMons[battler].moves[i]))
            return TRUE;
    }
    return FALSE;
}

static enum Move GetFirstMove(enum BattlerId battler, bool32 (*predicate)(enum Move))
{
    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        if (IsMoveUsableBy(battler, i) && predicate(gBattleMons[battler].moves[i]))
            return gBattleMons[battler].moves[i];
    }
    return MOVE_NONE;
}

// Drawback = stat drop, recharge, lock-in, recoil or self-KO that the user's item or ability doesn't cancel.
static bool32 HasDrawback(enum BattlerId battler, enum Move move)
{
    enum Ability ability = sProfile.abilities[battler];
    enum HoldEffect holdEffect = sProfile.holdEffects[battler];
    enum BattleMoveEffects moveEffect = GetMoveEffect(move);

    if (IsExplosionMove(move) || moveEffect == EFFECT_FINAL_GAMBIT)
        return TRUE;
    if (moveEffect == EFFECT_RECOIL && ability != ABILITY_ROCK_HEAD && ability != ABILITY_MAGIC_GUARD)
        return TRUE;

    for (u32 i = 0; i < GetMoveAdditionalEffectCount(move); i++)
    {
        const struct AdditionalEffect *effect = GetMoveAdditionalEffectById(move, i);
        if (!effect->self)
            continue;

        switch (effect->moveEffect)
        {
        case MOVE_EFFECT_STAT_MINUS:
            if (ability != ABILITY_CONTRARY && holdEffect != HOLD_EFFECT_WHITE_HERB && holdEffect != HOLD_EFFECT_EJECT_PACK)
                return TRUE;
            break;
        case MOVE_EFFECT_RECHARGE:
        case MOVE_EFFECT_THRASH:
            return TRUE;
        default:
            break;
        }
    }
    return FALSE;
}

static u32 GetMoveAccuracyForProfile(enum Move move)
{
    u32 accuracy = GetMoveAccuracy(move);
    return accuracy == 0 ? 101 : accuracy; // 0 means the move never misses
}

static bool32 CanStatusLand(enum BattlerId battlerAtk, enum BattlerId battlerDef, enum Move move)
{
    if (!IsEnemyBattler(battlerDef))
        return FALSE;

    enum MoveEffect status = GetMoveNonVolatileStatus(move);
    enum Ability abilityDef = sProfile.abilities[battlerDef];
    enum HoldEffect holdEffectDef = sProfile.holdEffects[battlerDef];
    u32 moveIndex = GetChosenMoveIndex(battlerAtk, move);

    if (abilityDef == ABILITY_MAGIC_BOUNCE || holdEffectDef == HOLD_EFFECT_CURE_STATUS)
        return FALSE;
    if (moveIndex != MAX_MON_MOVES && sProfile.dmg[battlerAtk][battlerDef][moveIndex].effectiveness == UQ_4_12(0.0))
        return FALSE;
    if (IsPowderMove(move) && (IS_BATTLER_OF_TYPE(battlerDef, TYPE_GRASS) || abilityDef == ABILITY_OVERCOAT || holdEffectDef == HOLD_EFFECT_SAFETY_GOGGLES))
        return FALSE;
    if (DoesSubstituteBlockMove(battlerAtk, battlerDef, move))
        return FALSE;
    if (!CanSetNonVolatileStatus(battlerAtk, battlerDef, sProfile.abilities[battlerAtk], abilityDef, status, CHECK_TRIGGER))
        return FALSE;

    // Abilities that turn the status into a benefit
    switch (abilityDef)
    {
    case ABILITY_GUTS:
    case ABILITY_MARVEL_SCALE:
    case ABILITY_QUICK_FEET:
        return FALSE;
    case ABILITY_POISON_HEAL:
    case ABILITY_TOXIC_BOOST:
        return status != MOVE_EFFECT_POISON && status != MOVE_EFFECT_TOXIC;
    case ABILITY_FLARE_BOOST:
        return status != MOVE_EFFECT_BURN;
    default:
        return TRUE;
    }
}

static void CountStatusKind(enum Move move)
{
    switch (GetMoveNonVolatileStatus(move))
    {
    case MOVE_EFFECT_PARALYSIS:
        AddCount(sProfile.tally.statusKind, PROFILE_STATUS_PARALYSIS);
        break;
    case MOVE_EFFECT_BURN:
        AddCount(sProfile.tally.statusKind, PROFILE_STATUS_BURN);
        break;
    case MOVE_EFFECT_POISON:
    case MOVE_EFFECT_TOXIC:
        AddCount(sProfile.tally.statusKind, PROFILE_STATUS_POISON);
        break;
    case MOVE_EFFECT_SLEEP:
        AddCount(sProfile.tally.statusKind, PROFILE_STATUS_SLEEP);
        break;
    default:
        break;
    }
}

static bool32 HasSupportMove(enum BattlerId battler)
{
    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        if (!IsMoveUsableBy(battler, i))
            continue;
        switch (GetMoveEffect(gBattleMons[battler].moves[i]))
        {
        case EFFECT_FOLLOW_ME:
        case EFFECT_TAILWIND:
        case EFFECT_TRICK_ROOM:
        case EFFECT_FIRST_TURN_ONLY:
        case EFFECT_REFLECT:
        case EFFECT_LIGHT_SCREEN:
        case EFFECT_AURORA_VEIL:
        case EFFECT_HELPING_HAND:
            return TRUE;
        default:
            break;
        }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Board state
// ---------------------------------------------------------------------------

static bool32 CanProfileBattlerSwitch(enum BattlerId battler)
{
    struct Pokemon *party = GetBattlerParty(battler);

    if (sProfile.holdEffects[battler] != HOLD_EFFECT_SHED_SHELL
     && (!CanBattlerEscape(battler) || IsAbilityPreventingEscape(battler)))
        return FALSE;

    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        if (!IsPartySlotOnField(battler, i) && IsMonUsable(&party[i]))
            return TRUE;
    }
    return FALSE;
}

static struct Pokemon *GetSwitchInMon(enum BattlerId battler)
{
    u32 partyIndex = gBattleStruct->monToSwitchIntoId[battler];
    if (partyIndex >= PARTY_SIZE)
        return NULL;
    return &GetBattlerParty(battler)[partyIndex];
}

static struct ProfileThreat EvaluateThreat(enum BattlerId battler, enum Move playerMove)
{
    struct ProfileThreat threat = {.tier = THREAT_T0, .threatBattler = MAX_BATTLERS_COUNT};
    u32 playerSpeed = GetProfileSpeed(battler, 0);
    enum Move threatKoMove = MOVE_NONE;

    for (enum BattlerId enemy = 0; enemy < gBattlersCount; enemy++)
    {
        if (!IsEnemyBattler(enemy))
            continue;

        enum ThreatTier tier = THREAT_T0;
        enum Move koMove = MOVE_NONE;
        u32 enemySpeed = GetProfileSpeed(enemy, 0);

        threat.totalBestDamage += GetBestDamage(enemy, battler, NULL);
        for (u32 i = 0; i < MAX_MON_MOVES; i++)
        {
            enum Move move = gBattleMons[enemy].moves[i];
            const struct ProfileDamage *dmg = &sProfile.dmg[enemy][battler][i];

            if (!IsMoveUsableBy(enemy, i) || !IsDamaging(move))
                continue;
            if (dmg->median >= gBattleMons[battler].hp)
            {
                bool32 enemyFirst = CompareTurnOrder(enemy, move, enemySpeed, battler, playerMove, playerSpeed) > 0;
                if (enemyFirst && tier < THREAT_T3)
                {
                    tier = THREAT_T3;
                    koMove = move;
                }
                else if (tier < THREAT_T2)
                {
                    tier = THREAT_T2;
                    koMove = move;
                }
            }
            else if (dmg->effectiveness >= UQ_4_12(2.0) && tier < THREAT_T1)
            {
                tier = THREAT_T1;
            }
        }

        if (tier > threat.tier)
        {
            threat.tier = tier;
            threat.threatBattler = enemy;
            threatKoMove = koMove;
        }
    }

    // The player acts first and KOs the threat: staying in is correct play.
    if (threat.tier >= THREAT_T2)
    {
        enum BattlerId enemy = threat.threatBattler;
        for (u32 i = 0; i < MAX_MON_MOVES; i++)
        {
            enum Move move = gBattleMons[battler].moves[i];
            if (!IsMoveUsableBy(battler, i) || !IsDamaging(move))
                continue;
            if (sProfile.dmg[battler][enemy][i].median >= gBattleMons[enemy].hp
             && CompareTurnOrder(battler, move, playerSpeed, enemy, threatKoMove, GetProfileSpeed(enemy, 0)) > 0)
            {
                threat.winningRace = TRUE;
                break;
            }
        }
    }
    return threat;
}

static enum BattlerId GetReferenceEnemy(const struct ProfileThreat *threat)
{
    if (threat->threatBattler != MAX_BATTLERS_COUNT)
        return threat->threatBattler;
    for (enum BattlerId enemy = 0; enemy < gBattlersCount; enemy++)
    {
        if (IsEnemyBattler(enemy))
            return enemy;
    }
    return MAX_BATTLERS_COUNT;
}

static bool32 HasResetCondition(enum BattlerId battler)
{
    const struct Volatiles *volatiles = &gBattleMons[battler].volatiles;

    return CountNegativeStatStages(battler) != 0
        || (gBattleMons[battler].status1 & STATUS1_TOXIC_POISON)
        || volatiles->confusionTimer != 0
        || volatiles->encoreTimer != 0
        || volatiles->tauntTimer != 0
        || volatiles->disableTimer != 0
        || volatiles->leechSeed != 0
        || volatiles->cursed
        || volatiles->perishSong
        || volatiles->yawn != 0;
}

static bool32 HasStallGain(enum BattlerId battler)
{
    enum HoldEffect holdEffect = sProfile.holdEffects[battler];
    bool32 missingHp = gBattleMons[battler].hp < gBattleMons[battler].maxHP;

    if (sProfile.abilities[battler] == ABILITY_SPEED_BOOST)
        return TRUE;
    if (missingHp && holdEffect == HOLD_EFFECT_LEFTOVERS)
        return TRUE;
    if (missingHp && holdEffect == HOLD_EFFECT_BLACK_SLUDGE && IS_BATTLER_OF_TYPE(battler, TYPE_POISON))
        return TRUE;

    for (enum BattlerId enemy = 0; enemy < gBattlersCount; enemy++)
    {
        if (!IsEnemyBattler(enemy))
            continue;
        const struct Volatiles *volatiles = &gBattleMons[enemy].volatiles;
        if ((gBattleMons[enemy].status1 & (STATUS1_PSN_ANY | STATUS1_BURN))
         || volatiles->leechSeed != 0
         || volatiles->cursed
         || volatiles->saltCure
         || volatiles->perishSong)
            return TRUE;
    }
    return FALSE;
}

static bool32 HasEnemyRaisedStats(void)
{
    for (enum BattlerId enemy = 0; enemy < gBattlersCount; enemy++)
    {
        if (IsEnemyBattler(enemy) && CountPositiveStatStages(enemy) != 0)
            return TRUE;
    }
    return FALSE;
}

static bool32 IsSetupRisky(enum BattlerId battler, enum Move setupMove)
{
    u32 damageSetupTurn = 0, damageNextTurn = 0;
    u32 playerSpeed = GetProfileSpeed(battler, GetMoveSpeedBoost(setupMove));

    for (enum BattlerId enemy = 0; enemy < gBattlersCount; enemy++)
    {
        enum Move bestMove;
        if (!IsEnemyBattler(enemy))
            continue;

        u32 bestDamage = GetBestDamage(enemy, battler, &bestMove);
        damageSetupTurn += bestDamage;
        if (CompareTurnOrder(enemy, bestMove, GetProfileSpeed(enemy, 0), battler, MOVE_NONE, playerSpeed) > 0)
            damageNextTurn += bestDamage;
        else
            damageNextTurn += GetBestPriorityDamage(enemy, battler);
    }
    return damageSetupTurn + damageNextTurn >= gBattleMons[battler].hp;
}

// ---------------------------------------------------------------------------
// Pending outcomes from earlier turns
// ---------------------------------------------------------------------------

static bool32 DidPlayerRevenge(enum BattlerId battler, enum BattlerId enemy)
{
    enum Move playerMove, enemyMove;

    GetBestDamage(enemy, battler, &enemyMove);
    if (GetBestDamage(battler, enemy, &playerMove) >= gBattleMons[enemy].hp
     && CompareTurnOrder(battler, playerMove, GetProfileSpeed(battler, 0), enemy, enemyMove, GetProfileSpeed(enemy, 0)) > 0)
        return TRUE;
    return AreAllDamagingMovesResisted(enemy, battler, UQ_4_12(0.0));
}

static void ResolvePendingOutcomes(enum BattlerId battler)
{
    struct ProfileBattlerState *state = &sProfile.battlers[battler];
    struct Pokemon *party = GetBattlerParty(battler);

    if (state->sackPending)
    {
        state->sackPending = FALSE;
        if (GetMonData(&party[state->sackPartyIndex], MON_DATA_HP) == 0)
        {
            enum BattlerId enemy = state->sackThreat;
            if (IsBattlerAlive(battler) && IsEnemyBattler(enemy))
                AddHabit(PROFILE_REVENGE_AFTER_SACK, state->sackStatused || DidPlayerRevenge(battler, enemy));
            else if (state->sackStatused)
                AddHabit(PROFILE_REVENGE_AFTER_SACK, TRUE);
        }
    }

    if (state->setupPending)
    {
        if (GetMonData(&party[state->setupPartyIndex], MON_DATA_HP) == 0)
        {
            AddHabit(PROFILE_SETUP_PUNISHED, TRUE);
            state->setupPending = 0;
        }
        else if (gBattlerPartyIndexes[battler] != state->setupPartyIndex)
        {
            state->setupPending = 0; // switched out before acting with the boost
        }
        else if (--state->setupPending == 0)
        {
            AddHabit(PROFILE_SETUP_PUNISHED, FALSE);
        }
    }
}

static void UpdateFieldTracking(void)
{
    for (enum BattlerId battler = 0; battler < gBattlersCount; battler++)
    {
        struct ProfileBattlerState *state = &sProfile.battlers[battler];

        if (gBattlerPartyIndexes[battler] != state->partyIndex)
        {
            state->partyIndex = gBattlerPartyIndexes[battler];
            state->firstAction = TRUE;
            state->justEntered = TRUE;
            state->protectedLastTurn = FALSE;
            state->scoutPending = FALSE;
            state->resetTurns = 0;
        }
        else
        {
            state->justEntered = FALSE;
        }
    }
}

// ---------------------------------------------------------------------------
// Habits
// ---------------------------------------------------------------------------

static void RecordLead(void)
{
    bool32 counters = FALSE;

    for (enum BattlerId battler = 0; battler < gBattlersCount && !counters; battler++)
    {
        if (!IsOnPlayerSide(battler) || !IsBattlerAlive(battler))
            continue;
        for (enum BattlerId enemy = 0; enemy < gBattlersCount && !counters; enemy++)
        {
            if (!IsEnemyBattler(enemy))
                continue;
            if (CanKO(battler, enemy) || AreAllDamagingMovesResisted(enemy, battler, UQ_4_12(0.5)))
                counters = TRUE;
            for (u32 i = 0; i < MAX_MON_MOVES && !counters; i++)
            {
                if (IsMoveUsableBy(battler, i) && IsDamaging(gBattleMons[battler].moves[i])
                 && sProfile.dmg[battler][enemy][i].effectiveness >= UQ_4_12(2.0))
                    counters = TRUE;
            }
        }
    }
    AddHabit(PROFILE_LEAD_COUNTERS_ENEMY_LEAD, counters);
}

static void RecordSwitchHabits(enum BattlerId battler, const struct ProfileThreat *threat, bool32 switched, bool32 canSwitch, enum Move move)
{
    struct ProfileBattlerState *state = &sProfile.battlers[battler];
    struct Pokemon *switchIn = switched ? GetSwitchInMon(battler) : NULL;
    enum BattlerId enemy = GetReferenceEnemy(threat);

    if (!canSwitch)
        return;

    // Switching under threat
    if (!threat->winningRace)
    {
        switch (threat->tier)
        {
        case THREAT_T0:
            AddHabit(PROFILE_SWITCH_T0, switched);
            break;
        case THREAT_T1:
            AddHabit(PROFILE_SWITCH_T1, switched);
            break;
        case THREAT_T2:
            AddHabit(PROFILE_SWITCH_T2, switched);
            break;
        case THREAT_T3:
            AddHabit(state->partyIndex == sProfile.acePartyIndex ? PROFILE_SWITCH_T3_ACE : PROFILE_SWITCH_T3_OTHER, switched);
            break;
        }
    }

    // Staying in at T2/T3
    if (threat->tier >= THREAT_T2 && !threat->winningRace && !switched && IsDamaging(move))
    {
        enum BattlerId threatBattler = threat->threatBattler;
        enum Move enemyMove;
        GetBestDamage(threatBattler, battler, &enemyMove);
        if (CompareTurnOrder(battler, move, GetProfileSpeed(battler, 0), threatBattler, enemyMove, GetProfileSpeed(threatBattler, 0)) != 0)
            AddHabit(PROFILE_MISREAD_RACE, GetMoveDamage(battler, threatBattler, move) >= gBattleMons[threatBattler].hp);
    }
    if (threat->tier == THREAT_T3 && !threat->winningRace && !switched)
    {
        enum BattlerId target = gBattleStruct->moveTarget[battler];
        state->sackPending = TRUE;
        state->sackPartyIndex = state->partyIndex;
        state->sackThreat = threat->threatBattler;
        state->sackStatused = IsStatusMove(move) && CanStatusLand(battler, target, move);
    }

    // Where the switch goes
    if (switchIn != NULL && enemy != MAX_BATTLERS_COUNT)
    {
        enum Move bestMove;
        bool32 hasRelevantMove = FALSE, allImmune = TRUE, allResisted = TRUE;

        GetBestDamage(enemy, battler, &bestMove);
        for (u32 i = 0; i < MAX_MON_MOVES; i++)
        {
            enum Move enemyMove = gBattleMons[enemy].moves[i];
            if (!IsMoveUsableBy(enemy, i) || !IsDamaging(enemyMove))
                continue;
            if (enemyMove != bestMove && !IS_BATTLER_OF_TYPE(enemy, GetMoveType(enemyMove)))
                continue;

            uq4_12_t effectiveness = GetMoveVsPartyMon(enemyMove, switchIn);
            hasRelevantMove = TRUE;
            if (effectiveness != UQ_4_12(0.0))
                allImmune = FALSE;
            if (effectiveness > UQ_4_12(0.5))
                allResisted = FALSE;
        }
        if (hasRelevantMove)
        {
            AddHabit(PROFILE_SWITCH_TO_IMMUNE, allImmune);
            AddHabit(PROFILE_SWITCH_TO_RESIST, allResisted);
        }
    }

    // Answering an enemy switch-in that walls this mon
    for (enum BattlerId newEnemy = 0; newEnemy < gBattlersCount; newEnemy++)
    {
        if (IsEnemyBattler(newEnemy) && sProfile.battlers[newEnemy].justEntered
         && AreAllDamagingMovesResisted(battler, newEnemy, UQ_4_12(0.5)))
        {
            AddHabit(PROFILE_REACTIVE_SWITCH, switched);
            break;
        }
    }

    // Switching while safe
    if (threat->tier == THREAT_T0)
    {
        bool32 threatensNow = FALSE;
        for (enum BattlerId target = 0; target < gBattlersCount && !threatensNow; target++)
        {
            if (!IsEnemyBattler(target))
                continue;
            if (CanKO(battler, target))
                threatensNow = TRUE;
            for (u32 i = 0; i < MAX_MON_MOVES && !threatensNow; i++)
            {
                if (IsMoveUsableBy(battler, i) && IsDamaging(gBattleMons[battler].moves[i])
                 && sProfile.dmg[battler][target][i].effectiveness >= UQ_4_12(2.0))
                    threatensNow = TRUE;
            }
        }

        if (!threatensNow)
        {
            struct Pokemon *party = GetBattlerParty(battler);
            bool32 hasCandidate = FALSE, pickedCandidate = FALSE;

            for (u32 slot = 0; slot < PARTY_SIZE; slot++)
            {
                bool32 isCandidate = FALSE;
                if (IsPartySlotOnField(battler, slot) || !IsMonUsable(&party[slot]))
                    continue;
                for (u32 i = 0; i < MAX_MON_MOVES && !isCandidate; i++)
                {
                    enum Move benchMove = GetMonData(&party[slot], MON_DATA_MOVE1 + i);
                    if (!IsDamaging(benchMove))
                        continue;
                    for (enum BattlerId target = 0; target < gBattlersCount; target++)
                    {
                        if (IsEnemyBattler(target)
                         && GetMoveVsSpecies(benchMove, gBattleMons[target].species, sProfile.abilities[target]) >= UQ_4_12(2.0))
                        {
                            isCandidate = TRUE;
                            break;
                        }
                    }
                }
                if (isCandidate)
                {
                    hasCandidate = TRUE;
                    if (switchIn == &party[slot])
                        pickedCandidate = TRUE;
                }
            }
            if (hasCandidate)
                AddHabit(PROFILE_SWITCH_REPOSITION, pickedCandidate);
        }

        if (switched)
        {
            bool32 readSwitch = FALSE;
            for (enum BattlerId enemySwitcher = 0; enemySwitcher < gBattlersCount; enemySwitcher++)
            {
                struct Pokemon *enemySwitchIn;
                if (!IsEnemyBattler(enemySwitcher) || gChosenActionByBattler[enemySwitcher] != B_ACTION_SWITCH)
                    continue;
                enemySwitchIn = GetSwitchInMon(enemySwitcher);
                if (switchIn != NULL && enemySwitchIn != NULL && DoesPartyMonBeat(switchIn, enemySwitchIn))
                    readSwitch = TRUE;
            }
            AddHabit(PROFILE_SWITCH_DOUBLE_READ, readSwitch);
        }
    }

    // Switching to clear a bad condition
    if (HasResetCondition(battler))
    {
        AddHabit(state->resetTurns == 0 ? PROFILE_RESET_IMMEDIATE : PROFILE_RESET_DELAYED, switched);
        if (!switched && state->resetTurns < UINT8_MAX)
            state->resetTurns++;
    }
    else
    {
        state->resetTurns = 0;
    }

    // Regenerator / Natural Cure
    if ((sProfile.abilities[battler] == ABILITY_REGENERATOR && gBattleMons[battler].hp < gBattleMons[battler].maxHP)
     || (sProfile.abilities[battler] == ABILITY_NATURAL_CURE && (gBattleMons[battler].status1 & STATUS1_ANY)))
        AddHabit(PROFILE_SWITCH_ABILITY, switched);
}

static void RecordSetupHabits(enum BattlerId battler, const struct ProfileThreat *threat, enum Move move)
{
    struct ProfileBattlerState *state = &sProfile.battlers[battler];
    bool32 usedSetup = IsSetupMove(move);

    if (KnowsMove(battler, IsSetupMove))
    {
        if (state->firstAction)
        {
            enum Move setupMove = usedSetup ? move : GetFirstMove(battler, IsSetupMove);
            AddHabit(IsSetupRisky(battler, setupMove) ? PROFILE_SETUP_ENTRY_RISKY : PROFILE_SETUP_ENTRY_SOUND, usedSetup);
        }
        else if (threat->tier == THREAT_T0)
        {
            AddHabit(PROFILE_SETUP_WHEN_SAFE, usedSetup);
        }
    }

    if (usedSetup)
    {
        state->setupPending = 2;
        state->setupPartyIndex = state->partyIndex;
    }
}

// Healing moves only (Recover, Roost, Rest...). Healing with bag items isn't tracked: most players
// run Nuzlocke rules, where items in battle are banned, so it would say nothing about them.
static void RecordHealHabit(enum BattlerId battler, const struct ProfileThreat *threat, enum Move move)
{
    u32 hp = gBattleMons[battler].hp;
    u32 maxHP = gBattleMons[battler].maxHP;

    if (hp * 100 >= maxHP * 40)
        return;
    if (!KnowsMove(battler, IsSelfHealingMove))
        return;
    if (hp + maxHP / 2 <= threat->totalBestDamage)
        return; // healing can't outpace the damage

    AddHabit(PROFILE_HEAL_AT_LOW_HP, IsSelfHealingMove(move));
}

static void RecordStatusHabits(enum BattlerId battler, enum Move move)
{
    struct ProfileBattlerState *state = &sProfile.battlers[battler];
    bool32 usedStatus = IsStatusMove(move);
    bool32 usedValid = FALSE;

    if (usedStatus)
    {
        usedValid = CanStatusLand(battler, gBattleStruct->moveTarget[battler], move);
        AddHabit(PROFILE_STATUS_INTO_BAD, !usedValid);
        if (usedValid)
            CountStatusKind(move);
    }

    if (state->firstAction)
    {
        bool32 anyValid = FALSE;
        for (u32 i = 0; i < MAX_MON_MOVES && !anyValid; i++)
        {
            enum Move statusMove = gBattleMons[battler].moves[i];
            if (!IsMoveUsableBy(battler, i) || !IsStatusMove(statusMove))
                continue;
            for (enum BattlerId enemy = 0; enemy < gBattlersCount; enemy++)
            {
                if (CanStatusLand(battler, enemy, statusMove))
                {
                    anyValid = TRUE;
                    break;
                }
            }
        }
        if (anyValid)
            AddHabit(PROFILE_STATUS_OPEN_VALID, usedValid);
    }
}

static void RecordMoveChoiceHabits(enum BattlerId battler, enum Move move)
{
    enum BattlerId target = gBattleStruct->moveTarget[battler];
    u32 chosen = GetChosenMoveIndex(battler, move);
    u32 strongest = MAX_MON_MOVES, strongestDamage = 0;
    u32 koCount = 0, bestCleanKoAccuracy = 0, bestKoAccuracy = 0;
    bool32 hasCleanKo = FALSE;

    if (chosen == MAX_MON_MOVES || !IsEnemyBattler(target))
        return;

    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        enum Move option = gBattleMons[battler].moves[i];
        if (!IsMoveUsableBy(battler, i) || !IsDamaging(option))
            continue;

        u32 damage = sProfile.dmg[battler][target][i].median;
        if (damage > strongestDamage)
        {
            strongestDamage = damage;
            strongest = i;
        }
        if (damage >= gBattleMons[target].hp)
        {
            u32 accuracy = GetMoveAccuracyForProfile(option);
            koCount++;
            bestKoAccuracy = max(bestKoAccuracy, accuracy);
            if (!HasDrawback(battler, option))
            {
                hasCleanKo = TRUE;
                bestCleanKoAccuracy = max(bestCleanKoAccuracy, accuracy);
            }
        }
    }
    if (strongest == MAX_MON_MOVES)
        return;

    u32 chosenDamage = sProfile.dmg[battler][target][chosen].median;
    u32 chosenAccuracy = GetMoveAccuracyForProfile(move);
    enum Move strongestMove = gBattleMons[battler].moves[strongest];

    // Clean finisher vs. over-killer
    if (koCount >= 2)
    {
        bool32 chosenKo = chosenDamage >= gBattleMons[target].hp;
        bool32 clean = hasCleanKo ? (!HasDrawback(battler, move) && chosenAccuracy >= bestCleanKoAccuracy)
                                  : chosenAccuracy >= bestKoAccuracy;
        AddHabit(PROFILE_KO_SAFE_PICK, chosenKo && clean);
    }

    // Accuracy greed and drawback greed
    bool32 hasAccurateAlternative = FALSE, hasCleanAlternative = FALSE;
    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        enum Move option = gBattleMons[battler].moves[i];
        if (i == strongest || !IsMoveUsableBy(battler, i) || !IsDamaging(option))
            continue;

        u32 damage = sProfile.dmg[battler][target][i].median;
        if (GetMoveAccuracyForProfile(option) >= 90 && (damage >= gBattleMons[target].hp || damage * 10 >= strongestDamage * 7))
            hasAccurateAlternative = TRUE;
        if (!HasDrawback(battler, option) && damage * 10 >= strongestDamage * 8)
            hasCleanAlternative = TRUE;
    }
    if (GetMoveAccuracyForProfile(strongestMove) < 90 && hasAccurateAlternative)
        AddHabit(PROFILE_ACCURACY_GREED, chosen == strongest);
    if (HasDrawback(battler, strongestMove) && hasCleanAlternative)
        AddHabit(PROFILE_DRAWBACK_USE, chosen == strongest);

    // Coverage aimed at the AI's best switch-in
    if (koCount != 0)
    {
        struct Pokemon *party = GetBattlerParty(target);
        struct Pokemon *bestSwitchIn = NULL;
        uq4_12_t bestResist = UQ_4_12(0.5);

        for (u32 slot = 0; slot < PARTY_SIZE; slot++)
        {
            if (IsPartySlotOnField(target, slot) || !IsMonUsable(&party[slot]))
                continue;
            uq4_12_t effectiveness = GetMoveVsPartyMon(strongestMove, &party[slot]);
            if (effectiveness <= bestResist)
            {
                bestResist = effectiveness;
                bestSwitchIn = &party[slot];
            }
        }
        if (bestSwitchIn != NULL)
        {
            AddHabit(PROFILE_ATTACK_THE_SWITCH, chosen != strongest
                && sProfile.dmg[battler][target][chosen].effectiveness < sProfile.dmg[battler][target][strongest].effectiveness
                && GetMoveVsPartyMon(move, bestSwitchIn) > bestResist);
        }
    }

    // Spread attacker
    if (IsDoubleBattle() && KnowsMove(battler, IsProfileSpreadMove))
    {
        u32 enemiesAlive = 0;
        for (enum BattlerId enemy = 0; enemy < gBattlersCount; enemy++)
        {
            if (IsEnemyBattler(enemy))
                enemiesAlive++;
        }
        if (enemiesAlive >= 2 && IsDamaging(GetFirstMove(battler, IsProfileSpreadMove)))
            AddHabit(PROFILE_SPREAD_PREFERENCE, IsProfileSpreadMove(move));
    }
}

static bool32 IsFinishingPriorityMove(enum BattlerId battler, enum BattlerId enemy, u32 moveIndex)
{
    enum Move move = gBattleMons[battler].moves[moveIndex];
    enum BattleMoveEffects effect = GetMoveEffect(move);

    if (!IsMoveUsableBy(battler, moveIndex) || !IsDamaging(move) || GetProfilePriority(battler, move) <= 0)
        return FALSE;
    if (effect == EFFECT_FIRST_TURN_ONLY || effect == EFFECT_SUCKER_PUNCH || effect == EFFECT_UPPER_HAND)
        return FALSE;
    if (sProfile.dmg[battler][enemy][moveIndex].median < gBattleMons[enemy].hp)
        return FALSE;
    return !Ai_IsPriorityBlocked(battler, enemy, move, gAiLogicData);
}

static void RecordPriorityHabits(enum BattlerId battler, enum Move move)
{
    bool32 neededChance = FALSE, redundantChance = FALSE, suckerChance = FALSE;
    bool32 usedPriority = IsDamaging(move) && GetProfilePriority(battler, move) > 0;
    u32 playerSpeed = GetProfileSpeed(battler, 0);

    for (enum BattlerId enemy = 0; enemy < gBattlersCount; enemy++)
    {
        bool32 priorityKo = FALSE, normalKo = FALSE;
        enum Move enemyMove;

        if (!IsEnemyBattler(enemy))
            continue;

        for (u32 i = 0; i < MAX_MON_MOVES; i++)
        {
            enum Move option = gBattleMons[battler].moves[i];
            if (!IsMoveUsableBy(battler, i) || !IsDamaging(option))
                continue;
            bool32 kos = sProfile.dmg[battler][enemy][i].median >= gBattleMons[enemy].hp;
            if (IsFinishingPriorityMove(battler, enemy, i))
                priorityKo = TRUE;
            else if (kos && GetProfilePriority(battler, option) <= 0)
                normalKo = TRUE;
            if (kos && GetMoveEffect(option) == EFFECT_SUCKER_PUNCH)
                suckerChance = TRUE;
        }
        if (!priorityKo)
            continue;

        GetBestDamage(enemy, battler, &enemyMove);
        if (CompareTurnOrder(enemy, enemyMove, GetProfileSpeed(enemy, 0), battler, MOVE_NONE, playerSpeed) > 0)
            neededChance = TRUE;
        else if (normalKo)
            redundantChance = TRUE;
    }

    if (neededChance)
        AddHabit(PROFILE_PRIORITY_NEEDED, usedPriority);
    if (redundantChance)
        AddHabit(PROFILE_PRIORITY_REDUNDANT, usedPriority);
    if (suckerChance)
        AddHabit(PROFILE_SUCKER_USE, GetMoveEffect(move) == EFFECT_SUCKER_PUNCH);

    enum BattlerId target = gBattleStruct->moveTarget[battler];
    if (usedPriority && IsEnemyBattler(target))
    {
        bool32 blocked = Ai_IsPriorityBlocked(battler, target, move, gAiLogicData)
                      || GetMoveDamage(battler, target, move) == 0;
        AddHabit(PROFILE_PRIORITY_INTO_BLOCK, blocked);
    }
}

static void RecordProtectHabits(enum BattlerId battler, const struct ProfileThreat *threat, bool32 switched, bool32 canSwitch, enum Move move)
{
    struct ProfileBattlerState *state = &sProfile.battlers[battler];
    bool32 knowsProtect = KnowsMove(battler, IsProtectMove);
    bool32 usedProtect = IsProtectMove(move);

    if (state->scoutPending)
    {
        state->scoutPending = FALSE;
        if (canSwitch && (threat->tier >= THREAT_T2 || HasEnemyRaisedStats()))
            AddHabit(PROFILE_SCOUT_THEN_SWITCH, switched);
    }

    if (!knowsProtect)
        return;

    if (state->firstAction)
    {
        AddHabit(IsDoubleBattle() ? PROFILE_PROTECT_SCOUT_DOUBLES : PROFILE_PROTECT_SCOUT_SINGLES, usedProtect);
        if (usedProtect)
            state->scoutPending = TRUE;
    }
    if (state->protectedLastTurn)
        AddHabit(PROFILE_PROTECT_REPEAT, usedProtect);
    else if (HasStallGain(battler))
        AddHabit(PROFILE_PROTECT_STALL, usedProtect);
}

static enum Move GetChosenMove(enum BattlerId battler)
{
    if (gChosenActionByBattler[battler] != B_ACTION_USE_MOVE)
        return MOVE_NONE;
    return gChosenMoveByBattler[battler];
}

static void RecordDoublesHabits(void)
{
    enum BattlerId left = GetBattlerAtPosition(B_POSITION_PLAYER_LEFT);
    enum BattlerId right = GetBattlerAtPosition(B_POSITION_PLAYER_RIGHT);
    enum BattlerId players[2] = {left, right};

    if (!IsBattlerAlive(left) || !IsBattlerAlive(right))
        return;

    // Ally-hitting spread move + Protect on the ally
    for (u32 i = 0; i < 2; i++)
    {
        enum BattlerId spreader = players[i];
        enum BattlerId ally = players[1 - i];
        bool32 hasCombo = FALSE;

        if (!KnowsMove(ally, IsProtectMove))
            continue;
        for (u32 j = 0; j < MAX_MON_MOVES; j++)
        {
            enum Move option = gBattleMons[spreader].moves[j];
            if (IsMoveUsableBy(spreader, j) && IsDamaging(option) && GetMoveTarget(option) == TARGET_FOES_AND_ALLY
             && sProfile.dmg[spreader][ally][j].effectiveness != UQ_4_12(0.0))
                hasCombo = TRUE;
        }
        if (hasCombo)
        {
            enum Move spreaderMove = GetChosenMove(spreader);
            AddHabit(PROFILE_PROTECT_SPREAD_COMBO, IsDamaging(spreaderMove) && GetMoveTarget(spreaderMove) == TARGET_FOES_AND_ALLY
                                                && IsProtectMove(GetChosenMove(ally)));
        }
    }

    // Redirection
    enum BattlerId redirector = MAX_BATTLERS_COUNT;
    for (enum BattlerId enemy = 0; enemy < gBattlersCount; enemy++)
    {
        if (IsEnemyBattler(enemy) && GetMoveEffect(GetChosenMove(enemy)) == EFFECT_FOLLOW_ME)
            redirector = enemy;
    }
    if (redirector != MAX_BATTLERS_COUNT)
    {
        enum Move redirectMove = GetChosenMove(redirector);
        for (u32 i = 0; i < 2; i++)
        {
            enum Move move = GetChosenMove(players[i]);
            if (!IsDamaging(move) || GetMoveTarget(move) != TARGET_SELECTED)
                continue;
            bool32 avoided = IsMoveRedirectionPrevented(players[i], move, sProfile.abilities[players[i]])
                          || gBattleStruct->moveTarget[players[i]] == redirector
                          || (IsPowderMove(redirectMove) && (IS_BATTLER_OF_TYPE(players[i], TYPE_GRASS)
                                                          || sProfile.abilities[players[i]] == ABILITY_OVERCOAT
                                                          || sProfile.holdEffects[players[i]] == HOLD_EFFECT_SAFETY_GOGGLES));
            AddHabit(PROFILE_REDIRECT_RESPECT, avoided);
        }
    }

    // Focus fire
    enum Move moveLeft = GetChosenMove(left);
    enum Move moveRight = GetChosenMove(right);
    enum BattlerId targetLeft = gBattleStruct->moveTarget[left];
    enum BattlerId targetRight = gBattleStruct->moveTarget[right];
    enum BattlerId enemies[2] = {GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT), GetBattlerAtPosition(B_POSITION_OPPONENT_RIGHT)};

    if (!IsDamaging(moveLeft) || !IsDamaging(moveRight)
     || GetMoveTarget(moveLeft) != TARGET_SELECTED || GetMoveTarget(moveRight) != TARGET_SELECTED
     || !IsEnemyBattler(targetLeft) || !IsEnemyBattler(targetRight)
     || !IsEnemyBattler(enemies[0]) || !IsEnemyBattler(enemies[1]))
        return;

    bool32 focused = targetLeft == targetRight;
    bool32 neededChance = FALSE, neededHit = FALSE, overkillChance = FALSE;
    u32 damage[2][2]; // player, enemy

    for (u32 e = 0; e < 2; e++)
    {
        damage[0][e] = GetMoveDamage(left, enemies[e], moveLeft);
        damage[1][e] = GetMoveDamage(right, enemies[e], moveRight);
    }
    for (u32 e = 0; e < 2; e++)
    {
        u32 hp = gBattleMons[enemies[e]].hp;
        u32 otherHp = gBattleMons[enemies[1 - e]].hp;
        bool32 leftKos = damage[0][e] >= hp, rightKos = damage[1][e] >= hp;

        if (!leftKos && !rightKos && damage[0][e] + damage[1][e] >= hp)
        {
            neededChance = TRUE;
            if (focused && targetLeft == enemies[e])
                neededHit = TRUE;
        }
        if ((leftKos || rightKos) && (leftKos ? damage[1][1 - e] : damage[0][1 - e]) * 4 >= otherHp)
            overkillChance = TRUE;
    }
    if (neededChance)
        AddHabit(PROFILE_FOCUS_NEEDED, neededHit);
    if (overkillChance)
        AddHabit(PROFILE_FOCUS_OVERKILL, focused);

    if (focused)
    {
        u32 e = (targetLeft == enemies[0]) ? 0 : 1;
        enum BattlerId focus = enemies[e], other = enemies[1 - e];
        u32 focusThreat = GetBestDamage(focus, left, NULL) + GetBestDamage(focus, right, NULL);
        u32 otherThreat = GetBestDamage(other, left, NULL) + GetBestDamage(other, right, NULL);

        if (focusThreat > otherThreat)
            AddCount(sProfile.tally.focusTarget, PROFILE_FOCUS_THREAT);
        if ((damage[0][e] + damage[1][e]) * gBattleMons[other].hp > (damage[0][1 - e] + damage[1][1 - e]) * gBattleMons[focus].hp)
            AddCount(sProfile.tally.focusTarget, PROFILE_FOCUS_EASY_KO);
        if (HasSupportMove(focus))
            AddCount(sProfile.tally.focusTarget, PROFILE_FOCUS_SUPPORT);
        if (GetProfileSpeed(focus, 0) > GetProfileSpeed(other, 0))
            AddCount(sProfile.tally.focusTarget, PROFILE_FOCUS_FASTEST);
    }
}

static void RecordBattlerTurn(enum BattlerId battler)
{
    u32 action = gChosenActionByBattler[battler];
    enum Move move = GetChosenMove(battler);
    bool32 switched = action == B_ACTION_SWITCH;
    bool32 canSwitch = CanProfileBattlerSwitch(battler);
    struct ProfileThreat threat = EvaluateThreat(battler, move);

    RecordSwitchHabits(battler, &threat, switched, canSwitch, move);
    RecordSetupHabits(battler, &threat, move);
    RecordHealHabit(battler, &threat, move);
    RecordStatusHabits(battler, move);
    if (IsDamaging(move))
        RecordMoveChoiceHabits(battler, move);
    RecordPriorityHabits(battler, move);
    RecordProtectHabits(battler, &threat, switched, canSwitch, move);
}

// ---------------------------------------------------------------------------
// Battle hooks
// ---------------------------------------------------------------------------

static u32 GetAceTurns(u32 personality)
{
    const struct PlayerBattleProfile *profile = GetPlayerProfile();

    for (u32 i = 0; i < PROFILE_ACE_SLOTS; i++)
    {
        if (profile->aceTurns[i] != 0 && profile->acePersonality[i] == personality)
            return profile->aceTurns[i];
    }
    return 0;
}

static u32 FindAcePartyIndex(void)
{
    u32 ace = PARTY_SIZE, aceTurns = PROFILE_ACE_MIN_TURNS - 1;
    u32 strongest = PARTY_SIZE, strongestStats = 0;

    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];
        if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE || GetMonData(mon, MON_DATA_IS_EGG))
            continue;

        u32 turns = GetAceTurns(GetMonData(mon, MON_DATA_PERSONALITY));
        u32 baseStats = GetTotalBaseStat(GetMonData(mon, MON_DATA_SPECIES));
        if (turns > aceTurns)
        {
            aceTurns = turns;
            ace = i;
        }
        if (baseStats > strongestStats)
        {
            strongestStats = baseStats;
            strongest = i;
        }
    }
    return ace != PARTY_SIZE ? ace : strongest;
}

void PlayerProfile_OnBattleStart(void)
{
    memset(&sProfile, 0, sizeof(sProfile));
    if (!ShouldRecordBattle())
        return;

    sProfile.active = TRUE;
    sProfile.acePartyIndex = FindAcePartyIndex();
    for (enum BattlerId battler = 0; battler < MAX_BATTLERS_COUNT; battler++)
    {
        sProfile.battlers[battler].partyIndex = gBattlerPartyIndexes[battler];
        sProfile.battlers[battler].firstAction = TRUE;
    }
}

void PlayerProfile_RecordTurn(void)
{
    enum Ability savedAbilities[MAX_BATTLERS_COUNT];
    enum HoldEffect savedHoldEffects[MAX_BATTLERS_COUNT];
    bool32 savedCalcInProgress;

    if (!sProfile.active)
        return;

    // The AI helpers read abilities and items from gAiLogicData; give them the real ones while recording.
    memcpy(savedAbilities, gAiLogicData->abilities, sizeof(savedAbilities));
    memcpy(savedHoldEffects, gAiLogicData->holdEffects, sizeof(savedHoldEffects));
    savedCalcInProgress = gAiLogicData->aiCalcInProgress;
    gAiLogicData->aiCalcInProgress = TRUE;
    for (enum BattlerId battler = 0; battler < MAX_BATTLERS_COUNT; battler++)
    {
        bool32 present = battler < gBattlersCount && IsBattlerAlive(battler);
        sProfile.abilities[battler] = gAiLogicData->abilities[battler] = present ? GetBattlerAbility(battler) : ABILITY_NONE;
        sProfile.holdEffects[battler] = gAiLogicData->holdEffects[battler] = present ? GetBattlerHoldEffect(battler) : HOLD_EFFECT_NONE;
    }

    UpdateFieldTracking();
    CalcAllProfileDamage();

    for (enum BattlerId battler = 0; battler < gBattlersCount; battler++)
    {
        if (IsOnPlayerSide(battler))
            ResolvePendingOutcomes(battler);
    }

    if (!sProfile.leadRecorded)
    {
        RecordLead();
        sProfile.leadRecorded = TRUE;
    }

    for (enum BattlerId battler = 0; battler < gBattlersCount; battler++)
    {
        u32 action = gChosenActionByBattler[battler];
        if (!IsOnPlayerSide(battler) || !IsBattlerAlive(battler))
            continue;
        if (action != B_ACTION_USE_MOVE && action != B_ACTION_SWITCH && action != B_ACTION_USE_ITEM)
            continue;
        RecordBattlerTurn(battler);
    }

    if (IsDoubleBattle())
        RecordDoublesHabits();

    for (enum BattlerId battler = 0; battler < gBattlersCount; battler++)
    {
        struct ProfileBattlerState *state = &sProfile.battlers[battler];
        if (!IsBattlerAlive(battler))
            continue;
        if (IsOnPlayerSide(battler) && state->partyIndex < PARTY_SIZE)
            AddCount(sProfile.turnsOnField, state->partyIndex);
        state->protectedLastTurn = IsProtectMove(GetChosenMove(battler));
        state->firstAction = FALSE;
    }

    memcpy(gAiLogicData->abilities, savedAbilities, sizeof(savedAbilities));
    memcpy(gAiLogicData->holdEffects, savedHoldEffects, sizeof(savedHoldEffects));
    gAiLogicData->aiCalcInProgress = savedCalcInProgress;
}

void PlayerProfile_OnFreeSwitchOffered(void)
{
    struct ProfileCounter *counter = &sProfile.tally.stats[PROFILE_FREE_SWITCH_TAKEN];

    if (sProfile.active && counter->chances < UINT8_MAX)
        counter->chances++;
}

void PlayerProfile_OnFreeSwitchTaken(void)
{
    struct ProfileCounter *counter = &sProfile.tally.stats[PROFILE_FREE_SWITCH_TAKEN];
    struct Pokemon *switchIn, *enemySwitchIn;

    if (!sProfile.active)
        return;
    if (counter->hits < counter->chances)
        counter->hits++;

    switchIn = GetBattlerMon(gBattlerAttacker);
    enemySwitchIn = GetSwitchInMon(gBattlerFainted);
    if (enemySwitchIn != NULL)
        AddHabit(PROFILE_FREE_SWITCH_COUNTER, DoesPartyMonBeat(switchIn, enemySwitchIn));
}

static void MergeCounter(struct ProfileCounter *saved, const struct ProfileCounter *battle)
{
    u32 hits = saved->hits + battle->hits;
    u32 chances = saved->chances + battle->chances;

    while (chances >= PROFILE_DECAY_THRESHOLD)
    {
        hits /= 2;
        chances /= 2;
    }
    saved->hits = min(hits, chances);
    saved->chances = chances;
}

static void MergeCounts(u8 *saved, const u8 *battle, u32 count)
{
    bool32 full = FALSE;

    for (u32 i = 0; i < count; i++)
    {
        u32 total = saved[i] + battle[i];
        saved[i] = min(total, UINT8_MAX);
        if (total >= UINT8_MAX)
            full = TRUE;
    }
    if (full)
    {
        for (u32 i = 0; i < count; i++)
            saved[i] /= 2;
    }
}

static void MergeAceTable(struct PlayerBattleProfile *profile)
{
    for (u32 partyIndex = 0; partyIndex < PARTY_SIZE; partyIndex++)
    {
        u32 turns = sProfile.turnsOnField[partyIndex];
        u32 personality, slot = PROFILE_ACE_SLOTS, lowest = 0;

        if (turns == 0)
            continue;
        personality = GetMonData(&gParties[B_TRAINER_PLAYER][partyIndex], MON_DATA_PERSONALITY);
        for (u32 i = 0; i < PROFILE_ACE_SLOTS; i++)
        {
            if (profile->aceTurns[i] != 0 && profile->acePersonality[i] == personality)
            {
                slot = i;
                break;
            }
            if (profile->aceTurns[i] < profile->aceTurns[lowest])
                lowest = i;
        }
        if (slot == PROFILE_ACE_SLOTS)
        {
            slot = lowest;
            profile->acePersonality[slot] = personality;
            profile->aceTurns[slot] = 0;
        }
        profile->aceTurns[slot] = min(profile->aceTurns[slot] + turns, UINT8_MAX);
    }

    if (++profile->battlesSinceAceDecay >= PROFILE_ACE_DECAY_BATTLES)
    {
        profile->battlesSinceAceDecay = 0;
        for (u32 i = 0; i < PROFILE_ACE_SLOTS; i++)
            profile->aceTurns[i] /= 2;
    }
}

void PlayerProfile_CommitBattle(void)
{
    struct PlayerBattleProfile *profile;
    u32 style = gSaveBlock2Ptr->optionsBattleStyle;

    if (!sProfile.active)
        return;
    sProfile.active = FALSE;
    profile = GetPlayerProfile();

    // Outcomes the battle ended before resolving: only fainting can still be judged.
    for (enum BattlerId battler = 0; battler < gBattlersCount; battler++)
    {
        struct ProfileBattlerState *state = &sProfile.battlers[battler];
        struct Pokemon *party = GetBattlerParty(battler);

        if (!IsOnPlayerSide(battler))
            continue;
        if (state->setupPending && GetMonData(&party[state->setupPartyIndex], MON_DATA_HP) == 0)
            AddHabit(PROFILE_SETUP_PUNISHED, TRUE);
        if (state->sackPending && state->sackStatused && GetMonData(&party[state->sackPartyIndex], MON_DATA_HP) == 0)
            AddHabit(PROFILE_REVENGE_AFTER_SACK, TRUE);
    }

    // A style change reshapes switching, so let the new style's data take over quickly.
    if (profile->battlesRecorded != 0 && profile->lastStyle != style)
    {
        for (u32 i = 0; i < ARRAY_COUNT(sStyleDependentStats); i++)
        {
            profile->stats[sStyleDependentStats[i]].hits /= 2;
            profile->stats[sStyleDependentStats[i]].chances /= 2;
        }
    }
    profile->lastStyle = style;
    if (style == OPTIONS_BATTLE_STYLE_SET)
        profile->battlesSet = min(profile->battlesSet + 1, UINT8_MAX);
    else
        profile->battlesSwitch = min(profile->battlesSwitch + 1, UINT8_MAX);

    for (u32 i = 0; i < PROFILE_STAT_COUNT; i++)
        MergeCounter(&profile->stats[i], &sProfile.tally.stats[i]);
    MergeCounts(profile->statusKind, sProfile.tally.statusKind, PROFILE_STATUS_KIND_COUNT);
    MergeCounts(profile->focusTarget, sProfile.tally.focusTarget, PROFILE_FOCUS_TARGET_COUNT);
    MergeAceTable(profile);
    profile->battlesRecorded = min(profile->battlesRecorded + 1, UINT8_MAX);
}
