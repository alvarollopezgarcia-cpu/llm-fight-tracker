# Hooks in the game

Every place where the tracker's code touches the hack, extracted by `export_portfolio.py` with a few lines of context. Telemetry and mailbox hooks compile away when `TELEMETRY_ENABLED` is `FALSE` (the release setting).

## `include/global.h`

Lines 26-32:

```c
#include "constants/items.h"
#include "constants/moves.h"
#include "config/save.h"
#include "player_profile.h"

// Prevent cross-jump optimization.
#define BLOCK_CROSS_JUMP asm("");
```

Lines 273-279:

```c
#if APRICORN_TREE_COUNT > 0
    u8 apricornTrees[NUM_APRICORN_TREE_BYTES];
#endif
    struct PlayerBattleProfile playerProfile;
}; /* max size 1624 bytes */

extern struct SaveBlock3 *gSaveBlock3Ptr;
```

## `src/battle_main.c`

Lines 43-49:

```c
#include "pokeball.h"
#include "pokedex.h"
#include "pokemon.h"
#include "telemetry.h"
#include "pokerus.h"
#include "random.h"
#include "recorded_battle.h"
```

Lines 3546-3553:

```c
        BattlePutTextOnWindow(gText_EmptyString3, B_WIN_MSG);
        AssignUsableGimmicks();
        SetShellSideArmCategory();
        PlayerProfile_OnBattleStart();
        TELEMETRY_CALL(Telemetry_LogBattleStart());
        SetAiLogicDataForTurn(gAiLogicData); // get assumed abilities, hold effects, etc of all battlers
        gBattleMainFunc = HandleTurnActionSelectionState;
        ResetSentPokesToOpponentValue();
```

Lines 4633-4640:

```c
    s32 turnOrderId = 0;
    enum BattlerId battler, battler2;

    PlayerProfile_RecordTurn();
    TELEMETRY_CALL(Telemetry_LogTurnActions());

    if (gBattleTypeFlags & BATTLE_TYPE_SAFARI)
    {
```

Lines 5288-5294:

```c
            TryPutBreakingNewsOnAir();
        }

        PlayerProfile_CommitBattle();
        BeginFastPaletteFade(3);
        FadeOutMapMusic(5);
        TryRestoreHeldItems();
```

## `src/battle_move_resolution.c`

Lines 10-16:

```c
#include "battle_stat_change.h"
#include "battle_scripts.h"
#include "battle_z_move.h"
#include "telemetry.h"
#include "battle_anim_scripts.h"
#include "item.h"
#include "battle_controllers.h"
```

Lines 3756-3762:

```c

static enum MoveEndResult MoveEndUpdateLastMoves(struct BattleCalcValues *cv)
{
    TELEMETRY_CALL(Telemetry_LogMoveResult(cv->battlerAtk, cv->battlerDef, cv->move));

    if (!IsOnPlayerSide(cv->battlerAtk))
        UpdateStallMons(cv);
```

## `src/battle_script_commands.c`

Lines 17-23:

```c
#include "item_menu.h"
#include "util.h"
#include "pokemon.h"
#include "telemetry.h"
#include "random.h"
#include "battle_controllers.h"
#include "battle_interface.h"
```

Lines 1909-1915:

```c
                }
            }

            TELEMETRY_LOG(TELEMETRY_FAINT, battler, gBattleMons[battler].species, gBattlerAttacker, gBattleTurnCounter);
            SetValuesOnFaint(battler);
            BattleScriptPush(cmd->nextInstr);
            gBattlescriptCurrInstr = BattleScript_FaintBattler;
```

Lines 2383-2389:

```c
                PREPARE_BYTE_NUMBER_BUFFER(gBattleTextBuff2, 3, GetMonData(&gParties[B_TRAINER_PLAYER][*expMonId], MON_DATA_LEVEL));

                gLeveledUpInBattle |= 1 << *expMonId;
                TELEMETRY_LOG(TELEMETRY_LEVEL_UP, *expMonId,
                              GetMonData(&gParties[B_TRAINER_PLAYER][*expMonId], MON_DATA_SPECIES),
                              GetMonData(&gParties[B_TRAINER_PLAYER][*expMonId], MON_DATA_LEVEL), 0);
                BattleScriptCall(BattleScript_LevelUp);
```

Lines 3304-3310:

