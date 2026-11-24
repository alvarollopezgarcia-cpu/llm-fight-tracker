#include "global.h"
#include "test/battle.h"
#include "battle_ai_player_profile.h"
#include "telemetry.h"

// AI_FLAG_PLAYER_PROFILE. Each test runs the same situation with and without the recorded habit.
// Only the profile flag scores moves (all start at AI_SCORE_DEFAULT), so any difference comes
// from the profile. AI_FLAG_MOVE_OMNISCIENCE stands in for "the AI has already seen that move";
// the League trainers don't have it and learn the player's moves by seeing them, as before.

static void SetHabit(enum PlayerProfileStat stat, u32 hits, u32 chances)
{
    GetPlayerProfile()->stats[stat].hits = hits;
    GetPlayerProfile()->stats[stat].chances = chances;
}

// AI battle tests fight TRAINER_LEAF_TEST (2), defined privately in test/test_runner_battle.c.
#define PLAN_TEST_TRAINER  2
#define PLAN_OTHER_TRAINER 1

// The League plan (struct AiDirective) as the bridge writes it, for the AI test trainer.
static struct AiTrainerDirective PlanEntry(u16 trainerId)
{
    struct AiTrainerDirective entry = {0};
    entry.trainerId = trainerId;
    entry.leadSlots[0] = entry.leadSlots[1] = AI_DIRECTIVE_KEEP;
    entry.switchPredictChance = AI_DIRECTIVE_KEEP;
    return entry;
}

static void SetPlan(const struct AiTrainerDirective *entry)
{
    struct AiDirective directive = {0};
    directive.magic = AI_DIRECTIVE_MAGIC;
    directive.planId = 1;
    directive.count = 1;
    directive.trainers[0] = *entry;
    Mailbox_SetDirective(&directive);
}

static void ResetProfileAndPlan(void)
{
    struct AiDirective none = {0};
    ResetPlayerProfile();
    Mailbox_SetDirective(&none);
}

TEST("Player Profile AI: switch prediction follows how often the player switches under threat")
{
    ResetProfileAndPlan();
    EXPECT_EQ(GetProfilePredictSwitchChance(), PREDICT_SWITCH_CHANCE); // no data: engine default

    SetHabit(PROFILE_SWITCH_T1, 2, 5);
    EXPECT_EQ(GetProfilePredictSwitchChance(), PREDICT_SWITCH_CHANCE); // too few chances to trust

    SetHabit(PROFILE_SWITCH_T1, 6, 10);
    SetHabit(PROFILE_SWITCH_T3_OTHER, 3, 10);
    EXPECT_EQ(GetProfilePredictSwitchChance(), 45); // 9 switches in 20 threats

    SetHabit(PROFILE_SWITCH_T1, 10, 10);
    SetHabit(PROFILE_SWITCH_T3_OTHER, 10, 10);
    EXPECT_EQ(GetProfilePredictSwitchChance(), PROFILE_AI_SWITCH_CHANCE_MAX);

    SetHabit(PROFILE_SWITCH_T1, 0, 10);
    SetHabit(PROFILE_SWITCH_T3_OTHER, 0, 10);
    EXPECT_EQ(GetProfilePredictSwitchChance(), PROFILE_AI_SWITCH_CHANCE_MIN);
}

AI_SINGLE_BATTLE_TEST("Player Profile AI: Taunts a player who sets up when given the chance")
{
    bool32 setsUp;

    PARAMETRIZE { setsUp = TRUE; }
    PARAMETRIZE { setsUp = FALSE; }
    GIVEN {
        ResetProfileAndPlan();
        SetHabit(PROFILE_SETUP_WHEN_SAFE, setsUp ? 18 : 2, 20);
        AI_FLAGS(AI_FLAG_PLAYER_PROFILE | AI_FLAG_MOVE_OMNISCIENCE);
        PLAYER(SPECIES_DRAGONITE) { Moves(MOVE_DRAGON_DANCE, MOVE_EXTREME_SPEED); }
        OPPONENT(SPECIES_SABLEYE) { Moves(MOVE_TAUNT, MOVE_SHADOW_BALL); }
    } WHEN {
        if (setsUp)
            TURN { MOVE(player, MOVE_EXTREME_SPEED); SCORE_GT(opponent, MOVE_TAUNT, MOVE_SHADOW_BALL); }
        else
            TURN { MOVE(player, MOVE_EXTREME_SPEED); SCORE_EQ(opponent, MOVE_TAUNT, MOVE_SHADOW_BALL); }
    }
}

