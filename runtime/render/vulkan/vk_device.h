// The Vulkan IRenderDevice's class (internal to ar_render_vulkan). Its members are defined in vulkan_device.cpp (init,
// targets, recording, the resolve, frames, read-back), vk_textures.cpp (textures, mips, samplers) and
// vk_device_pipes.cpp (draw pipelines, the warm-up, the pipeline cache, the fixed passes' pipelines).
//
// The Vulkan port of metal_device.mm (read it for the behaviour; the comments here say only where Vulkan differs).
// - Y: the shared matrices are Metal's (NDC +y up). Every draw pass flips with a negative-height viewport (Vulkan 1.1), so
//   the image, the winding (front face counter-clockwise, as Metal's encoder sets it) and gl_FragCoord (top-left origin)
//   all match Metal.
// - Depth: reverse-Z, cleared to 0, GREATER_OR_EQUAL when testing; D32_SFLOAT_S8_UINT with Metal's stencil convention.
// - Depth, stencil and (without VK_EXT_extended_dynamic_state) cull state are pipeline state in Vulkan 1.1: a PipeKey
//   (Metal's key) holds the compiled shader modules and its VkPipeline variants by draw state (vk::draw_state).
// - Multisample targets draw into transient multisample attachments resolved in the same render pass (colour averaged,
//   depth MAX through VK_KHR_depth_stencil_resolve).
// - Frames: one command buffer per frame, kFrames in flight; a completion thread waits the fences in order and calls
//   on_gpu_done. Resources replaced while frames may use them are retired with the current frame serial and destroyed
//   once that frame completed.
// - Decals: slope-scaled depth bias +decal_slope (reverse-Z: positive is toward the viewer, as Metal's setDepthBias).
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "vk_frame_ring.h"
#include "vk_pipelines.h"
#include "vk_resources.h"
#include "vulkan_device.h"
namespace ar {
namespace vkd {
using vk::Buffer;
using vk::Image;
using vk::Slice;
constexpr uint32_t kFrames = vk::FrameRing::kSlots;   // frames in flight (one ring each)
constexpr VkFormat kPlaceholderDepth = VK_FORMAT_D32_SFLOAT;

// The uniform structs: byte for byte metal_device.mm's, also glsl_emitter.cpp's std140 blocks.
struct DrawU { float prim[4]; float env[4]; float lod; float decal; float noise_seed; float fade; float clip[4]; float clip_on; uint32_t lflags;
               float pull; float talpha; };
static_assert(sizeof(DrawU) == 80, "DrawU matches the GLSL block");
struct LightU {
    float dir_on[4]; float tint_amb[4]; float contact[4]; float under_uv[16]; float under_range[4];
    float shadow[4]; float shadow_uv[16]; float cast[4];
    float relight[4];
    float ao[4];
    float ao_min[4];
    float ao_inv[4];
    float toy[4];
    float env0[4], env1[4], env2[4];
    float env3[4], env4[4], env5[4];
    float liquid[4];
    float liquid2[4];
    float water_box[8][4];
    float water_y[2][4];
    float water_foot[4];
    float cut[4]; float cut2[4];
    float trans[4]; float trans_rgb[4]; float trans_c[4];
    float tex[4];
};
static_assert(sizeof(LightU) == 704, "LightU matches the GLSL block");
struct ViewU { float clip_from_model[16]; float eye_pull[4]; float eye_view[4]; uint32_t layer, pad[3]; float wclip_lo[4]; float wclip_hi[4];
               float interp[4]; };
static_assert(sizeof(ViewU) == 160, "ViewU matches the GLSL block");

struct Rec {
    PassDesc pass{};
    ViewConstants view{};
    LightU light{};
    VkBuffer vb = VK_NULL_HANDLE, nb = VK_NULL_HANDLE, pb = VK_NULL_HANDLE;
    VkDeviceSize voff = 0, noff = 0, poff = 0;
    uint32_t nv = 0;
    std::vector<DrawPacket> draws;
    // each draw's two game textures as they were at record time (an upload that replaces one retires the old image
    // with this frame's serial, so the view stays valid until the frame completed)
    std::vector<VkImageView> tex;
    std::vector<uint32_t> sampled;   // targets (1-based) whose resolved colour the draws sample (bind_target_texture)
};
struct Target {
    Image color, depth;                          // the single-sample (resolved) images; views: 2D_ARRAY of all layers
    Image ms_color, ms_depth;                    // samples > 1: the multisample attachments
    VkImageView depth_sample = VK_NULL_HANDLE;   // depth aspect only, for the resolve
    VkImageView color_2d = VK_NULL_HANDLE;       // layer 0 as a 2D view (bind_target_texture; one-layer targets)
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkImageLayout color_layout = VK_IMAGE_LAYOUT_UNDEFINED, depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool ms_ready = false;                       // the multisample attachments were transitioned once
    bool memoryless = false;                     // transient multisample attachments in LAZILY_ALLOCATED memory
    uint32_t w = 0, h = 0, layers = 1, samples = 1;
    std::vector<Rec> recs;
    size_t nrec = 0;
    bool encoded = false;
};
// A PipeKey's compiled shaders and its pipelines by draw state.
struct Pipe {
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    bool ok = false;
    std::string why;
    std::map<uint32_t, VkPipeline> variants;   // VK_NULL_HANDLE: that variant failed (logged once)
};
struct Tex { Image im; uint32_t w = 0, h = 0, levels = 1; bool storage = false; };

class VulkanDevice final : public IRenderDevice {
public:
    explicit VulkanDevice(const VulkanDeviceInit &init);
    ~VulkanDevice() override;
    bool ok() const { return ok_; }
    bool frame_ok() const { return frame_ok_; }
    void accept_any_depth_resolve();

