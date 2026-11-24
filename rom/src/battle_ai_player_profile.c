#include "global.h"
#include "battle.h"
#include "battle_ai_main.h"
#include "battle_ai_player_profile.h"
#include "battle_ai_util.h"
#include "battle_util.h"
#include "fpmath.h"
#include "pokemon.h"
#include "random.h"
#include "battle_controllers.h"
#include "battle_setup.h"
#include "telemetry.h"
#include "constants/battle_ai.h"
#include "constants/battle_move_effects.h"

// AI_FLAG_PLAYER_PROFILE (see include/battle_ai_player_profile.h).
//
// Every rule reads one recorded habit and nudges move scores by a small amount, so the base AI
// still decides and the profile only tips close calls. A habit counts once the player had
// PROFILE_AI_MIN_SAMPLES chances to show it and their rate is at least PROFILE_AI_BIAS_THRESHOLD
// away from 50% (bias -100..100, from GetPlayerProfileBias). With an empty or thin profile, or a
// player without clear habits, nothing changes.
//
// The AI learns nothing new: checks on the player's moves use GetMovesArray, which only holds
// the moves the AI has seen unless the trainer already has a move omniscience flag.

static bool32 PlayerUsually(enum PlayerProfileStat stat)
{
    return GetPlayerProfileBias(stat, PROFILE_AI_MIN_SAMPLES) >= PROFILE_AI_BIAS_THRESHOLD;
}

static bool32 PlayerRarely(enum PlayerProfileStat stat)
{
    return GetPlayerProfileBias(stat, PROFILE_AI_MIN_SAMPLES) <= -PROFILE_AI_BIAS_THRESHOLD;
}

static bool32 AiKnowsMove(enum BattlerId battler, bool32 (*predicate)(enum Move))
{
    enum Move *moves = GetMovesArray(battler);

    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        if (moves[i] != MOVE_NONE && moves[i] != MOVE_UNAVAILABLE && predicate(moves[i]))
            return TRUE;
    }
    return FALSE;
}

static bool32 IsPriorityAttack(enum Move move)
{
    return GetMovePriority(move) > 0 && GetMoveCategory(move) != DAMAGE_CATEGORY_STATUS;
}

static bool32 IsSpreadAttack(enum Move move)
{
    return Profile_IsSpreadMove(move) && GetMoveCategory(move) != DAMAGE_CATEGORY_STATUS;
}

// gLastMoves is cleared on every switch-in, leads included: no move yet means its first action.
static bool32 HasNotActedYet(enum BattlerId battler)
{
    return gLastMoves[battler] == MOVE_NONE;
}

static bool32 AnyPlayerBattlerKnows(bool32 (*predicate)(enum Move))
{
    for (enum BattlerId battler = 0; battler < gBattlersCount; battler++)
    {
        if (IsOnPlayerSide(battler) && IsBattlerAlive(battler) && AiKnowsMove(battler, predicate))
            return TRUE;
    }
    return FALSE;
}

static bool32 AnyNewPlayerBattlerKnows(bool32 (*predicate)(enum Move))
{
    for (enum BattlerId battler = 0; battler < gBattlersCount; battler++)
    {
        if (IsOnPlayerSide(battler) && IsBattlerAlive(battler) && HasNotActedYet(battler) && AiKnowsMove(battler, predicate))
            return TRUE;
    }
    return FALSE;
}

static bool32 PlayerSetsUp(void)
{
    return PlayerUsually(PROFILE_SETUP_ENTRY_SOUND)
        || PlayerUsually(PROFILE_SETUP_ENTRY_RISKY)
        || PlayerUsually(PROFILE_SETUP_WHEN_SAFE);
}

static bool32 IsPhazingOrReset(enum Move move)
{
    enum BattleMoveEffects effect = GetMoveEffect(move);
    return effect == EFFECT_HAZE || effect == EFFECT_ROAR || effect == EFFECT_HIT_SWITCH_TARGET
        || MoveHasAdditionalEffect(move, MOVE_EFFECT_CLEAR_SMOG);
}

