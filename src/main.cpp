#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"
#include "SKSE/Logger.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace ZAPoisonNative
{
    namespace EffectManager
    {
        enum class WeaponSlot : std::uint32_t
        {
            kRightFirst = 0,
            kRightThird,
            kLeftFirst,
            kLeftThird,
            kTotal
        };

        struct ActiveEffect
        {
            std::uintptr_t attachNodeKey{ 0 };
            RE::FormID shaderID{ 0 };
            RE::ShaderReferenceEffect* effect{ nullptr };
            bool active{ false };
        };

        std::array<ActiveEffect, static_cast<std::size_t>(WeaponSlot::kTotal)> g_weaponEffects{};
        std::unordered_map<RE::FormID, ActiveEffect> g_worldEffects{};
        std::mutex g_effectLock;

        constexpr float kPersistentWeaponShaderDuration = -1.0f;
        constexpr float kWorldShaderDuration = 0.80f;

        WeaponSlot GetWeaponSlot(bool a_leftHand, bool a_firstPerson)
        {
            if (a_leftHand) {
                return a_firstPerson ? WeaponSlot::kLeftFirst : WeaponSlot::kLeftThird;
            }
            else {
                return a_firstPerson ? WeaponSlot::kRightFirst : WeaponSlot::kRightThird;
            }
        }

        std::uintptr_t GetNodeKey(RE::NiAVObject* a_node)
        {
            return reinterpret_cast<std::uintptr_t>(a_node);
        }

        RE::NiNode* GetNodeByName(RE::NiAVObject* a_root, const RE::BSFixedString& a_name)
        {
            if (!a_root) {
                return nullptr;
            }

            RE::NiAVObject* obj = a_root->GetObjectByName(a_name);
            return obj ? obj->AsNode() : nullptr;
        }

        RE::NiNode* FindPlayerWeaponNode(RE::PlayerCharacter* a_player, bool a_leftHand, bool a_firstPerson)
        {
            if (!a_player) {
                return nullptr;
            }

            RE::NiAVObject* root3D = a_player->Get3D(a_firstPerson);
            if (!root3D) {
                return nullptr;
            }

            RE::NiNode* handNode = nullptr;

            if (a_leftHand) {
                handNode = GetNodeByName(root3D, RE::BSFixedString("SHIELD"));
                if (!handNode) {
                    handNode = GetNodeByName(root3D, RE::BSFixedString("NPC L Weapon [LWea]"));
                }
            }
            else {
                handNode = GetNodeByName(root3D, RE::BSFixedString("WEAPON"));
                if (!handNode) {
                    handNode = GetNodeByName(root3D, RE::BSFixedString("NPC R Weapon [RWea]"));
                }
            }

            return handNode;
        }

        RE::TESEffectShader* LookupEffectShader(RE::FormID a_formID)
        {
            if (!a_formID) {
                return nullptr;
            }

            auto form = RE::TESForm::LookupByID(a_formID);
            return form ? form->As<RE::TESEffectShader>() : nullptr;
        }

        void StopAndClearTrackedEffect(ActiveEffect& a_state)
        {
            if (a_state.effect) {
                a_state.effect->Detach();
                a_state.effect = nullptr;
            }

            a_state.attachNodeKey = 0;
            a_state.shaderID = 0;
            a_state.active = false;
        }

        void ApplyPlayerPoisonFX(bool a_leftHand, bool a_enable, RE::FormID a_shaderID)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                SKSE::log::warn("ApplyPlayerPoisonFX: player was null");
                return;
            }

            RE::TESEffectShader* shader = LookupEffectShader(a_shaderID);
            if (a_enable && !shader) {
                SKSE::log::warn("ApplyPlayerPoisonFX: shader {:08X} not found", a_shaderID);
                return;
            }

            constexpr bool views[2] = { true, false };

            for (bool isFirstPerson : views) {
                RE::NiNode* handNode = FindPlayerWeaponNode(player, a_leftHand, isFirstPerson);
                RE::NiAVObject* attachNode = handNode;
                WeaponSlot slot = GetWeaponSlot(a_leftHand, isFirstPerson);
                std::uintptr_t nodeKey = GetNodeKey(attachNode);

                {
                    std::lock_guard<std::mutex> lock(g_effectLock);
                    auto& state = g_weaponEffects[static_cast<std::size_t>(slot)];

                    if (!a_enable) {
                        StopAndClearTrackedEffect(state);
                    }
                    else if (attachNode &&
                        state.active &&
                        state.attachNodeKey == nodeKey &&
                        state.shaderID == a_shaderID) {
                        SKSE::log::info("ApplyPlayerPoisonFX: {} hand ({}) already has shader {:08X}; skipping reapply",
                            a_leftHand ? "left" : "right",
                            isFirstPerson ? "1st" : "3rd",
                            a_shaderID);
                        continue;
                    }
                    else {
                        StopAndClearTrackedEffect(state);
                    }
                }

                if (!a_enable) {
                    SKSE::log::info("ApplyPlayerPoisonFX: disable requested for {} hand ({})",
                        a_leftHand ? "left" : "right",
                        isFirstPerson ? "1st" : "3rd");
                    continue;
                }

                if (!attachNode) {
                    SKSE::log::warn("ApplyPlayerPoisonFX: no attach node for {} hand in {} person",
                        a_leftHand ? "left" : "right",
                        isFirstPerson ? "first" : "third");
                    continue;
                }

                auto* effect = player->ApplyEffectShader(
                    shader,
                    kPersistentWeaponShaderDuration,
                    nullptr,
                    false,
                    false,
                    attachNode,
                    false);

                auto effectAddr = reinterpret_cast<std::uintptr_t>(effect);

                SKSE::log::info("ApplyPlayerPoisonFX: ApplyEffectShader returned 0x{:X} for {} hand ({})",
                    effectAddr,
                    a_leftHand ? "left" : "right",
                    isFirstPerson ? "1st" : "3rd");

                {
                    std::lock_guard<std::mutex> lock(g_effectLock);
                    auto& state = g_weaponEffects[static_cast<std::size_t>(slot)];
                    state.attachNodeKey = nodeKey;
                    state.shaderID = a_shaderID;
                    state.effect = (effectAddr > 0x10000) ? effect : nullptr;
                    state.active = true;
                }

                if (effectAddr <= 0x10000) {
                    SKSE::log::warn("ApplyPlayerPoisonFX: non-usable effect pointer 0x{:X}; treating slot as active without detachable pointer",
                        effectAddr);
                }
                else {
                    SKSE::log::info("ApplyPlayerPoisonFX: applied persistent shader {:08X} to {} hand ({})",
                        a_shaderID,
                        a_leftHand ? "left" : "right",
                        isFirstPerson ? "1st" : "3rd");
                }
            }
        }

        void ApplyWorldPoisonFX(RE::FormID a_refID, bool a_enable, RE::FormID a_shaderID)
        {
            auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID);
            if (!ref) {
                SKSE::log::warn("ApplyWorldPoisonFX: reference {:08X} not found", a_refID);
                return;
            }

            RE::NiAVObject* root3D = ref->Get3D();
            RE::TESEffectShader* shader = LookupEffectShader(a_shaderID);

            if (a_enable && !shader) {
                SKSE::log::warn("ApplyWorldPoisonFX: shader {:08X} not found", a_shaderID);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(g_effectLock);
                auto it = g_worldEffects.find(a_refID);

                if (it != g_worldEffects.end()) {
                    StopAndClearTrackedEffect(it->second);

                    if (!a_enable) {
                        g_worldEffects.erase(it);
                    }
                }
                else if (a_enable) {
                    g_worldEffects.emplace(a_refID, ActiveEffect{});
                }
            }

            if (!a_enable) {
                SKSE::log::info("ApplyWorldPoisonFX: disable requested for ref {:08X}", a_refID);
                return;
            }

            if (!root3D) {
                SKSE::log::warn("ApplyWorldPoisonFX: reference {:08X} has no 3D", a_refID);
                return;
            }

            auto* effect = ref->ApplyEffectShader(
                shader,
                kWorldShaderDuration,
                nullptr,
                false,
                false,
                root3D,
                false);

            auto effectAddr = reinterpret_cast<std::uintptr_t>(effect);

            SKSE::log::info("ApplyWorldPoisonFX: ApplyEffectShader returned 0x{:X} for ref {:08X}",
                effectAddr,
                a_refID);

            if (effectAddr <= 0x10000) {
                SKSE::log::warn("ApplyWorldPoisonFX: invalid effect pointer 0x{:X} for ref {:08X}",
                    effectAddr,
                    a_refID);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(g_effectLock);
                auto& state = g_worldEffects[a_refID];
                state.attachNodeKey = GetNodeKey(root3D);
                state.shaderID = a_shaderID;
                state.effect = nullptr;
                state.active = true;
            }

            SKSE::log::info("ApplyWorldPoisonFX: applied shader {:08X} to ref {:08X}",
                a_shaderID,
                a_refID);
        }

        void ClearAllPlayerPoisonFX()
        {
            std::lock_guard<std::mutex> lock(g_effectLock);

            for (auto& state : g_weaponEffects) {
                StopAndClearTrackedEffect(state);
            }

            SKSE::log::info("ClearAllPlayerPoisonFX: detached and cleared all tracked player poison FX");
        }
    }

    bool SetWeaponPoisonFX(RE::StaticFunctionTag*, bool abLeftHand, bool abEnable, RE::TESEffectShader* akEffectShader)
    {
        RE::FormID shaderID = akEffectShader ? akEffectShader->GetFormID() : 0;

        SKSE::log::info("SetWeaponPoisonFX called: hand={} enable={} shader={:08X}",
            abLeftHand ? "left" : "right",
            abEnable ? 1 : 0,
            shaderID);

        if (abEnable && !shaderID) {
            SKSE::log::warn("SetWeaponPoisonFX: enable requested but shader is null");
            return false;
        }

        SKSE::GetTaskInterface()->AddTask([abLeftHand, abEnable, shaderID]() {
            EffectManager::ApplyPlayerPoisonFX(abLeftHand, abEnable, shaderID);
            });

        return true;
    }

    bool SetWorldObjectPoisonFX(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref, bool abEnable, RE::TESEffectShader* akEffectShader)
    {
        if (!a_ref) {
            SKSE::log::warn("SetWorldObjectPoisonFX: reference was null");
            return false;
        }

        RE::FormID refID = a_ref->GetFormID();
        RE::FormID shaderID = akEffectShader ? akEffectShader->GetFormID() : 0;

        if (abEnable && !shaderID) {
            SKSE::log::warn("SetWorldObjectPoisonFX: enable requested but shader is null");
            return false;
        }

        SKSE::GetTaskInterface()->AddTask([refID, abEnable, shaderID]() {
            EffectManager::ApplyWorldPoisonFX(refID, abEnable, shaderID);
            });

        return true;
    }

    void ClearAllWeaponPoisonFX(RE::StaticFunctionTag*)
    {
        SKSE::GetTaskInterface()->AddTask([]() {
            EffectManager::ClearAllPlayerPoisonFX();
            });
    }

    bool RegisterPapyrusFunctions(RE::BSScript::IVirtualMachine* vm)
    {
        vm->RegisterFunction("SetWeaponPoisonFX", "ZAPoisonNative", SetWeaponPoisonFX);
        vm->RegisterFunction("SetWorldObjectPoisonFX", "ZAPoisonNative", SetWorldObjectPoisonFX);
        vm->RegisterFunction("ClearAllWeaponPoisonFX", "ZAPoisonNative", ClearAllWeaponPoisonFX);

        SKSE::log::info("Registered Papyrus functions for ZAPoisonNative");
        return true;
    }
}

namespace
{
    void InitializeLogging()
    {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }

        *path /= "ZAPoisonNative.log";

        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));

        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S] [%l] %v");
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    InitializeLogging();

    SKSE::log::info("ZAPoisonNative loaded");

    SKSE::GetPapyrusInterface()->Register(ZAPoisonNative::RegisterPapyrusFunctions);

    return true;
}