    // textures and samplers (vk_textures.cpp)
    void set_edge_coverage(bool on) override { edge_ = on; }
    void set_texture_filter(TextureFilter f) override;
    void upload_texture(uint32_t tex_id, uint32_t w, uint32_t h, const uint8_t *rgba) override;
    void set_fine_textures(bool on) override;
    uint32_t anisotropy() const override { return filter_ == TextureFilter::smooth ? (fine_ ? 16u : 4u) : 0u; }
    bool wait_mips_ready(double timeout_s) override;
    MipCounts mip_counts() const override { return MipCounts{mips_kernel_, mips_blit_, late_gamma_.size() + late_blit_.size()}; }
    bool read_texture_level(uint32_t tex_id, uint32_t level, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) override;

    // targets
    uint32_t create_target(uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) override;
    void resize_target(uint32_t t, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples) override;
    uint32_t supported_samples(uint32_t wanted, uint32_t layers) const override;
    uint32_t target_samples(uint32_t t) const override { return t == 0 || t > targets_.size() ? 1u : targets_[t - 1].samples; }
    void bind_target_texture(uint32_t tex_id, uint32_t target) override;
    void release_target(uint32_t t) override;
    bool memoryless_msaa() const override { return res_.lazy_available(); }
    void set_rate_map(uint32_t target, void *map) override { (void)target; (void)map; }   // no rate maps here

    // recording
    void encode_pass(uint32_t target, const PassDesc &pass, const ViewConstants &view, const fc_vertex *verts, uint32_t nv,
                     const DrawPacket *draws, uint32_t nd, const int8_t *normals, const float *prev_pos) override;
    void set_lighting(const LightingFrame *lf) override;
    // Task 3: the depth maps, AO, the environment reduction and composite
    void encode_underside_map(const UndersideDesc &, const fc_vertex *, uint32_t, const DrawPacket *, uint32_t, const DepthExtras &) override {}
    void encode_shadow_map(const ShadowFit &, const fc_vertex *, uint32_t, const DrawPacket *, uint32_t, const DepthExtras &) override {}
    bool bake_ao(uint32_t, const AoVolume &, const std::vector<ShadowFit> &, float, const fc_vertex *, uint32_t, const DrawPacket *,
                 uint32_t, const DepthExtras &) override { return false; }
    void encode_environment_reduce(void *, uint32_t) override {}
    bool environment_stats(EnvStats *, uint32_t *) const override { return false; }
    void encode_composite(uint32_t, void *, uint32_t, uint32_t) override {}
    void encode_resolve(uint32_t src, const ExternalView &dst) override;

    // frames
    void begin_frame() override;
    void end_frame(void (*done)(void *), void *ctx, void *drawable) override;
    void wait_idle() override;
    double last_gpu_ms() const override { return gpu_ms_.load(); }
    uint32_t render_encoders_last_frame() const override { return encoders_last_; }
    uint64_t late_passes() const override { return late_; }
    void *command_buffer() override { return (void *)cmd_; }
    bool read_target(uint32_t t, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) override;
    bool read_target_depth(uint32_t t, std::vector<float> &depth, uint32_t &w, uint32_t &h) override;

