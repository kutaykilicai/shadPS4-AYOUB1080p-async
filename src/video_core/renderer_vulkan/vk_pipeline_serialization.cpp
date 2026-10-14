// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <unordered_map>

#include "common/perf_stats.h"
#include "common/serdes.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 8u; // 7: legacy multiply-add (v_mac_legacy_f32),
                                                // 8: upstream ADDC/ALIGNBIT/CVT_PK_U8/thread id fixes
static constexpr u32 ShaderMetaVersion = 6u;
static constexpr u32 PipelineKeyVersion = 3u;

/// Compute shaders compiled with a different recompiler variant (see SetComputeShaderVariant)
/// are stored with another binary version, so changing the variant recompiles them once.
static u32 compute_shader_variant{};

static u32 BinaryVersionFor(const Shader::Info& info) {
    return info.hw_stage == Shader::HwStage::Compute
               ? ShaderBinaryVersion + compute_shader_variant * 100u
               : ShaderBinaryVersion;
}
} // namespace Serialization

namespace Vulkan {

void SetComputeShaderVariant(u32 variant) {
    Serialization::compute_shader_variant = variant;
}

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::BinaryVersionFor(info));

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version % 100u != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);
    // Compiled with another recompiler variant: compiled again.
    return binary_version == Serialization::BinaryVersionFor(info);
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);

    it.value() =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *infos[0], modules[0], sdata, true);

    infos.fill(nullptr);
    modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    ASSERT(is_new);

    it.value() = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, graphics_key, *pipeline_cache, infos,
        runtime_infos, fetch_shader, modules, sdata, true);

    infos.fill(nullptr);
    modules.fill(nullptr);
    fetch_shader = nullptr;

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.empty()) {
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};

    // The scout thread looks programs and their permutations up meanwhile.
    std::unique_lock lock{program_cache_mutex};
    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = CompileSPV(spv, instance.GetDevice());
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = CompileSPV(spv, instance.GetDevice());
        }
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    infos[stage] = &it_pgm.value()->info;
    modules[stage] = module;
    if (auto& fetch = it_pgm.value()->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }

    return true;
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    const char* background_env = std::getenv("SHADPS4_BACKGROUND_CACHE");
    // The zip archive store is read and written by one handle: background loading needs the
    // directory store, where reads and the game's new entries are separate files.
    const bool background =
        Common::Perf::GetTuning().background_cache_load.load(std::memory_order_relaxed) &&
        !(background_env != nullptr && background_env[0] == '0') &&
        !EmulatorSettings.IsPipelineCacheArchived();

    Storage::DataBase::Instance().Open();

    // Starts a fresh cache for the current profile, so it is filled during this session.
    const auto start_new_cache = [this] {
        Storage::DataBase::Instance().FinishPreload();
        std::vector<u8> new_profile(sizeof(profile));
        std::memcpy(new_profile.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(new_profile));
    };

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        start_new_cache();
        return;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        // Usually happens after an emulator update. Previously the cache was only ignored,
        // which also disabled saving, so it was never rebuilt and every session recompiled
        // every shader.
        LOG_WARNING(Render,
                    "Pipeline cache profile has unexpected size ({} != {}). Rebuilding the cache",
                    profile_data.size(), sizeof(Shader::Profile));
        Storage::DataBase::Instance().Reset();
        start_new_cache();
        return;
    }

    Shader::Profile cached_profile{};
    std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    if (cached_profile != profile) {
        LOG_WARNING(Render,
                    "Pipeline cache isn't compatible with current system. Rebuilding the cache");
        Storage::DataBase::Instance().Reset();
        start_new_cache();
        return;
    }

    if (background) {
        StartBackgroundLoad();
        return;
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;

            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);

            bool result{};
            if (is_compute) {
                result = LoadComputePipeline(ar);
            } else {
                result = LoadGraphicsPipeline(ar);
            }

            if (result) {
                ++num_pipelines;
            }
        });

    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

// Background cache load. A loader thread reads the store, deserializes the shaders and creates
// their modules; it touches no PipelineCache state. The command processor merges the shaders into
// program_cache and creates the pipelines (compiled by the PipelineCompiler workers) a few at a
// time, so the game starts at once. A pipeline the game asks for first is compiled normally and
// the loaded copy is dropped.
struct PipelineCache::PreloadStage {
    vk::Device device;
    std::unique_ptr<Program> program; ///< Moved into program_cache when the shader is new.
    Shader::StageSpecialization spec{};
    size_t perm_idx{};
    vk::ShaderModule module{};
    // Command processor only (MergePreloadStage).
    bool merged{};
    bool module_kept{};
    const Shader::Info* info{}; ///< Null: unusable.
    vk::ShaderModule result{};

