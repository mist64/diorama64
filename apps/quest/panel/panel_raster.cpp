#include "panel_raster.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace ar {
namespace panel {

struct Raster::Font {
    float px = 0.f, ascent = 0.f, descent = 0.f;
    int bw = 0, bh = 0;
    std::vector<uint8_t> atlas;
    stbtt_bakedchar chars[95];
};

namespace {
// The fallback: 5x7 glyphs for ' '..'~', one byte per column, bit 0 the top row.
const uint8_t kGlyphs[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x08, 0x2A, 0x1C, 0x2A, 0x08}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00}, {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78}, {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C}, {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00}, {0x08, 0x04, 0x08, 0x10, 0x08},
};

// UTF-8 to the printable ASCII both fonts have; the few non-ASCII characters the game's labels use become look-alikes.
std::string ascii(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) { cp = ((c & 7u) << 18) | ((s[i + 1] & 63u) << 12) | ((s[i + 2] & 63u) << 6) | (s[i + 3] & 63u); n = 4; }
        else if (c >= 0xE0 && i + 2 < s.size()) { cp = ((c & 15u) << 12) | ((s[i + 1] & 63u) << 6) | (s[i + 2] & 63u); n = 3; }
        else if (c >= 0xC0 && i + 1 < s.size()) { cp = ((c & 31u) << 6) | (s[i + 1] & 63u); n = 2; }
        i += n;
        if (cp >= 32 && cp < 127) o += (char)cp;
        else if (cp == 0x2026) o += "...";
        else if (cp == 0xD7) o += 'x';
        else if (cp == 0xB0) o += " deg";
        else if (cp == 0x2605 || cp == 0x2606) o += '*';
        else if (cp == 0x2013 || cp == 0x2014) o += '-';
        else if (cp == 0x2018 || cp == 0x2019) o += '\'';
        else if (cp == 0x201C || cp == 0x201D) o += '"';
        else if (cp == '\t') o += ' ';
        else if (cp >= 0x80) o += '?';
    }
    return o;
}

const uint8_t kBg[4] = {24, 26, 32, 255}, kTabBg[4] = {44, 48, 58, 255}, kTabHover[4] = {72, 78, 94, 255};
const uint8_t kAccent[4] = {64, 132, 246, 255}, kText[4] = {236, 238, 242, 255}, kDim[4] = {150, 156, 168, 255};
const uint8_t kHoverRow[4] = {255, 255, 255, 26}, kTrack[4] = {70, 74, 86, 255}, kPill[4] = {52, 56, 66, 255};
const uint8_t kButton[4] = {58, 64, 78, 255}, kButtonHover[4] = {84, 92, 112, 255}, kQuit[4] = {176, 58, 58, 255};
const uint8_t kHeading[4] = {124, 172, 255, 255}, kHint[4] = {255, 196, 80, 255}, kKnob[4] = {245, 246, 250, 255};
const uint8_t kLine[4] = {255, 255, 255, 30};

// The colour at a quarter alpha for disabled rows.
struct Col { uint8_t c[4]; };
Col dimmed(const uint8_t c[4], bool enabled) { Col o{{c[0], c[1], c[2], (uint8_t)(enabled ? c[3] : c[3] * 0.35f)}}; return o; }
}  // namespace

Raster::Raster(uint32_t w, uint32_t h) : w_(w), h_(h), px_((size_t)w * h * 4, 0) {}
Raster::~Raster() = default;

bool Raster::has_font() const { return fonts_[0] && fonts_[1]; }