    // pipelines (vk_device_pipes.cpp)
    bool compile_combiner(const fc_combiner_key &key, std::string *error, uint32_t samples) override;
    std::string pipeline_list() const override;
    size_t warm_pipelines(const std::string &list, uint32_t at_samples) override;
    size_t warm_pipelines_background(const std::string &list, const std::vector<DrawPacket> &draws, uint32_t samples) override;
    size_t pipelines_warming() override { collect_warmed(); return pool_.pending(); }
    bool wait_pipelines_warmed(double timeout_s) override;
    void hold_background_warm(bool hold) override { pool_.hold_background(hold); }
    uint32_t background_warm_peak() const override { return pool_.peak(); }
    uint64_t warm_compiles() const override { return pool_.compiles(); }
    uint64_t warm_promotions() const override { return promotions_; }
    void hold_promoted_warm(bool hold) override { pool_.hold_promoted(hold); }
    void hold_warm_compiles(bool hold) override { pool_.hold_compiles(hold); }
    size_t draw_time_builds() const override { return draw_builds_; }
    void set_async_pipelines(bool on) override { async_ = on; }
    bool async_pipelines() const override { return async_; }
    uint64_t deferred_draws() const override { return deferred_; }
    bool take_deferred_miss(std::string *key) override;
    void prepare_passes() override;
    bool take_pipeline_error(std::string *why) override;
    size_t pipeline_count() const override;

private:
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkQueryPool queries = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> pools;
        size_t pool_at = 0;
        uint64_t serial = 0;        // the frame last recorded in this slot
        bool submitted = false;     // its fence will signal (reset it before reuse)
    };
    struct Job { uint64_t serial; VkFence fence; VkQueryPool queries; void (*done)(void *); void *ctx; };
    struct Garbage { uint64_t serial; std::function<void()> free; };
    struct Upload { Tex tex; Buffer staging; };

    // init and teardown (vulkan_device.cpp)
    bool init_device();
    void detect_msaa(bool any_depth_mode);
    bool make_layouts();
    bool make_placeholders();
    bool make_resolve_shaders();
    void destroy_all();
    // frames and memory
    void retire(std::function<void()> f);
    void retire_image(Image im) { retire([this, im]() mutable { res_.destroy_image(im); }); }
    void collect_garbage();
    VkCommandBuffer begin_once();
    bool end_once();
    VkDescriptorSet alloc_set(VkDescriptorSetLayout layout);
    void completion_loop();
    // targets and recording
    void make_target(Target &t, uint32_t w, uint32_t h, uint32_t layers, uint32_t samples);
    void retire_target(Target &t);
    void destroy_target(Target &t);
    void transition_color(Target &t, VkImageLayout to, bool discard = false);
    void transition_depth(Target &t, VkImageLayout to, bool discard = false);
    VkCommandBuffer cmd_or_once() const { return recording_once_ ? once_cmd_ : cmd_; }
    bool read_image(Target &x, bool depth, std::vector<uint8_t> *out);
    VkRenderPass target_pass(uint32_t samples, bool clear, bool transient);
    void flush(Target &t);
    bool scissor_for(const Viewport &v, uint32_t bw, uint32_t bh, VkRect2D *out) const;
    void encode_rec(const Target &t, const Rec &r);
    VkDescriptorSet texture_set(const VkImageView tex[2], const VkSampler samp[2]);
    bool record_resolve(uint32_t src, const ExternalView &dst);
    VkRenderPass resolve_render_pass(VkFormat cf, VkFormat df);
    VkPipeline resolve_pipeline(VkRenderPass rp, bool depth, bool *pending);
    // textures (vk_textures.cpp)
    VkSampler sampler(const fc_sampler &s, bool depth_pass = false);
    VkImageView texture(uint32_t tex_id, uint32_t drawing_target) const;
    void apply_fine();
    void record_textures(bool frame_start);
    bool within_gamma_budget(size_t count, size_t texels) const;
    static bool gamma_eligible(const Tex &t) { return t.storage && t.levels > 1 && (size_t)t.w * t.h <= kFineMaxTexels; }
    void encode_blit_mips(const std::vector<Tex> &list);
    void encode_gamma_mips(const std::vector<Tex> &list);
    void prepare_mips();
    VkPipeline mip_pipeline_if_ready();
    static void forget(std::vector<Tex> &list, VkImage img);
    // pipelines (vk_device_pipes.cpp)
    vk::PipeKey pipe_key(const DrawPacket &d, bool depth, uint32_t samples) const;
    VkPipeline draw_pipeline(const DrawPacket &d, uint32_t samples, uint32_t state, std::string *err, bool at_draw);
    void store_built(const vk::Built &b);
    void record_refusal(const vk::PipeKey &pk, const std::string &why);
    bool take_warmed(const vk::WarmKey &key, bool wait);
    void collect_warmed();
    void maybe_save_pipeline_cache(bool force);
    void load_pipeline_cache();
    void save_pipeline_cache();   // now, after any save in flight (the destructor)
    static void write_pipeline_cache(VkDevice dev, VkPipelineCache cache, const std::string &path);
    const vk::FixedPipes &fixed_pipes();   // waits for prepare_passes' compile (or compiles now)

