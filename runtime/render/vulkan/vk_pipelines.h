// The Vulkan device's pipelines (internal to ar_render_vulkan): the pipeline key and the pipeline list's line format, the
// render passes, the draw, resolve and fixed-pass pipelines, and the warm-up pool that builds them off the render thread.
// Everything here but WarmPool's caller-side methods is free of device state, so the pool's workers may call it.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>
#include "render_device.h"
namespace ar {
namespace vk {
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
constexpr VkFormat kMapFormat = VK_FORMAT_D32_SFLOAT;   // the depth maps (shadow, underside, footprint, AO views)
// encode_resolve's push constants (std430: the same offsets as the GLSL block)
struct ResolveU { float dst_origin[2]; uint32_t src_origin[2]; uint32_t src_last[2]; uint32_t linearize; uint32_t src_layer; };
static_assert(sizeof(ResolveU) == 32, "ResolveU matches the GLSL block");

// metal_device.mm's stencil convention: the low bits mark where a focus owner is the front-most opaque surface; the
// silhouette pass draws the owner where it is hidden and the bits are still 0, then bumps them, so each hidden pixel
// blends once. Bit 0x80 marks where a draw wrote depth.
enum StencilMode : uint8_t { kStencilKeep = 0, kStencilMarkFocus = 1, kStencilClearFocus = 2, kStencilSilhouette = 3, kStencilFaint = 4 };
constexpr uint32_t kStencilDepthWritten = 0x80, kStencilFocusBits = 0x7F;
uint8_t stencil_mode(const DrawPacket &d);
uint32_t stencil_ref(uint8_t mode);
// A draw pipeline's state beyond its key: compare op (bits 0-2), depth write (3), stencil mode (4-6), cull mode (7-8; 0
// when the cull mode is dynamic). Mark-focus and clear-focus differ only in the (dynamic) reference: one state.
uint32_t state_key(VkCompareOp cmp, bool write, uint8_t stencil, VkCullModeFlags cull);
// The state a draw is drawn with in a pass with (depth) or without a depth test.
uint32_t draw_state(const DrawPacket &d, bool pass_depth, bool dynamic_cull);

// metal_device.mm's PipeKey: PipeIdentity plus the target (format, depth, samples) and the derived alpha-to-coverage.
struct PipeKey {
    uint64_t slots = 0;
    uint32_t opts = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint8_t blend = 0, depth = 0, flat = 0, depth_only = 0, table_shadow = 0, clip = 0, samples = 1, a2c = 0, cut = 0;
    bool operator==(const PipeKey &o) const {
        return slots == o.slots && opts == o.opts && fmt == o.fmt && blend == o.blend && depth == o.depth && flat == o.flat &&
               depth_only == o.depth_only && table_shadow == o.table_shadow && clip == o.clip && samples == o.samples &&
               a2c == o.a2c && cut == o.cut;
    }
};
struct PipeKeyHash { size_t operator()(const PipeKey &k) const; };
// Metal's pipeline_list line ("%016llx %x %u %u %u %u %u %u %u %u[ 1]\n": slots, opts, format, blend, depth, flat,
// depth_only, table_shadow, clip, samples, then cut only when 1) with the VkFormat number as the format.
std::string pipeline_line(const PipeKey &k);
// One line (with or without its newline). False for a line over 96 bytes, with fewer than 8 fields (8: no clip, 9: no
// samples, 10: no cut) or a field out of range; a2c is not in the line (0).
bool parse_pipeline_line(const std::string &line, PipeKey *out);
// A list's lines that name the renderer's target format, at most the first 4096 lines; the draw each names.
struct ListLine { DrawPacket d; bool depth; uint32_t samples; };
std::vector<ListLine> parse_pipeline_lines(const std::string &list);
// metal_device.mm pipeline_form: every silhouette copy shares the shade combiner's pipeline.
DrawPacket pipeline_form(const DrawPacket &d);

VkShaderModule make_module(VkDevice dev, const std::vector<uint32_t> &spv);
// A target's render pass. samples 1: RGBA8 + D32S8, colour, depth and stencil loaded (or cleared) and stored. samples > 1:
// the multisample colour and depth (attachments 0, 1) resolve into the single-sample ones (2, 3: depth by depth_mode,
// stencil by stencil_mode, through VK_KHR_depth_stencil_resolve); transient: the multisample attachments are not stored.
// Every attachment is in its attachment layout before and after (the device's barriers order the passes). All passes of
// one sample count are compatible.
struct TargetPassDesc {
    uint32_t samples = 1;
    bool clear = false, transient = false;
    VkResolveModeFlagBits depth_mode = VK_RESOLVE_MODE_NONE, stencil_mode = VK_RESOLVE_MODE_NONE;
    PFN_vkCreateRenderPass2KHR create2 = nullptr;
};
VkRenderPass make_target_pass(VkDevice dev, const TargetPassDesc &d);
// The resolve's render pass into an external image: colour (and depth when df is not UNDEFINED) loaded and stored,
// in and out in the attachment layouts.
VkRenderPass make_resolve_pass(VkDevice dev, VkFormat cf, VkFormat df);
// A depth map's render pass: D32_SFLOAT cleared and stored, in and out DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
VkRenderPass make_map_pass(VkDevice dev);
// The resolve's shaders: vertex, fragment, and the fragment that also writes gl_FragDepth.
bool make_resolve_shaders(VkDevice dev, VkShaderModule out[3], std::string *err);
VkPipeline make_resolve_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout, VkRenderPass rp, VkShaderModule vs,
                                 VkShaderModule fs, bool depth);

// What a draw pipeline is built against (immutable while the pool runs, but rp[s] for a sample count is set before
// the first job of that count is queued).
struct BuildEnv {
    VkDevice dev = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass rp[5] = {};   // by sample count (1, 2, 4): a compatible pass
    bool dynamic_cull = false;
    bool alpha_to_one = false;   // the alphaToOne feature is enabled: set with alpha-to-coverage
};
// A draw's combiner shaders (emit_glsl + compile_glsl). refused: the GLSL or the driver failed, not a combiner the
// renderer cannot draw (unsupported).
bool make_combiner_shaders(VkDevice dev, const DrawPacket &draw, bool a2c, VkShaderModule *vs, VkShaderModule *fs, std::string *why,
                           bool *refused);
VkPipeline make_draw_pipeline(const BuildEnv &env, VkShaderModule vs, VkShaderModule fs, const PipeKey &k, uint32_t state, std::string *why);

// The fixed passes' pipelines (prepare_passes): the depth maps' (depth.vert, without and with depth_cut.frag, each
// without and with depth clamp), the AO fill and the gamma mip kernels. Null where a compile failed (logged).
struct FixedLayouts {
    VkPipelineLayout depth = VK_NULL_HANDLE, ao = VK_NULL_HANDLE, mip = VK_NULL_HANDLE;
    VkRenderPass map_pass = VK_NULL_HANDLE;
};
struct FixedPipes { VkPipeline depth[2][2] = {}; VkPipeline ao = VK_NULL_HANDLE; };   // depth[cut][clamp]
FixedPipes make_fixed_pipelines(VkDevice dev, VkPipelineCache cache, const FixedLayouts &l);
VkPipeline make_mip_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout);
VkPipeline make_compute_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout, const char *shader);

