// The Quest session: the shared runtime and renderer on QxPlatform's OpenXR frame loop and the Vulkan device, the
// counterpart of VosSession reduced to the cube (no audio, no foveation, no probes).
//
// Threads: every member runs on the thread that calls frame() (android_main's loop), except the game thread that
// start() creates (it calls the platform's get_pad) and the device's completion thread (it only ends runtime frames).
#pragma once
#include <stdint.h>
#include <memory>
#include <string>
extern "C" {
#include "ar_runtime.h"
}
#include "qx_platform.h"
#include "render_device.h"
#include "renderer.h"

namespace ar {

class QxSession {
public:
    QxSession(const ar_adapter *adapter, QxPlatform &platform, const std::string &files_dir);
    ~QxSession();
    QxSession(const QxSession &) = delete;
    QxSession &operator=(const QxSession &) = delete;
    // The Vulkan device (pipeline cache at files_dir/pipelines.vkcache), the renderer, the runtime, init_game, the texture
    // sink and the game thread. After the platform started. On failure everything it made is torn down again.
    bool start(const std::string &rom_path, const std::string &save_dir, std::string *err);
    bool frame();        // one display frame; false when the platform stopped
    void stop();         // stop_game_thread, wait_idle, destroy; safe to call twice and after a failed start
    uint64_t ticks_done() const;

private:
    const ar_adapter *adapter_;
    QxPlatform &platform_;
    std::string files_dir_;
    std::unique_ptr<IRenderDevice> dev_;
    std::unique_ptr<Renderer> renderer_;
    ar_runtime *rt_ = nullptr;
    uint32_t target_ = 0, w_ = 0, h_ = 0, layers_ = 0, samples_ = 0;
    int64_t last_time_ = 0;
    bool have_time_ = false, logged_first_ = false;
};

}
