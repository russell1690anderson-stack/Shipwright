#include "SharedBoss.h"
#include "soh/Network/Anchor/Anchor.h"

#include <map>
#include <queue>

extern "C" {
#include "z64.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
}

static s16 sSharedSceneNum = -1;
static s16 sSharedActorId = -1;
static s32 sSharedHealth = -1;

static void SharedBoss_ClearSync();

static bool SharedBoss_IsActive() {
    return Anchor::Instance != nullptr && Anchor::Instance->isEnabled && Anchor::Instance->isConnected &&
           CVarGetInteger(CVAR_REMOTE_ANCHOR("SharedBossFights"), 1) != 0;
}

extern "C" void SharedBoss_Reset(Actor* boss) {
    sSharedSceneNum = (gPlayState != nullptr) ? gPlayState->sceneNum : -1;
    sSharedActorId = boss->id;
    sSharedHealth = -1;
    SharedBoss_ClearSync();
}

extern "C" void SharedBoss_ReportHealth(Actor* boss, s32 health) {
    if (!SharedBoss_IsActive() || gPlayState == nullptr) {
        return;
    }
    if (health < 0) {
        health = 0;
    }
    Anchor::Instance->SendPacket_BossHealth(boss->id, health);
}

extern "C" s32 SharedBoss_GetSharedHealth(Actor* boss) {
    if (!SharedBoss_IsActive() || gPlayState == nullptr) {
        return -1;
    }
    if (sSharedSceneNum != gPlayState->sceneNum || sSharedActorId != boss->id) {
        return -1;
    }
    return sSharedHealth;
}

void SharedBoss_OnRemoteHealth(s16 sceneNum, s16 actorId, s32 health) {
    if (gPlayState == nullptr || gPlayState->sceneNum != sceneNum) {
        return;
    }
    if (sSharedSceneNum != sceneNum || sSharedActorId != actorId) {
        sSharedSceneNum = sceneNum;
        sSharedActorId = actorId;
        sSharedHealth = -1;
    }
    if (sSharedHealth < 0 || health < sSharedHealth) {
        sSharedHealth = health;
    }
}

// --- Movement sync ---

// How many frames a leader can go quiet before someone else takes over (20 frames = 1 second)
#define LEADER_TIMEOUT_FRAMES 20
// After this game sends a hit event, ignore the leader for a moment so their older state
// (from before they heard about the hit) doesn't undo it.
#define EVENT_GRACE_FRAMES 10

struct RemoteBossState {
    s16 sceneNum;
    s16 actorId;
    SharedBossState state;
    u32 frameReceived;
};

static std::map<uint32_t, RemoteBossState> sRemoteStates;
static std::queue<SharedBossState> sPendingEvents;
static s16 sEventSceneNum = -1;
static s16 sEventActorId = -1;
static u32 sGraceUntilFrame = 0;

static u32 SharedBoss_Frame() {
    return gPlayState != nullptr ? (u32)gPlayState->state.frames : 0;
}

extern "C" void SharedBoss_SendState(Actor* boss, const SharedBossState* state) {
    if (!SharedBoss_IsActive() || gPlayState == nullptr) {
        return;
    }
    Anchor::Instance->SendPacket_BossState(boss->id, *state, false);
}

extern "C" s32 SharedBoss_GetLeaderState(Actor* boss, SharedBossState* out) {
    if (!SharedBoss_IsActive() || gPlayState == nullptr) {
        return 0;
    }

    u32 now = SharedBoss_Frame();
    if (now < sGraceUntilFrame) {
        return 0;
    }

    uint32_t ownId = Anchor::Instance->ownClientId;
    const RemoteBossState* leader = nullptr;
    uint32_t leaderId = ownId;

    for (auto it = sRemoteStates.begin(); it != sRemoteStates.end();) {
        const RemoteBossState& remote = it->second;
        bool stale = (now - remote.frameReceived) > LEADER_TIMEOUT_FRAMES || remote.frameReceived > now;
        bool otherFight = remote.sceneNum != gPlayState->sceneNum || remote.actorId != boss->id;
        bool gone = !Anchor::Instance->clients.contains(it->first) || !Anchor::Instance->clients[it->first].online;
        if (stale || otherFight || gone) {
            it = sRemoteStates.erase(it);
            continue;
        }
        if (it->first < leaderId) {
            leaderId = it->first;
            leader = &remote;
        }
        ++it;
    }

    if (leader == nullptr) {
        return 0;
    }
    *out = leader->state;
    return 1;
}

extern "C" void SharedBoss_SendEvent(Actor* boss, const SharedBossState* state) {
    if (!SharedBoss_IsActive() || gPlayState == nullptr) {
        return;
    }
    sGraceUntilFrame = SharedBoss_Frame() + EVENT_GRACE_FRAMES;
    Anchor::Instance->SendPacket_BossState(boss->id, *state, true);
}