// ---------------------------------------------------------------------------------------------------------------------
// The warm-up pool: metal_device.mm's warm lanes, promotions and holds on two worker threads.
// ---------------------------------------------------------------------------------------------------------------------
struct WarmKey {
    PipeKey pk;
    uint32_t state = 0;
    bool operator==(const WarmKey &o) const { return pk == o.pk && state == o.state; }
};
struct WarmKeyHash { size_t operator()(const WarmKey &k) const { return PipeKeyHash()(k.pk) * 31u + k.state; } };
struct WarmJob {
    WarmKey key;
    DrawPacket d{};
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;   // given: only the state's pipeline is built
};
struct Built {
    WarmKey key;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    bool own_modules = false;    // made by this build (the job gave none)
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool ok = false, refused = false;
    std::string why;
};
// Builds a job's shaders (unless given) and its pipeline; any thread.
Built build_warm(const BuildEnv &env, const WarmJob &j);

class WarmPool {
public:
    using Compile = std::function<Built(const WarmJob &)>;
    // background: warm_pipelines_background's list (in order); urgent: a draw's own miss (ahead of the background);
    // inline: warm_pipelines' jobs (ahead of all, never held; the calling thread compiles too).
    enum Kind : uint8_t { kBackground = 0, kUrgent = 1, kInline = 2 };
    static constexpr int kWorkers = 2;
    static constexpr uint32_t kMaxPromotions = 4;
    WarmPool() = default;
    WarmPool(const WarmPool &) = delete;
    WarmPool &operator=(const WarmPool &) = delete;
    ~WarmPool() { stop(); }
    void start(Compile compile);
    // Drops the queued jobs, releases the holds and joins the workers (a running job finishes; collect takes it).
    void stop();
    // false: the key is already queued, running or built and not collected
    bool enqueue(const WarmJob &job, Kind kind = kBackground);
    // A queued job not started yet is removed and handed to the caller, which compiles it (counted: count_compile).
    bool claim(const WarmKey &key, WarmJob *out);
    // A queued background job runs ahead of the list (the promoted hold gates it, not the background one); at most
    // kMaxPromotions promoted jobs are in flight. False when not promoted.
    bool promote(const WarmKey &key);
    bool busy(const WarmKey &key) const;       // queued, running or built and not collected
    bool busy_pipe(const PipeKey &pk) const;   // busy for any draw state of the key
    bool promote_pipe(const PipeKey &pk);      // promote: the key's first queued background job, whatever its state
    bool done(const WarmKey &key) const;       // built and not collected
    // Blocks until the key is built (true) or not in the pool; a held job keeps it waiting.
    void wait(const WarmKey &key);
    bool wait_all(double timeout_s);           // nothing queued or running
    // warm_pipelines: queues the jobs as inline, compiles them on the calling thread and the workers, and returns once all
    // of them are built.
    void run(const std::vector<WarmJob> &jobs);
    // The built results, handed over on the calling thread (the render thread).
    void collect(const std::function<void(const Built &)> &f);
    size_t pending() const;                    // queued + running + built and not collected
    uint32_t running() const;                  // background jobs compiling now
    uint32_t peak() const;
    uint64_t compiles() const;
    void count_compile();
    void hold_background(bool hold);
    void hold_promoted(bool hold);
    void hold_compiles(bool hold);

private:
    enum State : uint8_t { kQueued, kRunning, kDone };
    struct Entry { WarmJob job; Kind kind; bool promoted; };
    void loop();
    bool pickable(size_t *at) const;           // under m_
    void execute(Entry e, std::unique_lock<std::mutex> &l, bool honour_compile_hold);
    Compile compile_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Entry> queue_;
    std::unordered_map<WarmKey, State, WarmKeyHash> keys_;
    std::vector<Built> done_;
    std::vector<std::thread> workers_;
    bool stop_ = false, hold_bg_ = false, hold_promo_ = false, hold_compile_ = false;
    uint32_t running_ = 0, bg_running_ = 0, peak_ = 0, promoting_ = 0;
    uint64_t compiles_ = 0;
};
}
}
