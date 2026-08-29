/*
    Handies - runtime animated fingers for GTA San Andreas pedestrians.

    DFF files retain the native 32-node ped hierarchy. After GTA completes its
    normal animation setup, this plugin gives each ped a private geometry and
    appends 26 runtime-only finger nodes. GTA continues animating only its
    original 32 frames; Handies evaluates the fingers after each native update.
*/

#include "plugin.h"

#include "AnimBlendFrameData.h"
#include "CAnimBlendClumpData.h"
#include "CPed.h"
#include "CPedIntelligence.h"
#include "CTaskManager.h"
#include "CTimer.h"
#include "CWeapon.h"
#include "RpHAnimBlendInterpFrame.h"
#include "common.h"
#include "eAnimations.h"
#include "ePedState.h"
#include "eTaskType.h"

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

constexpr std::uintptr_t update_animations_call_address{0x535F94};
constexpr std::size_t max_tracked_peds{256};
constexpr std::size_t max_atomics_per_ped{16};
constexpr int native_bone_count{32};
constexpr int runtime_bone_count{58};
constexpr int finger_bones_per_hand{15};
constexpr int pose_count{3};
constexpr int left_hand_id{34};
constexpr int right_hand_id{24};
constexpr int left_extra_id_base{1005};
constexpr int right_extra_id_base{1105};
constexpr char data_file_name[]{"Handies.dat"};
constexpr char ini_file_name[]{"Handies.ini"};
constexpr char log_file_name[]{"Handies.log"};
constexpr std::array<char, 8> data_magic{'H', 'N', 'D', '2', 'D', 'A', 'T', '\0'};
constexpr std::uint32_t data_version{1};
constexpr float minimum_visible_animation_blend{0.01F};

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

struct RuntimeBinding {
    RpHAnimHierarchy* hierarchy{};
    const RuntimeProfile* profile{};
};

struct PedEntry {
    CPed* ped{};
    RpClump* clump{};
    std::array<RuntimeBinding, max_atomics_per_ped> bindings{};
    std::size_t binding_count{};
    float grip{};
    float fucku_blend{};
};

struct AtomicList {
    std::array<RpAtomic*, max_atomics_per_ped> values{};
    std::size_t size{};
    bool overflow{};
};

struct PreparedAtomic {
    RpAtomic* atomic{};
    RpGeometry* geometry{};
    std::size_t binding_index{};
};