bool Raster::load_font(const char *path) {
    FILE *f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) return false;
    std::vector<uint8_t> data;
    uint8_t b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) data.insert(data.end(), b, b + n);
    std::fclose(f);
    stbtt_fontinfo info;
    if (data.size() < 12 || !stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0))) return false;
    std::unique_ptr<Font> made[2];
    const float sizes[2] = {16.f, 24.f};
    for (int i = 0; i < 2; i++) {
        std::unique_ptr<Font> ft(new Font);
        ft->px = sizes[i];
        ft->bw = 512; ft->bh = i == 0 ? 128 : 256;
        ft->atlas.assign((size_t)ft->bw * ft->bh, 0);
        if (stbtt_BakeFontBitmap(data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0), ft->px, ft->atlas.data(), ft->bw, ft->bh,
                                 32, 95, ft->chars) <= 0)
            return false;
        int asc = 0, desc = 0, gap = 0;
        stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
        const float s = stbtt_ScaleForPixelHeight(&info, ft->px);
        ft->ascent = asc * s; ft->descent = desc * s;
        made[i] = std::move(ft);
    }
    fonts_[0] = std::move(made[0]);
    fonts_[1] = std::move(made[1]);
    return true;
}

void Raster::blend(int x, int y, const uint8_t c[4], float cover) {
    if (x < 0 || y < 0 || x >= (int)w_ || y >= (int)h_ || cover <= 0.f) return;
    uint8_t *d = &px_[((size_t)y * w_ + (size_t)x) * 4];
    const uint32_t a = cover >= 1.f ? c[3] : (uint32_t)(c[3] * cover + 0.5f);
    if (a == 255 || d[3] == 0) {   // opaque, or onto nothing: the colour as it is
        d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = (uint8_t)a;
        return;
    }
    if (d[3] == 255) {   // onto an opaque pixel (the backing is opaque): no division by the result's alpha
        for (int k = 0; k < 3; k++) d[k] = (uint8_t)((c[k] * a + d[k] * (255u - a) + 127u) / 255u);
        return;
    }
    // straight alpha "over", in 0..255 integers: oa = a + da (1 - a); rgb = (c a + d da (1 - a)) / oa
    const uint32_t da = d[3] * (255u - a) / 255u, oa = a + da;
    if (!oa) return;
    for (int k = 0; k < 3; k++) d[k] = (uint8_t)((c[k] * a + d[k] * da + oa / 2) / oa);
    d[3] = (uint8_t)oa;
}

void Raster::rect(float x0, float y0, float x1, float y1, const uint8_t c[4], float r) {
    r = std::max(0.f, std::min(r, 0.5f * std::min(x1 - x0, y1 - y0)));
    // pixel centres inside [x0, x1] x [y0, y1]
    const int ix0 = std::max(0, (int)std::ceil(x0 - 0.5f)), iy0 = std::max(0, (int)std::ceil(y0 - 0.5f));
    const int ix1 = std::min((int)w_ - 1, (int)std::floor(x1 - 0.5f)), iy1 = std::min((int)h_ - 1, (int)std::floor(y1 - 0.5f));
    for (int y = iy0; y <= iy1; y++) {
        const float py = y + 0.5f;
        const bool band = py < y0 + r || py > y1 - r;   // a row through the corners
        // the straight part of the row: corners only at its ends
        const int sx0 = band ? std::max(ix0, (int)std::ceil(x0 + r - 0.5f)) : ix0;
        const int sx1 = band ? std::min(ix1, (int)std::floor(x1 - r - 0.5f)) : ix1;
        if (c[3] == 255 && sx0 <= sx1) {   // an opaque span: copied, not blended
            uint8_t *d = &px_[((size_t)y * w_ + (size_t)sx0) * 4];
            for (int x = sx0; x <= sx1; x++, d += 4) std::memcpy(d, c, 4);
        } else {
            for (int x = sx0; x <= sx1; x++) blend(x, y, c, 1.f);
        }
        if (!band) continue;
        const float cy = std::min(std::max(py, y0 + r), y1 - r);
        for (int x = ix0; x <= ix1; x++) {
            if (x == sx0 && sx0 <= sx1) { x = sx1; continue; }
            const float px = x + 0.5f;
            const float cx = std::min(std::max(px, x0 + r), x1 - r);
            const float dd = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
            blend(x, y, c, std::min(1.f, std::max(0.f, r - dd + 0.5f)));
        }
    }
}

