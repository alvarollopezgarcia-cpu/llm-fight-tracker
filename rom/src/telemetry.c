#include "global.h"
#include "battle.h"
#include "battle_util.h"
#include "event_data.h"
#include "pokemon.h"
#include "string_util.h"
#include "telemetry.h"
#include "constants/characters.h"

// See include/telemetry.h. Everything here is compiled out when TELEMETRY_ENABLED is FALSE.

static const u8 sText_NoReport[] = _("No hay ningún informe todavía.\nConecta el rastreador del PC.");

#if TELEMETRY_ENABLED

EWRAM_DATA struct TelemetryBuffer gTelemetry = {0};
EWRAM_DATA struct Mailbox gMailbox = {0};

STATIC_ASSERT(sizeof(struct TelemetryEvent) == 8, TelemetryEventSize);
STATIC_ASSERT(sizeof(struct Mailbox) == 4 + MAILBOX_REPORT_SIZE + MAILBOX_LINE_SIZE + sizeof(struct AiDirective), MailboxLayout);
STATIC_ASSERT(MAILBOX_REPORT_SIZE <= sizeof(gStringVar4), MailboxReportFitsStringVar4);
STATIC_ASSERT(MAILBOX_LINE_SIZE <= sizeof(gStringVar4), MailboxLineFitsStringVar4);

// The reader writes the texts and the magic between frames, so a text is complete when magic is set.
static bool32 Mailbox_Has(u8 *text, u32 size)
{
    if (gMailbox.magic != MAILBOX_MAGIC)
        return FALSE;
    text[size - 1] = EOS; // never trust the terminator
    return text[0] != EOS;
}

void Mailbox_LoadReport(void)
{
    if (Mailbox_Has(gMailbox.report, MAILBOX_REPORT_SIZE))
        StringCopy(gStringVar4, gMailbox.report);
    else
        StringCopy(gStringVar4, sText_NoReport);
}

void Mailbox_LoadChampionLine(void)
{
    gSpecialVar_Result = Mailbox_Has(gMailbox.championLine, MAILBOX_LINE_SIZE);
    if (gSpecialVar_Result)
        StringCopy(gStringVar4, gMailbox.championLine);
}
STATIC_ASSERT((TELEMETRY_CAPACITY & (TELEMETRY_CAPACITY - 1)) == 0, TelemetryCapacityPowerOfTwo);

void Telemetry_Log(u32 type, u32 arg, u32 a, u32 b, u32 c)
{
    struct TelemetryEvent *event = &gTelemetry.events[gTelemetry.head % TELEMETRY_CAPACITY];

    event->type = type;
    event->arg = arg;
    event->a = a;
    event->b = b;
    event->c = c;
    gTelemetry.magic = TELEMETRY_MAGIC;
    gTelemetry.head++; // written last: the reader only trusts entries below head
}

static u32 GetPartySpecies(u32 battler, u32 partyIndex)
{
    if (partyIndex >= PARTY_SIZE)
        return SPECIES_NONE;
    return GetMonData(&GetBattlerParty(battler)[partyIndex], MON_DATA_SPECIES);
}

// The leads never go through switchinanim, so log every battler on the field once.
void Telemetry_LogBattleStart(void)
{
    for (enum BattlerId battler = 0; battler < gBattlersCount; battler++)
    {
        if (!IsBattlerAlive(battler))
            continue;
        Telemetry_Log(TELEMETRY_SWITCH_IN, battler, gBattleMons[battler].species, gBattlerPartyIndexes[battler],
                      gBattleTurnCounter);
    }
}

// The player's choices for the turn, once they're all locked in.
void Telemetry_LogTurnActions(void)
{
    for (enum BattlerId battler = 0; battler < gBattlersCount; battler++)
    {
        u32 action = gChosenActionByBattler[battler];
        u32 detail = 0;

        if (!IsOnPlayerSide(battler) || !IsBattlerAlive(battler))
            continue;
        if (action == B_ACTION_USE_MOVE)
            detail = gChosenMoveByBattler[battler];
        else if (action == B_ACTION_SWITCH)
            detail = GetPartySpecies(battler, gBattleStruct->monToSwitchIntoId[battler]);
        else if (action != B_ACTION_USE_ITEM && action != B_ACTION_RUN)
            continue;
        Telemetry_Log(TELEMETRY_ACTION, battler, action, detail, gBattleTurnCounter);
    }
}