AI_SINGLE_BATTLE_TEST("Player Profile AI: resets a set-up player's boosts")
{
    bool32 setsUp;

    PARAMETRIZE { setsUp = TRUE; }
    PARAMETRIZE { setsUp = FALSE; }
    GIVEN {
        ResetProfileAndPlan();
        SetHabit(PROFILE_SETUP_ENTRY_SOUND, setsUp ? 18 : 2, 20);
        AI_FLAGS(AI_FLAG_PLAYER_PROFILE | AI_FLAG_MOVE_OMNISCIENCE);
        PLAYER(SPECIES_DRAGONITE) { Speed(1); Moves(MOVE_DRAGON_DANCE, MOVE_EXTREME_SPEED); }
        OPPONENT(SPECIES_WEEZING) { Speed(100); Moves(MOVE_HAZE, MOVE_SLUDGE_BOMB); }
    } WHEN {
        TURN { MOVE(player, MOVE_DRAGON_DANCE); } // Weezing moves first, so the boost is there next turn either way
        if (setsUp)
            TURN { MOVE(player, MOVE_EXTREME_SPEED); SCORE_GT(opponent, MOVE_HAZE, MOVE_SLUDGE_BOMB); }
        else
            TURN { MOVE(player, MOVE_EXTREME_SPEED); SCORE_EQ(opponent, MOVE_HAZE, MOVE_SLUDGE_BOMB); }
    }
}

AI_SINGLE_BATTLE_TEST("Player Profile AI: stops a low player mon that heals with moves")
{
    bool32 heals;

    PARAMETRIZE { heals = TRUE; }
    PARAMETRIZE { heals = FALSE; }
    GIVEN {
        ResetProfileAndPlan();
        SetHabit(PROFILE_HEAL_AT_LOW_HP, heals ? 18 : 2, 20);
        AI_FLAGS(AI_FLAG_PLAYER_PROFILE | AI_FLAG_MOVE_OMNISCIENCE);
        PLAYER(SPECIES_BLISSEY) { HP(100); MaxHP(400); Moves(MOVE_SOFT_BOILED, MOVE_SEISMIC_TOSS); }
        OPPONENT(SPECIES_SABLEYE) { Moves(MOVE_HEAL_BLOCK, MOVE_SHADOW_BALL); }
    } WHEN {
        if (heals)
            TURN { MOVE(player, MOVE_SEISMIC_TOSS); SCORE_GT(opponent, MOVE_HEAL_BLOCK, MOVE_SHADOW_BALL); }
        else
            TURN { MOVE(player, MOVE_SEISMIC_TOSS); SCORE_EQ(opponent, MOVE_HEAL_BLOCK, MOVE_SHADOW_BALL); }
    }
}

AI_SINGLE_BATTLE_TEST("Player Profile AI: doesn't attack into a player who scouts with Protect on entry")
{
    bool32 scouts;

    PARAMETRIZE { scouts = TRUE; }
    PARAMETRIZE { scouts = FALSE; }
    GIVEN {
        ResetProfileAndPlan();
        SetHabit(PROFILE_PROTECT_SCOUT_SINGLES, scouts ? 18 : 2, 20);
        AI_FLAGS(AI_FLAG_PLAYER_PROFILE | AI_FLAG_MOVE_OMNISCIENCE);
        PLAYER(SPECIES_WOBBUFFET) { Moves(MOVE_PROTECT, MOVE_TACKLE); }
        OPPONENT(SPECIES_WOBBUFFET) { Moves(MOVE_FEINT, MOVE_TACKLE); }
    } WHEN {
        if (scouts)
            TURN { MOVE(player, MOVE_PROTECT); SCORE_GT(opponent, MOVE_FEINT, MOVE_TACKLE); }
        else
            TURN { MOVE(player, MOVE_PROTECT); SCORE_EQ(opponent, MOVE_FEINT, MOVE_TACKLE); }
    }
}