float Raster::text_width(const std::string &raw, Size sz) const {
    const std::string s = ascii(raw);
    if (const Font *f = fonts_[sz].get()) {
        float w = 0.f;
        for (char ch : s) w += f->chars[ch - 32].xadvance;
        return w;
    }
    const float scale = sz == big ? 3.f : 2.f;
    return (float)s.size() * 6.f * scale;
}

float Raster::text(float x, float cy, const std::string &raw, Size sz, const uint8_t c[4], float max_w) {
    std::string s = ascii(raw);
    if (text_width(s, sz) > max_w) {   // cut to fit with "..."
        while (!s.empty() && text_width(s + "...", sz) > max_w) s.pop_back();
        s += "...";
    }
    const float x_start = x;
    if (const Font *f = fonts_[sz].get()) {
        float pen = x, base = std::round(cy + 0.5f * (f->ascent + f->descent));
        for (char ch : s) {
            stbtt_aligned_quad q;
            float yy = base;
            stbtt_GetBakedQuad(f->chars, f->bw, f->bh, ch - 32, &pen, &yy, &q, 1);
            const int gx0 = (int)std::lround(q.x0), gy0 = (int)std::lround(q.y0);
            const int sx = (int)std::lround(q.s0 * f->bw), sy = (int)std::lround(q.t0 * f->bh);
            const int gw = (int)std::lround((q.s1 - q.s0) * f->bw), gh = (int)std::lround((q.t1 - q.t0) * f->bh);
            for (int j = 0; j < gh; j++)
                for (int i = 0; i < gw; i++) {
                    const uint8_t a = f->atlas[(size_t)(sy + j) * f->bw + (size_t)(sx + i)];
                    if (a) blend(gx0 + i, gy0 + j, c, a / 255.f);
                }
        }
        return pen - x_start;
    }
    const int scale = sz == big ? 3 : 2;
    const float top = std::round(cy - 3.5f * scale);
    for (char ch : s) {
        const uint8_t *g = kGlyphs[ch - 32];
        for (int col = 0; col < 5; col++)
            for (int rw = 0; rw < 7; rw++)
                if (g[col] & (1u << rw))
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++) blend((int)x + col * scale + dx, (int)top + rw * scale + dy, c, 1.f);
        x += 6.f * scale;
    }
    return x - x_start;
}

float Raster::text_centred(float x0, float x1, float cy, const std::string &s, Size sz, const uint8_t c[4]) {
    Size use = sz;
    if (use == big && text_width(s, big) > x1 - x0 - 8.f) use = small;
    const float w = std::min(text_width(s, use), x1 - x0 - 8.f);
    return text(0.5f * (x0 + x1 - w), cy, s, use, c, x1 - x0 - 8.f);
}

void Raster::clear() {   // the rounded backing is drawn once, then copied
    if (bg_.empty()) {
        std::fill(px_.begin(), px_.end(), 0);
        rect(0.f, 0.f, (float)w_, (float)h_, kBg, 22.f);
        bg_ = px_;
    }
    std::memcpy(px_.data(), bg_.data(), px_.size());
}

