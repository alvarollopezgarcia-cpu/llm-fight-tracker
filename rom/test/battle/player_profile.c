#include "global.h"
#include "test/battle.h"
#include "event_data.h"

#define PROFILE_STAT(stat) (GetPlayerProfile()->stats[stat])

SINGLE_BATTLE_TEST("Player Profile: switching out of a fast KO is recorded for non-ace mons")
{
    bool32 switchOut;

    PARAMETRIZE { switchOut = TRUE; }
    PARAMETRIZE { switchOut = FALSE; }
    GIVEN {
        ResetPlayerProfile();
        FlagClear(FLAG_SYS_GAME_CLEAR);
        PLAYER(SPECIES_WOBBUFFET) { HP(1); Speed(1); Moves(MOVE_SCRATCH); }
        PLAYER(SPECIES_MEWTWO) { Speed(1); Moves(MOVE_SCRATCH); } // highest base stats, so it's the ace
        OPPONENT(SPECIES_WOBBUFFET) { Speed(100); Moves(MOVE_SCRATCH); }
    } WHEN {
        if (switchOut)
            TURN { SWITCH(player, 1); MOVE(opponent, MOVE_SCRATCH); }
        else
            TURN { MOVE(player, MOVE_SCRATCH); MOVE(opponent, MOVE_SCRATCH); SEND_OUT(player, 1); }
    } THEN {
        EXPECT_EQ(GetPlayerProfile()->battlesRecorded, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SWITCH_T3_OTHER).chances, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SWITCH_T3_OTHER).hits, switchOut ? 1 : 0);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SWITCH_T3_ACE).chances, 0);
    }
}

SINGLE_BATTLE_TEST("Player Profile: setting up into a faster KO counts as risky and punished")
{
    GIVEN {
        ResetPlayerProfile();
        FlagClear(FLAG_SYS_GAME_CLEAR);
        PLAYER(SPECIES_WOBBUFFET) { HP(1); Speed(1); Moves(MOVE_SWORDS_DANCE, MOVE_SCRATCH); }
        OPPONENT(SPECIES_WOBBUFFET) { Speed(100); Moves(MOVE_SCRATCH); }
    } WHEN {
        TURN { MOVE(player, MOVE_SWORDS_DANCE); MOVE(opponent, MOVE_SCRATCH); }
    } THEN {
        EXPECT_EQ(PROFILE_STAT(PROFILE_SETUP_ENTRY_RISKY).chances, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SETUP_ENTRY_RISKY).hits, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SETUP_ENTRY_SOUND).chances, 0);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SETUP_PUNISHED).hits, 1);
    }
}

SINGLE_BATTLE_TEST("Player Profile: setting up when the foe can't threaten counts as sound")
{
    GIVEN {
        ResetPlayerProfile();
        FlagClear(FLAG_SYS_GAME_CLEAR);
        PLAYER(SPECIES_WOBBUFFET) { Moves(MOVE_SWORDS_DANCE, MOVE_SCRATCH); }
        OPPONENT(SPECIES_WOBBUFFET) { Moves(MOVE_CELEBRATE); }
    } WHEN {
        TURN { MOVE(player, MOVE_SWORDS_DANCE); MOVE(opponent, MOVE_CELEBRATE); }
        TURN { MOVE(player, MOVE_SCRATCH); MOVE(opponent, MOVE_CELEBRATE); }
        TURN { MOVE(player, MOVE_SCRATCH); MOVE(opponent, MOVE_CELEBRATE); }
    } THEN {
        EXPECT_EQ(PROFILE_STAT(PROFILE_SETUP_ENTRY_SOUND).chances, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SETUP_ENTRY_SOUND).hits, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SETUP_WHEN_SAFE).chances, 2);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SETUP_WHEN_SAFE).hits, 0);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SETUP_PUNISHED).chances, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SETUP_PUNISHED).hits, 0);
    }
}

SINGLE_BATTLE_TEST("Player Profile: Protect on the first action counts as scouting")
{
    GIVEN {
        ResetPlayerProfile();
        FlagClear(FLAG_SYS_GAME_CLEAR);
        PLAYER(SPECIES_WOBBUFFET) { Moves(MOVE_PROTECT, MOVE_SCRATCH); }
        OPPONENT(SPECIES_WOBBUFFET) { Moves(MOVE_SCRATCH); }
    } WHEN {
        TURN { MOVE(player, MOVE_PROTECT); MOVE(opponent, MOVE_SCRATCH); }
        TURN { MOVE(player, MOVE_PROTECT); MOVE(opponent, MOVE_SCRATCH); }
    } THEN {
        EXPECT_EQ(PROFILE_STAT(PROFILE_PROTECT_SCOUT_SINGLES).chances, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_PROTECT_SCOUT_SINGLES).hits, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_PROTECT_SCOUT_DOUBLES).chances, 0);
        EXPECT_EQ(PROFILE_STAT(PROFILE_PROTECT_REPEAT).chances, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_PROTECT_REPEAT).hits, 1);
    }
}