```c
    battler = GetBattlerForBattleScript(cmd->battler);

    GetBattlerPartyState(battler)->sentOut = TRUE;
    TELEMETRY_LOG(TELEMETRY_SWITCH_IN, battler,
                  GetMonData(&GetBattlerParty(battler)[gBattlerPartyIndexes[battler]], MON_DATA_SPECIES),
                  gBattlerPartyIndexes[battler], gBattleTurnCounter);

```

Lines 9123-9139:

```c
    gBattlerTarget = gBattleStruct->savedBattlerTarget[gBattleStruct->savedTargetCount];
}

void BS_PlayerProfileFreeSwitchOffered(void)
{
    NATIVE_ARGS();
    PlayerProfile_OnFreeSwitchOffered();
    gBattlescriptCurrInstr = cmd->nextInstr;
}

void BS_PlayerProfileFreeSwitchTaken(void)
{
    NATIVE_ARGS();
    PlayerProfile_OnFreeSwitchTaken();
    gBattlescriptCurrInstr = cmd->nextInstr;
}

```

## `src/battle_util.c`

Lines 16-22:

```c
#include "config_changes.h"
#include "party_menu.h"
#include "pokemon.h"
#include "telemetry.h"
#include "international_string_util.h"
#include "item.h"
#include "util.h"
```

Lines 637-643:

```c

    gLastUsedItem = gBattleResources->bufferB[gBattlerAttacker][1] | (gBattleResources->bufferB[gBattlerAttacker][2] << 8);
    if (IsOnPlayerSide(gBattlerAttacker))
        TELEMETRY_LOG(TELEMETRY_ITEM_IN_BATTLE, gBattlerAttacker, gLastUsedItem, gBattleMons[gBattlerAttacker].species,
                      gBattleTurnCounter);
    if (X_ITEM_FRIENDSHIP_INCREASE > 0
        && GetItemEffectType(gLastUsedItem) == ITEM_EFFECT_X_ITEM
```

## `src/battle_setup.c`

Lines 2-10:

```c
#include "data.h"
#include "main.h"
#include "battle.h"
#include "battle_ai_player_profile.h"
#include "constants/battle_ai.h"
#include "telemetry.h"
#include "battle_frontier.h"
#include "battle_pike.h"
#include "battle_pyramid.h"
```

Lines 2260-2279:

```c
    Free(trainerGen);
}

// AI_FLAG_PLAYER_PROFILE trainers pick their leads: the League plan's choice if the planner wrote
// one for this trainer, else a chance to dodge the counter a player who counters leads will bring.
static void ChooseProfileLeads(struct Pokemon *party, u16 trainerNum)
{
    const struct Trainer *trainer = GetTrainerStructFromId(trainerNum);
    bool32 doubleLeads = trainer->battleType == TRAINER_BATTLE_TYPE_DOUBLES && !(gBattleTypeFlags & BATTLE_TYPE_TWO_OPPONENTS);
    u32 count = 0;

    if (!(trainer->aiFlags & AI_FLAG_PLAYER_PROFILE))
        return;
    while (count < PARTY_SIZE && GetMonData(&party[count], MON_DATA_SPECIES) != SPECIES_NONE)
        count++;
    if (!ProfileAI_ApplyPlanLeads(party, count, trainer->aiFlags, doubleLeads ? 2 : 1, Mailbox_GetDirective(trainerNum)))
        ProfileAI_ChangeLead(party, count, trainer->aiFlags, doubleLeads ? 2 : 1);
}

static void CreateNPCTrainerParty(struct Pokemon *party, u16 trainerNum)
```

Lines 2281-2287:

```c
    if (!GetTrainerStructFromId(trainerNum)->overrideTrainer)
    {
        CreateNPCTrainerPartyFromTrainer(party, GetTrainerStructFromId(trainerNum));
        ChooseProfileLeads(party, trainerNum);
        return;
    }

```

Lines 2295-2301:

```c
    if (tempTrainer.partySize == 0)
        tempTrainer.partySize = origTrainer->partySize;
    CreateNPCTrainerPartyFromTrainer(party, (const struct Trainer *)(&tempTrainer));
    ChooseProfileLeads(party, trainerNum);
}

void CreateTrainerPartyForPlayer(void)
```

## `src/battle_ai_main.c`

Lines 7-13:

```c
#include "battle_ai_items.h"
#include "battle_ai_switch.h"
#include "battle_ai_main.h"
#include "battle_ai_player_profile.h"
#include "battle_ai_record.h"
#include "battle_stat_change.h"
#include "battle_controllers.h"
```