void Raster::row(const Control &c, float y, const Layout &L, bool hovered) {
    const float W = (float)L.width, cy = y + 0.5f * L.row_h, m = L.margin;
    float wx0, wx1;
    widget_span(L, &wx0, &wx1);
    const bool on = c.enabled;
    const Col text_c = dimmed(kText, on), dim_c = dimmed(kDim, on), acc = dimmed(kAccent, on);
    if (hovered && on && c.kind != Kind::text) rect(m - 10.f, y + 1.f, wx1 + 6.f, y + L.row_h - 1.f, kHoverRow, 8.f);
    if (c.kind == Kind::text) {
        if (c.id.compare(0, 10, "text.head.") == 0) {
            text(m, cy + 3.f, c.label, big, kHeading, wx1 - m);
            rect(m, y + L.row_h - 3.f, wx1, y + L.row_h - 2.f, kLine);
            return;
        }
        // up to two lines, wrapped at spaces
        const std::string s = ascii(c.label);
        const float maxw = W - 2.f * m - kScrollW;
        size_t cut = s.size();
        if (text_width(s, small) > maxw) {
            cut = 0;
            for (size_t i = 0; i <= s.size(); i++)
                if ((i == s.size() || s[i] == ' ') && text_width(s.substr(0, i), small) <= maxw) cut = i;
            if (cut == 0) cut = s.size();
        }
        if (cut < s.size()) {
            text(m, y + 10.f, s.substr(0, cut), small, kDim, maxw);
            text(m, y + 27.f, s.substr(cut + 1), small, kDim, maxw);
        } else {
            text(m, cy, s, small, kDim, maxw);
        }
        return;
    }
    if (c.kind == Kind::button) {
        const uint8_t *bg = c.id == "app.quit" ? kQuit : hovered && on ? kButtonHover : kButton;
        const Col b = dimmed(bg, on);
        rect(m, y + 3.f, wx1, y + L.row_h - 3.f, b.c, 10.f);
        text_centred(m, wx1, cy, c.label, big, text_c.c);
        return;
    }
    const float lw = kLabelW - 14.f;
    if (!c.note.empty()) {
        text(m, y + 11.f, c.label, small, text_c.c, lw);
        text(m, y + 27.f, c.note, small, dim_c.c, lw);
    } else {
        text(m, cy, c.label, text_width(c.label, big) <= lw ? big : small, text_c.c, lw);
    }
    const float py0 = y + 4.f, py1 = y + L.row_h - 4.f;
    switch (c.kind) {
    case Kind::toggle: {
        const bool v = c.options.size() == 2 && c.value == c.options[1];
        const float x1 = wx1, x0 = x1 - 64.f;
        rect(x0, py0, x1, py1, v ? acc.c : dimmed(kTrack, on).c, 14.f);
        const float r = 0.5f * (py1 - py0) - 3.f, kx = v ? x1 - 3.f - r : x0 + 3.f + r;
        rect(kx - r, cy - r, kx + r, cy + r, dimmed(kKnob, on).c, r);
        text(wx0, cy, v ? "On" : "Off", small, dim_c.c, x0 - wx0 - 8.f);
        break;
    }
    case Kind::slider: {
        float sx0, sx1;
        slider_span(L, &sx0, &sx1);
        sx0 += 10.f; sx1 -= 10.f;
        char *end = nullptr;
        float v = std::strtof(c.value.c_str(), &end);
        if (end == c.value.c_str() || !std::isfinite(v)) v = c.min;
        float t = (c.log_scale && c.min > 0.f && v > 0.f) ? (std::log(v) - std::log(c.min)) / (std::log(c.max) - std::log(c.min))
                                                         : (v - c.min) / (c.max - c.min);
        t = std::min(1.f, std::max(0.f, t));
        rect(sx0, cy - 3.f, sx1, cy + 3.f, dimmed(kTrack, on).c, 3.f);
        const float kx = sx0 + t * (sx1 - sx0);
        rect(sx0, cy - 3.f, kx, cy + 3.f, acc.c, 3.f);
        rect(kx - 10.f, cy - 10.f, kx + 10.f, cy + 10.f, dimmed(kKnob, on).c, 10.f);
        text(sx1 + 18.f, cy, format_value(v) + c.unit, small, text_c.c, wx1 - sx1 - 18.f);
        break;
    }
    case Kind::segmented: {
        const size_t n = c.options.size();
        if (!n) break;
        const int sel = option_index(c);
        const float pw = (wx1 - wx0) / (float)n;
        for (size_t i = 0; i < n; i++) {
            const float x0 = wx0 + pw * (float)i + 2.f, x1 = wx0 + pw * (float)(i + 1) - 2.f;
            rect(x0, py0, x1, py1, (int)i == sel ? acc.c : dimmed(kPill, on).c, 12.f);
            text_centred(x0, x1, cy, option_label(c, i), n <= 3 ? big : small, text_c.c);
        }
        break;
    }
    case Kind::picker:
    case Kind::stepper: {
        const bool pk = c.kind == Kind::picker;
        const float aw = 0.25f * (wx1 - wx0);   // act_on's left quarter steps back
        const Col bc = dimmed(hovered && on ? kButtonHover : kButton, on);
        rect(wx0, py0, wx0 + aw - 4.f, py1, bc.c, 10.f);
        rect(wx1 - 48.f, py0, wx1, py1, bc.c, 10.f);
        text_centred(wx0, wx0 + aw - 4.f, cy, pk ? "<" : "-", big, text_c.c);
        text_centred(wx1 - 48.f, wx1, cy, pk ? ">" : "+", big, text_c.c);
        const int i = option_index(c);
        const std::string shown = i >= 0 ? option_label(c, (size_t)i) : c.value;
        text_centred(wx0 + aw, wx1 - 52.f, cy, shown, big, text_c.c);
        break;
    }
    default: break;
    }
}

