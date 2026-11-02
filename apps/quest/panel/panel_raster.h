// The settings panel drawn on the CPU into an RGBA texture (straight alpha, as the renderer's blended draws expect):
// the tab strip on top, the visible rows of the current tab (the hovered one highlighted, disabled ones dimmed), the
// scroll column, and the status line and the hint at the bottom. Text uses a TrueType font baked once at 24 and 16 px
// (stb_truetype) when load_font succeeded, else a built-in 5x7 bitmap font, so the panel is never blank.
#pragma once
#include <stdint.h>
#include <memory>
#include <string>
#include <vector>
#include "panel_model.h"

namespace ar {
namespace panel {

class Raster {
public:
    Raster(uint32_t w, uint32_t h);
    ~Raster();
    Raster(const Raster &) = delete;
    Raster &operator=(const Raster &) = delete;
    bool load_font(const char *ttf_path);   // false (the bitmap font stays) when the file is missing or not a font
    bool has_font() const;
    void draw(const std::vector<Tab> &tabs, int tab, const Hit &hover, const Layout &L, int scroll_rows,
              const std::string &status, const std::string &hint);
    const uint8_t *rgba() const { return px_.data(); }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }

    // How many leading characters (advances adv) fit with an ellipsis of width ell inside max_w: the width is kept
    // incrementally, one subtraction per removed character (all of them when the whole string fits).
    static size_t fit_chars(const std::vector<float> &adv, float ell, float max_w);

    struct Font;
private:
    enum Size { small = 0, big = 1 };
    void clear();
    void blend(int x, int y, const uint8_t c[4], float cover);
    void rect(float x0, float y0, float x1, float y1, const uint8_t c[4], float radius = 0.f);
    float text_width(const std::string &s, Size sz) const;
    // Drawn with its left edge at x and its vertical centre at cy; cut with "..." to max_w. Returns the width drawn.
    float text(float x, float cy, const std::string &s, Size sz, const uint8_t c[4], float max_w = 1e9f);
    float text_centred(float x0, float x1, float cy, const std::string &s, Size sz, const uint8_t c[4]);
    void row(const Control &c, float y, const Layout &L, bool hovered);
    uint32_t w_, h_;
    std::vector<uint8_t> px_, bg_;
    std::unique_ptr<Font> fonts_[2];
};

}  // namespace panel
}  // namespace ar