AI_SINGLE_BATTLE_TEST("Player Profile AI: attacks right after a Protect when the player rarely repeats it")
{
    bool32 rarelyRepeats;

    PARAMETRIZE { rarelyRepeats = TRUE; }
    PARAMETRIZE { rarelyRepeats = FALSE; }
    GIVEN {
        ResetProfileAndPlan();
        SetHabit(PROFILE_PROTECT_REPEAT, rarelyRepeats ? 1 : 10, 20);
        AI_FLAGS(AI_FLAG_PLAYER_PROFILE);
        PLAYER(SPECIES_WOBBUFFET) { Moves(MOVE_PROTECT, MOVE_TACKLE); }
        OPPONENT(SPECIES_WOBBUFFET) { Moves(MOVE_TACKLE, MOVE_CELEBRATE); }
    } WHEN {
        TURN { MOVE(player, MOVE_PROTECT); }
        if (rarelyRepeats)
            TURN { MOVE(player, MOVE_TACKLE); SCORE_GT(opponent, MOVE_TACKLE, MOVE_CELEBRATE); }
        else
            TURN { MOVE(player, MOVE_TACKLE); SCORE_EQ(opponent, MOVE_TACKLE, MOVE_CELEBRATE); }
    }
}

AI_SINGLE_BATTLE_TEST("Player Profile AI: a low AI mon protects against a player who finishes with priority")
{
    bool32 usesPriority;

    PARAMETRIZE { usesPriority = TRUE; }
    PARAMETRIZE { usesPriority = FALSE; }
    GIVEN {
        ResetProfileAndPlan();
        SetHabit(PROFILE_PRIORITY_NEEDED, usesPriority ? 18 : 2, 20);
        AI_FLAGS(AI_FLAG_PLAYER_PROFILE | AI_FLAG_MOVE_OMNISCIENCE);
        PLAYER(SPECIES_SCIZOR) { Moves(MOVE_BULLET_PUNCH, MOVE_X_SCISSOR); }
        OPPONENT(SPECIES_WOBBUFFET) { HP(50); MaxHP(300); Moves(MOVE_PROTECT, MOVE_TACKLE); }
    } WHEN {
        if (usesPriority)
            TURN { MOVE(player, MOVE_X_SCISSOR); SCORE_GT(opponent, MOVE_PROTECT, MOVE_TACKLE); }
        else
            TURN { MOVE(player, MOVE_X_SCISSOR); SCORE_EQ(opponent, MOVE_PROTECT, MOVE_TACKLE); }
    }
}

AI_SINGLE_BATTLE_TEST("Player Profile AI: only uses moves the AI has seen")
{
    GIVEN {
        ResetProfileAndPlan();
        SetHabit(PROFILE_SETUP_WHEN_SAFE, 18, 20);
        AI_FLAGS(AI_FLAG_PLAYER_PROFILE); // no omniscience: Dragon Dance hasn't been used yet
        PLAYER(SPECIES_DRAGONITE) { Moves(MOVE_DRAGON_DANCE, MOVE_EXTREME_SPEED); }
        OPPONENT(SPECIES_SABLEYE) { Moves(MOVE_TAUNT, MOVE_SHADOW_BALL); }
    } WHEN {
        TURN { MOVE(player, MOVE_EXTREME_SPEED); SCORE_EQ(opponent, MOVE_TAUNT, MOVE_SHADOW_BALL); }
    }
}

AI_DOUBLE_BATTLE_TEST("Player Profile AI: Wide Guard against a player who prefers spread moves")
{
    bool32 spreads;

    PARAMETRIZE { spreads = TRUE; }
    PARAMETRIZE { spreads = FALSE; }
    GIVEN {
        ResetProfileAndPlan();
        SetHabit(PROFILE_SPREAD_PREFERENCE, spreads ? 18 : 2, 20);
        AI_FLAGS(AI_FLAG_PLAYER_PROFILE | AI_FLAG_MOVE_OMNISCIENCE);
        PLAYER(SPECIES_GARCHOMP) { Moves(MOVE_EARTHQUAKE, MOVE_DRAGON_CLAW); }
        PLAYER(SPECIES_WOBBUFFET) { Moves(MOVE_CELEBRATE); }
        OPPONENT(SPECIES_HITMONTOP) { Moves(MOVE_WIDE_GUARD, MOVE_TACKLE); }
        OPPONENT(SPECIES_WOBBUFFET) { Moves(MOVE_CELEBRATE); }
    } WHEN {
        if (spreads)
            TURN { MOVE(playerLeft, MOVE_DRAGON_CLAW, target: opponentRight); SCORE_GT(opponentLeft, MOVE_WIDE_GUARD, MOVE_TACKLE, target: playerLeft); }
        else
            TURN { MOVE(playerLeft, MOVE_DRAGON_CLAW, target: opponentRight); SCORE_EQ(opponentLeft, MOVE_WIDE_GUARD, MOVE_TACKLE, target: playerLeft); }
    }
}

