/*
    Handies - embedded animated fingers for GTA San Andreas pedestrians.
    The animation compatibility repair follows gta-reversed's
    RpAnimBlendClumpUpdateAnimations and CAnimBlendAssociation::Init flows.
*/

#include "plugin.h"

#include "CAnimBlendAssociation.h"
#include "CAnimBlendClumpData.h"
#include "CAnimBlendHierarchy.h"
#include "CAnimManager.h"
#include "CKeyGen.h"
#include "CPed.h"
#include "CPedIntelligence.h"
#include "CTaskManager.h"
#include "CTimer.h"
#include "CWeapon.h"
#include "common.h"
#include "eAnimations.h"
#include "ePedState.h"
#include "eTaskType.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace handies {

namespace {

constexpr std::uintptr_t entity_update_animations_call_address{0x535F94};
constexpr std::size_t max_tracked_peds{128};
constexpr std::uint32_t stale_entry_milliseconds{2000};
constexpr int standard_ped_bone_count{32};
constexpr int maximum_supported_bone_count{64};
constexpr int left_extra_bone_id{1005};
constexpr int right_extra_bone_id{1105};
constexpr char animation_block_name[]{"handies"};
constexpr char animation_file_name[]{"Handies.ifp"};
constexpr char left_pose_name[]{"LHGripPed"};
constexpr char right_pose_name[]{"RHGripPed"};
constexpr char ini_file_name[]{"Handies.ini"};
constexpr char log_file_name[]{"Handies.log"};
constexpr float rest_pose_time{0.56F};
constexpr float fist_pose_time{0.6666667F};
constexpr float fucku_pose_time{1.3333333F};
constexpr float minimum_visible_animation_blend{0.01F};

struct Settings {
    bool enabled{true};
    bool enable_player{true};
    bool enable_npcs{true};
    float grip_transition_speed{0.12F};
    float fucku_transition_speed{0.08F};
};

struct PedEntry {
    CPed* ped{nullptr};
    CAnimBlendAssociation* left_pose{nullptr};
    CAnimBlendAssociation* right_pose{nullptr};
    float grip{0.0F};
    float fucku_blend{0.0F};
    std::uint32_t last_seen{0};
};

[[nodiscard]] int read_int(
    const char* const path,
    const char* const key,
    const int fallback) noexcept {
    return GetPrivateProfileIntA("Handies", key, fallback, path);
}

[[nodiscard]] float read_float(
    const char* const path,
    const char* const key,
    const float fallback) noexcept {
    std::array<char, 64> fallback_text{};
    std::array<char, 64> value_text{};
    std::snprintf(fallback_text.data(), fallback_text.size(), "%.3f", fallback);
    GetPrivateProfileStringA(
        "Handies",
        key,
        fallback_text.data(),
        value_text.data(),
        static_cast<DWORD>(value_text.size()),
        path);
    char* parse_end{nullptr};
    const float parsed{std::strtof(value_text.data(), &parse_end)};
    return parse_end != value_text.data() ? parsed : fallback;
}

[[nodiscard]] constexpr float smoothstep(const float value) noexcept {
    const float clamped{std::clamp(value, 0.0F, 1.0F)};
    return clamped * clamped * (3.0F - 2.0F * clamped);
}

[[nodiscard]] bool wants_closed_fist(CPed& ped) noexcept {
    if (ped.m_pIntelligence != nullptr) {
        auto& tasks{ped.m_pIntelligence->m_TaskMgr};
        if (tasks.FindActiveTaskByType(TASK_SIMPLE_FIGHT) != nullptr ||
            tasks.FindActiveTaskByType(TASK_SIMPLE_FIGHT_CTRL) != nullptr) {
            return true;
        }
    }

    if (ped.m_ePedState == PEDSTATE_ATTACK ||
        ped.m_ePedState == PEDSTATE_FIGHT ||
        ped.m_ePedState == PEDSTATE_AIMGUN ||
        ped.m_ePedState == PEDSTATE_SNIPER_MODE ||
        ped.m_ePedState == PEDSTATE_ROCKETLAUNCHER_MODE) {
        return true;
    }

    const CWeapon* const weapon{ped.GetWeapon()};
    return weapon != nullptr && weapon->m_nState == WEAPONSTATE_FIRING;
}

[[nodiscard]] bool is_playing_fucku(const CPed& ped) noexcept {
    if (ped.m_pRwClump == nullptr) {
        return false;
    }
    if (ped.m_pIntelligence != nullptr &&
        ped.m_pIntelligence->m_TaskMgr.FindActiveTaskByType(
            TASK_SIMPLE_SHAKE_FIST) != nullptr) {
        return true;
    }

    const CAnimBlendAssociation* association{
        RpAnimBlendClumpGetAssociation(ped.m_pRwClump, "FUCKU")};
    if (association == nullptr) {
        association = RpAnimBlendClumpGetAssociation(
            ped.m_pRwClump,
            static_cast<unsigned int>(ANIM_DEFAULT_FUCKU));
    }
    return association != nullptr &&
           association->m_fBlendAmount > minimum_visible_animation_blend;
}

[[nodiscard]] bool has_embedded_finger_bones(
    const CAnimBlendClumpData& data) noexcept {
    if (data.m_nNumFrames <= standard_ped_bone_count ||
        data.m_nNumFrames > maximum_supported_bone_count ||
        data.m_pFrames == nullptr) {
        return false;
    }

    bool has_left{false};
    bool has_right{false};
    for (int index{0}; index < data.m_nNumFrames; ++index) {
        const unsigned int bone_id{data.m_pFrames[index].m_nNodeId};
        has_left = has_left || bone_id == left_extra_bone_id;
        has_right = has_right || bone_id == right_extra_bone_id;
    }
    return has_left && has_right;
}

[[nodiscard]] bool association_is_linked(
    RpClump* const clump,
    const CAnimBlendAssociation* const target) noexcept {
    if (clump == nullptr || target == nullptr) {
        return false;
    }
    for (CAnimBlendAssociation* association{
             RpAnimBlendClumpGetFirstAssociation(clump)};
         association != nullptr;
         association = RpAnimBlendGetNextAssociation(association)) {
        if (association == target) {
            return true;
        }
    }
    return false;
}

} // namespace