static u32 HpPercent(enum BattlerId battler)
{
    return gBattleMons[battler].maxHP ? gBattleMons[battler].hp * 100 / gBattleMons[battler].maxHP : 0;
}

// The League plan for the trainer that controls this battler, if the planner wrote one.
static const struct AiTrainerDirective *GetBattlerDirective(enum BattlerId battler)
{
    u16 trainerId = GetBattlerTrainer(battler) == B_TRAINER_OPPONENT_B ? TRAINER_BATTLE_PARAM.opponentB
                                                                       : TRAINER_BATTLE_PARAM.opponentA;
    return Mailbox_GetDirective(trainerId);
}

// With a plan, the plan picks which habits to punish; without one, a rule waits for a clear habit.
static bool32 Punish(const struct AiTrainerDirective *plan, u16 rule, bool32 playerHasHabit)
{
    return plan != NULL ? (plan->punishMask & rule) != 0 : playerHasHabit;
}

static u32 BestAttackingStat(enum BattlerId battler)
{
    return max(gBattleMons[battler].attack, gBattleMons[battler].spAttack);
}

// The player battler the plan wants the AI to focus, or MAX_BATTLERS_COUNT.
static enum BattlerId GetFocusTarget(enum DirectiveFocusTarget focus)
{
    enum BattlerId target = MAX_BATTLERS_COUNT;

    for (enum BattlerId battler = 0; battler < gBattlersCount; battler++)
    {
        if (!IsOnPlayerSide(battler) || !IsBattlerAlive(battler))
            continue;
        if (target == MAX_BATTLERS_COUNT
         || (focus == DIRECTIVE_FOCUS_LOWEST_HP && HpPercent(battler) < HpPercent(target))
         || (focus == DIRECTIVE_FOCUS_FASTEST && gBattleMons[battler].speed > gBattleMons[target].speed)
         || (focus == DIRECTIVE_FOCUS_STRONGEST && BestAttackingStat(battler) > BestAttackingStat(target)))
            target = battler;
    }
    return target;
}

static s32 ClampWeight(s32 weight)
{
    return max(-AI_DIRECTIVE_WEIGHT_MAX, min(weight, AI_DIRECTIVE_WEIGHT_MAX));
}