// Lead change. Tyranitar is the documented lead: the player's usual counters hit it with Fighting
// (x4), Water, Grass, Ground, Bug, Steel or Fairy. Against those types Skarmory takes 3.5 in total,
// Gyarados 4 and Wobbuffet 7.5, so Skarmory is the best replacement.

static void MakeParty(struct Pokemon *party, const enum Species *species, u32 count)
{
    for (u32 i = 0; i < count; i++)
        CreateMon(&party[i], species[i], 50, 0, OTID_STRUCT_PRESET(0));
}

static enum Species PartySpecies(struct Pokemon *party, u32 slot)
{
    return GetMonData(&party[slot], MON_DATA_SPECIES);
}

TEST("Player Profile AI: lead change chance follows how often the player counters leads")
{
    ResetProfileAndPlan();
    EXPECT_EQ(GetProfileLeadChangeChance(), 0); // no data

    SetHabit(PROFILE_LEAD_COUNTERS_ENEMY_LEAD, 12, 20); // 60%: bias 20, not a clear habit
    EXPECT_EQ(GetProfileLeadChangeChance(), 0);

    SetHabit(PROFILE_LEAD_COUNTERS_ENEMY_LEAD, 13, 20); // 65%: bias 30
    EXPECT_EQ(GetProfileLeadChangeChance(), 30);

    SetHabit(PROFILE_LEAD_COUNTERS_ENEMY_LEAD, 20, 20); // always: capped
    EXPECT_EQ(GetProfileLeadChangeChance(), PROFILE_AI_LEAD_CHANCE_MAX);
}

TEST("Player Profile AI: a countered lead is replaced by the mon that takes its counters best")
{
    static const enum Species team[] = {SPECIES_TYRANITAR, SPECIES_GYARADOS, SPECIES_SKARMORY};
    struct Pokemon party[3];

    ResetProfileAndPlan();
    SetHabit(PROFILE_LEAD_COUNTERS_ENEMY_LEAD, 18, 20);
    MakeParty(party, team, 3);
    SET_RNG(RNG_AI_PROFILE_LEAD, TRUE);

    EXPECT(ProfileAI_ChangeLead(party, 3, 0, 1));
    EXPECT_EQ(PartySpecies(party, 0), SPECIES_SKARMORY);
    EXPECT_EQ(PartySpecies(party, 1), SPECIES_GYARADOS);
    EXPECT_EQ(PartySpecies(party, 2), SPECIES_TYRANITAR);
}

TEST("Player Profile AI: the lead stays when the roll fails or the player doesn't counter leads")
{
    static const enum Species team[] = {SPECIES_TYRANITAR, SPECIES_GYARADOS, SPECIES_SKARMORY};
    struct Pokemon party[3];

    ResetProfileAndPlan();
    SetHabit(PROFILE_LEAD_COUNTERS_ENEMY_LEAD, 18, 20);
    MakeParty(party, team, 3);
    SET_RNG(RNG_AI_PROFILE_LEAD, FALSE);
    EXPECT(!ProfileAI_ChangeLead(party, 3, 0, 1));
    EXPECT_EQ(PartySpecies(party, 0), SPECIES_TYRANITAR);

    SetHabit(PROFILE_LEAD_COUNTERS_ENEMY_LEAD, 2, 20); // rarely counters: never changes
    SET_RNG(RNG_AI_PROFILE_LEAD, TRUE);
    EXPECT(!ProfileAI_ChangeLead(party, 3, 0, 1));
    EXPECT_EQ(PartySpecies(party, 0), SPECIES_TYRANITAR);
}

TEST("Player Profile AI: the ace never becomes the lead")
{
    static const enum Species team[] = {SPECIES_TYRANITAR, SPECIES_GYARADOS, SPECIES_SKARMORY};
    struct Pokemon party[3];

    ResetProfileAndPlan();
    SetHabit(PROFILE_LEAD_COUNTERS_ENEMY_LEAD, 18, 20);
    MakeParty(party, team, 3);
    SET_RNG(RNG_AI_PROFILE_LEAD, TRUE);

    EXPECT(ProfileAI_ChangeLead(party, 3, AI_FLAG_ACE_POKEMON, 1)); // Skarmory is the ace
    EXPECT_EQ(PartySpecies(party, 0), SPECIES_GYARADOS);
    EXPECT_EQ(PartySpecies(party, 2), SPECIES_SKARMORY);
}

