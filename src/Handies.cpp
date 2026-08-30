/*
    Handies - runtime animated fingers for GTA San Andreas pedestrians.

    DFF files retain the native 32-node ped hierarchy. Handies evaluates
    per-hand morph profiles derived from the original GHANDS finger rigs and
    applies them only around each draw. No finger node enters GTA's HAnim.
*/

#include "plugin.h"

#include "AnimBlendFrameData.h"
#include "CAnimBlendAssociation.h"
#include "CAnimBlendClumpData.h"
#include "CHandObject.h"
#include "CPed.h"
#include "CPedIntelligence.h"
#include "CPools.h"
#include "CTask.h"
#include "CTaskManager.h"
#include "CTimer.h"
#include "CWeapon.h"
#include "RpHAnimBlendInterpFrame.h"
#include "common.h"
#include "eAnimations.h"
#include "ePedState.h"
#include "eTaskType.h"
#include "calling.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <type_traits>
#include <vector>

namespace handies {
namespace {

constexpr std::uintptr_t entity_render_clump_call_address{0x53439C};
constexpr std::size_t max_tracked_peds{256};
constexpr std::size_t max_atomics_per_ped{16};
constexpr int native_bone_count{32};
constexpr int runtime_bone_count{62};
constexpr int finger_bones_per_hand{15};
constexpr int pose_count{3};
constexpr int hand_signal_count{5};
constexpr int left_hand_id{34};
constexpr int right_hand_id{24};
constexpr char data_file_name[]{"Handies.dat"};
constexpr char ini_file_name[]{"Handies.ini"};
constexpr char log_file_name[]{"Handies.log"};
constexpr std::array<char, 8> data_magic{'H', 'N', 'D', '2', 'D', 'A', 'T', '\0'};
constexpr std::uint32_t data_version{3};
constexpr float minimum_visible_animation_blend{0.01F};
constexpr std::uintptr_t hand_object_vtable_address{0x866EE0};
constexpr std::uintptr_t hand_object_pre_render_slot{
    hand_object_vtable_address + 17U * sizeof(std::uintptr_t)};
constexpr std::uintptr_t hand_object_render_slot{
    hand_object_vtable_address + 18U * sizeof(std::uintptr_t)};

struct Settings {
    bool enabled{true};
    bool enable_player{true};
    bool enable_npcs{true};
    float grip_transition_speed{0.12F};
    float fucku_transition_speed{0.08F};
};

struct RuntimeProfile {
    std::uint64_t geometry_hash{};
    std::uint32_t vertex_count{};
    std::array<RwV3d, finger_bones_per_hand * 2> translations{};
    std::vector<RwUInt32> indices{};
    std::vector<RwMatrixWeights> weights{};
    std::array<RwMatrix, runtime_bone_count> inverse_matrices{};
};

using PoseTable = std::array<
    std::array<std::array<RtQuat, finger_bones_per_hand>, pose_count>,
    2>;

struct FingerKey {
    float time{};
    RtQuat rotation{};
};

struct FingerTrack {
    std::vector<FingerKey> keys{};
};

struct HandSignalAnimation {
    float duration{};
    std::array<FingerTrack, finger_bones_per_hand> tracks{};
};

using HandSignalTable = std::array<
    std::array<HandSignalAnimation, hand_signal_count>,
    2>;

struct HandSignalState {
    int animation_index{-1};
    float time{};
    bool left{};
    bool right{};
};

// ABI view of CTaskSimplePlayHandSignalAnim in GTA SA 1.0 US. Keeping this
// private avoids constructing or replacing the native task; Handies only reads
// the association, selected hand animation, and side information it owns.
struct NativeHandSignalTaskView {
    void* vtable{};
    CTask* parent{};
    CAnimBlendAssociation* body_animation{};
    std::uint8_t flags{};
    std::array<std::byte, 3> first_padding{};
    std::int32_t hand_animation_id{};
    float blend_delta{};
    std::uint8_t use_fat_hands{};
    std::array<std::byte, 3> second_padding{};
    CHandObject* left_hand{};
    CHandObject* right_hand{};
};

static_assert(sizeof(NativeHandSignalTaskView) == 0x24);
static_assert(offsetof(NativeHandSignalTaskView, body_animation) == 0x08);
static_assert(offsetof(NativeHandSignalTaskView, hand_animation_id) == 0x10);
static_assert(offsetof(NativeHandSignalTaskView, right_hand) == 0x20);

struct RuntimeBinding {
    RpHAnimHierarchy* source_hierarchy{};
    const RuntimeProfile* profile{};
    std::array<RwMatrix, runtime_bone_count> pose_matrices{};
};

struct RuntimeAtomic {
    RpAtomic* atomic{};
    std::size_t binding_index{};
    std::vector<RwV3d> base_vertices{};
    std::vector<RwV3d> base_normals{};
};

struct PedEntry {
    CPed* ped{};
    RpClump* clump{};
    std::array<RuntimeBinding, max_atomics_per_ped> bindings{};
    std::size_t binding_count{};
    std::array<RuntimeAtomic, max_atomics_per_ped> atomics{};
    std::size_t atomic_count{};
    float grip{};
    float fucku_blend{};
};

struct AtomicList {
    std::array<RpAtomic*, max_atomics_per_ped> values{};
    std::size_t size{};
    bool overflow{};
};

struct HierarchyPlan {
    RpHAnimHierarchy* source_hierarchy{};
    const RuntimeProfile* profile{};
};

class BinaryReader final {
public:
    explicit BinaryReader(const std::vector<std::byte>& bytes) noexcept
        : bytes_{bytes} {
    }

    template <typename Value>
    [[nodiscard]] bool read(Value& value) noexcept {
        static_assert(std::is_trivially_copyable_v<Value>);
        if (remaining() < sizeof(Value)) {
            return false;
        }
        std::memcpy(&value, bytes_.data() + position_, sizeof(Value));
        position_ += sizeof(Value);
        return true;
    }

