#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/SharedBoss.h"
#include <nlohmann/json.hpp>
#include <algorithm>

extern "C" {
#include "functions.h"
extern PlayState* gPlayState;
}

/**
 * BOSS_HEALTH (OOT recomp fork)
 *
 * Sent when the local player damages a boss. Only sent to other clients in the same scene,
 * who lower their own copy of the boss to this health (see SharedBoss.h).
 */

void Anchor::SendPacket_BossHealth(s16 actorId, s32 health) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;

    payload["type"] = BOSS_HEALTH;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["actorId"] = actorId;
    payload["health"] = health;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_BossHealth(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    s16 sceneNum = payload.at("sceneNum").get<s16>();
    s16 actorId = payload.at("actorId").get<s16>();
    s32 health = payload.at("health").get<s32>();

    SharedBoss_OnRemoteHealth(sceneNum, actorId, health);
}

/**
 * BOSS_STATE (OOT recomp fork)
 *
 * The boss's position, animation and AI state. Sent every frame by every player whose boss is
 * mid-fight (event = false); the player with the lowest client ID leads and everyone else copies
 * them. Also sent once when a local hit changes the boss's state (event = true), which every
 * receiver applies straight away.
 */

void Anchor::SendPacket_BossState(s16 actorId, const SharedBossState& state, bool isEvent) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json vars = nlohmann::json::array();
    for (int i = 0; i < SHARED_BOSS_VAR_COUNT; i++) {
        vars.push_back(state.vars[i]);
    }

    nlohmann::json payload;
    payload["type"] = BOSS_STATE;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["actorId"] = actorId;
    payload["event"] = isEvent;
    payload["action"] = state.action;
    payload["vars"] = vars;
    payload["pos"] = { state.pos[0], state.pos[1], state.pos[2] };
    payload["rot"] = { state.rot[0], state.rot[1], state.rot[2] };
    payload["shapeRot"] = { state.shapeRot[0], state.shapeRot[1], state.shapeRot[2] };
    payload["move"] = { state.speedXZ, state.velocityY, state.gravity };
    payload["animFrame"] = state.animFrame;
    nlohmann::json minions = nlohmann::json::array();
    for (int i = 0; i < state.minionCount && i < SHARED_BOSS_MAX_MINIONS; i++) {
        const SharedBossMinion& m = state.minions[i];
        minions.push_back(
            { m.state, m.action, m.timer, m.pos[0], m.pos[1], m.pos[2], m.rotY, m.shapeRotY, m.speedXZ, m.velocityY });
    }
    payload["minions"] = minions;
    if (!isEvent) {
        payload["quiet"] = true;
    }

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_BossState(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    SharedBossState state = {};
    state.action = payload.at("action").get<s16>();
    auto& vars = payload.at("vars");
    for (int i = 0; i < SHARED_BOSS_VAR_COUNT && i < (int)vars.size(); i++) {
        state.vars[i] = vars[i].get<s16>();
    }
    for (int i = 0; i < 3; i++) {
        state.pos[i] = payload.at("pos")[i].get<f32>();
        state.rot[i] = payload.at("rot")[i].get<s16>();
        state.shapeRot[i] = payload.at("shapeRot")[i].get<s16>();
    }
    state.speedXZ = payload.at("move")[0].get<f32>();
    state.velocityY = payload.at("move")[1].get<f32>();
    state.gravity = payload.at("move")[2].get<f32>();
    state.animFrame = payload.at("animFrame").get<f32>();
    if (payload.contains("minions")) {
        auto& minions = payload["minions"];
        state.minionCount = (s16)std::min<size_t>(minions.size(), SHARED_BOSS_MAX_MINIONS);
        for (int i = 0; i < state.minionCount; i++) {
            auto& m = minions[i];
            state.minions[i].state = m[0].get<s16>();
            state.minions[i].action = m[1].get<s16>();
            state.minions[i].timer = m[2].get<s16>();
            state.minions[i].pos[0] = m[3].get<f32>();
            state.minions[i].pos[1] = m[4].get<f32>();
            state.minions[i].pos[2] = m[5].get<f32>();
            state.minions[i].rotY = m[6].get<s16>();
            state.minions[i].shapeRotY = m[7].get<s16>();
            state.minions[i].speedXZ = m[8].get<f32>();
            state.minions[i].velocityY = m[9].get<f32>();
        }
    }

    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    s16 sceneNum = payload.at("sceneNum").get<s16>();
    s16 actorId = payload.at("actorId").get<s16>();

    if (payload.value("event", false)) {
        SharedBoss_OnRemoteEvent(sceneNum, actorId, state);
    } else {
        SharedBoss_OnRemoteState(clientId, sceneNum, actorId, state);
    }
}

/**
 * BOSS_MINION_KILLED (OOT recomp fork)
 *
 * A boss's minion (e.g. a baby Gohma) was killed in the sender's game. Everyone else in the room
 * kills their copy of it too.
 */

void Anchor::SendPacket_BossMinionKilled(s16 actorId, s16 index) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = BOSS_MINION_KILLED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["actorId"] = actorId;
    payload["index"] = index;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_BossMinionKilled(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    SharedBoss_OnRemoteMinionKilled(payload.at("sceneNum").get<s16>(), payload.at("actorId").get<s16>(),
                                    payload.at("index").get<s16>());
}