TEST("Player Profile AI: in doubles the easier-to-counter lead is the one replaced")
{
    static const enum Species team[] = {SPECIES_GYARADOS, SPECIES_TYRANITAR, SPECIES_WOBBUFFET, SPECIES_SKARMORY};
    struct Pokemon party[4];

    ResetProfileAndPlan();
    SetHabit(PROFILE_LEAD_COUNTERS_ENEMY_LEAD, 18, 20);
    MakeParty(party, team, 4);
    SET_RNG(RNG_AI_PROFILE_LEAD, TRUE);

    // Tyranitar has 7 weaknesses, Gyarados 2: Tyranitar's slot changes, Gyarados keeps leading
    EXPECT(ProfileAI_ChangeLead(party, 4, 0, 2));
    EXPECT_EQ(PartySpecies(party, 0), SPECIES_GYARADOS);
    EXPECT_EQ(PartySpecies(party, 1), SPECIES_SKARMORY);
    EXPECT_EQ(PartySpecies(party, 3), SPECIES_TYRANITAR);
}

// League plan (AiDirective): written by the bridge when the player walks into the League.
// With a plan the plan decides which habits to punish; without one the profile rules above run alone.

TEST("Player Profile AI: a plan is found by trainer id, and only once it is written")
{
    struct AiTrainerDirective entry = PlanEntry(PLAN_TEST_TRAINER);
    struct AiDirective unwritten = {0};

    ResetProfileAndPlan();
    EXPECT(Mailbox_GetDirective(PLAN_TEST_TRAINER) == NULL);

    SetPlan(&entry);
    EXPECT(Mailbox_GetDirective(PLAN_TEST_TRAINER) != NULL);
    EXPECT(Mailbox_GetDirective(PLAN_OTHER_TRAINER) == NULL);
    EXPECT(Mailbox_GetDirective(0) == NULL);

    unwritten.count = 1; // entries without the magic don't count
    unwritten.trainers[0] = entry;
    Mailbox_SetDirective(&unwritten);
    EXPECT(Mailbox_GetDirective(PLAN_TEST_TRAINER) == NULL);
}

AI_SINGLE_BATTLE_TEST("Player Profile AI: the plan decides which habits to punish")
{
    bool32 planPunishesSetup, playerSetsUp;

    PARAMETRIZE { planPunishesSetup = TRUE;  playerSetsUp = FALSE; } // the LLM saw something the counters didn't
    PARAMETRIZE { planPunishesSetup = FALSE; playerSetsUp = TRUE; }  // the plan chose to ignore the habit
    GIVEN {
        struct AiTrainerDirective entry = PlanEntry(PLAN_TEST_TRAINER);
        ResetProfileAndPlan();
        SetHabit(PROFILE_SETUP_WHEN_SAFE, playerSetsUp ? 18 : 2, 20);
        entry.punishMask = planPunishesSetup ? DIRECTIVE_PUNISH_SETUP : 0;
        SetPlan(&entry);
        AI_FLAGS(AI_FLAG_PLAYER_PROFILE | AI_FLAG_MOVE_OMNISCIENCE);
        PLAYER(SPECIES_DRAGONITE) { Moves(MOVE_DRAGON_DANCE, MOVE_EXTREME_SPEED); }
        OPPONENT(SPECIES_SABLEYE) { Moves(MOVE_TAUNT, MOVE_SHADOW_BALL); }
    } WHEN {
        if (planPunishesSetup)
            TURN { MOVE(player, MOVE_EXTREME_SPEED); SCORE_GT(opponent, MOVE_TAUNT, MOVE_SHADOW_BALL); }
        else
            TURN { MOVE(player, MOVE_EXTREME_SPEED); SCORE_EQ(opponent, MOVE_TAUNT, MOVE_SHADOW_BALL); }
    }
}