    ~PreloadStage() {
        if (module && !module_kept) {
            device.destroyShaderModule(module);
        }
    }
};

struct PipelineCache::PreloadEntry {
    bool compute{};
    GraphicsPipelineKey graphics_key{};
    ComputePipelineKey compute_key{};
    GraphicsPipeline::SerializationSupport sdata{};
    std::array<std::shared_ptr<PreloadStage>, MaxShaderStages> stages{};
};

struct PipelineCache::BackgroundLoad {
    std::mutex mutex;
    std::vector<PreloadEntry> inbox; ///< Guarded by mutex.
    std::atomic<bool> reading{true};
    std::atomic<u32> total{0}, read{0};
    // Command processor only.
    u32 published{}, skipped{};
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_pump{};
    std::jthread thread; ///< Last member: joined before the rest is destroyed.
};

void PipelineCache::StartBackgroundLoad() {
    background_load = new BackgroundLoad{};
    background_load_active = true;
    background_load->thread =
        std::jthread{[this](std::stop_token stoken) { BackgroundLoadThread(stoken); }};
    LOG_INFO(Render, "Pipeline cache loading in the background, the game starts meanwhile");
}

std::shared_ptr<PipelineCache::PreloadStage> PipelineCache::ReadPreloadStage(
    u64 hash, std::unordered_map<u64, std::shared_ptr<PreloadStage>>& stages) {
    if (const auto it = stages.find(hash); it != stages.end()) {
        return it->second; // null when it could not be read
    }
    auto& slot = stages[hash];
    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta, fmt::format("{:#018x}", hash),
                                       meta_blob);
    if (meta_blob.empty()) {
        return nullptr;
    }
    auto stage = std::make_shared<PreloadStage>();
    stage->device = instance.GetDevice();
    stage->program = std::make_unique<Program>();
    stage->spec.info = &stage->program->info;
    Serialization::Archive meta_ar{std::move(meta_blob)};
    if (!LoadShaderMeta(meta_ar, stage->program->info, stage->spec, stage->perm_idx)) {
        return nullptr;
    }
    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(
        Storage::BlobType::ShaderBinary,
        fmt::format("{:#018x}_{}", stage->program->info.pgm_hash, stage->perm_idx), spv);
    if (spv.empty()) {
        return nullptr;
    }
    stage->module = CompileSPV(spv, instance.GetDevice());
    slot = stage;
    return stage;
}

void PipelineCache::BackgroundLoadThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:CacheLoader");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    auto& load = *background_load;
    std::unordered_map<u64, std::shared_ptr<PreloadStage>> stages; // this thread only
    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            if (stoken.stop_requested()) {
                return;
            }
            load.total.fetch_add(1, std::memory_order_relaxed);
            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};
            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                return;
            }
            u32 is_compute{};
            pldata.Read(is_compute);
            PreloadEntry entry{};
            entry.compute = is_compute != 0;
            if (entry.compute) {
                entry.compute_key.Deserialize(ar);
                ComputePipeline::SerializationSupport compute_data{};
                compute_data.Deserialize(ar);
                entry.stages[0] = ReadPreloadStage(entry.compute_key.value, stages);
                if (!entry.stages[0]) {
                    return;
                }
            } else {
                entry.graphics_key.Deserialize(ar);
                entry.sdata.Deserialize(ar);
                for (u32 i = 0; i < MaxShaderStages; ++i) {
                    const u64 hash = entry.graphics_key.stage_hashes[i];
                    if (!hash) {
                        continue;
                    }
                    entry.stages[i] = ReadPreloadStage(hash, stages);
                    if (!entry.stages[i]) {
                        return;
                    }
                }
            }
            load.read.fetch_add(1, std::memory_order_relaxed);
            std::scoped_lock lk{load.mutex};
            load.inbox.push_back(std::move(entry));
        });
    load.reading.store(false, std::memory_order_release);
}

bool PipelineCache::MergePreloadStage(PreloadStage& stage) {
    // Called with program_cache_mutex held exclusively (the scout thread reads the programs).
    stage.merged = true;
    const u64 hash = stage.program->info.pgm_hash;
    auto it_pgm = program_cache.find(hash);
    if (it_pgm == program_cache.end()) {
        Program* const program = stage.program.get();
        program->InsertPermut(stage.module, std::move(stage.spec), stage.perm_idx);
        program_cache.emplace(hash, std::move(stage.program));
        stage.module_kept = true;
        stage.result = stage.module;
        stage.info = &program->info;
        return true;
    }
    Program& program = *it_pgm.value();
    const auto it = std::ranges::find(program.modules, stage.spec, &Program::Module::spec);
    if (it != program.modules.end()) {
        if (size_t(std::distance(program.modules.begin(), it)) != stage.perm_idx) {
            return false; // the game numbered its permutations differently
        }
        stage.result = it->module;
    } else {
        if (stage.perm_idx < program.modules.size() && program.modules[stage.perm_idx].module) {
            return false; // the index holds another permutation
        }
        stage.spec.info = &program.info;
        program.InsertPermut(stage.module, std::move(stage.spec), stage.perm_idx);
        stage.module_kept = true;
        stage.result = stage.module;
    }
    stage.info = &program.info;
    return true;
}

