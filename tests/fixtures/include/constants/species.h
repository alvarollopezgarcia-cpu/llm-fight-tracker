#ifndef GUARD_CONSTANTS_SPECIES_H
#define GUARD_CONSTANTS_SPECIES_H

// NOTE: The defines here are named in order to match Pokémon Showdown's naming conventions. This is done to avoid issues with trainerproc parsing species names. If a Pokémon does not have an entry on Showdown, use your best judgement.
// Additionally, we have tried to reduce down to as few defines as possible, because programs like porymap do not always properly handle having multiple defines.
// Discussion: https://github.com/rh-hideout/pokeemerald-expansion/pull/5075#issuecomment-2384088602
// Showdown: https://github.com/smogon/Pokemon-Showdown/blob/master/data/pokedex.ts

enum __attribute__((packed)) Species
{
    SPECIES_NONE = 0,
    SPECIES_BULBASAUR = 1,
    SPECIES_IVYSAUR = 2,
};