    [[nodiscard]] bool read_bytes(void* destination, std::size_t size) noexcept {
        if (remaining() < size) {
            return false;
        }
        std::memcpy(destination, bytes_.data() + position_, size);
        position_ += size;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - position_;
    }

private:
    const std::vector<std::byte>& bytes_;
    std::size_t position_{};
};

[[nodiscard]] int read_int(
    const char* path,
    const char* key,
    int fallback) noexcept {
    return GetPrivateProfileIntA("Handies", key, fallback, path);
}

[[nodiscard]] float read_float(
    const char* path,
    const char* key,
    float fallback) noexcept {
    std::array<char, 64> fallback_text{};
    std::array<char, 64> value_text{};
    std::snprintf(fallback_text.data(), fallback_text.size(), "%.3f", fallback);
    GetPrivateProfileStringA(
        "Handies", key, fallback_text.data(), value_text.data(),
        static_cast<DWORD>(value_text.size()), path);
    char* parse_end{};
    const float parsed{std::strtof(value_text.data(), &parse_end)};
    return parse_end != value_text.data() ? parsed : fallback;
}

[[nodiscard]] constexpr float smoothstep(float value) noexcept {
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
    if (ped.m_ePedState == PEDSTATE_ATTACK || ped.m_ePedState == PEDSTATE_FIGHT ||
        ped.m_ePedState == PEDSTATE_AIMGUN ||
        ped.m_ePedState == PEDSTATE_SNIPER_MODE ||
        ped.m_ePedState == PEDSTATE_ROCKETLAUNCHER_MODE) {
        return true;
    }
    const CWeapon* weapon{ped.GetWeapon()};
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
    CAnimBlendAssociation* association{
        RpAnimBlendClumpGetAssociation(ped.m_pRwClump, "FUCKU")};
    if (association == nullptr) {
        association = RpAnimBlendClumpGetAssociation(
            ped.m_pRwClump, static_cast<unsigned int>(ANIM_DEFAULT_FUCKU));
    }
    return association != nullptr &&
           association->m_fBlendAmount > minimum_visible_animation_blend;
}

[[nodiscard]] HandSignalState read_hand_signal_state(CPed& ped) noexcept {
    HandSignalState result{};
    if (ped.m_pIntelligence == nullptr) return result;

    CTask* const task{ped.m_pIntelligence->m_TaskMgr.FindTaskByType(
        TASK_SECONDARY_PARTIAL_ANIM, TASK_SIMPLE_HANDSIGNAL_ANIM)};
    if (task == nullptr) return result;

    const auto* const signal{
        reinterpret_cast<const NativeHandSignalTaskView*>(task)};
    CAnimBlendAssociation* const association{signal->body_animation};
    const int animation_index{
        signal->hand_animation_id - static_cast<int>(ANIM_HANDSIGNAL_GSIGN1)};
    if (association == nullptr || animation_index < 0 ||
        animation_index >= hand_signal_count ||
        association->m_fBlendAmount <= minimum_visible_animation_blend) {
        return result;
    }

    const unsigned short group{association->m_nAnimGroup};
    if (group != static_cast<unsigned short>(ANIM_GROUP_HANDSIGNAL) &&
        group != static_cast<unsigned short>(ANIM_GROUP_HANDSIGNALL)) {
        return result;
    }

    result.animation_index = animation_index;
    result.time = std::max(association->m_fCurrentTime, 0.0F);
    result.left = true;
    result.right = group == static_cast<unsigned short>(ANIM_GROUP_HANDSIGNAL);
    return result;
}

[[nodiscard]] RtQuat normalized_lerp(
    const RtQuat& first,
    RtQuat second,
    float amount) noexcept {
    const float dot{
        first.imag.x * second.imag.x + first.imag.y * second.imag.y +
        first.imag.z * second.imag.z + first.real * second.real};
    if (dot < 0.0F) {
        second.imag.x = -second.imag.x;
        second.imag.y = -second.imag.y;
        second.imag.z = -second.imag.z;
        second.real = -second.real;
    }
    RtQuat result{};
    result.imag.x = first.imag.x + (second.imag.x - first.imag.x) * amount;
    result.imag.y = first.imag.y + (second.imag.y - first.imag.y) * amount;
    result.imag.z = first.imag.z + (second.imag.z - first.imag.z) * amount;
    result.real = first.real + (second.real - first.real) * amount;
    const float length_squared{
        result.imag.x * result.imag.x + result.imag.y * result.imag.y +
        result.imag.z * result.imag.z + result.real * result.real};
    if (length_squared <= 1.0e-12F) {
        result.imag = {0.0F, 0.0F, 0.0F};
        result.real = 1.0F;
        return result;
    }
    const float inverse_length{1.0F / std::sqrt(length_squared)};
    result.imag.x *= inverse_length;
    result.imag.y *= inverse_length;
    result.imag.z *= inverse_length;
    result.real *= inverse_length;
    return result;
}

[[nodiscard]] RtQuat sample_track(
    const FingerTrack& track,
    float time) noexcept {
    if (track.keys.empty()) {
        RtQuat identity{};
        identity.real = 1.0F;
        return identity;
    }
    if (time <= track.keys.front().time) return track.keys.front().rotation;
    if (time >= track.keys.back().time) return track.keys.back().rotation;
    for (std::size_t index{1}; index < track.keys.size(); ++index) {
        const FingerKey& second{track.keys[index]};
        if (time > second.time) continue;
        const FingerKey& first{track.keys[index - 1]};
        const float span{second.time - first.time};
        const float amount{span > 1.0e-6F
            ? std::clamp((time - first.time) / span, 0.0F, 1.0F)
            : 0.0F};
        return normalized_lerp(first.rotation, second.rotation, amount);
    }
    return track.keys.back().rotation;
}

[[nodiscard]] int parent_source_id(int source_id) noexcept {
    switch (source_id) {
    case 3:
    case 6:
    case 9:
    case 12:
    case 15:
        return 2;
    default:
        return source_id - 1;
    }
}

RpAtomic* collect_atomic(RpAtomic* atomic, void* data) noexcept {
    auto& list{*static_cast<AtomicList*>(data)};
    if (list.size >= list.values.size()) {
        list.overflow = true;
    } else {
        list.values[list.size++] = atomic;
    }
    return atomic;
}

[[nodiscard]] std::uint64_t hash_geometry(const RpGeometry* geometry) noexcept {
    if (geometry == nullptr) return 0;
    const RwInt32 count{RpGeometryGetNumVertices(geometry)};
    const RpMorphTarget* morph_target{RpGeometryGetMorphTarget(geometry, 0)};
    const RwV3d* vertices{RpMorphTargetGetVertices(morph_target)};
    if (count <= 0 || vertices == nullptr) return 0;
    std::uint64_t value{0xCBF29CE484222325ULL};
    for (RwInt32 index{}; index < count; ++index) {
        const auto* bytes{reinterpret_cast<const unsigned char*>(&vertices[index])};
        for (std::size_t byte{}; byte < sizeof(RwV3d); ++byte) {
            value ^= bytes[byte];
            value *= 0x100000001B3ULL;
        }
    }
    return value;
}


} // namespace

class HandiesMod final {
public:
    HandiesMod() {
        instance_ = this;
        resolve_module_paths();
        load_settings();
        const bool data_loaded{load_runtime_data()};
        if (data_loaded) install_hand_object_hooks();
        log(data_loaded
                ? "Handies activo: Skin nativa y perfiles morph de manos cargados."
                : "ERROR: Handies.dat no pudo cargarse; el mod queda inactivo.");

        plugin::Events::initGameEvent += [this] {
            release_all();
            load_settings();
            install_final_render_hook();
            if (profiles_.empty() && !load_runtime_data()) {
                log("ERROR: Handies.dat sigue sin estar disponible al iniciar partida.");
            } else {
                install_hand_object_hooks();
            }
        };
        plugin::Events::gameProcessEvent += [this] { on_game_process(); };
        plugin::Events::pedRenderEvent.before += [this](CPed* ped) {
            on_ped_render(ped);
        };
        plugin::Events::pedSetModelEvent.after += [this](CPed* ped, int) {
            remove_for_ped(ped);
        };
        plugin::Events::pedDtorEvent.before += [this](CPed* ped) {
            remove_for_ped(ped);
        };
        plugin::Events::shutdownPoolsEvent += [this] { release_all(); };
    }

private:
    using ClumpRenderFunction = RpClump*(__cdecl*)(RpClump*);