s32 AI_PlayerProfile(enum BattlerId battlerAtk, enum BattlerId battlerDef, enum Move move, s32 score)
{
    enum BattleMoveEffects effect = GetMoveEffect(move);
    bool32 damaging = GetMoveCategory(move) != DAMAGE_CATEGORY_STATUS;
    enum BattlerId partner = GetPartnerBattler(battlerAtk);
    bool32 partnerAlive = IsDoubleBattle() && IsBattlerAlive(partner);
    const struct AiTrainerDirective *plan;

    // While predicting, the engine also scores the player's moves with the trainer's flags.
    // The player's habits only shape the AI's own choices.
    if (IsOnPlayerSide(battlerAtk))
        return score;
    plan = GetBattlerDirective(battlerAtk);

    if (IsOnPlayerSide(battlerDef) && IsBattlerAlive(battlerDef))
    {
        // Sets up when given the chance: Taunt before it happens, reset or phaze once it has.
        if (Punish(plan, DIRECTIVE_PUNISH_SETUP, PlayerSetsUp()) && AiKnowsMove(battlerDef, Profile_IsSetupMove))
        {
            if (effect == EFFECT_TAUNT && !AnyStatIsRaised(battlerDef))
                ADJUST_SCORE(DECENT_EFFECT);
            else if (AnyStatIsRaised(battlerDef) && IsPhazingOrReset(move))
                ADJUST_SCORE(GOOD_EFFECT);
        }

        // Heals with a move at low HP (moves only: Nuzlocke players don't heal with items in battle).
        if (Punish(plan, DIRECTIVE_PUNISH_HEALING, PlayerUsually(PROFILE_HEAL_AT_LOW_HP)) && HpPercent(battlerDef) < 40
         && AiKnowsMove(battlerDef, Profile_IsSelfHealingMove)
         && (effect == EFFECT_TAUNT || effect == EFFECT_HEAL_BLOCK))
            ADJUST_SCORE(DECENT_EFFECT);

        // Scouts with Protect on its first action: an attack into it is wasted, a Protect-breaking one isn't.
        enum PlayerProfileStat scout = IsDoubleBattle() ? PROFILE_PROTECT_SCOUT_DOUBLES : PROFILE_PROTECT_SCOUT_SINGLES;
        if (damaging && HasNotActedYet(battlerDef) && Punish(plan, DIRECTIVE_PUNISH_PROTECT_SCOUT, PlayerUsually(scout))
         && AiKnowsMove(battlerDef, Profile_IsProtectMove))
        {
            ADJUST_SCORE(MoveIgnoresProtect(move) ? DECENT_EFFECT : -DECENT_EFFECT);
        }
        // Right after a Protect: hit it if the player rarely protects twice, don't if they often do.
        else if (damaging && Profile_IsProtectMove(gLastMoves[battlerDef]))
        {
            if (Punish(plan, DIRECTIVE_PUNISH_PROTECT_REPEAT, PlayerRarely(PROFILE_PROTECT_REPEAT)))
                ADJUST_SCORE(DECENT_EFFECT);
            else if (plan == NULL && PlayerUsually(PROFILE_PROTECT_REPEAT) && !MoveIgnoresProtect(move))
                ADJUST_SCORE(-DECENT_EFFECT);
        }

        // Plan: focus one player mon in doubles.
        if (plan != NULL && damaging && partnerAlive && plan->focusTarget != DIRECTIVE_FOCUS_NONE
         && plan->focusTarget < DIRECTIVE_FOCUS_COUNT && battlerDef == GetFocusTarget(plan->focusTarget))
            ADJUST_SCORE(DECENT_EFFECT);
    }

    // Opens with status moves: shield the AI's side while the player's new mons haven't acted.
    if ((effect == EFFECT_SAFEGUARD || (effect == EFFECT_TERRAIN && GetMoveTerrainType(move) == B_TERRAIN_MISTY))
     && Punish(plan, DIRECTIVE_PUNISH_STATUS, PlayerUsually(PROFILE_STATUS_OPEN_VALID)) && AnyNewPlayerBattlerKnows(Profile_IsStatusMove))
        ADJUST_SCORE(DECENT_EFFECT);

    // Finishes with priority when slower: a low AI mon protects instead of trusting its speed.
    if (Profile_IsProtectMove(move) && HpPercent(battlerAtk) <= 33
     && Punish(plan, DIRECTIVE_PUNISH_PRIORITY, PlayerUsually(PROFILE_PRIORITY_NEEDED)) && AnyPlayerBattlerKnows(IsPriorityAttack))
        ADJUST_SCORE(DECENT_EFFECT);

    // Doubles: spread attacks get answered by Wide Guard.
    if (partnerAlive && effect == EFFECT_PROTECT && GetMoveProtectMethod(move) == PROTECT_WIDE_GUARD
     && Punish(plan, DIRECTIVE_PUNISH_SPREAD, PlayerUsually(PROFILE_SPREAD_PREFERENCE)) && AnyPlayerBattlerKnows(IsSpreadAttack))
        ADJUST_SCORE(DECENT_EFFECT);

    // Doubles: focus fire lands on the weaker AI mon, so it protects and its partner redirects.
    if (partnerAlive && Punish(plan, DIRECTIVE_PUNISH_FOCUS, PlayerUsually(PROFILE_FOCUS_NEEDED) || PlayerUsually(PROFILE_FOCUS_OVERKILL)))
    {
        bool32 isLikelyTarget = HpPercent(battlerAtk) < HpPercent(partner);
        if (isLikelyTarget && Profile_IsProtectMove(move))
            ADJUST_SCORE(DECENT_EFFECT);
        else if (!isLikelyTarget && effect == EFFECT_FOLLOW_ME)
            ADJUST_SCORE(DECENT_EFFECT);
    }

    // Plan: overall style, as bounded weights on attacks and on the AI's own setup.
    if (plan != NULL)
    {
        if (damaging && plan->aggression != 0)
            ADJUST_SCORE(ClampWeight(plan->aggression));
        if (Profile_IsSetupMove(move) && plan->selfSetup != 0)
            ADJUST_SCORE(ClampWeight(plan->selfSetup));
    }

    return score;
}