SINGLE_BATTLE_TEST("Player Profile: a status move into an immune target counts as a bad status")
{
    GIVEN {
        ResetPlayerProfile();
        FlagClear(FLAG_SYS_GAME_CLEAR);
        PLAYER(SPECIES_WOBBUFFET) { Moves(MOVE_THUNDER_WAVE); }
        OPPONENT(SPECIES_SANDSHREW) { Moves(MOVE_CELEBRATE); }
    } WHEN {
        TURN { MOVE(player, MOVE_THUNDER_WAVE); MOVE(opponent, MOVE_CELEBRATE); }
    } THEN {
        EXPECT_EQ(PROFILE_STAT(PROFILE_STATUS_INTO_BAD).chances, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_STATUS_INTO_BAD).hits, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_STATUS_OPEN_VALID).chances, 0);
    }
}

SINGLE_BATTLE_TEST("Player Profile: battles after the game is cleared are not recorded")
{
    GIVEN {
        ResetPlayerProfile();
        FlagSet(FLAG_SYS_GAME_CLEAR);
        PLAYER(SPECIES_WOBBUFFET) { Moves(MOVE_SCRATCH); }
        OPPONENT(SPECIES_WOBBUFFET) { Moves(MOVE_SCRATCH); }
    } WHEN {
        TURN { MOVE(player, MOVE_SCRATCH); MOVE(opponent, MOVE_SCRATCH); }
    } THEN {
        EXPECT_EQ(GetPlayerProfile()->battlesRecorded, 0);
        EXPECT_EQ(PROFILE_STAT(PROFILE_LEAD_COUNTERS_ENEMY_LEAD).chances, 0);
        FlagClear(FLAG_SYS_GAME_CLEAR);
    }
}

DOUBLE_BATTLE_TEST("Player Profile: Earthquake with a Protecting ally counts as the spread combo")
{
    GIVEN {
        ResetPlayerProfile();
        FlagClear(FLAG_SYS_GAME_CLEAR);
        PLAYER(SPECIES_EXCADRILL) { Moves(MOVE_EARTHQUAKE); }
        PLAYER(SPECIES_TYRANITAR) { Moves(MOVE_PROTECT, MOVE_CRUNCH); }
        OPPONENT(SPECIES_WOBBUFFET) { Moves(MOVE_CELEBRATE); }
        OPPONENT(SPECIES_WOBBUFFET) { Moves(MOVE_CELEBRATE); }
    } WHEN {
        TURN { MOVE(playerLeft, MOVE_EARTHQUAKE); MOVE(playerRight, MOVE_PROTECT); MOVE(opponentLeft, MOVE_CELEBRATE); MOVE(opponentRight, MOVE_CELEBRATE); }
    } THEN {
        EXPECT_EQ(PROFILE_STAT(PROFILE_PROTECT_SPREAD_COMBO).chances, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_PROTECT_SPREAD_COMBO).hits, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_PROTECT_SCOUT_DOUBLES).hits, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_SPREAD_PREFERENCE).hits, 1);
    }
}

DOUBLE_BATTLE_TEST("Player Profile: both player mons hitting the same foe counts as focus fire")
{
    GIVEN {
        ASSUME(GetMoveEffect(MOVE_SONIC_BOOM) == EFFECT_FIXED_HP_DAMAGE);
        ResetPlayerProfile();
        FlagClear(FLAG_SYS_GAME_CLEAR);
        PLAYER(SPECIES_WOBBUFFET) { Moves(MOVE_SONIC_BOOM); }
        PLAYER(SPECIES_WOBBUFFET) { Moves(MOVE_SONIC_BOOM); }
        OPPONENT(SPECIES_WOBBUFFET) { HP(30); Moves(MOVE_CELEBRATE); }
        OPPONENT(SPECIES_WOBBUFFET) { Moves(MOVE_CELEBRATE); }
    } WHEN {
        TURN { MOVE(playerLeft, MOVE_SONIC_BOOM, target: opponentLeft); MOVE(playerRight, MOVE_SONIC_BOOM, target: opponentLeft); MOVE(opponentLeft, MOVE_CELEBRATE); MOVE(opponentRight, MOVE_CELEBRATE); }
    } THEN {
        EXPECT_EQ(PROFILE_STAT(PROFILE_FOCUS_NEEDED).chances, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_FOCUS_NEEDED).hits, 1);
        EXPECT_EQ(PROFILE_STAT(PROFILE_FOCUS_OVERKILL).chances, 0);
    }
}