AI_SINGLE_BATTLE_TEST("Player Profile AI: plan aggression and self setup weigh the AI's own moves")
{
    s8 aggression, selfSetup;

    PARAMETRIZE { aggression = 0;  selfSetup = 0; }
    PARAMETRIZE { aggression = 2;  selfSetup = 0; }
    PARAMETRIZE { aggression = 0;  selfSetup = 2; }
    PARAMETRIZE { aggression = -2; selfSetup = 0; }
    GIVEN {
        struct AiTrainerDirective entry = PlanEntry(PLAN_TEST_TRAINER);
        ResetProfileAndPlan();
        entry.aggression = aggression;
        entry.selfSetup = selfSetup;
        SetPlan(&entry);
        AI_FLAGS(AI_FLAG_PLAYER_PROFILE);
        PLAYER(SPECIES_WOBBUFFET) { Moves(MOVE_CELEBRATE); }
        OPPONENT(SPECIES_WOBBUFFET) { Moves(MOVE_TACKLE, MOVE_SWORDS_DANCE, MOVE_CELEBRATE); }
    } WHEN {
        if (aggression > 0)
            TURN { MOVE(player, MOVE_CELEBRATE); SCORE_GT(opponent, MOVE_TACKLE, MOVE_CELEBRATE); }
        else if (aggression < 0)
            TURN { MOVE(player, MOVE_CELEBRATE); SCORE_LT(opponent, MOVE_TACKLE, MOVE_CELEBRATE); }
        else if (selfSetup > 0)
            TURN { MOVE(player, MOVE_CELEBRATE); SCORE_GT(opponent, MOVE_SWORDS_DANCE, MOVE_TACKLE); }
        else
            TURN { MOVE(player, MOVE_CELEBRATE); SCORE_EQ(opponent, MOVE_TACKLE, MOVE_CELEBRATE); SCORE_EQ(opponent, MOVE_SWORDS_DANCE, MOVE_CELEBRATE); }
    }
}

TEST("Player Profile AI: plan leads move to the front and the rest keep their order")
{
    static const enum Species team[] = {SPECIES_TYRANITAR, SPECIES_GYARADOS, SPECIES_SKARMORY, SPECIES_WOBBUFFET};
    struct AiTrainerDirective entry = PlanEntry(PLAN_TEST_TRAINER);
    struct Pokemon party[4];

    MakeParty(party, team, 3);
    entry.leadSlots[0] = 2;
    EXPECT(ProfileAI_ApplyPlanLeads(party, 3, 0, 1, &entry));
    EXPECT_EQ(PartySpecies(party, 0), SPECIES_SKARMORY);
    EXPECT_EQ(PartySpecies(party, 1), SPECIES_TYRANITAR);
    EXPECT_EQ(PartySpecies(party, 2), SPECIES_GYARADOS);

    MakeParty(party, team, 4); // doubles: two leads
    entry.leadSlots[0] = 3;
    entry.leadSlots[1] = 1;
    EXPECT(ProfileAI_ApplyPlanLeads(party, 4, 0, 2, &entry));
    EXPECT_EQ(PartySpecies(party, 0), SPECIES_WOBBUFFET);
    EXPECT_EQ(PartySpecies(party, 1), SPECIES_GYARADOS);
    EXPECT_EQ(PartySpecies(party, 2), SPECIES_TYRANITAR);
    EXPECT_EQ(PartySpecies(party, 3), SPECIES_SKARMORY);
}

TEST("Player Profile AI: invalid plan leads leave the party as documented")
{
    static const enum Species team[] = {SPECIES_TYRANITAR, SPECIES_GYARADOS, SPECIES_SKARMORY};
    struct AiTrainerDirective entry = PlanEntry(PLAN_TEST_TRAINER);
    struct Pokemon party[3];

    MakeParty(party, team, 3);
    EXPECT(!ProfileAI_ApplyPlanLeads(party, 3, 0, 1, NULL));        // no plan
    EXPECT(!ProfileAI_ApplyPlanLeads(party, 3, 0, 1, &entry));      // plan keeps the documented lead
    entry.leadSlots[0] = 2;
    EXPECT(!ProfileAI_ApplyPlanLeads(party, 3, AI_FLAG_ACE_POKEMON, 1, &entry)); // slot 2 is the ace
    entry.leadSlots[0] = 1;
    entry.leadSlots[1] = 1;
    EXPECT(!ProfileAI_ApplyPlanLeads(party, 3, 0, 2, &entry));      // the same mon twice
    EXPECT_EQ(PartySpecies(party, 0), SPECIES_TYRANITAR);
    EXPECT_EQ(PartySpecies(party, 1), SPECIES_GYARADOS);
    EXPECT_EQ(PartySpecies(party, 2), SPECIES_SKARMORY);
}