// With AI_FLAG_PREDICT_SWITCH, how often the AI assumes the player will switch when it thinks they
// should: the player's own switch rate under threat (T1-T3), instead of a fixed chance.
u32 GetProfilePredictSwitchChance(void)
{
    static const enum PlayerProfileStat switchStats[] =
    {
        PROFILE_SWITCH_T1,
        PROFILE_SWITCH_T2,
        PROFILE_SWITCH_T3_ACE,
        PROFILE_SWITCH_T3_OTHER,
    };
    u32 rate = GetPlayerProfileRate(switchStats, ARRAY_COUNT(switchStats), PROFILE_AI_MIN_SAMPLES);

    if (rate == UINT32_MAX)
        return PREDICT_SWITCH_CHANCE;
    return min(max(rate, PROFILE_AI_SWITCH_CHANCE_MIN), PROFILE_AI_SWITCH_CHANCE_MAX);
}

// The plan's switch prediction for this battler's trainer, else the profile's.
u32 GetBattlerPredictSwitchChance(enum BattlerId battler)
{
    const struct AiTrainerDirective *plan = GetBattlerDirective(battler);

    if (plan != NULL && plan->switchPredictChance <= 100)
        return plan->switchPredictChance;
    return GetProfilePredictSwitchChance();
}

// ---------------------------------------------------------------------------
// Lead change
// ---------------------------------------------------------------------------
//
// Players can read every trainer's team in the public docs, so many open with a counter to the
// documented lead. A trainer with AI_FLAG_PLAYER_PROFILE facing a player who does that
// (PROFILE_LEAD_COUNTERS_ENEMY_LEAD) sometimes leads with someone else. It doesn't peek at the
// player's party: it assumes the counter hits the documented lead super effectively, and sends
// the benched mon that takes those types best. The docs become a likely guess, not an answer key.

static bool32 IsAttackingType(enum Type type)
{
    return type != TYPE_NONE && type != TYPE_MYSTERY && type != TYPE_STELLAR;
}

// Type effectiveness of one attacking type against a species (abilities and Tera ignored).
static uq4_12_t TypeEffectiveness(enum Type attackType, enum Species species)
{
    enum Type type1 = GetSpeciesType(species, 0), type2 = GetSpeciesType(species, 1);
    uq4_12_t effectiveness = GetTypeModifier(attackType, type1);

    if (type2 != type1)
        effectiveness = uq4_12_multiply(effectiveness, GetTypeModifier(attackType, type2));
    return effectiveness;
}

// How hard a species is hit by the types that are super effective against the documented lead.
static u32 DamageTakenFromCounters(enum Species species, enum Species lead)
{
    u32 total = 0;

    for (enum Type type = 0; type < NUMBER_OF_MON_TYPES; type++)
    {
        if (IsAttackingType(type) && TypeEffectiveness(type, lead) >= UQ_4_12(2.0))
            total += TypeEffectiveness(type, species);
    }
    return total;
}

static u32 CountWeaknesses(enum Species species)
{
    u32 count = 0;

    for (enum Type type = 0; type < NUMBER_OF_MON_TYPES; type++)
    {
        if (IsAttackingType(type) && TypeEffectiveness(type, species) >= UQ_4_12(2.0))
            count++;
    }
    return count;
}

