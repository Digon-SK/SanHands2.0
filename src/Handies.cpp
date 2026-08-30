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
#include "CAnimBlendHierarchy.h"
#include "CAnimBlendStaticAssociation.h"
#include "CAnimManager.h"
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
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace handies {
namespace {

constexpr std::uintptr_t entity_render_clump_call_address{0x53439C};
constexpr std::size_t max_tracked_peds{256};
constexpr std::size_t max_atomics_per_ped{16};
constexpr int native_bone_count{32};
constexpr int native_hand_signal_count{5};
constexpr int maximum_animation_groups{139};
constexpr char data_file_name[]{"Handies.dat"};
constexpr char ini_file_name[]{"Handies.ini"};
constexpr char log_file_name[]{"Handies.log"};
constexpr std::array<char, 8> data_magic{'H', 'N', 'D', '2', 'D', 'A', 'T', '\0'};
constexpr std::uint32_t data_version{5};
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

struct MorphTarget {
    std::string name{};
    std::vector<RwV3d> positions{};
    std::vector<RwV3d> normals{};
};

struct MorphTemplate {
    std::string name{};
    std::uint32_t vertex_count{};
    std::vector<MorphTarget> targets{};
};

struct MorphWeightKey {
    float time{};
    float weight{};
};

struct HandAnimation {
    std::string name{};
    float duration{};
    std::vector<MorphWeightKey> keys{};
};

struct RuntimeHand {
    std::uint32_t start{};
    std::uint32_t count{};
    std::uint32_t template_index{};
    RwMatrix transform{};
};

struct RuntimeProfile {
    std::uint64_t geometry_hash{};
    std::uint32_t vertex_count{};
    std::array<RuntimeHand, 2> hands{};
};

struct HandSequenceProfile {
    std::string name{};
    int left_animation{-1};
    int right_animation{-1};
    bool sync_to_ped{true};
    bool loop{};
    float speed{1.0F};
    float weight{1.0F};
    int priority{};
};

struct PedAnimationMapping {
    unsigned short group{};
    short animation{};
    std::size_t profile_index{};
};

struct ActiveHandSequence {
    const HandSequenceProfile* profile{};
    const CAnimBlendAssociation* association{};
};

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

struct RuntimeAtomic {
    RpAtomic* atomic{};
    const RuntimeProfile* profile{};
    std::vector<RwV3d> base_vertices{};
    std::vector<RwV3d> base_normals{};
};

struct PedEntry {
    CPed* ped{};
    RpClump* clump{};
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

[[nodiscard]] std::string read_ini_string(
    const char* path,
    const char* section,
    const char* key,
    const char* fallback = "") {
    std::array<char, 256> value{};
    GetPrivateProfileStringA(
        section, key, fallback, value.data(),
        static_cast<DWORD>(value.size()), path);
    return value.data();
}

[[nodiscard]] std::string trim_copy(std::string_view value) {
    const auto first{value.find_first_not_of(" \t\r\n")};
    if (first == std::string_view::npos) return {};
    const auto last{value.find_last_not_of(" \t\r\n")};
    return std::string{value.substr(first, last - first + 1)};
}

[[nodiscard]] bool equals_ignore_case(
    std::string_view first,
    std::string_view second) noexcept {
    if (first.size() != second.size()) return false;
    for (std::size_t index{}; index < first.size(); ++index) {
        const unsigned char a{static_cast<unsigned char>(first[index])};
        const unsigned char b{static_cast<unsigned char>(second[index])};
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

[[nodiscard]] float parse_float(
    const std::string& value,
    float fallback) noexcept {
    char* parse_end{};
    const float parsed{std::strtof(value.c_str(), &parse_end)};
    return parse_end != value.c_str() && std::isfinite(parsed)
        ? parsed
        : fallback;
}

[[nodiscard]] int parse_int(
    const std::string& value,
    int fallback) noexcept {
    char* parse_end{};
    const long parsed{std::strtol(value.c_str(), &parse_end, 10)};
    if (parse_end == value.c_str() ||
        parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return fallback;
    }
    return static_cast<int>(parsed);
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

    CTask* const task{ped.m_pIntelligence->m_TaskMgr.FindActiveTaskByType(
        TASK_SIMPLE_HANDSIGNAL_ANIM)};
    if (task == nullptr) return result;

    const auto* const signal{
        reinterpret_cast<const NativeHandSignalTaskView*>(task)};
    CAnimBlendAssociation* const association{signal->body_animation};
    const int animation_index{
        signal->hand_animation_id - static_cast<int>(ANIM_HANDSIGNAL_GSIGN1)};
    if (association == nullptr || animation_index < 0 ||
        animation_index >= native_hand_signal_count ||
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

[[nodiscard]] float sample_track(
    const HandAnimation& animation,
    float time) noexcept {
    if (animation.keys.empty()) return 0.0F;
    if (time <= animation.keys.front().time) {
        return animation.keys.front().weight;
    }
    if (time >= animation.keys.back().time) {
        return animation.keys.back().weight;
    }
    for (std::size_t index{1}; index < animation.keys.size(); ++index) {
        const MorphWeightKey& second{animation.keys[index]};
        if (time > second.time) continue;
        const MorphWeightKey& first{animation.keys[index - 1]};
        const float span{second.time - first.time};
        const float amount{span > 1.0e-6F
            ? std::clamp((time - first.time) / span, 0.0F, 1.0F)
            : 0.0F};
        return std::clamp(
            first.weight + (second.weight - first.weight) * amount,
            0.0F, 1.0F);
    }
    return std::clamp(animation.keys.back().weight, 0.0F, 1.0F);
}

[[nodiscard]] const MorphTarget* find_morph_target(
    const MorphTemplate& morph_template,
    std::string_view name) noexcept {
    for (const auto& target : morph_template.targets) {
        if (equals_ignore_case(target.name, name)) return &target;
    }
    return nullptr;
}

void normalize_vector(RwV3d& value) noexcept {
    const float length_squared{
        value.x * value.x + value.y * value.y + value.z * value.z};
    if (length_squared <= 1.0e-12F) return;
    const float inverse_length{1.0F / std::sqrt(length_squared)};
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
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
            configuration_retry_count_ = 0;
            next_configuration_retry_ms_ = 0;
            configured_sequence_logged_ = false;
            native_signal_logged_ = false;
            load_settings();
            install_final_render_hook();
            if (profiles_.empty() && !load_runtime_data()) {
                log("ERROR: Handies.dat sigue sin estar disponible al iniciar partida.");
            } else {
                install_hand_object_hooks();
                load_sequence_configuration();
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

    [[nodiscard]] int find_hand_animation_index(
        std::string_view name) const noexcept {
        if (name.empty() || equals_ignore_case(name, "none")) return -1;
        for (std::size_t index{}; index < hand_animations_.size(); ++index) {
            if (equals_ignore_case(hand_animations_[index].name, name)) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    [[nodiscard]] std::size_t find_sequence_profile_index(
        std::string_view name) const noexcept {
        for (std::size_t index{}; index < sequence_profiles_.size(); ++index) {
            if (equals_ignore_case(sequence_profiles_[index].name, name)) {
                return index;
            }
        }
        return sequence_profiles_.size();
    }

    void add_animation_mappings(
        std::string_view source_text,
        std::size_t profile_index) {
        const std::string source{trim_copy(source_text)};
        const auto separator{source.rfind('.')};
        if (separator == std::string::npos || separator == 0 ||
            separator + 1 >= source.size()) {
            return;
        }
        std::string owner{trim_copy(std::string_view{source}.substr(0, separator))};
        const std::string animation_name{
            trim_copy(std::string_view{source}.substr(separator + 1))};
        if (owner.size() > 4 && equals_ignore_case(
                std::string_view{owner}.substr(owner.size() - 4), ".ifp")) {
            owner.resize(owner.size() - 4);
        }
        const int group_count{std::clamp(
            CAnimManager::ms_numAnimAssocDefinitions,
            0, maximum_animation_groups)};
        const auto add_for_group = [&](int group) {
            if (group < 0 || group >= group_count ||
                CAnimManager::ms_aAnimAssocGroups == nullptr) {
                return;
            }
            const CAnimBlendAssocGroup& assoc_group{
                CAnimManager::ms_aAnimAssocGroups[group]};
            if (assoc_group.m_pAssociations == nullptr ||
                assoc_group.m_nNumAnimations == 0) {
                return;
            }
            CAnimBlendStaticAssociation* const association{
                CAnimManager::GetAnimAssociation(
                    group, animation_name.c_str())};
            if (association == nullptr) return;
            const auto duplicate{std::find_if(
                animation_mappings_.begin(), animation_mappings_.end(),
                [association](const PedAnimationMapping& mapping) {
                    return mapping.group == association->m_nAnimGroup &&
                           mapping.animation == association->m_nAnimId;
                })};
            if (duplicate != animation_mappings_.end()) {
                duplicate->profile_index = profile_index;
            } else {
                animation_mappings_.push_back({
                    association->m_nAnimGroup,
                    association->m_nAnimId,
                    profile_index});
            }
        };

        // These native groups are available by fixed ID even when their
        // descriptor names have not yet been exposed by CAnimManager.
        if (equals_ignore_case(owner, "handsignal")) {
            add_for_group(static_cast<int>(ANIM_GROUP_HANDSIGNAL));
        } else if (equals_ignore_case(owner, "handsignall")) {
            add_for_group(static_cast<int>(ANIM_GROUP_HANDSIGNALL));
        }
        for (int group{}; group < group_count; ++group) {
            const CAnimBlendAssocGroup& assoc_group{
                CAnimManager::ms_aAnimAssocGroups[group]};
            if (assoc_group.m_pAssociations == nullptr ||
                assoc_group.m_nNumAnimations == 0) {
                continue;
            }
            const char* const group_name{CAnimManager::GetAnimGroupName(group)};
            const char* const block_name{CAnimManager::GetAnimBlockName(group)};
            const bool owner_matches{owner == "*" ||
                (group_name != nullptr && equals_ignore_case(owner, group_name)) ||
                (block_name != nullptr && equals_ignore_case(owner, block_name))};
            if (!owner_matches) continue;
            add_for_group(group);
        }
    }

    void load_sequence_configuration() {
        sequence_profiles_.clear();
        animation_mappings_.clear();
        if (hand_animations_.empty()) return;

        std::array<char, 32768> section_names{};
        const DWORD section_length{GetPrivateProfileSectionNamesA(
            section_names.data(), static_cast<DWORD>(section_names.size()),
            ini_path_.data())};
        if (section_length == 0 ||
            section_length >= section_names.size() - 2) {
            log("ADVERTENCIA: no se pudieron enumerar los perfiles de manos del INI.");
            return;
        }
        constexpr std::string_view profile_prefix{"HandProfile."};
        for (const char* section{section_names.data()}; *section != '\0';
             section += std::strlen(section) + 1) {
            const std::string_view section_name{section};
            if (section_name.size() <= profile_prefix.size() ||
                !equals_ignore_case(
                    section_name.substr(0, profile_prefix.size()),
                    profile_prefix)) {
                continue;
            }
            HandSequenceProfile profile{};
            profile.name = trim_copy(section_name.substr(profile_prefix.size()));
            const std::string left_name{trim_copy(read_ini_string(
                ini_path_.data(), section, "Left", "None"))};
            const std::string right_name{trim_copy(read_ini_string(
                ini_path_.data(), section, "Right", "None"))};
            profile.left_animation = find_hand_animation_index(left_name);
            profile.right_animation = find_hand_animation_index(right_name);
            if ((!equals_ignore_case(left_name, "none") &&
                 profile.left_animation < 0) ||
                (!equals_ignore_case(right_name, "none") &&
                 profile.right_animation < 0)) {
                std::array<char, 256> message{};
                std::snprintf(
                    message.data(), message.size(),
                    "Perfil %s omitido: secuencia IFP Left=%s Right=%s no disponible.",
                    profile.name.c_str(), left_name.c_str(), right_name.c_str());
                log(message.data());
                continue;
            }
            if (profile.left_animation < 0 && profile.right_animation < 0) {
                continue;
            }
            const std::string sync_mode{read_ini_string(
                ini_path_.data(), section, "TimeMode", "Ped")};
            profile.sync_to_ped = !equals_ignore_case(
                trim_copy(sync_mode), "Seconds");
            profile.loop = parse_int(read_ini_string(
                ini_path_.data(), section, "Loop", "0"), 0) != 0;
            profile.speed = std::clamp(parse_float(read_ini_string(
                ini_path_.data(), section, "Speed", "1.0"), 1.0F),
                0.01F, 20.0F);
            profile.weight = std::clamp(parse_float(read_ini_string(
                ini_path_.data(), section, "Weight", "1.0"), 1.0F),
                0.0F, 1.0F);
            profile.priority = std::clamp(parse_int(read_ini_string(
                ini_path_.data(), section, "Priority", "0"), 0),
                -10000, 10000);
            sequence_profiles_.push_back(std::move(profile));
        }

        std::array<char, 32768> mapping_entries{};
        const DWORD mapping_length{GetPrivateProfileSectionA(
            "PedAnimationMappings", mapping_entries.data(),
            static_cast<DWORD>(mapping_entries.size()), ini_path_.data())};
        if (mapping_length > 0 && mapping_length < mapping_entries.size() - 2) {
            for (const char* item{mapping_entries.data()}; *item != '\0';
                 item += std::strlen(item) + 1) {
                const std::string_view entry{item};
                const auto equals{entry.find('=')};
                if (equals == std::string_view::npos) continue;
                const std::string source{trim_copy(entry.substr(0, equals))};
                const std::string profile_name{trim_copy(entry.substr(equals + 1))};
                const std::size_t profile_index{
                    find_sequence_profile_index(profile_name)};
                if (profile_index >= sequence_profiles_.size()) {
                    continue;
                }
                add_animation_mappings(source, profile_index);
            }
        }

        std::array<char, 192> message{};
        std::snprintf(
            message.data(), message.size(),
            "Secuencias configurables: IFP=%u perfiles=%u asociaciones PED=%u.",
            static_cast<unsigned int>(hand_animations_.size()),
            static_cast<unsigned int>(sequence_profiles_.size()),
            static_cast<unsigned int>(animation_mappings_.size()));
        log(message.data());
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
            std::uint32_t template_count{};
            if (!reader.read(template_count) || template_count != 4) return false;
            std::vector<MorphTemplate> morph_templates{};
            morph_templates.reserve(template_count);
            for (std::uint32_t template_index{}; template_index < template_count;
                 ++template_index) {
                MorphTemplate morph_template{};
                std::uint32_t name_length{};
                std::uint32_t target_count{};
                if (!reader.read(name_length) ||
                    !reader.read(morph_template.vertex_count) ||
                    !reader.read(target_count) || name_length == 0 ||
                    name_length > 63 || morph_template.vertex_count == 0 ||
                    morph_template.vertex_count > 10000 || target_count == 0 ||
                    target_count > 32) {
                    return false;
                }
                morph_template.name.resize(name_length);
                if (!reader.read_bytes(morph_template.name.data(), name_length)) {
                    return false;
                }
                morph_template.targets.reserve(target_count);
                for (std::uint32_t target_index{}; target_index < target_count;
                     ++target_index) {
                    MorphTarget target{};
                    std::uint32_t target_name_length{};
                    if (!reader.read(target_name_length) ||
                        target_name_length == 0 || target_name_length > 63) {
                        return false;
                    }
                    target.name.resize(target_name_length);
                    if (!reader.read_bytes(
                            target.name.data(), target_name_length)) {
                        return false;
                    }
                    target.positions.resize(morph_template.vertex_count);
                    target.normals.resize(morph_template.vertex_count);
                    for (auto& position : target.positions) {
                        if (!reader.read(position) || !std::isfinite(position.x) ||
                            !std::isfinite(position.y) ||
                            !std::isfinite(position.z)) {
                            return false;
                        }
                    }
                    for (auto& normal : target.normals) {
                        if (!reader.read(normal) || !std::isfinite(normal.x) ||
                            !std::isfinite(normal.y) || !std::isfinite(normal.z)) {
                            return false;
                        }
                    }
                    morph_template.targets.push_back(std::move(target));
                }
                morph_templates.push_back(std::move(morph_template));
            }
            std::uint32_t hand_animation_count{};
            if (!reader.read(hand_animation_count) || hand_animation_count == 0 ||
                hand_animation_count > 256) {
                return false;
            }
            std::vector<HandAnimation> hand_animations{};
            hand_animations.reserve(hand_animation_count);
            for (std::uint32_t animation_index{};
                 animation_index < hand_animation_count; ++animation_index) {
                HandAnimation animation{};
                std::uint32_t name_length{};
                if (!reader.read(name_length) || name_length == 0 ||
                    name_length > 63) {
                    return false;
                }
                animation.name.resize(name_length);
                if (!reader.read_bytes(animation.name.data(), name_length) ||
                    !reader.read(animation.duration) ||
                    !std::isfinite(animation.duration) ||
                    animation.duration <= 0.0F || animation.duration > 30.0F) {
                    return false;
                }
                for (const auto& previous : hand_animations) {
                    if (equals_ignore_case(previous.name, animation.name)) {
                        return false;
                    }
                }
                std::uint32_t key_count{};
                if (!reader.read(key_count) || key_count == 0 || key_count > 256) {
                    return false;
                }
                animation.keys.resize(key_count);
                float previous_time{-1.0F};
                for (auto& key : animation.keys) {
                    if (!reader.read(key.time) || !reader.read(key.weight) ||
                        !std::isfinite(key.time) ||
                        !std::isfinite(key.weight) || key.time < previous_time ||
                        key.time < 0.0F ||
                        key.time > animation.duration + 1.0e-4F ||
                        key.weight < 0.0F || key.weight > 1.0F) {
                        return false;
                    }
                    previous_time = key.time;
                }
                hand_animations.push_back(std::move(animation));
            }
            std::vector<RuntimeProfile> profiles{};
            profiles.reserve(profile_count);
            for (std::uint32_t profile_index{}; profile_index < profile_count;
                 ++profile_index) {
                RuntimeProfile profile{};
                if (!reader.read(profile.geometry_hash) ||
                    !reader.read(profile.vertex_count) ||
                    profile.vertex_count == 0 || profile.vertex_count > 200000) {
                    return false;
                }
                for (auto& hand : profile.hands) {
                    std::array<float, 16> values{};
                    if (!reader.read(hand.start) || !reader.read(hand.count) ||
                        !reader.read(hand.template_index) ||
                        !reader.read(values) ||
                        hand.template_index >= morph_templates.size() ||
                        hand.count != morph_templates[hand.template_index].vertex_count ||
                        hand.start > profile.vertex_count ||
                        hand.count > profile.vertex_count - hand.start) {
                        return false;
                    }
                    hand.transform.right = {values[0], values[1], values[2]};
                    hand.transform.flags = 0;
                    hand.transform.up = {values[4], values[5], values[6]};
                    hand.transform.pad1 = 0;
                    hand.transform.at = {values[8], values[9], values[10]};
                    hand.transform.pad2 = 0;
                    hand.transform.pos = {values[12], values[13], values[14]};
                    hand.transform.pad3 = 0;
                }
                profiles.push_back(std::move(profile));
            }
            if (reader.remaining() != 0) return false;
            morph_templates_ = std::move(morph_templates);
            hand_animations_ = std::move(hand_animations);
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
        if (primary_hierarchy == nullptr ||
            primary_hierarchy->numNodes != native_bone_count) {
            std::array<char, 192> message{};
            std::snprintf(
                message.data(), message.size(),
                "Inyeccion omitida: hierarchy=%p nodes=%d.",
                static_cast<void*>(primary_hierarchy),
                primary_hierarchy != nullptr ? primary_hierarchy->numNodes : -1);
            log_injection_failure(message.data());
            return false;
        }
        AtomicList atomics{};
        RpClumpForAllAtomics(clump, collect_atomic, &atomics);
        if (atomics.overflow || atomics.size == 0) {
            log_injection_failure("Inyeccion omitida: clump sin atomics utilizables.");
            return false;
        }
        std::array<const RuntimeProfile*, max_atomics_per_ped> profiles{};
        AtomicList matched{};
        for (std::size_t source_index{}; source_index < atomics.size;
             ++source_index) {
            RpAtomic* const atomic{atomics.values[source_index]};
            RpGeometry* geometry{RpAtomicGetGeometry(atomic)};
            RpSkin* skin{geometry != nullptr ? RpSkinGeometryGetSkin(geometry) : nullptr};
            RpHAnimHierarchy* hierarchy{RpSkinAtomicGetHAnimHierarchy(atomic)};
            const RuntimeProfile* const profile{find_profile(geometry)};
            if (skin == nullptr || RpSkinGetNumBones(skin) != native_bone_count ||
                profile == nullptr || hierarchy == nullptr ||
                hierarchy->numNodes != native_bone_count) {
                continue;
            }
            const std::size_t index{matched.size++};
            matched.values[index] = atomic;
            profiles[index] = profile;
        }
        if (matched.size == 0) {
            RpAtomic* const atomic{atomics.values[0]};
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
                static_cast<unsigned>(atomics.size),
                geometry != nullptr ? RpGeometryGetNumVertices(geometry) : -1,
                static_cast<unsigned long long>(hash_geometry(geometry)),
                skin != nullptr ? static_cast<unsigned>(RpSkinGetNumBones(skin)) : 0U,
                hierarchy != nullptr ? hierarchy->numNodes : -1);
            log_injection_failure(message.data());
            return false;
        }
        entry.clump = clump;
        entry.atomic_count = matched.size;
        for (std::size_t index{}; index < matched.size; ++index) {
            RpGeometry* const geometry{
                RpAtomicGetGeometry(matched.values[index])};
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
            entry.atomics[index].atomic = matched.values[index];
            entry.atomics[index].profile = profiles[index];
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
        constexpr unsigned int retry_interval_ms{1000U};
        constexpr unsigned int maximum_retries{10U};
        const unsigned int now{CTimer::m_snTimeInMilliseconds};
        if (animation_mappings_.empty() &&
            configuration_retry_count_ < maximum_retries &&
            now >= next_configuration_retry_ms_) {
            try {
                load_sequence_configuration();
            } catch (...) {
                log("ERROR: no se pudo reintentar la configuracion de secuencias.");
            }
            ++configuration_retry_count_;
            next_configuration_retry_ms_ = now + retry_interval_ms;
        }
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

    [[nodiscard]] ActiveHandSequence find_active_hand_sequence(
        const CPed& ped) const noexcept {
        ActiveHandSequence result{};
        if (ped.m_pRwClump == nullptr || animation_mappings_.empty()) {
            return result;
        }
        int selected_priority{std::numeric_limits<int>::min()};
        float selected_blend{-1.0F};
        for (CAnimBlendAssociation* association{
                 RpAnimBlendClumpGetFirstAssociation(ped.m_pRwClump)};
             association != nullptr;
             association = RpAnimBlendGetNextAssociation(association)) {
            if (association->m_fBlendAmount <= minimum_visible_animation_blend) {
                continue;
            }
            for (const auto& mapping : animation_mappings_) {
                if (mapping.group != association->m_nAnimGroup ||
                    mapping.animation != association->m_nAnimId ||
                    mapping.profile_index >= sequence_profiles_.size()) {
                    continue;
                }
                const HandSequenceProfile& profile{
                    sequence_profiles_[mapping.profile_index]};
                if (profile.priority > selected_priority ||
                    (profile.priority == selected_priority &&
                     association->m_fBlendAmount > selected_blend)) {
                    result.profile = &profile;
                    result.association = association;
                    selected_priority = profile.priority;
                    selected_blend = association->m_fBlendAmount;
                }
            }
        }
        return result;
    }

    [[nodiscard]] static float sequence_time(
        const HandAnimation& animation,
        const HandSequenceProfile& profile,
        const CAnimBlendAssociation& association) noexcept {
        float time{};
        if (profile.sync_to_ped && association.m_pHierarchy != nullptr &&
            association.m_pHierarchy->m_fTotalTime > 1.0e-6F) {
            const float progress{std::max(association.m_fCurrentTime, 0.0F) /
                association.m_pHierarchy->m_fTotalTime};
            time = progress * animation.duration * profile.speed;
        } else {
            time = std::max(association.m_fCurrentTime, 0.0F) * profile.speed;
        }
        if (profile.loop && animation.duration > 1.0e-6F) {
            const float wrapped{std::fmod(time, animation.duration)};
            return wrapped >= 0.0F ? wrapped : wrapped + animation.duration;
        }
        return std::clamp(time, 0.0F, animation.duration);
    }

    [[nodiscard]] PedEntry* prepare_for_clump_render(RpClump* clump) noexcept {
        PedEntry* entry{find_entry_by_clump(clump)};
        if (entry == nullptr) {
            entry = prepare_entry_for_ped(find_ped_for_clump(clump));
            if (entry == nullptr) return nullptr;
            update_finger_state(*entry);
        }
        apply_blendshapes(*entry);
        return entry;
    }

    void apply_blendshapes(PedEntry& entry) noexcept {
        const ActiveHandSequence configured_sequence{entry.ped != nullptr
            ? find_active_hand_sequence(*entry.ped)
            : ActiveHandSequence{}};
        const HandSignalState signal{entry.ped != nullptr
            ? read_hand_signal_state(*entry.ped)
            : HandSignalState{}};
        if (configured_sequence.profile != nullptr &&
            !configured_sequence_logged_) {
            std::array<char, 160> message{};
            std::snprintf(
                message.data(), message.size(),
                "Blendshape por asociacion PED activo: perfil=%s.",
                configured_sequence.profile->name.c_str());
            log(message.data());
            configured_sequence_logged_ = true;
        }
        if (signal.animation_index >= 0 && !native_signal_logged_) {
            std::array<char, 160> message{};
            std::snprintf(
                message.data(), message.size(),
                "Blendshape por tarea HANDSIGNAL activo: indice=%d left=%d right=%d.",
                signal.animation_index + 1,
                signal.left ? 1 : 0,
                signal.right ? 1 : 0);
            log(message.data());
            native_signal_logged_ = true;
        }
        for (std::size_t atomic_index{}; atomic_index < entry.atomic_count;
             ++atomic_index) {
            RuntimeAtomic& item{entry.atomics[atomic_index]};
            const RuntimeProfile* const profile{item.profile};
            RpGeometry* const geometry{RpAtomicGetGeometry(item.atomic)};
            if (profile == nullptr || geometry == nullptr ||
                item.base_vertices.size() != profile->vertex_count) {
                continue;
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
            for (std::size_t side{}; side < profile->hands.size(); ++side) {
                const RuntimeHand& hand{profile->hands[side]};
                if (hand.template_index >= morph_templates_.size()) continue;
                const MorphTemplate& morph_template{
                    morph_templates_[hand.template_index]};
                if (hand.count != morph_template.vertex_count) continue;

                const MorphTarget* const grip{
                    find_morph_target(morph_template, "Grip")};
                const MorphTarget* const fucku{side == 1
                    ? find_morph_target(morph_template, "FuckU")
                    : nullptr};
                const MorphTarget* sequence_target{};
                float sequence_blend{};
                bool configured_side_active{};
                if (configured_sequence.profile != nullptr &&
                    configured_sequence.association != nullptr) {
                    const int animation_index{side == 0
                        ? configured_sequence.profile->left_animation
                        : configured_sequence.profile->right_animation};
                    if (animation_index >= 0 &&
                        static_cast<std::size_t>(animation_index) <
                            hand_animations_.size()) {
                        const HandAnimation& animation{
                            hand_animations_[static_cast<std::size_t>(animation_index)]};
                        sequence_target = find_morph_target(
                            morph_template, animation.name);
                        if (sequence_target != nullptr) {
                            sequence_blend = sample_track(
                                animation,
                                sequence_time(
                                    animation, *configured_sequence.profile,
                                    *configured_sequence.association));
                            sequence_blend *= std::clamp(
                                configured_sequence.association->m_fBlendAmount *
                                    configured_sequence.profile->weight,
                                0.0F, 1.0F);
                            configured_side_active = true;
                        }
                    }
                }
                if (!configured_side_active && signal.animation_index >= 0 &&
                    ((side == 0 && signal.left) ||
                     (side == 1 && signal.right))) {
                    std::array<char, 16> animation_name{};
                    std::snprintf(
                        animation_name.data(), animation_name.size(),
                        "%cHGsign%d", side == 0 ? 'L' : 'R',
                        signal.animation_index + 1);
                    const int fallback_index{
                        find_hand_animation_index(animation_name.data())};
                    if (fallback_index >= 0) {
                        const HandAnimation& animation{
                            hand_animations_[static_cast<std::size_t>(fallback_index)]};
                        sequence_target = find_morph_target(
                            morph_template, animation.name);
                        sequence_blend = sequence_target != nullptr
                            ? sample_track(
                                animation, std::min(signal.time, animation.duration))
                            : 0.0F;
                    }
                }

                const float grip_blend{std::clamp(entry.grip, 0.0F, 1.0F)};
                const float fucku_blend{side == 1
                    ? smoothstep(entry.fucku_blend)
                    : 0.0F};
                for (std::uint32_t local_index{}; local_index < hand.count;
                     ++local_index) {
                    const std::size_t vertex{
                        static_cast<std::size_t>(hand.start + local_index)};
                    RwV3d position{item.base_vertices[vertex]};
                    RwV3d normal{has_normals
                        ? item.base_normals[vertex]
                        : RwV3d{}};
                    const auto blend_target = [&](
                        const MorphTarget* target, float amount) noexcept {
                        if (target == nullptr || amount <= 0.0F) return;
                        RwV3d target_position{};
                        RwV3dTransformPoint(
                            &target_position,
                            &target->positions[local_index], &hand.transform);
                        position.x += (target_position.x - position.x) * amount;
                        position.y += (target_position.y - position.y) * amount;
                        position.z += (target_position.z - position.z) * amount;
                        if (has_normals) {
                            RwV3d target_normal{};
                            RwV3dTransformVector(
                                &target_normal,
                                &target->normals[local_index], &hand.transform);
                            normalize_vector(target_normal);
                            normal.x += (target_normal.x - normal.x) * amount;
                            normal.y += (target_normal.y - normal.y) * amount;
                            normal.z += (target_normal.z - normal.z) * amount;
                        }
                    };
                    blend_target(grip, grip_blend);
                    blend_target(fucku, fucku_blend);
                    blend_target(
                        sequence_target, std::clamp(sequence_blend, 0.0F, 1.0F));
                    vertices[vertex] = position;
                    if (has_normals) {
                        normalize_vector(normal);
                        normals[vertex] = normal;
                    }
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
    std::vector<MorphTemplate> morph_templates_{};
    std::vector<HandAnimation> hand_animations_{};
    std::vector<HandSequenceProfile> sequence_profiles_{};
    std::vector<PedAnimationMapping> animation_mappings_{};
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
    unsigned int configuration_retry_count_{};
    unsigned int next_configuration_retry_ms_{};
    bool configured_sequence_logged_{};
    bool native_signal_logged_{};
};

HandiesMod handies_mod{};

} // namespace handies