void Raster::draw(const std::vector<Tab> &tabs, int tab, const Hit &hover, const Layout &L, int scroll_rows,
                  const std::string &status, const std::string &hint) {
    clear();
    const float W = (float)L.width, H = (float)L.height;
    if (!tabs.empty()) {   // the tab strip
        const float tw = W / (float)tabs.size();
        for (size_t i = 0; i < tabs.size(); i++) {
            const bool active = (int)i == tab, hov = hover.tab_strip && hover.tab == (int)i;
            rect(tw * (float)i + 8.f, 8.f, tw * (float)(i + 1) - 8.f, kTabStripH - 4.f, active ? kAccent : hov ? kTabHover : kTabBg, 14.f);
            text_centred(tw * (float)i + 8.f, tw * (float)(i + 1) - 8.f, 0.5f * (kTabStripH + 4.f), tabs[i].name, big, kText);
        }
    }
    if (tab >= 0 && tab < (int)tabs.size()) {
        const std::vector<Control> &cs = tabs[(size_t)tab].controls;
        const int rows = visible_rows(L);
        for (int k = 0; k < rows; k++) {
            const int idx = scroll_rows + k;
            if (idx < 0 || idx >= (int)cs.size()) break;
            const bool hov = !hover.tab_strip && hover.tab == tab && hover.control == idx;
            row(cs[(size_t)idx], content_top(L) + (float)k * L.row_h, L, hov);
        }
        const int ms = max_scroll(tabs, tab, L);
        if (ms > 0) {   // the scroll column: up and down halves, a thumb for where the rows are
            float x0, x1;
            scroll_span(L, &x0, &x1);
            const float top = content_top(L), bottom = top + (float)rows * L.row_h, mid = 0.5f * (top + bottom);
            rect(x0, top, x1, mid - 3.f, hover.scroll < 0 ? kButtonHover : kButton, 10.f);
            rect(x0, mid + 3.f, x1, bottom, hover.scroll > 0 ? kButtonHover : kButton, 10.f);
            const float cx = 0.5f * (x0 + x1);
            for (int i = 0; i < 12; i++) {   // the arrows
                rect(cx - (float)i, top + 14.f + (float)i, cx + (float)i, top + 15.f + (float)i, kText);
                rect(cx - (float)i, bottom - 15.f - (float)i, cx + (float)i, bottom - 14.f - (float)i, kText);
            }
            const float track0 = top + 40.f, track1 = bottom - 40.f, n = (float)cs.size();
            const float t0 = track0 + (track1 - track0) * (float)scroll_rows / n;
            const float t1 = track0 + (track1 - track0) * (float)std::min((int)cs.size(), scroll_rows + rows) / n;
            rect(cx - 3.f, t0, cx + 3.f, t1, kAccent, 3.f);
        }
    }
    const float fy = H - kFooterH;
    rect(L.margin, fy + 2.f, W - L.margin, fy + 3.f, kLine);
    text(L.margin, fy + 22.f, status, small, kDim, W - 2.f * L.margin);
    if (!hint.empty()) text(L.margin, fy + 50.f, hint, small, kHint, W - 2.f * L.margin);
}

}  // namespace panel
}  // namespace ar