extern "C" s32 SharedBoss_PopEvent(Actor* boss, SharedBossState* out) {
    if (gPlayState == nullptr || sPendingEvents.empty()) {
        return 0;
    }
    if (sEventSceneNum != gPlayState->sceneNum || sEventActorId != boss->id) {
        sPendingEvents = {};
        return 0;
    }
    *out = sPendingEvents.front();
    sPendingEvents.pop();
    return 1;
}

void SharedBoss_OnRemoteState(uint32_t clientId, s16 sceneNum, s16 actorId, const SharedBossState& state) {
    if (gPlayState == nullptr || gPlayState->sceneNum != sceneNum) {
        return;
    }
    sRemoteStates[clientId] = { sceneNum, actorId, state, SharedBoss_Frame() };
}

void SharedBoss_OnRemoteEvent(s16 sceneNum, s16 actorId, const SharedBossState& state) {
    if (gPlayState == nullptr || gPlayState->sceneNum != sceneNum) {
        return;
    }
    if (sEventSceneNum != sceneNum || sEventActorId != actorId) {
        sPendingEvents = {};
        sEventSceneNum = sceneNum;
        sEventActorId = actorId;
    }
    // Keep only a few; old hits are meaningless once newer ones arrive
    if (sPendingEvents.size() < 4) {
        sPendingEvents.push(state);
    }
}

static std::queue<s16> sPendingMinionKills;
static s16 sMinionSceneNum = -1;
static s16 sMinionActorId = -1;

extern "C" s32 SharedBoss_GetPlayerCount(Actor* boss) {
    if (!SharedBoss_IsActive() || gPlayState == nullptr) {
        return 1;
    }
    u32 now = SharedBoss_Frame();
    s32 count = 1;
    for (auto& [clientId, remote] : sRemoteStates) {
        if (remote.sceneNum == gPlayState->sceneNum && remote.actorId == boss->id && remote.frameReceived <= now &&
            (now - remote.frameReceived) <= LEADER_TIMEOUT_FRAMES && Anchor::Instance->clients.contains(clientId) &&
            Anchor::Instance->clients[clientId].online) {
            count++;
        }
    }
    return count;
}

extern "C" void SharedBoss_SendMinionKilled(Actor* boss, s16 index) {
    if (!SharedBoss_IsActive() || gPlayState == nullptr) {
        return;
    }
    Anchor::Instance->SendPacket_BossMinionKilled(boss->id, index);
}

extern "C" s16 SharedBoss_PopMinionKilled(Actor* boss) {
    if (gPlayState == nullptr || sPendingMinionKills.empty()) {
        return -1;
    }
    if (sMinionSceneNum != gPlayState->sceneNum || sMinionActorId != boss->id) {
        sPendingMinionKills = {};
        return -1;
    }
    s16 index = sPendingMinionKills.front();
    sPendingMinionKills.pop();
    return index;
}

void SharedBoss_OnRemoteMinionKilled(s16 sceneNum, s16 actorId, s16 index) {
    if (gPlayState == nullptr || gPlayState->sceneNum != sceneNum || index < 0 || index >= SHARED_BOSS_MAX_MINIONS) {
        return;
    }
    if (sMinionSceneNum != sceneNum || sMinionActorId != actorId) {
        sPendingMinionKills = {};
        sMinionSceneNum = sceneNum;
        sMinionActorId = actorId;
    }
    if (sPendingMinionKills.size() < SHARED_BOSS_MAX_MINIONS) {
        sPendingMinionKills.push(index);
    }
}

static void SharedBoss_ClearSync() {
    sRemoteStates.clear();
    sPendingEvents = {};
    sPendingMinionKills = {};
    sGraceUntilFrame = 0;
}

// Another player has to be at least this much closer than the current choice to take over, so
// the boss doesn't keep flipping between two players standing about the same distance away.
#define TARGET_SWITCH_MARGIN 40.0f

extern "C" Actor* SharedBoss_GetTargetActor(PlayState* play, Actor* from) {
    Actor* localPlayer = &GET_PLAYER(play)->actor;

    if (!SharedBoss_IsActive() || play == nullptr) {
        return localPlayer;
    }

    Actor* best = localPlayer;
    f32 bestDist = Actor_WorldDistXYZToActor(from, localPlayer) - TARGET_SWITCH_MARGIN;

    for (auto& [clientId, client] : Anchor::Instance->clients) {
        if (client.self || !client.online || !client.isSaveLoaded || client.player == nullptr ||
            client.sceneNum != play->sceneNum) {
            continue;
        }
        Player* other = client.player;
        if (other->actor.update == nullptr || (other->stateFlags1 & PLAYER_STATE1_DEAD)) {
            continue;
        }
        f32 dist = Actor_WorldDistXYZToActor(from, &other->actor);
        if (dist < bestDist) {
            best = &other->actor;
            bestDist = dist;
        }
    }
    return best;
}