struct HierarchyPlan {
    RpHAnimHierarchy* old_hierarchy{};
    RpHAnimHierarchy* new_hierarchy{};
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

[[nodiscard]] int target_bone_id(int side, int source_id) noexcept {
    if (side == 0) {
        if (source_id == 3) return 35;
        if (source_id == 4) return 36;
        return 1000 + source_id;
    }
    if (source_id == 3) return 25;
    if (source_id == 4) return 26;
    return 1100 + source_id;
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

[[nodiscard]] RpGeometry* clone_geometry(const RpGeometry* source) {
    if (source == nullptr) return nullptr;
    const RwInt32 vertex_count{RpGeometryGetNumVertices(source)};
    const RwInt32 triangle_count{RpGeometryGetNumTriangles(source)};
    const RwInt32 morph_count{RpGeometryGetNumMorphTargets(source)};
    const RwInt32 texcoord_count{RpGeometryGetNumTexCoordSets(source)};
    if (vertex_count <= 0 || triangle_count <= 0 || morph_count <= 0 ||
        texcoord_count < 0 || texcoord_count > rwMAXTEXTURECOORDS) {
        return nullptr;
    }
    const RwUInt32 format{
        (RpGeometryGetFlags(source) & rpGEOMETRYFLAGSMASK) |
        rpGEOMETRYTEXCOORDSETS(static_cast<RwUInt32>(texcoord_count))};
    RpGeometry* clone{RpGeometryCreate(vertex_count, triangle_count, format)};
    if (clone == nullptr) return nullptr;
    if (morph_count > 1 &&
        RpGeometryAddMorphTargets(clone, morph_count - 1) < 0) {
        RpGeometryDestroy(clone);
        return nullptr;
    }

    for (RwInt32 morph_index{}; morph_index < morph_count; ++morph_index) {
        const RpMorphTarget* source_morph{
            RpGeometryGetMorphTarget(source, morph_index)};
        RpMorphTarget* target_morph{RpGeometryGetMorphTarget(clone, morph_index)};
        const RwV3d* source_vertices{RpMorphTargetGetVertices(source_morph)};
        RwV3d* target_vertices{RpMorphTargetGetVertices(target_morph)};
        if (source_vertices == nullptr || target_vertices == nullptr) {
            RpGeometryDestroy(clone);
            return nullptr;
        }
        std::memcpy(target_vertices, source_vertices,
                    sizeof(RwV3d) * static_cast<std::size_t>(vertex_count));
        RpMorphTargetSetBoundingSphere(
            target_morph, RpMorphTargetGetBoundingSphere(source_morph));

        const RwV3d* source_normals{RpMorphTargetGetVertexNormals(source_morph)};
        RwV3d* target_normals{RpMorphTargetGetVertexNormals(target_morph)};
        if (source_normals != nullptr && target_normals != nullptr) {
            std::memcpy(target_normals, source_normals,
                        sizeof(RwV3d) * static_cast<std::size_t>(vertex_count));
        }
    }

    const RwRGBA* source_colors{RpGeometryGetPreLightColors(source)};
    RwRGBA* target_colors{RpGeometryGetPreLightColors(clone)};
    if (source_colors != nullptr && target_colors != nullptr) {
        std::memcpy(target_colors, source_colors,
                    sizeof(RwRGBA) * static_cast<std::size_t>(vertex_count));
    }
    for (RwInt32 uv_index{}; uv_index < texcoord_count; ++uv_index) {
        const auto coordinate_index{
            static_cast<RwTextureCoordinateIndex>(uv_index)};
        const RwTexCoords* source_uvs{
            RpGeometryGetVertexTexCoords(source, coordinate_index)};
        RwTexCoords* target_uvs{
            RpGeometryGetVertexTexCoords(clone, coordinate_index)};
        if (source_uvs == nullptr || target_uvs == nullptr) {
            RpGeometryDestroy(clone);
            return nullptr;
        }
        std::memcpy(target_uvs, source_uvs,
                    sizeof(RwTexCoords) * static_cast<std::size_t>(vertex_count));
    }

    const RpTriangle* source_triangles{RpGeometryGetTriangles(source)};
    RpTriangle* target_triangles{RpGeometryGetTriangles(clone)};
    if (source_triangles == nullptr || target_triangles == nullptr) {
        RpGeometryDestroy(clone);
        return nullptr;
    }
    for (RwInt32 triangle_index{}; triangle_index < triangle_count;
         ++triangle_index) {
        const RpTriangle& source_triangle{source_triangles[triangle_index]};
        RpTriangle& target_triangle{target_triangles[triangle_index]};
        RpGeometryTriangleSetVertexIndices(
            clone, &target_triangle, source_triangle.vertIndex[0],
            source_triangle.vertIndex[1], source_triangle.vertIndex[2]);
        RpMaterial* material{
            RpGeometryTriangleGetMaterial(source, &source_triangle)};
        if (material == nullptr ||
            RpGeometryTriangleSetMaterial(clone, &target_triangle, material) == nullptr) {
            RpGeometryDestroy(clone);
            return nullptr;
        }
    }
    if (RpGeometryUnlock(clone) == nullptr) {
        RpGeometryDestroy(clone);
        return nullptr;
    }
    return clone;
}

} // namespace

class HandiesMod final {
public:
    HandiesMod() {
        instance_ = this;
        resolve_module_paths();
        load_settings();
        const bool data_loaded{load_runtime_data()};
        install_update_hook();
        log(data_loaded
                ? "Handies activo: esqueleto nativo y dedos agregados en memoria."
                : "ERROR: Handies.dat no pudo cargarse; el mod queda inactivo.");

        plugin::Events::initGameEvent += [this] {
            entries_.fill({});
            load_settings();
            if (profiles_.empty() && !load_runtime_data()) {
                log("ERROR: Handies.dat sigue sin estar disponible al iniciar partida.");
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
        plugin::Events::shutdownPoolsEvent += [this] { entries_.fill({}); };
    }

private:
    using UpdateAnimationsFunction = void(__cdecl*)(RpClump*, float, bool);

    void resolve_module_paths() noexcept {
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
        const HANDLE file{CreateFileA(
            log_path_.data(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written{};
        WriteFile(file, message, static_cast<DWORD>(std::strlen(message)),
                  &written, nullptr);
        constexpr char newline[]{"\r\n"};
        WriteFile(file, newline, 2, &written, nullptr);
        CloseHandle(file);
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
            profiles_ = std::move(profiles);
            return true;
        } catch (...) {
            return false;
        }
    }

    void install_update_hook() noexcept {
        const auto previous{injector::MakeCALL(
            update_animations_call_address,
            injector::raw_ptr(&update_animations_hook))};
        if (const UpdateAnimationsFunction original{previous.get()}; original != nullptr) {
            original_update_animations_ = original;
        }
        log(original_update_animations_ != nullptr
                ? "Hook nativo de actualización instalado."
                : "ERROR: no se pudo instalar el hook de actualización.");
    }

    static void __cdecl update_animations_hook(
        RpClump* clump, float time_step, bool on_screen) noexcept {
        if (original_update_animations_ != nullptr) {
            original_update_animations_(clump, time_step, on_screen);
        }
        if (instance_ != nullptr) instance_->apply_for_clump(clump);
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

    static void destroy_prepared(
        std::array<PreparedAtomic, max_atomics_per_ped>& prepared) noexcept {
        for (auto& item : prepared) {
            if (item.geometry != nullptr) {
                RpGeometryDestroy(item.geometry);
                item.geometry = nullptr;
            }
        }
    }

    static void destroy_new_hierarchies(
        std::array<HierarchyPlan, max_atomics_per_ped>& plans) noexcept {
        for (auto& plan : plans) {
            if (plan.new_hierarchy == nullptr) continue;
            RtAnimAnimation* animation{
                plan.new_hierarchy->currentAnim != nullptr
                    ? plan.new_hierarchy->currentAnim->pCurrentAnim
                    : nullptr};
            if (animation != nullptr) {
                RtAnimAnimationDestroy(animation);
                plan.new_hierarchy->currentAnim->pCurrentAnim = nullptr;
            }
            RpHAnimHierarchyDestroy(plan.new_hierarchy);
            plan.new_hierarchy = nullptr;
        }
    }

    [[nodiscard]] static RpHAnimHierarchy* create_runtime_hierarchy(
        RpHAnimHierarchy* old_hierarchy) noexcept {
        if (old_hierarchy == nullptr || old_hierarchy->numNodes != native_bone_count ||
            old_hierarchy->pNodeInfo == nullptr || old_hierarchy->currentAnim == nullptr ||
            old_hierarchy->pMatrixArray == nullptr ||
            old_hierarchy->parentFrame == nullptr) {
            return nullptr;
        }
        std::array<RwUInt32, runtime_bone_count> node_flags{};
        std::array<RwInt32, runtime_bone_count> node_ids{};
        for (int index{}; index < native_bone_count; ++index) {
            node_flags[index] =
                static_cast<RwUInt32>(old_hierarchy->pNodeInfo[index].flags);
            node_ids[index] = old_hierarchy->pNodeInfo[index].nodeID;
        }
        for (int offset{}; offset < 13; ++offset) {
            node_ids[native_bone_count + offset] = left_extra_id_base + offset;
            node_ids[native_bone_count + 13 + offset] = right_extra_id_base + offset;
        }
        const auto hierarchy_flags{static_cast<RpHAnimHierarchyFlag>(
            rpHANIMHIERARCHYUPDATEMODELLINGMATRICES |
            rpHANIMHIERARCHYUPDATELTMS)};
        RpHAnimHierarchy* hierarchy{RpHAnimHierarchyCreate(
            runtime_bone_count, node_flags.data(), node_ids.data(), hierarchy_flags,
            static_cast<RwInt32>(sizeof(RpHAnimBlendInterpFrame)))};
        if (hierarchy == nullptr || hierarchy->currentAnim == nullptr) {
            if (hierarchy != nullptr) RpHAnimHierarchyDestroy(hierarchy);
            return nullptr;
        }
        RtAnimAnimation* animation{RpAnimBlendCreateAnimationForHierarchy(hierarchy)};
        if (animation == nullptr ||
            !RtAnimInterpolatorSetCurrentAnim(hierarchy->currentAnim, animation)) {
            if (animation != nullptr) RtAnimAnimationDestroy(animation);
            RpHAnimHierarchyDestroy(hierarchy);
            return nullptr;
        }
        for (int index{}; index < native_bone_count; ++index) {
            std::memcpy(rtANIMGETINTERPFRAME(hierarchy->currentAnim, index),
                        rtANIMGETINTERPFRAME(old_hierarchy->currentAnim, index),
                        sizeof(RpHAnimBlendInterpFrame));
        }
        std::memcpy(hierarchy->pMatrixArray, old_hierarchy->pMatrixArray,
                    sizeof(RwMatrix) * native_bone_count);
        return hierarchy;
    }

    [[nodiscard]] bool inject_runtime_skeleton(PedEntry& entry) {
        RpClump* clump{entry.ped != nullptr ? entry.ped->m_pRwClump : nullptr};
        if (clump == nullptr) return false;
        RpHAnimHierarchy* primary_hierarchy{GetAnimHierarchyFromSkinClump(clump)};
        CAnimBlendClumpData* data{RpClumpGetAnimBlendClumpData(clump)};
        if (primary_hierarchy == nullptr || data == nullptr ||
            data->m_nNumFrames != native_bone_count || data->m_pFrames == nullptr) {
            return false;
        }
        AtomicList atomics{};
        RpClumpForAllAtomics(clump, collect_atomic, &atomics);
        if (atomics.overflow || atomics.size == 0) return false;
        std::array<const RuntimeProfile*, max_atomics_per_ped> profiles{};
        std::array<std::size_t, max_atomics_per_ped> atomic_binding_indices{};
        std::array<HierarchyPlan, max_atomics_per_ped> plans{};
        std::size_t plan_count{};
        for (std::size_t index{}; index < atomics.size; ++index) {
            RpGeometry* geometry{RpAtomicGetGeometry(atomics.values[index])};
            RpSkin* skin{geometry != nullptr ? RpSkinGeometryGetSkin(geometry) : nullptr};
            RpHAnimHierarchy* hierarchy{
                RpSkinAtomicGetHAnimHierarchy(atomics.values[index])};
            profiles[index] = find_profile(geometry);
            if (skin == nullptr || RpSkinGetNumBones(skin) != native_bone_count ||
                profiles[index] == nullptr || hierarchy == nullptr ||
                hierarchy->numNodes != native_bone_count) {
                return false;
            }
            std::size_t binding_index{plan_count};
            for (std::size_t plan_index{}; plan_index < plan_count; ++plan_index) {
                if (plans[plan_index].old_hierarchy == hierarchy) {
                    binding_index = plan_index;
                    break;
                }
            }
            if (binding_index == plan_count) {
                plans[plan_count] = {hierarchy, nullptr, profiles[index]};
                ++plan_count;
            }
            atomic_binding_indices[index] = binding_index;
        }
        std::size_t primary_binding{plan_count};
        for (std::size_t index{}; index < plan_count; ++index) {
            plans[index].new_hierarchy =
                create_runtime_hierarchy(plans[index].old_hierarchy);
            if (plans[index].new_hierarchy == nullptr) {
                destroy_new_hierarchies(plans);
                return false;
            }
            if (plans[index].old_hierarchy == primary_hierarchy) {
                primary_binding = index;
            }
        }
        if (primary_binding == plan_count) {
            destroy_new_hierarchies(plans);
            return false;
        }

        std::array<PreparedAtomic, max_atomics_per_ped> prepared{};
        for (std::size_t index{}; index < atomics.size; ++index) {
            RpGeometry* clone{clone_geometry(RpAtomicGetGeometry(atomics.values[index]))};
            if (clone == nullptr) {
                destroy_prepared(prepared);
                destroy_new_hierarchies(plans);
                return false;
            }
            RuntimeProfile* profile{const_cast<RuntimeProfile*>(profiles[index])};
            RpSkin* runtime_skin{RpSkinCreate(
                profile->vertex_count, runtime_bone_count, profile->weights.data(),
                profile->indices.data(), profile->inverse_matrices.data())};
            if (runtime_skin == nullptr) {
                RpGeometryDestroy(clone);
                destroy_prepared(prepared);
                destroy_new_hierarchies(plans);
                return false;
            }
            RpSkinGeometrySetSkin(clone, runtime_skin);
            prepared[index] = {
                atomics.values[index], clone, atomic_binding_indices[index]};
        }

        for (std::size_t index{}; index < atomics.size; ++index) {
            RpAtomicSetGeometry(prepared[index].atomic, prepared[index].geometry, 0);
            RpSkinAtomicSetHAnimHierarchy(
                prepared[index].atomic,
                plans[prepared[index].binding_index].new_hierarchy);
            RpGeometryDestroy(prepared[index].geometry);
            prepared[index].geometry = nullptr;
        }
        for (std::size_t index{}; index < plan_count; ++index) {
            RwFrame* owner_frame{plans[index].old_hierarchy->parentFrame};
            RpHAnimFrameSetHierarchy(owner_frame, plans[index].new_hierarchy);
            plans[index].new_hierarchy->parentFrame = owner_frame;
        }
        for (int index{}; index < native_bone_count; ++index) {
            data->m_pFrames[index].m_pIFrame = reinterpret_cast<IFrame*>(
                rtANIMGETINTERPFRAME(
                    plans[primary_binding].new_hierarchy->currentAnim, index));
        }
        for (std::size_t index{}; index < plan_count; ++index) {
            RtAnimAnimation* old_animation{
                plans[index].old_hierarchy->currentAnim->pCurrentAnim};
            if (old_animation != nullptr) {
                RtAnimAnimationDestroy(old_animation);
                plans[index].old_hierarchy->currentAnim->pCurrentAnim = nullptr;
            }
            RpHAnimHierarchyDestroy(plans[index].old_hierarchy);
        }
        entry.clump = clump;
        entry.binding_count = plan_count;
        for (std::size_t index{}; index < plan_count; ++index) {
            entry.bindings[index] = {
                plans[index].new_hierarchy, plans[index].profile};
        }
        return true;
    }

    void on_ped_render(CPed* ped) noexcept {
        if (!settings_.enabled || profiles_.empty() || ped == nullptr ||
            ped->m_pRwClump == nullptr) return;
        const bool is_player{ped == FindPlayerPed()};
        if ((is_player && !settings_.enable_player) ||
            (!is_player && !settings_.enable_npcs)) return;
        PedEntry* entry{reserve_entry(ped)};
        if (entry == nullptr) return;
        if (entry->clump == nullptr) {
            try {
                if (!inject_runtime_skeleton(*entry)) {
                    *entry = {};
                    return;
                }
            } catch (...) {
                *entry = {};
                log("ERROR: excepción al preparar un ped; se conservó su modelo nativo.");
                return;
            }
        }
        apply_finger_matrices(*entry);
    }

    void on_game_process() noexcept {
        if (!settings_.enabled) return;
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

    void apply_for_clump(RpClump* clump) noexcept {
        if (PedEntry* entry{find_entry_by_clump(clump)}; entry != nullptr) {
            apply_finger_matrices(*entry);
        }
    }

    void apply_finger_matrices(PedEntry& entry) const noexcept {
        for (std::size_t binding_index{};
             binding_index < entry.binding_count;
             ++binding_index) {
            const RuntimeBinding& binding{entry.bindings[binding_index]};
            if (binding.hierarchy == nullptr || binding.profile == nullptr ||
                binding.hierarchy->numNodes != runtime_bone_count ||
                binding.hierarchy->pMatrixArray == nullptr) {
                continue;
            }
            RwMatrix* matrices{binding.hierarchy->pMatrixArray};
            for (int side{}; side < 2; ++side) {
                for (int source_id{3}; source_id <= 17; ++source_id) {
                    const int target_id{target_bone_id(side, source_id)};
                    const int target_index{
                        RpHAnimIDGetIndex(binding.hierarchy, target_id)};
                    const int parent_source{parent_source_id(source_id)};
                    const int parent_id{parent_source == 2
                        ? (side == 0 ? left_hand_id : right_hand_id)
                        : target_bone_id(side, parent_source)};
                    const int parent_index{
                        RpHAnimIDGetIndex(binding.hierarchy, parent_id)};
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
    }

    void remove_for_ped(const CPed* ped) noexcept {
        if (ped == nullptr) return;
        for (auto& entry : entries_) {
            if (entry.ped == ped) {
                entry = {};
                return;
            }
        }
    }

    Settings settings_{};
    PoseTable poses_{};
    std::vector<RuntimeProfile> profiles_{};
    std::array<PedEntry, max_tracked_peds> entries_{};
    std::array<char, MAX_PATH> ini_path_{};
    std::array<char, MAX_PATH> log_path_{};
    std::array<char, MAX_PATH> data_path_{};

    inline static HandiesMod* instance_{};
    inline static UpdateAnimationsFunction original_update_animations_{
        reinterpret_cast<UpdateAnimationsFunction>(0x4D34F0)};
};

HandiesMod handies_mod{};

} // namespace handies