    void resolve_module_paths() noexcept {
        std::array<char, MAX_PATH> temp_path{};
        const DWORD temp_length{GetTempPathA(
            static_cast<DWORD>(temp_path.size()), temp_path.data())};
        if (temp_length > 0 && temp_length < temp_path.size()) {
            std::snprintf(fallback_log_path_.data(), fallback_log_path_.size(),
                          "%s%s", temp_path.data(), log_file_name);
        } else {
            std::strcpy(fallback_log_path_.data(), log_file_name);
        }
        HMODULE module{};
        const auto address{reinterpret_cast<LPCSTR>(this)};
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                address, &module) == FALSE) {
            std::strcpy(ini_path_.data(), ini_file_name);
            std::strcpy(log_path_.data(), log_file_name);
            std::strcpy(data_path_.data(), data_file_name);
            return;
        }
        std::array<char, MAX_PATH> module_path{};
        GetModuleFileNameA(module, module_path.data(),
                           static_cast<DWORD>(module_path.size()));
        char* separator{std::strrchr(module_path.data(), '\\')};
        if (separator != nullptr) separator[1] = '\0';
        else module_path[0] = '\0';
        std::snprintf(ini_path_.data(), ini_path_.size(), "%s%s",
                      module_path.data(), ini_file_name);
        std::snprintf(log_path_.data(), log_path_.size(), "%s%s",
                      module_path.data(), log_file_name);
        std::snprintf(data_path_.data(), data_path_.size(), "%s%s",
                      module_path.data(), data_file_name);
    }

    void load_settings() noexcept {
        settings_.enabled = read_int(ini_path_.data(), "Enabled", 1) != 0;
        settings_.enable_player = read_int(ini_path_.data(), "Player", 1) != 0;
        settings_.enable_npcs = read_int(ini_path_.data(), "NPCs", 1) != 0;
        settings_.grip_transition_speed = std::clamp(
            read_float(ini_path_.data(), "GripTransitionSpeed", 0.12F),
            0.01F, 1.0F);
        settings_.fucku_transition_speed = std::clamp(
            read_float(ini_path_.data(), "FuckUTransitionSpeed", 0.08F),
            0.01F, 1.0F);
    }

    void log(const char* message) const noexcept {
        OutputDebugStringA("Handies: ");
        OutputDebugStringA(message);
        OutputDebugStringA("\r\n");
        static_cast<void>(append_log(log_path_.data(), message));
        if (_stricmp(log_path_.data(), fallback_log_path_.data()) != 0) {
            static_cast<void>(append_log(fallback_log_path_.data(), message));
        }
    }

    [[nodiscard]] static bool append_log(
        const char* path,
        const char* message) noexcept {
        const HANDLE file{CreateFileA(
            path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written{};
        const BOOL message_written{WriteFile(
            file, message, static_cast<DWORD>(std::strlen(message)), &written,
            nullptr)};
        constexpr char newline[]{"\r\n"};
        const BOOL newline_written{WriteFile(file, newline, 2, &written, nullptr)};
        CloseHandle(file);
        return message_written != FALSE && newline_written != FALSE;
    }

    [[nodiscard]] bool load_runtime_data() {
        try {
            std::ifstream input{data_path_.data(), std::ios::binary | std::ios::ate};
            if (!input) return false;
            const std::streamoff file_size{input.tellg()};
            if (file_size <= 0 || file_size > 256LL * 1024LL * 1024LL) return false;
            input.seekg(0);
            std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
            if (!input.read(
                    reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(file_size))) {
                return false;
            }
            BinaryReader reader{bytes};
            std::array<char, 8> magic{};
            std::uint32_t version{};
            std::uint32_t profile_count{};
            if (!reader.read_bytes(magic.data(), magic.size()) ||
                magic != data_magic || !reader.read(version) ||
                version != data_version || !reader.read(profile_count) ||
                profile_count == 0 || profile_count > 1024) {
                return false;
            }
            PoseTable poses{};
            for (auto& side : poses) {
                for (auto& pose : side) {
                    for (auto& quaternion : pose) {
                        std::array<float, 4> packed{};
                        if (!reader.read(packed)) return false;
                        quaternion.imag = {packed[0], packed[1], packed[2]};
                        quaternion.real = packed[3];
                    }
                }
            }
            HandSignalTable hand_signals{};
            for (auto& side : hand_signals) {
                for (auto& animation : side) {
                    if (!reader.read(animation.duration) ||
                        !std::isfinite(animation.duration) ||
                        animation.duration <= 0.0F || animation.duration > 30.0F) {
                        return false;
                    }
                    for (auto& track : animation.tracks) {
                        std::uint32_t key_count{};
                        if (!reader.read(key_count) || key_count == 0 || key_count > 64) {
                            return false;
                        }
                        track.keys.resize(key_count);
                        float previous_time{-1.0F};
                        for (auto& key : track.keys) {
                            std::array<float, 4> packed{};
                            if (!reader.read(key.time) || !reader.read(packed) ||
                                !std::isfinite(key.time) || key.time < previous_time ||
                                key.time < 0.0F ||
                                key.time > animation.duration + 1.0e-4F) {
                                return false;
                            }
                            key.rotation.imag = {packed[0], packed[1], packed[2]};
                            key.rotation.real = packed[3];
                            const float length_squared{
                                packed[0] * packed[0] + packed[1] * packed[1] +
                                packed[2] * packed[2] + packed[3] * packed[3]};
                            if (!std::isfinite(length_squared) ||
                                std::abs(length_squared - 1.0F) > 0.01F) {
                                return false;
                            }
                            previous_time = key.time;
                        }
                    }
                }
            }
            std::vector<RuntimeProfile> profiles{};
            profiles.reserve(profile_count);
            for (std::uint32_t profile_index{}; profile_index < profile_count;
                 ++profile_index) {
                RuntimeProfile profile{};
                std::uint32_t bone_count{};
                if (!reader.read(profile.geometry_hash) ||
                    !reader.read(profile.vertex_count) || !reader.read(bone_count) ||
                    profile.vertex_count == 0 || profile.vertex_count > 200000 ||
                    bone_count != runtime_bone_count) {
                    return false;
                }
                for (auto& translation : profile.translations) {
                    if (!reader.read(translation)) return false;
                }
                profile.indices.resize(profile.vertex_count);
                for (auto& packed_indices : profile.indices) {
                    std::array<std::uint8_t, 4> values{};
                    if (!reader.read(values)) return false;
                    packed_indices = static_cast<RwUInt32>(values[0]) |
                        (static_cast<RwUInt32>(values[1]) << 8U) |
                        (static_cast<RwUInt32>(values[2]) << 16U) |
                        (static_cast<RwUInt32>(values[3]) << 24U);
                }
                profile.weights.resize(profile.vertex_count);
                for (auto& weights : profile.weights) {
                    if (!reader.read(weights)) return false;
                }
                for (auto& matrix : profile.inverse_matrices) {
                    std::array<float, 16> values{};
                    if (!reader.read(values)) return false;
                    matrix.right = {values[0], values[1], values[2]};
                    matrix.flags = 0;
                    matrix.up = {values[4], values[5], values[6]};
                    matrix.pad1 = 0;
                    matrix.at = {values[8], values[9], values[10]};
                    matrix.pad2 = 0;
                    matrix.pos = {values[12], values[13], values[14]};
                    matrix.pad3 = 0;
                }
                profiles.push_back(std::move(profile));
            }
            if (reader.remaining() != 0) return false;
            poses_ = poses;
            hand_signals_ = std::move(hand_signals);
            profiles_ = std::move(profiles);
            return true;
        } catch (...) {
            return false;
        }
    }

    void install_final_render_hook() noexcept {
        if (final_render_hook_attempted_) return;
        final_render_hook_attempted_ = true;

        // Apply the hand morph after CPed::PreRender and restore its shared
        // geometry immediately after RpClumpRender. GTA's HAnim and Skin are
        // never replaced or expanded.
        if (injector::ReadMemory<std::uint8_t>(
                entity_render_clump_call_address, true) != 0xE8U) {
            log("ERROR: la llamada final a RpClumpRender no es compatible.");
            return;
        }
        const auto previous{injector::MakeCALL(
            entity_render_clump_call_address,
            injector::raw_ptr(&render_clump_with_animated_fingers))};
        original_clump_render_ = previous.get();
        final_render_hook_installed_ = original_clump_render_ != nullptr;
        log(final_render_hook_installed_
                ? "Deformacion final de dedos enlazada a RpClumpRender."
                : "ERROR: no se pudo enlazar la deformacion final de dedos.");
    }

    static RpClump* __cdecl render_clump_with_animated_fingers(
        RpClump* clump) noexcept {
        PedEntry* const entry{instance_ != nullptr
            ? instance_->prepare_for_clump_render(clump)
            : nullptr};
        RpClump* const result{original_clump_render_ != nullptr
            ? original_clump_render_(clump)
            : clump};
        if (instance_ != nullptr && entry != nullptr) {
            instance_->restore_hand_profiles(*entry);
        }
        return result;
    }

    void install_hand_object_hooks() noexcept {
        if (hand_object_hooks_installed_) return;

        const auto pre_render_address{
            injector::ReadMemory<std::uintptr_t>(hand_object_pre_render_slot, true)};
        const auto render_address{
            injector::ReadMemory<std::uintptr_t>(hand_object_render_slot, true)};
        if (pre_render_address == 0 || render_address == 0) {
            log("ERROR: no se encontraron los métodos nativos de CHandObject.");
            return;
        }

        original_hand_pre_render_ = pre_render_address;
        original_hand_render_ = render_address;
        injector::WriteMemory<std::uintptr_t>(
            hand_object_pre_render_slot,
            reinterpret_cast<std::uintptr_t>(&hand_object_pre_render_hook), true);
        injector::WriteMemory<std::uintptr_t>(
            hand_object_render_slot,
            reinterpret_cast<std::uintptr_t>(&hand_object_render_hook), true);
        hand_object_hooks_installed_ = true;
        log("Manos externas nativas anuladas; las señales deforman la geometría del ped.");
    }

    [[nodiscard]] bool should_suppress_native_hand(
        const CHandObject* hand) const noexcept {
        if (!settings_.enabled || hand == nullptr || hand->m_pPed == nullptr ||
            hand->m_pPed->m_pRwClump == nullptr || profiles_.empty()) {
            return false;
        }
        CPed* const ped{hand->m_pPed};
        const bool is_player{ped == FindPlayerPed()};
        if ((is_player && !settings_.enable_player) ||
            (!is_player && !settings_.enable_npcs)) {
            return false;
        }
        AtomicList atomics{};
        RpClumpForAllAtomics(ped->m_pRwClump, collect_atomic, &atomics);
        if (atomics.overflow) return false;
        for (std::size_t index{}; index < atomics.size; ++index) {
            if (find_profile(RpAtomicGetGeometry(atomics.values[index])) != nullptr) {
                return true;
            }
        }
        return false;
    }

    static void __fastcall hand_object_pre_render_hook(
        CHandObject* hand,
        void*) noexcept {
        if (instance_ != nullptr && instance_->should_suppress_native_hand(hand)) {
            hand->bIsVisible = false;
            hand->m_nObjectFlags.bDoNotRender = true;
            return;
        }
        if (original_hand_pre_render_ != 0) {
            injector::thiscall<void(CHandObject*)>::call(
                original_hand_pre_render_, hand);
        }
    }

    static void __fastcall hand_object_render_hook(
        CHandObject* hand,
        void*) noexcept {
        if (instance_ != nullptr && instance_->should_suppress_native_hand(hand)) {
            return;
        }
        if (original_hand_render_ != 0) {
            injector::thiscall<void(CHandObject*)>::call(original_hand_render_, hand);
        }
    }

    [[nodiscard]] const RuntimeProfile* find_profile(
        const RpGeometry* geometry) const noexcept {
        if (geometry == nullptr) return nullptr;
        const auto count{static_cast<std::uint32_t>(RpGeometryGetNumVertices(geometry))};
        const std::uint64_t hash{hash_geometry(geometry)};
        for (const auto& profile : profiles_) {
            if (profile.vertex_count == count && profile.geometry_hash == hash) {
                return &profile;
            }
        }
        return nullptr;
    }

    [[nodiscard]] PedEntry* find_entry_by_ped(CPed* ped) noexcept {
        for (auto& entry : entries_) if (entry.ped == ped) return &entry;
        return nullptr;
    }

    [[nodiscard]] PedEntry* find_entry_by_clump(RpClump* clump) noexcept {
        for (auto& entry : entries_) if (entry.clump == clump) return &entry;
        return nullptr;
    }

    [[nodiscard]] PedEntry* reserve_entry(CPed* ped) noexcept {
        if (PedEntry* existing{find_entry_by_ped(ped)}; existing != nullptr) {
            return existing;
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

    static void destroy_entry(PedEntry& entry) noexcept {
        entry = {};
    }

    void release_all() noexcept {
        for (auto& entry : entries_) destroy_entry(entry);
    }

    void log_injection_failure(const char* message) noexcept {
        constexpr std::size_t maximum_failure_logs{24};
        if (injection_failure_logs_ >= maximum_failure_logs) return;
        ++injection_failure_logs_;
        log(message);
    }

    [[nodiscard]] bool prepare_hand_profiles(PedEntry& entry) {
        RpClump* clump{entry.ped != nullptr ? entry.ped->m_pRwClump : nullptr};
        if (clump == nullptr) return false;
        RpHAnimHierarchy* primary_hierarchy{GetAnimHierarchyFromSkinClump(clump)};
        CAnimBlendClumpData* data{RpClumpGetAnimBlendClumpData(clump)};
        if (primary_hierarchy == nullptr || data == nullptr ||
            data->m_nNumFrames < native_bone_count || data->m_pFrames == nullptr) {
            std::array<char, 192> message{};
            std::snprintf(
                message.data(), message.size(),
                "Inyeccion omitida: hierarchy=%p animData=%p frames=%d frameData=%p.",
                static_cast<void*>(primary_hierarchy), static_cast<void*>(data),
                data != nullptr ? data->m_nNumFrames : -1,
                data != nullptr ? static_cast<void*>(data->m_pFrames) : nullptr);
            log_injection_failure(message.data());
            return false;
        }
        AtomicList atomics{};
        RpClumpForAllAtomics(clump, collect_atomic, &atomics);
        if (atomics.overflow || atomics.size == 0) {
            log_injection_failure("Inyeccion omitida: clump sin atomics utilizables.");
            return false;
        }
        const AtomicList clump_atomics{atomics};
        atomics = {};
        std::array<const RuntimeProfile*, max_atomics_per_ped> profiles{};
        std::array<std::size_t, max_atomics_per_ped> atomic_binding_indices{};
        std::array<HierarchyPlan, max_atomics_per_ped> plans{};
        std::size_t plan_count{};
        for (std::size_t source_index{}; source_index < clump_atomics.size;
             ++source_index) {
            RpAtomic* const atomic{clump_atomics.values[source_index]};
            RpGeometry* geometry{RpAtomicGetGeometry(atomic)};
            RpSkin* skin{geometry != nullptr ? RpSkinGeometryGetSkin(geometry) : nullptr};
            RpHAnimHierarchy* hierarchy{RpSkinAtomicGetHAnimHierarchy(atomic)};
            const RuntimeProfile* const profile{find_profile(geometry)};
            if (skin == nullptr || RpSkinGetNumBones(skin) != native_bone_count ||
                profile == nullptr || hierarchy == nullptr ||
                hierarchy->numNodes != native_bone_count) {
                continue;
            }
            const std::size_t index{atomics.size++};
            atomics.values[index] = atomic;
            profiles[index] = profile;
            std::size_t binding_index{plan_count};
            for (std::size_t plan_index{}; plan_index < plan_count; ++plan_index) {
                if (plans[plan_index].source_hierarchy == hierarchy &&
                    plans[plan_index].profile == profiles[index]) {
                    binding_index = plan_index;
                    break;
                }
            }
            if (binding_index == plan_count) {
                plans[plan_count] = {hierarchy, profiles[index]};
                ++plan_count;
            }
            atomic_binding_indices[index] = binding_index;
        }
        if (atomics.size == 0) {
            RpAtomic* const atomic{clump_atomics.values[0]};
            RpGeometry* const geometry{atomic != nullptr
                ? RpAtomicGetGeometry(atomic)
                : nullptr};
            RpSkin* const skin{geometry != nullptr
                ? RpSkinGeometryGetSkin(geometry)
                : nullptr};
            RpHAnimHierarchy* const hierarchy{atomic != nullptr
                ? RpSkinAtomicGetHAnimHierarchy(atomic)
                : nullptr};
            std::array<char, 256> message{};
            std::snprintf(
                message.data(), message.size(),
                "Inyeccion omitida: atomics=%u vertices=%d hash=%016llX skinBones=%u hierarchyNodes=%d profile=0.",
                static_cast<unsigned>(clump_atomics.size),
                geometry != nullptr ? RpGeometryGetNumVertices(geometry) : -1,
                static_cast<unsigned long long>(hash_geometry(geometry)),
                skin != nullptr ? static_cast<unsigned>(RpSkinGetNumBones(skin)) : 0U,
                hierarchy != nullptr ? hierarchy->numNodes : -1);
            log_injection_failure(message.data());
            return false;
        }
        entry.clump = clump;
        entry.binding_count = plan_count;
        for (std::size_t index{}; index < plan_count; ++index) {
            entry.bindings[index].source_hierarchy =
                plans[index].source_hierarchy;
            entry.bindings[index].profile = plans[index].profile;
        }
        entry.atomic_count = atomics.size;
        for (std::size_t index{}; index < atomics.size; ++index) {
            RpGeometry* const geometry{
                RpAtomicGetGeometry(atomics.values[index])};
            const RpMorphTarget* const morph{
                RpGeometryGetMorphTarget(geometry, 0)};
            const RwV3d* const vertices{RpMorphTargetGetVertices(morph)};
            const RwV3d* const normals{RpMorphTargetGetVertexNormals(morph)};
            if (vertices == nullptr) {
                destroy_entry(entry);
                log_injection_failure(
                    "Inyeccion omitida: geometria sin vertices base para el perfil.");
                return false;
            }
            const std::size_t vertex_count{profiles[index]->vertex_count};
            entry.atomics[index].atomic = atomics.values[index];
            entry.atomics[index].binding_index = atomic_binding_indices[index];
            entry.atomics[index].base_vertices.assign(
                vertices, vertices + vertex_count);
            if (normals != nullptr) {
                entry.atomics[index].base_normals.assign(
                    normals, normals + vertex_count);
            }
        }
        log("Ped preparado: Skin nativa 32 y perfiles morph de manos activos.");
        return true;
    }

    void on_ped_render(CPed* ped) noexcept {
        static_cast<void>(prepare_entry_for_ped(ped));
    }

    [[nodiscard]] bool is_enabled_for(const CPed* ped) const noexcept {
        if (!settings_.enabled || profiles_.empty() || ped == nullptr ||
            ped->m_pRwClump == nullptr) {
            return false;
        }
        const bool is_player{ped == FindPlayerPed()};
        return (is_player && settings_.enable_player) ||
            (!is_player && settings_.enable_npcs);
    }

    [[nodiscard]] PedEntry* prepare_entry_for_ped(CPed* ped) noexcept {
        if (!is_enabled_for(ped)) return nullptr;
        PedEntry* entry{reserve_entry(ped)};
        if (entry == nullptr) return nullptr;
        if (entry->clump == nullptr) {
            try {
                if (!prepare_hand_profiles(*entry)) {
                    destroy_entry(*entry);
                    return nullptr;
                }
            } catch (...) {
                destroy_entry(*entry);
                log("ERROR: excepción al preparar un ped; se conservó su modelo nativo.");
                return nullptr;
            }
        }
        return entry;
    }

    [[nodiscard]] static CPed* find_ped_for_clump(RpClump* clump) noexcept {
        auto* const pool{CPools::ms_pPedPool};
        if (clump == nullptr || pool == nullptr || pool->m_pObjects == nullptr ||
            pool->m_byteMap == nullptr) {
            return nullptr;
        }
        for (int index{}; index < pool->m_nSize; ++index) {
            CPed* const ped{pool->GetAt(index)};
            if (ped != nullptr && ped->m_pRwClump == clump) return ped;
        }
        return nullptr;
    }

    void on_game_process() noexcept {
        if (!settings_.enabled) return;
        // Handies is loaded before some graphics ASIs. Installing on the first
        // gameplay tick preserves and chains the final target chosen by them.
        install_final_render_hook();
        for (auto& entry : entries_) {
            if (entry.ped == nullptr) continue;
            update_finger_state(entry);
        }
    }

    void update_finger_state(PedEntry& entry) const noexcept {
        if (entry.ped == nullptr) return;
        const float target_fucku{is_playing_fucku(*entry.ped) ? 1.0F : 0.0F};
        const float fucku_step{std::clamp(
            settings_.fucku_transition_speed * CTimer::ms_fTimeStep, 0.001F, 1.0F)};
        entry.fucku_blend += std::clamp(
            target_fucku - entry.fucku_blend, -fucku_step, fucku_step);
        const float target_grip{wants_closed_fist(*entry.ped) ? 1.0F : 0.0F};
        const float grip_step{std::clamp(
            settings_.grip_transition_speed * CTimer::ms_fTimeStep, 0.001F, 1.0F)};
        entry.grip += std::clamp(target_grip - entry.grip, -grip_step, grip_step);
    }

    [[nodiscard]] PedEntry* prepare_for_clump_render(RpClump* clump) noexcept {
        PedEntry* entry{find_entry_by_clump(clump)};
        if (entry == nullptr) {
            entry = prepare_entry_for_ped(find_ped_for_clump(clump));
            if (entry == nullptr) return nullptr;
            update_finger_state(*entry);
        }
        apply_finger_matrices(*entry);
        return entry;
    }

    void apply_finger_matrices(PedEntry& entry) noexcept {
        const HandSignalState signal{entry.ped != nullptr
            ? read_hand_signal_state(*entry.ped)
            : HandSignalState{}};
        for (std::size_t binding_index{};
             binding_index < entry.binding_count;
            ++binding_index) {
            RuntimeBinding& binding{entry.bindings[binding_index]};
            if (binding.source_hierarchy == nullptr ||
                binding.profile == nullptr ||
                binding.source_hierarchy->numNodes != native_bone_count ||
                binding.source_hierarchy->pNodeInfo == nullptr) {
                continue;
            }
            RwMatrix* const matrices{binding.pose_matrices.data()};
            for (int index{}; index < native_bone_count; ++index) {
                RwMatrixInvert(
                    &matrices[index],
                    &binding.profile->inverse_matrices[
                        static_cast<std::size_t>(index)]);
            }
            for (int side{}; side < 2; ++side) {
                const int hand_index{RpHAnimIDGetIndex(
                    binding.source_hierarchy,
                    side == 0 ? left_hand_id : right_hand_id)};
                if (hand_index < 0 || hand_index >= native_bone_count) continue;
                for (int source_id{3}; source_id <= 17; ++source_id) {
                    const int target_index{
                        native_bone_count + side * finger_bones_per_hand +
                        source_id - 3};
                    const int parent_source{parent_source_id(source_id)};
                    const int parent_index{parent_source == 2
                        ? hand_index
                        : native_bone_count + side * finger_bones_per_hand +
                            parent_source - 3};
                    if (target_index < 0 || target_index >= runtime_bone_count ||
                        parent_index < 0 || parent_index >= runtime_bone_count) {
                        break;
                    }
                    const std::size_t finger_index{
                        static_cast<std::size_t>(source_id - 3)};
                    RtQuat rotation{normalized_lerp(
                        poses_[side][0][finger_index],
                        poses_[side][1][finger_index],
                        std::clamp(entry.grip, 0.0F, 1.0F))};
                    if (side == 1) {
                        rotation = normalized_lerp(
                            rotation, poses_[side][2][finger_index],
                            smoothstep(entry.fucku_blend));
                    }
                    const bool signal_active{
                        signal.animation_index >= 0 &&
                        ((side == 0 && signal.left) ||
                         (side == 1 && signal.right))};
                    if (signal_active) {
                        const HandSignalAnimation& animation{
                            hand_signals_[side][static_cast<std::size_t>(
                                signal.animation_index)]};
                        rotation = sample_track(
                            animation.tracks[finger_index],
                            std::min(signal.time, animation.duration));
                    }
                    RwMatrix local{};
                    RtQuatConvertToMatrix(&rotation, &local);
                    local.pos = binding.profile->translations[
                        static_cast<std::size_t>(side * finger_bones_per_hand) +
                        finger_index];
                    RwMatrixMultiply(&matrices[target_index], &local,
                                     &matrices[parent_index]);
                }
            }
        }
        deform_hand_profiles(entry);
    }

    void deform_hand_profiles(PedEntry& entry) noexcept {
        constexpr float minimum_weight{1.0e-6F};
        for (std::size_t atomic_index{}; atomic_index < entry.atomic_count;
             ++atomic_index) {
            RuntimeAtomic& item{entry.atomics[atomic_index]};
            if (item.atomic == nullptr || item.binding_index >= entry.binding_count) {
                continue;
            }
            RuntimeBinding& binding{entry.bindings[item.binding_index]};
            const RuntimeProfile* const profile{binding.profile};
            RpGeometry* const geometry{RpAtomicGetGeometry(item.atomic)};
            if (profile == nullptr || geometry == nullptr ||
                item.base_vertices.size() != profile->vertex_count ||
                profile->weights.size() != profile->vertex_count ||
                profile->indices.size() != profile->vertex_count) {
                continue;
            }
            std::array<RwMatrix, runtime_bone_count> skin_matrices{};
            for (int bone{}; bone < runtime_bone_count; ++bone) {
                RwMatrixMultiply(
                    &skin_matrices[static_cast<std::size_t>(bone)],
                    &profile->inverse_matrices[static_cast<std::size_t>(bone)],
                    &binding.pose_matrices[static_cast<std::size_t>(bone)]);
            }
            const bool has_normals{
                item.base_normals.size() == item.base_vertices.size()};
            const RwInt32 lock_flags{rpGEOMETRYLOCKVERTICES |
                (has_normals ? rpGEOMETRYLOCKNORMALS : 0)};
            if (RpGeometryLock(geometry, lock_flags) == nullptr) continue;
            RpMorphTarget* const morph{RpGeometryGetMorphTarget(geometry, 0)};
            RwV3d* const vertices{RpMorphTargetGetVertices(morph)};
            RwV3d* const normals{has_normals
                ? RpMorphTargetGetVertexNormals(morph)
                : nullptr};
            if (vertices == nullptr || (has_normals && normals == nullptr)) {
                RpGeometryUnlock(geometry);
                continue;
            }
            for (std::size_t vertex{}; vertex < item.base_vertices.size(); ++vertex) {
                const RwUInt32 packed{profile->indices[vertex]};
                const std::array<RwUInt32, 4> bone_indices{
                    packed & 0xFFU,
                    (packed >> 8U) & 0xFFU,
                    (packed >> 16U) & 0xFFU,
                    (packed >> 24U) & 0xFFU};
                const RwMatrixWeights& packed_weights{profile->weights[vertex]};
                const std::array<float, 4> weights{
                    packed_weights.w0, packed_weights.w1,
                    packed_weights.w2, packed_weights.w3};
                bool belongs_to_finger_profile{};
                for (std::size_t slot{}; slot < weights.size(); ++slot) {
                    if (weights[slot] > minimum_weight &&
                        bone_indices[slot] >= native_bone_count) {
                        belongs_to_finger_profile = true;
                        break;
                    }
                }
                if (!belongs_to_finger_profile) continue;

                RwV3d morphed{};
                RwV3d morphed_normal{};
                for (std::size_t slot{}; slot < weights.size(); ++slot) {
                    const float weight{weights[slot]};
                    const RwUInt32 bone{bone_indices[slot]};
                    if (weight <= minimum_weight || bone >= runtime_bone_count) {
                        continue;
                    }
                    RwV3d transformed{};
                    RwV3dTransformPoint(
                        &transformed, &item.base_vertices[vertex],
                        &skin_matrices[bone]);
                    morphed.x += transformed.x * weight;
                    morphed.y += transformed.y * weight;
                    morphed.z += transformed.z * weight;
                    if (has_normals) {
                        RwV3d transformed_normal{};
                        RwV3dTransformVector(
                            &transformed_normal, &item.base_normals[vertex],
                            &skin_matrices[bone]);
                        morphed_normal.x += transformed_normal.x * weight;
                        morphed_normal.y += transformed_normal.y * weight;
                        morphed_normal.z += transformed_normal.z * weight;
                    }
                }
                vertices[vertex] = morphed;
                if (has_normals) {
                    const float length_squared{
                        morphed_normal.x * morphed_normal.x +
                        morphed_normal.y * morphed_normal.y +
                        morphed_normal.z * morphed_normal.z};
                    if (length_squared > 1.0e-12F) {
                        const float inverse_length{
                            1.0F / std::sqrt(length_squared)};
                        morphed_normal.x *= inverse_length;
                        morphed_normal.y *= inverse_length;
                        morphed_normal.z *= inverse_length;
                    }
                    normals[vertex] = morphed_normal;
                }
            }
            RpGeometryUnlock(geometry);
        }
    }

    static void restore_hand_profiles(PedEntry& entry) noexcept {
        for (std::size_t atomic_index{}; atomic_index < entry.atomic_count;
             ++atomic_index) {
            RuntimeAtomic& item{entry.atomics[atomic_index]};
            RpGeometry* const geometry{item.atomic != nullptr
                ? RpAtomicGetGeometry(item.atomic)
                : nullptr};
            if (geometry == nullptr || item.base_vertices.empty()) continue;
            const bool has_normals{
                item.base_normals.size() == item.base_vertices.size()};
            const RwInt32 lock_flags{rpGEOMETRYLOCKVERTICES |
                (has_normals ? rpGEOMETRYLOCKNORMALS : 0)};
            if (RpGeometryLock(geometry, lock_flags) == nullptr) continue;
            RpMorphTarget* const morph{RpGeometryGetMorphTarget(geometry, 0)};
            RwV3d* const vertices{RpMorphTargetGetVertices(morph)};
            RwV3d* const normals{has_normals
                ? RpMorphTargetGetVertexNormals(morph)
                : nullptr};
            if (vertices != nullptr) {
                std::memcpy(
                    vertices, item.base_vertices.data(),
                    sizeof(RwV3d) * item.base_vertices.size());
            }
            if (normals != nullptr) {
                std::memcpy(
                    normals, item.base_normals.data(),
                    sizeof(RwV3d) * item.base_normals.size());
            }
            RpGeometryUnlock(geometry);
        }
    }

    void remove_for_ped(const CPed* ped) noexcept {
        if (ped == nullptr) return;
        for (auto& entry : entries_) {
            if (entry.ped == ped) {
                destroy_entry(entry);
                return;
            }
        }
    }

    Settings settings_{};
    PoseTable poses_{};
    HandSignalTable hand_signals_{};
    std::vector<RuntimeProfile> profiles_{};
    std::array<PedEntry, max_tracked_peds> entries_{};
    std::array<char, MAX_PATH> ini_path_{};
    std::array<char, MAX_PATH> log_path_{};
    std::array<char, MAX_PATH> fallback_log_path_{};
    std::array<char, MAX_PATH> data_path_{};

    inline static HandiesMod* instance_{};
    inline static ClumpRenderFunction original_clump_render_{};
    inline static std::uintptr_t original_hand_pre_render_{0x59ECD0};
    inline static std::uintptr_t original_hand_render_{0x59EE80};
    bool hand_object_hooks_installed_{};
    bool final_render_hook_attempted_{};
    bool final_render_hook_installed_{};
    std::size_t injection_failure_logs_{};
};

HandiesMod handies_mod{};

} // namespace handies