// Party slots [0, result) may lead: aces stay in the back.
static u32 GetLeadCandidateCount(u32 count, u64 aiFlags)
{
    if (aiFlags & AI_FLAG_DOUBLE_ACE_POKEMON)
        return count >= 2 ? count - 2 : 0;
    if (aiFlags & AI_FLAG_ACE_POKEMON)
        return count >= 1 ? count - 1 : 0;
    return count;
}

// Leads chosen by the League plan: they move to the front, everyone else keeps their order.
// Returns FALSE (party untouched) without a plan, when the plan keeps the documented leads,
// or when a slot is out of range, repeated or an ace.
bool32 ProfileAI_ApplyPlanLeads(struct Pokemon *party, u32 count, u64 aiFlags, u32 leadCount,
                                const struct AiTrainerDirective *plan)
{
    u32 candidates = GetLeadCandidateCount(count, aiFlags), order[PARTY_SIZE], n = 0;
    struct Pokemon copy[PARTY_SIZE];

    if (plan == NULL || leadCount == 0 || leadCount > 2 || count > PARTY_SIZE)
        return FALSE;
    for (u32 pos = 0; pos < leadCount; pos++)
    {
        u32 slot = plan->leadSlots[pos];
        if (slot >= candidates || (pos == 1 && slot == plan->leadSlots[0]))
            return FALSE;
        order[n++] = slot;
    }
    for (u32 slot = 0; slot < count; slot++)
    {
        if (slot != order[0] && (leadCount == 1 || slot != order[1]))
            order[n++] = slot;
    }

    memcpy(copy, party, count * sizeof(struct Pokemon));
    for (u32 i = 0; i < count; i++)
        party[i] = copy[order[i]];
    return TRUE;
}

// Percent chance to change the lead: the player's counter rate above the threshold, capped.
u32 GetProfileLeadChangeChance(void)
{
    s32 bias = GetPlayerProfileBias(PROFILE_LEAD_COUNTERS_ENEMY_LEAD, PROFILE_AI_MIN_SAMPLES);

    if (bias < PROFILE_AI_BIAS_THRESHOLD)
        return 0;
    return min((u32)bias, PROFILE_AI_LEAD_CHANCE_MAX);
}

// party[0..count) is one trainer's team; party[0..leadCount) are its leads (2 in a doubles battle).
// Returns TRUE if a lead was swapped with a benched mon.
bool32 ProfileAI_ChangeLead(struct Pokemon *party, u32 count, u64 aiFlags, u32 leadCount)
{
    u32 lastSwappable = GetLeadCandidateCount(count, aiFlags), leadSlot = 0, bestSlot = PARTY_SIZE, bestDamage;
    enum Species lead;

    if (lastSwappable <= leadCount)
        return FALSE; // nobody on the bench to send instead
    if (GetProfileLeadChangeChance() == 0)
        return FALSE; // the player doesn't counter documented leads (or there's too little data)
    if (!RandomPercentage(RNG_AI_PROFILE_LEAD, GetProfileLeadChangeChance()))
        return FALSE;

    // In doubles, replace the lead that is easiest to counter
    for (u32 i = 1; i < leadCount; i++)
    {
        if (CountWeaknesses(GetMonData(&party[i], MON_DATA_SPECIES)) > CountWeaknesses(GetMonData(&party[leadSlot], MON_DATA_SPECIES)))
            leadSlot = i;
    }
    lead = GetMonData(&party[leadSlot], MON_DATA_SPECIES);

    bestDamage = DamageTakenFromCounters(lead, lead);
    for (u32 i = leadCount; i < lastSwappable; i++)
    {
        u32 damage = DamageTakenFromCounters(GetMonData(&party[i], MON_DATA_SPECIES), lead);
        if (damage < bestDamage)
        {
            bestDamage = damage;
            bestSlot = i;
        }
    }
    if (bestSlot == PARTY_SIZE)
        return FALSE; // nobody takes the counter better than the lead itself

    struct Pokemon temp = party[leadSlot];
    party[leadSlot] = party[bestSlot];
    party[bestSlot] = temp;
    return TRUE;
}
