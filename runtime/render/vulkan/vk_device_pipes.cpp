#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <unordered_set>
#include "combiner.h"
#include "pipeline_warm.h"
#include "vk_device.h"
#include "vos_log.h"
// The draw pipelines: lookup and draw-time builds, the async miss path with promotions, the warm-up (blocking and in the
// background), the pipeline cache file, and the fixed passes' pipelines (metal_device.mm pipeline, take_warmed,
// warm_pipelines*, prepare_passes).
namespace ar {
namespace vkd {
using vk::PipeKey;
using vk::WarmJob;
using vk::WarmKey;
using vk::WarmPool;
namespace {
double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

vk::PipeKey VulkanDevice::pipe_key(const DrawPacket &draw, bool depth, uint32_t samples) const {
    const PipeIdentity k = pipe_identity(vk::pipeline_form(draw));
    PipeKey pk;
    pk.slots = k.slots; pk.opts = k.opts; pk.fmt = vk::kColorFormat; pk.blend = k.blend; pk.depth = (uint8_t)depth;
    pk.flat = k.silhouette; pk.depth_only = k.depth_only; pk.table_shadow = k.table_shadow; pk.clip = k.clip;
    pk.samples = (uint8_t)samples;
    pk.a2c = (uint8_t)(edge_ && samples > 1 && (k.opts & FC_CC_TEXTURE_EDGE) ? 1 : 0);
    pk.cut = k.cut;
    return pk;
}
// at_draw: a draw needs it now (a compile here stalls the frame: counted in draw_time_builds). With async pipelines a
// draw never compiles (nor waits for a compile): the miss is queued, logged and the draw skipped (a cutaway draw falls
// back to its uncut pipeline). A key's further draw states are variants built from its shaders (not counted).
VkPipeline VulkanDevice::draw_pipeline(const DrawPacket &d, uint32_t samples, uint32_t state, std::string *err, bool at_draw) {
    if (!ok_) { if (err) *err = "no device"; return VK_NULL_HANDLE; }
    if (d.silhouette && decode_combiner(d.cc).unsupported) return VK_NULL_HANDLE;
    const PipeKey pk = pipe_key(d, true, samples);
    const WarmKey wk{pk, state};
    const bool defer = at_draw && async_;
    auto it = pipes_.find(pk);
    if (it != pipes_.end() && it->second.ok) {   // the common case: no pool lookup
        auto v = it->second.variants.find(state);
        if (v != it->second.variants.end()) return v->second;
    }
    if (take_warmed(wk, !defer)) it = pipes_.find(pk);
    if (it != pipes_.end()) {
        if (!it->second.ok) { if (err) *err = it->second.why; return VK_NULL_HANDLE; }
        auto v = it->second.variants.find(state);
        if (v != it->second.variants.end()) return v->second;
    }
    const bool have = it != pipes_.end();
    WarmJob job{wk, d, have ? it->second.vs : VK_NULL_HANDLE, have ? it->second.fs : VK_NULL_HANDLE};
    if (defer) {
        // a key already warming (in any draw state) is promoted, not compiled twice: its variant follows once it landed
        const bool warming = !have && pool_.busy_pipe(pk);
        if (!warming && !pool_.busy(wk) && pool_.enqueue(job, WarmPool::kUrgent) && !have) draw_builds_++;
        if (!have && missed_.emplace(pk, true).second) {
            char b[96];
            std::snprintf(b, sizeof b, "%016llx/%x/b%u/f%u/d%u/t%u/c%u/s%u/k%u", (unsigned long long)pk.slots, pk.opts, pk.blend, pk.flat,
                          pk.depth_only, pk.table_shadow, pk.clip, pk.samples, (unsigned)pk.cut);
            miss_log_.push_back(b);
        }
        if (d.cutaway) {   // drawn uncut (its normal pipeline) until the variant is built: never missing
            DrawPacket plain = d;
            plain.cutaway = 0;
            return draw_pipeline(plain, samples, state, err, at_draw);
        }
        if (pool_.promote(wk) || (warming && pool_.promote_pipe(pk))) promotions_++;
        deferred_++;
        return VK_NULL_HANDLE;
    }
    const vk::Built b = vk::build_warm(env_, job);
    store_built(b);
    if (!have && at_draw) draw_builds_++;
    it = pipes_.find(pk);
    if (it == pipes_.end() || !it->second.ok) { if (err) *err = b.why; return VK_NULL_HANDLE; }
    auto v = it->second.variants.find(state);
    return v != it->second.variants.end() ? v->second : VK_NULL_HANDLE;
}
// A build's result (from the pool or this thread): a key's first success brings its shaders; later ones add a variant.
void VulkanDevice::store_built(const vk::Built &b) {
    auto ins = pipes_.try_emplace(b.key.pk);
    Pipe &p = ins.first->second;
    const bool had_shaders = p.vs != VK_NULL_HANDLE;
    if (b.ok) {
        if (!had_shaders) { p.vs = b.vs; p.fs = b.fs; p.ok = true; p.why.clear(); }
        else if (b.own_modules) { vkDestroyShaderModule(dev_, b.vs, nullptr); vkDestroyShaderModule(dev_, b.fs, nullptr); }
        VkPipeline &slot = p.variants[b.key.state];
        if (slot) vkDestroyPipeline(dev_, b.pipeline, nullptr);   // built twice (an inline and a queued build raced): keep one
        else slot = b.pipeline;
        built_since_save_++;
    } else if (had_shaders) {   // a variant the driver refused: never retried
        auto v = p.variants.find(b.key.state);
        if (v == p.variants.end()) {
            p.variants[b.key.state] = VK_NULL_HANDLE;
            vos_log("AR_VULKAN pipeline variant %x: %s", b.key.state, b.why.c_str());
            record_refusal(b.key.pk, "pipeline variant: " + b.why);
        }
    } else if (ins.second || p.why.empty()) {   // the key fails (once)
        p.ok = false;
        p.why = b.why.empty() ? std::string("no pipeline") : b.why;
        if (b.refused) record_refusal(b.key.pk, b.why);
    }
}
void VulkanDevice::record_refusal(const PipeKey &pk, const std::string &why) {
    vos_log("AR_VULKAN combiner %016llx/%x: %s", (unsigned long long)pk.slots, pk.opts, why.c_str());
    char head[80];
    std::snprintf(head, sizeof head, "combiner %016llx/%x samples=%u: ", (unsigned long long)pk.slots, pk.opts, (unsigned)pk.samples);
    refused_.push_back(head + why);
    if (pk.samples > 1) msaa_refused_ = true;
}
// A key in the pool: collected when built; with wait, one not started is claimed and compiled here, one running is
// waited for.
bool VulkanDevice::take_warmed(const WarmKey &key, bool wait) {
    if (!pool_.busy(key)) return false;
    WarmJob j;
    if (wait && pool_.claim(key, &j)) {
        pool_.count_compile();
        store_built(vk::build_warm(env_, j));
        return true;
    }
    if (wait) pool_.wait(key);
    else if (!pool_.done(key)) return false;
    collect_warmed();
    return true;
}
void VulkanDevice::collect_warmed() {
    pool_.collect([this](const vk::Built &b) { store_built(b); });
}
bool VulkanDevice::compile_combiner(const fc_combiner_key &key, std::string *error, uint32_t samples) {
    DrawPacket d{};
    d.cc = normalize_combiner(key);
    d.blend = (d.cc.opts & FC_CC_ALPHA) != 0;
    d.depth_test = 1; d.depth_write = d.blend ? 0 : 1; d.cull = FC_CULL_BACK;   // the likeliest draw state
    return draw_pipeline(d, supported_samples(samples, 1), vk::draw_state(d, true, set_cull_ != nullptr), error, false) != VK_NULL_HANDLE;
}
std::string VulkanDevice::pipeline_list() const {
    std::string out;
    for (const auto &kv : pipes_) if (kv.second.ok) out += vk::pipeline_line(kv.first);
    return out;
}
// The pipelines a list names, compiled in parallel on the pool and this thread; the call returns once they are built. A
// queued background job is claimed into the set; one already compiling is waited for. A sample count the device cannot
// draw is refused before anything compiles, and turns multisampling off (as a refused multisample pipeline does).
size_t VulkanDevice::warm_pipelines(const std::string &list, uint32_t at_samples) {
    if (!ok_) return 0;
    if (at_samples > 1 && supported_samples(at_samples, 1) != at_samples) {
        char b[128];
        std::snprintf(b, sizeof b, "warm_pipelines: samples=%u not supported here (at most %u)", at_samples, supported_samples(at_samples, 1));
        vos_log("AR_VULKAN %s", b);
        refused_.push_back(b);
        msaa_refused_ = true;
        return 0;
    }
    collect_warmed();
    const bool dyn = set_cull_ != nullptr;
    std::vector<WarmJob> jobs;
    std::vector<WarmKey> elsewhere;
    std::unordered_set<WarmKey, vk::WarmKeyHash> seen;
    for (const vk::ListLine &l : vk::parse_pipeline_lines(list)) {
        const uint32_t s = supported_samples(at_samples ? at_samples : l.samples, 1);
        const WarmKey wk{pipe_key(l.d, l.depth, s), vk::draw_state(l.d, true, dyn)};
        if (pipes_.count(wk.pk) || !seen.insert(wk).second) continue;
        if (pool_.busy(wk)) {
            WarmJob j;
            if (pool_.claim(wk, &j)) { pool_.count_compile(); jobs.push_back(j); }
            else elsewhere.push_back(wk);
            continue;
        }
        jobs.push_back(WarmJob{wk, l.d, VK_NULL_HANDLE, VK_NULL_HANDLE});
    }
    pool_.run(jobs);
    for (const WarmKey &k : elsewhere) pool_.wait(k);
    size_t built = 0;
    pool_.collect([&](const vk::Built &b) {
        store_built(b);
        if (b.ok && seen.count(b.key)) built++;
    });
    return built;
}
size_t VulkanDevice::warm_pipelines_background(const std::string &list, const std::vector<DrawPacket> &draws, uint32_t samples) {
    if (!ok_) return 0;
    std::vector<DrawPacket> want = draws;
    for (const vk::ListLine &l : vk::parse_pipeline_lines(list)) {
        if (!l.depth) continue;
        want.push_back(l.d);
        for (const DrawPacket &z : pipeline_siblings(l.d)) want.push_back(z);
    }
    const uint32_t s = supported_samples(samples, 1);
    const bool dyn = set_cull_ != nullptr;
    size_t queued = 0;
    for (const DrawPacket &d : want) {
        if (d.silhouette && decode_combiner(d.cc).unsupported) continue;
        const WarmKey wk{pipe_key(d, true, s), vk::draw_state(d, true, dyn)};
        if (pipes_.count(wk.pk) || pool_.busy(wk)) continue;
        if (pool_.enqueue(WarmJob{wk, d, VK_NULL_HANDLE, VK_NULL_HANDLE}, WarmPool::kBackground)) queued++;
    }
    return queued;
}
bool VulkanDevice::wait_pipelines_warmed(double timeout_s) {
    const bool idle = pool_.wait_all(timeout_s);
    collect_warmed();
    return idle && pool_.pending() == 0;
}
bool VulkanDevice::take_deferred_miss(std::string *key) {
    if (miss_log_.empty()) return false;
    if (key) *key = miss_log_.front();
    miss_log_.erase(miss_log_.begin());
    return true;
}
bool VulkanDevice::take_pipeline_error(std::string *why) {
    if (refused_.empty()) return false;
    if (why) *why = refused_.front();
    refused_.erase(refused_.begin());
    return true;
}
size_t VulkanDevice::pipeline_count() const {
    size_t n = 0;
    for (const auto &kv : pipes_) n += kv.second.ok ? 1 : 0;
    return n;
}

// The fixed passes' pipelines (depth maps, AO fill, composite) and the mip kernel compile on another thread from here on;
// their first use (fixed_pipes) waits for them. Without it they compile at that first use.
void VulkanDevice::prepare_passes() {
    if (!ok_) return;
    prepare_mips();
    start_fixed();
}
// metal_device.mm prepare_ao: the AO kernel compiles off the render thread as soon as AO is wanted (here with the other
// fixed passes' pipelines).
void VulkanDevice::prepare_ao() {
    if (ok_) start_fixed();
}
void VulkanDevice::start_fixed() {
    if (fixed_job_.valid() || fixed_taken_) return;
    VkDevice dev = dev_;
    VkPipelineCache cache = cache_;
    const vk::FixedLayouts layouts = fixed_layouts_;
    fixed_job_ = std::async(std::launch::async, [=] { return vk::make_fixed_pipelines(dev, cache, layouts); }).share();
}
const vk::FixedPipes &VulkanDevice::fixed_pipes() {
    if (!fixed_taken_ && ok_) {
        fixed_ = fixed_job_.valid() ? fixed_job_.get() : vk::make_fixed_pipelines(dev_, cache_, fixed_layouts_);
        fixed_job_ = std::shared_future<vk::FixedPipes>();
        fixed_taken_ = true;
        built_since_save_++;
    }
    return fixed_;
}

// ---------------------------------------------------------------------------------------------------------------------
// The pipeline cache file: read at start; written after a frame that built pipelines (at most every 5 s) and at the end
// ---------------------------------------------------------------------------------------------------------------------
void VulkanDevice::load_pipeline_cache() {
    std::vector<char> data;
    if (!cache_path_.empty()) {
        std::ifstream in(cache_path_, std::ios::binary);
        if (in) data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(dev_, &ci, nullptr, &cache_) != VK_SUCCESS) {   // a foreign or damaged file: start empty
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        if (vkCreatePipelineCache(dev_, &ci, nullptr, &cache_) != VK_SUCCESS) cache_ = VK_NULL_HANDLE;
    }
    if (!data.empty()) vos_log("AR_VULKAN pipeline cache: %zu bytes from %s", data.size(), cache_path_.c_str());
}
// On another thread (the render thread never serializes the cache nor writes the file); one save at a time.
void VulkanDevice::maybe_save_pipeline_cache(bool force) {
    if ((!built_since_save_ && !force) || !cache_ || cache_path_.empty()) return;
    const double t = now_s();
    if (!force && t - last_save_s_ < 5.0) return;
    if (save_job_.valid() && save_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;   // next frame
    last_save_s_ = t;
    built_since_save_ = 0;
    VkDevice dev = dev_;
    VkPipelineCache cache = cache_;
    const std::string path = cache_path_;
    save_job_ = std::async(std::launch::async, [=] { write_pipeline_cache(dev, cache, path); });
}
void VulkanDevice::save_pipeline_cache() {
    if (save_job_.valid()) save_job_.wait();
    if (cache_ && !cache_path_.empty()) write_pipeline_cache(dev_, cache_, cache_path_);
}
// The VkPipelineCache is internally synchronized: the pool's workers may add to it meanwhile. Written beside the file and
// renamed over it, so a crash mid-write never leaves a torn cache.
void VulkanDevice::write_pipeline_cache(VkDevice dev, VkPipelineCache cache, const std::string &path) {
    size_t n = 0;
    if (vkGetPipelineCacheData(dev, cache, &n, nullptr) != VK_SUCCESS || n == 0) return;
    std::vector<char> data(n);
    const VkResult r = vkGetPipelineCacheData(dev, cache, &n, data.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), (std::streamsize)n);
        if (!out) { vos_log("AR_VULKAN pipeline cache: could not write %s", tmp.c_str()); return; }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) vos_log("AR_VULKAN pipeline cache: could not replace %s", path.c_str());
}
}
}