Lines 121-127:

```c
    [34] = NULL,                     // AI_FLAG_ABILITY_OMNISCIENCE
    [35] = NULL,                     // AI_FLAG_ITEM_OMNISCIENCE
    [36] = NULL,                     // AI_FLAG_MOVE_OMNISCIENCE
    [37] = AI_PlayerProfile,         // AI_FLAG_PLAYER_PROFILE
    [38] = NULL,                     // Unused
    [39] = NULL,                     // Unused
    [40] = NULL,                     // Unused
```

Lines 416-422:

```c

    // Determine whether AI will use predictions this turn

    u32 predictSwitchChance = (gAiThinkingStruct->aiFlags[battler] & AI_FLAG_PLAYER_PROFILE) ? GetBattlerPredictSwitchChance(battler) : PREDICT_SWITCH_CHANCE;
    gAiLogicData->predictingSwitch = (gAiThinkingStruct->aiFlags[battler] & AI_FLAG_PREDICT_SWITCH) ? RandomPercentage(RNG_AI_PREDICT_SWITCH, predictSwitchChance) : FALSE;
    gAiLogicData->predictingMove = (gAiThinkingStruct->aiFlags[battler] & AI_FLAG_PREDICT_MOVE) ? RandomPercentage(RNG_AI_PREDICT_MOVE, PREDICT_MOVE_CHANCE) : FALSE;

```

## `include/constants/battle_ai.h`

Lines 44-50:

```c
#define AI_FLAG_ABILITY_OMNISCIENCE         AI_FLAG(34)  // AI knows the player's ability
#define AI_FLAG_ITEM_OMNISCIENCE            AI_FLAG(35)  // AI knows the player's item
#define AI_FLAG_MOVE_OMNISCIENCE            AI_FLAG(36)  // AI knows the player's moves
#define AI_FLAG_PLAYER_PROFILE              AI_FLAG(37)  // AI adapts to the player's habits recorded over the game (src/player_profile.c). Only uses what the AI already knows about the player's moves.

// The following options are enough to have a basic/smart trainer. Any other addtion could make the trainer worse/better depending on the flag
#define AI_FLAG_BASIC_TRAINER         (AI_FLAG_CHECK_BAD_MOVE | AI_FLAG_TRY_TO_FAINT | AI_FLAG_CHECK_VIABILITY)
```

## `include/config/ai.h`

Lines 108-119:

```c
#define PREDICT_SWITCH_CHANCE                                   50
#define PREDICT_MOVE_CHANCE                                     100

// AI_FLAG_PLAYER_PROFILE settings: the AI reacts to habits the game recorded about the player (src/player_profile.c)
#define PROFILE_AI_MIN_SAMPLES                                  12 // A habit is ignored until the player had this many chances to show it.
#define PROFILE_AI_BIAS_THRESHOLD                               30 // Bias (-100..100, 0 = 50% of the time) a habit needs before a rule reacts to it.
#define PROFILE_AI_SWITCH_CHANCE_MIN                            10 // With Predict Switch, the chance to predict a switch follows how often the player
#define PROFILE_AI_SWITCH_CHANCE_MAX                            90 // switches when threatened, kept within these bounds.
#define PROFILE_AI_LEAD_CHANCE_MAX                              75 // Most often a trainer leads with something other than its documented lead
                                                                   // when the player keeps countering it (the chance follows how often they do).

// AI Terastalization chances
```

## `src/overworld.c`

Lines 1-6:

```c
#include "global.h"
#include "overworld.h"
#include "telemetry.h"
#include "battle_pyramid.h"
#include "battle_setup.h"
#include "battle_util.h"
```

Lines 393-399:

```c
// code
void DoWhiteOut(void)
{
    TELEMETRY_LOG(TELEMETRY_WHITEOUT, 0, ((u8)gSaveBlock1Ptr->location.mapGroup << 8) | (u8)gSaveBlock1Ptr->location.mapNum, 0, 0);
    RunScriptImmediately(EventScript_WhiteOut);
    HealPlayerParty();
    Overworld_ResetStateAfterWhiteOut();
```

## `src/party_menu.c`

Lines 49-55:

```c
#include "party_menu.h"
#include "player_pc.h"
#include "pokemon.h"
#include "telemetry.h"
#include "pokemon_icon.h"
#include "pokemon_jump.h"
#include "pokemon_storage_system.h"
```

