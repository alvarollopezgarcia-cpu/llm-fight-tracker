#ifndef GUARD_BATTLE_AI_PLAYER_PROFILE_H
#define GUARD_BATTLE_AI_PLAYER_PROFILE_H

// AI_FLAG_PLAYER_PROFILE: the AI reacts to habits the game recorded about the player over the
// whole run (src/player_profile.c). It never learns anything the AI doesn't already know: rules
// that depend on the player's moves only look at the moves the AI has seen (GetMovesArray).

s32 AI_PlayerProfile(enum BattlerId battlerAtk, enum BattlerId battlerDef, enum Move move, s32 score);
u32 GetProfilePredictSwitchChance(void);
u32 GetBattlerPredictSwitchChance(enum BattlerId battler);
u32 GetProfileLeadChangeChance(void);
bool32 ProfileAI_ChangeLead(struct Pokemon *party, u32 count, u64 aiFlags, u32 leadCount);

struct AiTrainerDirective;
bool32 ProfileAI_ApplyPlanLeads(struct Pokemon *party, u32 count, u64 aiFlags, u32 leadCount,
                                const struct AiTrainerDirective *plan);

#endif // GUARD_BATTLE_AI_PLAYER_PROFILE_H