class HandiesMod final {
public:
    HandiesMod() {
        resolve_module_paths();
        load_settings();
        install_animation_compatibility_hook();
        log("Handies cargado; manos integradas y compatibilidad de 58 huesos activa.");

        plugin::Events::initGameEvent += [this] {
            animation_block_index_ = -1;
            resources_requested_ = false;
            load_settings();
        };
        plugin::Events::gameProcessEvent += [this] { on_game_process(); };
        plugin::Events::pedRenderEvent.before += [this](CPed* const ped) {
            on_ped_render(ped);
        };
        plugin::Events::pedSetModelEvent.after += [this](CPed* const ped, int) {
            remove_for_ped(ped);
        };
        plugin::Events::pedDtorEvent.before += [this](CPed* const ped) {
            remove_for_ped(ped);
        };
        plugin::Events::shutdownPoolsEvent += [this] {
            entries_.fill({});
            animation_block_index_ = -1;
            resources_requested_ = false;
        };
    }

private:
    using UpdateAnimationsFunction = void(__cdecl*)(RpClump*, float, bool);

    void resolve_module_paths() noexcept {
        HMODULE module{nullptr};
        const auto address{reinterpret_cast<LPCSTR>(this)};
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                address,
                &module) == FALSE) {
            std::strcpy(ini_path_.data(), ini_file_name);
            std::strcpy(log_path_.data(), log_file_name);
            std::strcpy(animation_path_.data(), animation_file_name);
            return;
        }

        std::array<char, MAX_PATH> module_path{};
        GetModuleFileNameA(
            module,
            module_path.data(),
            static_cast<DWORD>(module_path.size()));
        char* const separator{std::strrchr(module_path.data(), '\\')};
        if (separator != nullptr) {
            separator[1] = '\0';
        } else {
            module_path[0] = '\0';
        }
        std::snprintf(
            ini_path_.data(), ini_path_.size(), "%s%s", module_path.data(), ini_file_name);
        std::snprintf(
            log_path_.data(), log_path_.size(), "%s%s", module_path.data(), log_file_name);
        std::snprintf(
            animation_path_.data(),
            animation_path_.size(),
            "%s%s",
            module_path.data(),
            animation_file_name);
    }

    void load_settings() noexcept {
        Settings loaded{};
        loaded.enabled = read_int(ini_path_.data(), "Enabled", 1) != 0;
        loaded.enable_player = read_int(ini_path_.data(), "Player", 1) != 0;
        loaded.enable_npcs = read_int(ini_path_.data(), "NPCs", 1) != 0;
        loaded.grip_transition_speed = std::clamp(
            read_float(ini_path_.data(), "GripTransitionSpeed", 0.12F),
            0.01F,
            1.0F);
        loaded.fucku_transition_speed = std::clamp(
            read_float(ini_path_.data(), "FuckUTransitionSpeed", 0.08F),
            0.01F,
            1.0F);
        settings_ = loaded;
    }

    void log(const char* const message) const noexcept {
        const HANDLE file{CreateFileA(
            log_path_.data(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr)};
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        DWORD written{0};
        WriteFile(
            file,
            message,
            static_cast<DWORD>(std::strlen(message)),
            &written,
            nullptr);
        constexpr char newline[]{"\r\n"};
        WriteFile(file, newline, 2, &written, nullptr);
        CloseHandle(file);
    }

    void install_animation_compatibility_hook() noexcept {
        const auto previous{injector::MakeCALL(
            entity_update_animations_call_address,
            injector::raw_ptr(&update_animations_compatible))};
        const UpdateAnimationsFunction original{previous.get()};
        if (original != nullptr) {
            original_update_animations_ = original;
        }
        log(original_update_animations_ != nullptr
                ? "Hook de RpAnimBlendClumpUpdateAnimations instalado."
                : "ERROR: no se pudo instalar el hook de animaciones.");
    }

    static void __cdecl update_animations_compatible(
        RpClump* const clump,
        const float time_step,
        const bool on_screen) noexcept {
        repair_animation_associations(clump);
        if (original_update_animations_ != nullptr) {
            original_update_animations_(clump, time_step, on_screen);
        }
    }

    static void repair_animation_associations(RpClump* const clump) noexcept {
        if (clump == nullptr) {
            return;
        }
        CAnimBlendClumpData* const data{RpClumpGetAnimBlendClumpData(clump)};
        if (data == nullptr || !has_embedded_finger_bones(*data)) {
            return;
        }

        for (CAnimBlendAssociation* association{
                 RpAnimBlendClumpGetFirstAssociation(clump)};
             association != nullptr;
             association = RpAnimBlendGetNextAssociation(association)) {
            if (association->m_pHierarchy == nullptr ||
                association->m_nNumBlendNodes == data->m_nNumFrames) {
                continue;
            }

            const float current_time{association->m_fCurrentTime};
            if (association->m_pNodeArray != nullptr) {
                association->FreeAnimBlendNodeArray();
                association->m_pNodeArray = nullptr;
            }
            association->Init(clump, association->m_pHierarchy);
            association->SetCurrentTime(current_time);
        }
    }

    [[nodiscard]] bool request_resources() noexcept {
        if (resources_requested_) {
            return animation_block_index_ >= 0;
        }
        resources_requested_ = true;

        RwStream* const stream{RwStreamOpen(
            rwSTREAMFILENAME,
            rwSTREAMREAD,
            animation_path_.data())};
        if (stream == nullptr) {
            log("ERROR: no se pudo abrir Handies.ifp.");
            return false;
        }
        CAnimManager::LoadAnimFile(stream, true, nullptr);
        RwStreamClose(stream, nullptr);
        animation_block_index_ =
            CAnimManager::GetAnimationBlockIndex(animation_block_name);
        if (animation_block_index_ < 0) {
            log("ERROR: Handies.ifp no registro el bloque HANDIES.");
            return false;
        }
        log("Handies.ifp cargado; poses parciales integradas disponibles.");
        return true;
    }

    [[nodiscard]] CAnimBlendHierarchy* find_pose(
        const char* const name) const noexcept {
        if (animation_block_index_ < 0) {
            return nullptr;
        }
        CAnimBlock* const block{
            &CAnimManager::ms_aAnimBlocks[animation_block_index_]};
        return CAnimManager::GetAnimation(name, block);
    }

    [[nodiscard]] static CAnimBlendAssociation* attach_or_find_pose(
        RpClump* const clump,
        CAnimBlendHierarchy* const hierarchy) noexcept {
        if (clump == nullptr || hierarchy == nullptr) {
            return nullptr;
        }
        for (CAnimBlendAssociation* association{
                 RpAnimBlendClumpGetFirstAssociation(clump)};
             association != nullptr;
             association = RpAnimBlendGetNextAssociation(association)) {
            if (association->m_pHierarchy == hierarchy) {
                return association;
            }
        }

        CAnimBlendAssociation* const association{
            CAnimManager::AddAnimation(clump, hierarchy, 0)};
        if (association == nullptr) {
            return nullptr;
        }
        association->m_bPlaying = true;
        association->m_bLooped = false;
        association->m_bFreezeLastFrame = false;
        association->m_bPartial = true;
        association->m_bIndestructible = true;
        association->m_fBlendAmount = 1.0F;
        association->m_fBlendDelta = 0.0F;
        association->m_fSpeed = 0.0F;
        association->SetCurrentTime(rest_pose_time);
        return association;
    }

    void on_ped_render(CPed* const ped) noexcept {
        if (!settings_.enabled || ped == nullptr || ped->m_pRwClump == nullptr ||
            animation_block_index_ < 0) {
            return;
        }
        const bool is_player{ped == FindPlayerPed()};
        if ((is_player && !settings_.enable_player) ||
            (!is_player && !settings_.enable_npcs)) {
            return;
        }

        CAnimBlendClumpData* const data{
            RpClumpGetAnimBlendClumpData(ped->m_pRwClump)};
        if (data == nullptr || !has_embedded_finger_bones(*data)) {
            return;
        }

        PedEntry* const entry{find_or_create_entry(ped)};
        if (entry == nullptr) {
            return;
        }
        entry->last_seen = CTimer::m_snTimeInMilliseconds;
        ensure_pose_associations(*entry);
    }

    [[nodiscard]] PedEntry* find_or_create_entry(CPed* const ped) noexcept {
        for (auto& entry : entries_) {
            if (entry.ped == ped) {
                return &entry;
            }
        }
        for (auto& entry : entries_) {
            if (entry.ped == nullptr) {
                entry = {};
                entry.ped = ped;
                return &entry;
            }
        }
        return nullptr;
    }

    void ensure_pose_associations(PedEntry& entry) const noexcept {
        if (entry.ped == nullptr || entry.ped->m_pRwClump == nullptr) {
            return;
        }
        RpClump* const clump{entry.ped->m_pRwClump};
        if (!association_is_linked(clump, entry.left_pose)) {
            entry.left_pose = attach_or_find_pose(clump, find_pose(left_pose_name));
        }
        if (!association_is_linked(clump, entry.right_pose)) {
            entry.right_pose = attach_or_find_pose(clump, find_pose(right_pose_name));
        }
    }

    void on_game_process() noexcept {
        if (!settings_.enabled || !request_resources()) {
            return;
        }
        const std::uint32_t now{CTimer::m_snTimeInMilliseconds};
        for (auto& entry : entries_) {
            if (entry.ped == nullptr) {
                continue;
            }
            if (now - entry.last_seen > stale_entry_milliseconds) {
                entry = {};
                continue;
            }
            ensure_pose_associations(entry);
            update_finger_pose(entry);
        }
    }

    void update_finger_pose(PedEntry& entry) const noexcept {
        if (entry.ped == nullptr) {
            return;
        }
        const bool fucku_active{is_playing_fucku(*entry.ped)};
        const float target_fucku{fucku_active ? 1.0F : 0.0F};
        const float maximum_fucku_change{std::clamp(
            settings_.fucku_transition_speed * CTimer::ms_fTimeStep,
            0.001F,
            1.0F)};
        entry.fucku_blend += std::clamp(
            target_fucku - entry.fucku_blend,
            -maximum_fucku_change,
            maximum_fucku_change);

        const float target_grip{wants_closed_fist(*entry.ped) ? 1.0F : 0.0F};
        const float maximum_grip_change{std::clamp(
            settings_.grip_transition_speed * CTimer::ms_fTimeStep,
            0.001F,
            1.0F)};
        entry.grip += std::clamp(
            target_grip - entry.grip,
            -maximum_grip_change,
            maximum_grip_change);
        const float pose_time{
            rest_pose_time + (fist_pose_time - rest_pose_time) * entry.grip};

        if (entry.left_pose != nullptr) {
            entry.left_pose->SetCurrentTime(pose_time);
        }
        if (entry.right_pose != nullptr) {
            const float fucku_weight{smoothstep(entry.fucku_blend)};
            entry.right_pose->SetCurrentTime(
                pose_time + (fucku_pose_time - pose_time) * fucku_weight);
        }
    }

    void remove_for_ped(const CPed* const ped) noexcept {
        if (ped == nullptr) {
            return;
        }
        for (auto& entry : entries_) {
            if (entry.ped == ped) {
                entry = {};
                return;
            }
        }
    }

    Settings settings_{};
    std::array<PedEntry, max_tracked_peds> entries_{};
    std::array<char, MAX_PATH> ini_path_{};
    std::array<char, MAX_PATH> log_path_{};
    std::array<char, MAX_PATH> animation_path_{};
    int animation_block_index_{-1};
    bool resources_requested_{false};

    inline static UpdateAnimationsFunction original_update_animations_{
        reinterpret_cast<UpdateAnimationsFunction>(0x4D34F0)};
};

HandiesMod handies_mod{};

} // namespace handies