Lines 5910-5916:

```c
        else
        {
            if (blockedByCap && holdEffectParam == 0)
                TELEMETRY_LOG(TELEMETRY_CAP_BLOCKED, TELEMETRY_SOURCE_BAG, GetMonData(mon, MON_DATA_SPECIES),
                              GetCurrentLevelCap(), 0);
            gPartyMenuUseExitCallback = FALSE;
            DisplayPartyMenuMessage(gText_WontHaveEffect, TRUE);
```

Lines 5928-5934:

```c
        UpdateMonDisplayInfoAfterRareCandy(gPartyMenu.slotId, mon);
        RemoveBagItem(gSpecialVar_ItemId, 1);
        if (holdEffectParam == 0)
            TELEMETRY_LOG(TELEMETRY_RARE_CANDY, TELEMETRY_SOURCE_BAG, GetMonData(mon, MON_DATA_SPECIES), sFinalLevel, 1);
        GetMonNickname(mon, gStringVar1);
        if (sFinalLevel > sInitialLevel)
        {
```

Lines 6250-6256:

```c
          && !LearnsMoveAtLevel(species, level)
          && GetEvolutionTargetSpecies(mon, EVO_MODE_NORMAL, ITEM_NONE, NULL, &canStopEvo, CHECK_EVO) == SPECIES_NONE);
    // One event per stop (a new move or an evolution ends a run), with the candies it spent
    TELEMETRY_LOG(TELEMETRY_RARE_CANDY, TELEMETRY_SOURCE_POKENAV, species, level, level - startLevel);

    // Same as a Rare Candy, Task_TryLearningNextMove and PartyMenuTryEvolution handle the rest
    sInitialLevel = level;
```

Lines 7471-7477:

```c

void ChooseMonToGiveMailFromMailbox(void)
{
    InitPartyMenu(PARTY_MENU_TYPE_FIELD, PARTY_LAYOUT_SINGLE, PARTY_ACTION_GIVE_MAILBOX_MAIL, FALSE, PARTY_MSG_GIVE_TO_WHICH_MON, Task_HandleChooseMonInput, Mailbox_ReturnToMailListAfterDeposit);
}

static void TryGiveMailToSelectedMon(u8 taskId)
```

## `src/start_menu.c`

Lines 27-33:

```c
#include "new_game.h"
#include "option_menu.h"
#include "overworld.h"
#include "telemetry.h"
#include "palette.h"
#include "party_menu.h"
#include "pokedex.h"
```

Lines 103-109:

```c
static bool8 StartMenuPokeNavSubmenuCallback(void);
static bool8 HandlePokeNavSubmenuInput(void);
static bool8 StartMenuPokeNavHealCallback(void);
static bool8 StartMenuPokeNavReportCallback(void);
static bool8 StartMenuPokeNavLevelCallback(void);
static bool8 StartMenuPokeNavLevelNothingToDoCallback(void);
static bool8 StartMenuPokeNavMapCallback(void);
```

Lines 811-817:

```c
    POKENAV_SUBMENU_HEAL,
    POKENAV_SUBMENU_LEVEL,
    POKENAV_SUBMENU_PHONE,
    POKENAV_SUBMENU_REPORT, // only with TELEMETRY_ENABLED: the LLM tracker's report on the player's style
    POKENAV_SUBMENU_COUNT,
};

```

Lines 822-828:

```c
    [POKENAV_SUBMENU_HEAL]  = COMPOUND_STRING("CURAR"),
    [POKENAV_SUBMENU_LEVEL] = COMPOUND_STRING("NIVEL"),
    [POKENAV_SUBMENU_PHONE] = COMPOUND_STRING("TELÉFONO"),
    [POKENAV_SUBMENU_REPORT] = COMPOUND_STRING("INFORME"),
};

EWRAM_DATA static u8 sPokeNavSubmenuWindowId = 0;
```

Lines 855-862:

```c
        sPokeNavSubmenuActions[sPokeNavSubmenuNumActions++] = POKENAV_SUBMENU_HEAL;
    sPokeNavSubmenuActions[sPokeNavSubmenuNumActions++] = POKENAV_SUBMENU_LEVEL;
    sPokeNavSubmenuActions[sPokeNavSubmenuNumActions++] = POKENAV_SUBMENU_PHONE;
    if (TELEMETRY_ENABLED)
        sPokeNavSubmenuActions[sPokeNavSubmenuNumActions++] = POKENAV_SUBMENU_REPORT;
    template.height = sPokeNavSubmenuNumActions * 2;

    sPokeNavSubmenuWindowId = AddWindow(&template);
```

