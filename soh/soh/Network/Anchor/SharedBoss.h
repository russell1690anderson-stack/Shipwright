#pragma once

// OOT recomp fork: shared boss fights over Anchor.
//
// Every player still runs their own copy of the boss, but its health is shared: whenever
// someone in the boss room damages the boss, their new health value is sent to everyone else
// in that room, and each game lowers its own boss to the lowest health anyone has reported.
// When one player lands the final hit, the boss dies for everyone in the room.

#include <libultraship/libultra/types.h>

struct Actor;
struct PlayState;

// Snapshot of a boss's movement and AI state, sent by the player whose game is "leading" the
// fight so everyone else's copy of the boss moves the same way. `action` is a boss-specific
// index (each boss keeps its own table), `vars` holds boss-specific timers/counters.
#define SHARED_BOSS_VAR_COUNT 8
#define SHARED_BOSS_MAX_MINIONS 12

// Small enemies a boss spawns (e.g. Gohma's babies). `state`: 0 not spawned, 1 alive, -1 dead.
typedef struct SharedBossMinion {
    s16 state;
    s16 action;
    s16 timer;
    f32 pos[3];
    s16 rotY;
    s16 shapeRotY;
    f32 speedXZ;
    f32 velocityY;
} SharedBossMinion;

typedef struct SharedBossState {
    s16 action;
    s16 vars[SHARED_BOSS_VAR_COUNT];
    f32 pos[3];
    s16 rot[3];
    s16 shapeRot[3];
    f32 speedXZ;
    f32 velocityY;
    f32 gravity;
    f32 animFrame;
    s16 minionCount;
    SharedBossMinion minions[SHARED_BOSS_MAX_MINIONS];
} SharedBossState;

#ifdef __cplusplus
extern "C" {
#endif

// Call from the boss's Init. Clears any shared health left over from an earlier fight.
void SharedBoss_Reset(struct Actor* boss);

// Call after the local player damages the boss, with the boss's new health.
void SharedBoss_ReportHealth(struct Actor* boss, s32 health);

// Lowest health any other player in this room has reported for this boss, or -1 if none
// (or if shared boss fights are off / not connected).
s32 SharedBoss_GetSharedHealth(struct Actor* boss);

// --- Movement sync ---
// Every player whose boss is mid-fight calls SharedBoss_SendState each frame. The player with the
// lowest Anchor client ID among them "leads": everyone else copies the leader's state.

// Broadcast this frame's state to the other players in the room.
void SharedBoss_SendState(struct Actor* boss, const SharedBossState* state);

// If another player is leading this fight, copies their latest state into `out` and returns 1.
// Returns 0 if this game is leading (or nobody else is fighting, or sync is off).
s32 SharedBoss_GetLeaderState(struct Actor* boss, SharedBossState* out);

// A local hit changed the boss's state (stunned, knocked off the ceiling, damaged). Sends it to
// everyone so the leader adopts it too.
void SharedBoss_SendEvent(struct Actor* boss, const SharedBossState* state);

// Next hit event received from another player for this boss. Returns 1 and fills `out`, or 0.
s32 SharedBoss_PopEvent(struct Actor* boss, SharedBossState* out);

// The player a boss (or its minion) at `from` should go after: whoever is nearest, counting the
// other online players' Links in this room. Just this player's Link when not playing online.
struct Actor* SharedBoss_GetTargetActor(struct PlayState* play, struct Actor* from);

// How many players are in this fight right now (this player plus everyone sending boss state).
s32 SharedBoss_GetPlayerCount(struct Actor* boss);

// A minion was killed in this player's game; tell everyone so it dies in their games too.
void SharedBoss_SendMinionKilled(struct Actor* boss, s16 index);

// Next minion another player killed, or -1 if none.
s16 SharedBoss_PopMinionKilled(struct Actor* boss);

#ifdef __cplusplus
}

// Called by the Anchor packet handlers.
void SharedBoss_OnRemoteHealth(s16 sceneNum, s16 actorId, s32 health);
void SharedBoss_OnRemoteState(uint32_t clientId, s16 sceneNum, s16 actorId, const SharedBossState& state);
void SharedBoss_OnRemoteEvent(s16 sceneNum, s16 actorId, const SharedBossState& state);
void SharedBoss_OnRemoteMinionKilled(s16 sceneNum, s16 actorId, s16 index);
#endif