void PipelineCache::PumpBackgroundLoad() {
    if (!background_load_active) [[likely]] {
        return;
    }
    auto& load = *background_load;
    const auto now = std::chrono::steady_clock::now();
    if (now - load.last_pump < std::chrono::milliseconds{2}) {
        return;
    }
    load.last_pump = now;
    const bool reading = load.reading.load(std::memory_order_acquire);
    std::vector<PreloadEntry> batch;
    {
        std::scoped_lock lk{load.mutex};
        const size_t n = std::min<size_t>(load.inbox.size(), 16);
        batch.assign(std::make_move_iterator(load.inbox.begin()),
                     std::make_move_iterator(load.inbox.begin() + n));
        load.inbox.erase(load.inbox.begin(), load.inbox.begin() + n);
    }
    static const std::array<Shader::RuntimeInfo, MaxShaderStages> no_runtime_infos{};
    for (auto& entry : batch) {
        if (entry.compute ? compute_pipelines.contains(entry.compute_key)
                          : graphics_pipelines.contains(entry.graphics_key)) {
            ++load.skipped; // the game compiled it first
            continue;
        }
        std::array<const Shader::Info*, MaxShaderStages> stage_infos{};
        std::array<vk::ShaderModule, MaxShaderStages> stage_modules{};
        const Shader::Gcn::FetchShaderData* stage_fetch = nullptr;
        bool usable = true;
        {
            std::unique_lock lock{program_cache_mutex};
            for (u32 i = 0; i < MaxShaderStages && usable; ++i) {
                if (!entry.stages[i]) {
                    continue;
                }
                auto& stage = *entry.stages[i];
                if (!stage.merged) {
                    MergePreloadStage(stage);
                }
                usable = stage.info != nullptr;
                stage_infos[i] = stage.info;
                stage_modules[i] = stage.result;
            }
            if (usable) {
                // As the synchronous load: the fetch shader of the last stage that has one.
                for (u32 i = 0; i < MaxShaderStages; ++i) {
                    if (!entry.stages[i]) {
                        continue;
                    }
                    Program* const program = program_cache.find(stage_infos[i]->pgm_hash)->second.get();
                    const size_t idx = entry.stages[i]->perm_idx;
                    if (idx < program->modules.size()) {
                        if (auto& fetch = program->modules[idx].spec.fetch_shader_data;
                            !fetch.Empty()) {
                            stage_fetch = &fetch;
                        }
                    }
                }
            }
        }
        if (!usable) {
            ++load.skipped;
            continue;
        }
        Pipeline* pipeline{};
        if (entry.compute) {
            ComputePipeline::SerializationSupport compute_data{};
            auto& slot = compute_pipelines[entry.compute_key];
            slot = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                     *pipeline_cache, entry.compute_key,
                                                     *stage_infos[0], stage_modules[0],
                                                     compute_data, true, true);
            pipeline = slot.get();
        } else {
            auto& slot = graphics_pipelines[entry.graphics_key];
            slot = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, profile, entry.graphics_key, *pipeline_cache,
                stage_infos, no_runtime_infos, stage_fetch, stage_modules, entry.sdata, true, true);
            pipeline = slot.get();
        }
        SubmitPreload(pipeline);
        ++load.published;
    }
    if (reading) {
        return;
    }
    {
        std::scoped_lock lk{load.mutex};
        if (!load.inbox.empty()) {
            return;
        }
    }
    // Logged once: everything read was published or skipped (the workers still compile).
    LOG_INFO(Render,
             "Background pipeline cache load finished in {:.1f} s: {} pipelines queued for "
             "compilation, {} skipped, {} entries in the store",
             std::chrono::duration<double>(now - load.started).count(), load.published,
             load.skipped, load.total.load());
    background_load_active = false;
    StopBackgroundLoad();
}

void PipelineCache::StopBackgroundLoad() {
    if (!background_load) {
        return;
    }
    background_load_active = false;
    background_load->thread.request_stop();
    if (background_load->thread.joinable()) {
        background_load->thread.join();
    }
    delete background_load; // unpublished entries destroy their unused modules
    background_load = nullptr;
}

void PipelineCache::Sync() {
    StopBackgroundLoad();
    Storage::DataBase::Instance().Close();
}


} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (!fetch_shader_data.Empty()) {
        spec.Write(sizeof(fetch_shader_data));
        fetch_shader_data.Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