static u32 GetMoveResult(u32 target)
{
    u32 flags = gBattleStruct->moveResultFlags[target];

    if (gBattleStruct->unableToUseMove)
        return TELEMETRY_RESULT_CANT_MOVE;
    if (flags & MOVE_RESULT_PROTECTED)
        return TELEMETRY_RESULT_PROTECTED;
    if (flags & MOVE_RESULT_MISSED)
        return TELEMETRY_RESULT_MISSED;
    if (flags & (MOVE_RESULT_DOESNT_AFFECT_FOE | MOVE_RESULT_ONE_HIT_KO_NO_AFFECT))
        return TELEMETRY_RESULT_NO_EFFECT;
    if (flags & MOVE_RESULT_FAILED)
        return TELEMETRY_RESULT_FAILED;
    if (flags & (MOVE_RESULT_SUPER_EFFECTIVE | MOVE_RESULT_EXTREMELY_EFFECTIVE))
        return TELEMETRY_RESULT_SUPER_EFFECTIVE;
    if (flags & (MOVE_RESULT_NOT_VERY_EFFECTIVE | MOVE_RESULT_MOSTLY_INEFFECTIVE))
        return TELEMETRY_RESULT_NOT_VERY_EFFECTIVE;
    return TELEMETRY_RESULT_HIT;
}

// Once per target (and per strike of a multi-hit move), for both sides.
void Telemetry_LogMoveResult(u32 attacker, u32 target, u32 move)
{
    u32 result = GetMoveResult(target);

    if (gSpecialStatuses[target].criticalHit && result != TELEMETRY_RESULT_CANT_MOVE)
        result |= TELEMETRY_RESULT_CRIT;
    Telemetry_Log(TELEMETRY_MOVE_RESULT, attacker | (target << 4), move, result, gBattleTurnCounter);
}

// Logged just before the party is healed, with how hurt it was.
void Telemetry_LogHeal(u32 source)
{
    u32 hp = 0, maxHp = 0, fainted = 0;

    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gParties[B_TRAINER_PLAYER][i];

        if (!GetMonData(mon, MON_DATA_SANITY_HAS_SPECIES) || GetMonData(mon, MON_DATA_SANITY_IS_EGG))
            continue;
        hp += GetMonData(mon, MON_DATA_HP);
        maxHp += GetMonData(mon, MON_DATA_MAX_HP);
        if (GetMonData(mon, MON_DATA_HP) == 0)
            fainted++;
    }
    Telemetry_Log(TELEMETRY_HEAL, source,
                  ((u8)gSaveBlock1Ptr->location.mapGroup << 8) | (u8)gSaveBlock1Ptr->location.mapNum,
                  maxHp ? hp * 100 / maxHp : 100, fainted);
}

void Telemetry_LogCenterHeal(void)
{
    Telemetry_LogHeal(TELEMETRY_SOURCE_CENTER);
}

#else

void Telemetry_Log(u32 type, u32 arg, u32 a, u32 b, u32 c)
{
}

void Telemetry_LogBattleStart(void)
{
}

void Telemetry_LogTurnActions(void)
{
}

void Telemetry_LogMoveResult(u32 attacker, u32 target, u32 move)
{
}

void Telemetry_LogHeal(u32 source)
{
}

void Telemetry_LogCenterHeal(void)
{
}

void Mailbox_LoadReport(void)
{
    StringCopy(gStringVar4, sText_NoReport);
}

void Mailbox_LoadChampionLine(void)
{
    gSpecialVar_Result = FALSE;
}

#endif // TELEMETRY_ENABLED

// ---------------------------------------------------------------------------
// League plan (AI directive)
// ---------------------------------------------------------------------------
//
// Written by the tracker into gMailbox.directive. Test builds have no telemetry, so they keep a
// copy of their own that tests fill with Mailbox_SetDirective.

STATIC_ASSERT(sizeof(struct AiTrainerDirective) == 12, AiTrainerDirectiveSize);
STATIC_ASSERT(sizeof(struct AiDirective) == 8 + 12 * AI_DIRECTIVE_MAX_TRAINERS, AiDirectiveSize);

#if TELEMETRY_ENABLED
#define sDirective (gMailbox.directive)
static bool32 IsDirectiveWritten(void)
{
    return gMailbox.magic == MAILBOX_MAGIC && gMailbox.directive.magic == AI_DIRECTIVE_MAGIC;
}
#elif TESTING
EWRAM_DATA static struct AiDirective sTestDirective = {0};
#define sDirective sTestDirective
static bool32 IsDirectiveWritten(void)
{
    return sTestDirective.magic == AI_DIRECTIVE_MAGIC;
}
#endif

const struct AiTrainerDirective *Mailbox_GetDirective(u16 trainerId)
{
#if TELEMETRY_ENABLED || TESTING
    if (trainerId == 0 || !IsDirectiveWritten())
        return NULL;
    for (u32 i = 0; i < sDirective.count && i < AI_DIRECTIVE_MAX_TRAINERS; i++)
    {
        if (sDirective.trainers[i].trainerId == trainerId)
            return &sDirective.trainers[i];
    }
#endif
    return NULL;
}

void Mailbox_SetDirective(const struct AiDirective *directive)
{
#if TELEMETRY_ENABLED
    gMailbox.directive = *directive;
    gMailbox.magic = MAILBOX_MAGIC;
#elif TESTING
    sTestDirective = *directive;
#endif
}