    VkInstance inst_;
    VkPhysicalDevice phys_;
    VkDevice dev_;
    uint32_t qfam_;
    VkQueue queue_;
    PFN_vkGetInstanceProcAddr gipa_;
    std::string cache_path_;
    bool ok_ = false;
    vk::Memory res_;
    VkPhysicalDeviceProperties props_{};
    VkDeviceSize ring_align_ = 256;
    bool timestamps_ = false;
    uint64_t timestamp_mask_ = ~0ull;
    bool depth_sampled_ = false;
    bool storage_rgba8_ = false;   // RGBA8 storage images: the gamma mip kernel writes the textures' levels
    bool depth_sample_warned_ = false;
    // multisampling
    PFN_vkCreateRenderPass2KHR create_rp2_ = nullptr;
    PFN_vkCmdSetCullModeEXT set_cull_ = nullptr;   // VK_EXT_extended_dynamic_state enabled: cull is dynamic
    VkSampleCountFlags sample_counts_ = VK_SAMPLE_COUNT_1_BIT;
    VkResolveModeFlagBits depth_resolve_ = VK_RESOLVE_MODE_NONE, stencil_resolve_ = VK_RESOLVE_MODE_NONE;
    bool msaa_refused_ = false;
    std::map<uint32_t, VkRenderPass> target_passes_;   // by samples | clear << 8 | transient << 9
    // commands
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer once_cmd_ = VK_NULL_HANDLE;
    VkFence once_fence_ = VK_NULL_HANDLE;
    bool recording_once_ = false;
    // pipelines
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set0_layout_ = VK_NULL_HANDLE, set1_layout_ = VK_NULL_HANDLE, resolve_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout depth_set0_layout_ = VK_NULL_HANDLE, depth_set1_layout_ = VK_NULL_HANDLE, ao_set_layout_ = VK_NULL_HANDLE,
                          mip_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE, resolve_layout_ = VK_NULL_HANDLE;
    vk::FixedLayouts fixed_layouts_;
    vk::BuildEnv env_;
    vk::WarmPool pool_;
    std::unordered_map<vk::PipeKey, Pipe, vk::PipeKeyHash> pipes_;
    std::vector<std::string> refused_;
    size_t draw_builds_ = 0;
    uint64_t promotions_ = 0, deferred_ = 0;
    bool async_ = false;
    std::unordered_map<vk::PipeKey, bool, vk::PipeKeyHash> missed_;
    std::vector<std::string> miss_log_;
    uint64_t built_since_save_ = 0;
    double last_save_s_ = -1e9;
    std::future<void> save_job_;
    std::shared_future<vk::FixedPipes> fixed_job_;
    vk::FixedPipes fixed_{};
    bool fixed_taken_ = false;
    VkShaderModule resolve_vs_ = VK_NULL_HANDLE, resolve_fs_ = VK_NULL_HANDLE, resolve_fs_depth_ = VK_NULL_HANDLE;
    std::map<uint64_t, VkRenderPass> resolve_passes_;
    std::map<uint64_t, VkPipeline> resolve_pipes_;
    std::map<uint64_t, std::shared_future<VkPipeline>> resolve_jobs_;
    bool edge_ = false;
    // samplers and textures
    VkSampler linear_clamp_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE, compare_sampler_ = VK_NULL_HANDLE;
    std::map<uint32_t, VkSampler> samplers_;
    Image white_, depth_ph_, ao_ph_, env_ph_;
    std::unordered_map<uint32_t, Tex> textures_;
    std::vector<Upload> uploads_;          // staged, not yet recorded
    std::vector<Tex> pending_mips_, late_gamma_, late_blit_;
    TextureFilter filter_ = TextureFilter::smooth;
    bool fine_ = false, fine_want_ = false, mip_failed_ = false;
    uint64_t mips_kernel_ = 0, mips_blit_ = 0;
    size_t gamma_n_ = 0, gamma_texels_ = 0;   // this frame's kernel work
    std::shared_future<VkPipeline> mip_job_;
    VkPipeline mip_pipe_ = VK_NULL_HANDLE;
    bool mip_taken_ = false;
    std::map<uint32_t, uint32_t> target_tex_;
    // targets and frames
    std::deque<Target> targets_;   // a deque: create_target never moves the targets (flush holds references)
    LightU light_{};
    vk::FrameRing ring_;
    Frame frames_[kFrames];
    uint32_t slot_ = 0;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;   // the open frame's
    bool frame_open_ = false;
    bool frame_ok_ = false;                  // vulkan_device_frame_ok
    uint64_t frame_serial_ = 0;              // the last frame begun
    uint32_t encoders_ = 0, encoders_last_ = 0;
    uint64_t late_ = 0;
    std::deque<Garbage> garbage_;
    std::atomic<double> gpu_ms_{-1.0};
    // shared with the completion thread (under m_)
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    uint64_t completed_serial_ = 0, submitted_serial_ = 0;
    bool stop_ = false;
    std::thread done_thread_;
};
}
}