Lines 918-926:

```c
            FadeScreen(FADE_TO_BLACK, 0);
            gMenuCallback = StartMenuPokeNavCallback;
            break;
        case POKENAV_SUBMENU_REPORT:
            RemovePokeNavSubmenuWindow();
            gMenuCallback = StartMenuPokeNavReportCallback;
            break;
        }
    }
```

Lines 979-989:

```c
}

// INFORME: the LLM tracker's latest report on how the player fights, sent through the mailbox
static bool8 StartMenuPokeNavReportCallback(void)
{
    RemoveExtraStartMenuWindows();
    HideStartMenu();
    ScriptContext_SetupScript(EventScript_PokeNavReport);
    return TRUE;
}

```

Lines 999-1005:

```c
    }
    else
    {
        TELEMETRY_CALL(Telemetry_LogHeal(TELEMETRY_SOURCE_POKENAV));
        FlagSet(FLAG_POKENAV_HEAL_USED);
        ScriptContext_SetupScript(EventScript_PokeNavHeal);
    }
```

## `data/battle_scripts_1.s`

Lines 2758-2764:

```asm
	jumpifbyte CMP_EQUAL, sBATTLE_STYLE, OPTIONS_BATTLE_STYLE_SET, BattleScript_FaintedMonSendOutNew
	jumpifcantswitch BS_PLAYER1, BattleScript_FaintedMonSendOutNew
	setbyte sILLUSION_NICK_HACK, 1
	callnative BS_PlayerProfileFreeSwitchOffered
@ Yes/No for sending out a new Pokémon when the opponent is switching
	printstring STRINGID_ENEMYABOUTTOSWITCHPKMN
	setbyte gBattleCommunication, 0
```

Lines 2789-2795:

```asm
	switchinanim BS_ATTACKER, FALSE, FALSE
	waitstate
	setbyte sSHIFT_SWITCHED, 1
	callnative BS_PlayerProfileFreeSwitchTaken
BattleScript_FaintedMonSendOutNew:
	drawpartystatussummary BS_FAINTED
	getswitchedmondata BS_FAINTED
```

## `data/scripts/pkmn_center_nurse.inc`

Lines 47-53:

```asm
	waitfieldeffect FLDEFF_POKECENTER_HEAL
	applymovement VAR_0x800B, Common_Movement_WalkInPlaceFasterDown
	waitmovement 0
	callnative Telemetry_LogCenterHeal
	special HealPlayerParty
	clearflag FLAG_POKENAV_HEAL_USED
	callnative UpdateFollowingPokemon
```

## `data/scripts/flash.inc`

Lines 24-34:

```asm
	releaseall
	end

@ POKéNAV INFORME (start menu > POKéNAV > INFORME, only with TELEMETRY_ENABLED):
@ the LLM tracker's report, written to the mailbox by tools/llm_tracker
EventScript_PokeNavReport::
	lockall
	callnative Mailbox_LoadReport
	msgbox gStringVar4, MSGBOX_DEFAULT
	releaseall
	end
```

## `data/maps/EverGrandeCity_ChampionsRoom/scripts.pory`

Lines 50-58:

```asm
EverGrandeCity_ChampionsRoom_EventScript_Wallace::
	playbgm MUS_ENCOUNTER_CHAMPION, FALSE
	call EverGrandeCity_ChampionsRoom_EventScript_MsgIntroSpeech
	@ Development builds (TELEMETRY_ENABLED): a line about the player's style, sent by the LLM tracker
	callnative Mailbox_LoadChampionLine
	call_if_eq VAR_RESULT, TRUE, EverGrandeCity_ChampionsRoom_EventScript_MsgMailboxLine
	trainerbattle_no_intro TRAINER_WALLACE, EverGrandeCity_ChampionsRoom_Text_Defeat
	goto EverGrandeCity_ChampionsRoom_EventScript_Defeated
	end
```

Lines 484-490:

```asm
	msgbox EverGrandeCity_ChampionsRoom_Text_IntroSpeech_Female, MSGBOX_DEFAULT
	return

EverGrandeCity_ChampionsRoom_EventScript_MsgMailboxLine:
	msgbox gStringVar4, MSGBOX_DEFAULT
	return

```
