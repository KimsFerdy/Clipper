#include "app/Thumbnails.h"

#include <GLFW/glfw3.h>

#include <algorithm>

// clang-format off
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
// clang-format on

namespace clipper {

namespace {

// box filter: every output pixel is the average of the source pixels under it
std::vector<uint8_t> shrink(const uint8_t* src, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> out(static_cast<size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            unsigned sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* row = src + (static_cast<size_t>(sy) * sw + x0) * 4;
                for (int sx = x0; sx < x1; ++sx, row += 4) {
                    for (int c = 0; c < 4; ++c) sum[c] += row[c];
                }
            }
            const unsigned n = static_cast<unsigned>((y1 - y0) * (x1 - x0));
            uint8_t* o = &out[(static_cast<size_t>(y) * dw + x) * 4];
            for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>(sum[c] / n);
        }
    }
    return out;
}

}  // namespace

Thumbnails::Thumbnails(std::function<void()> wake) : wake_(std::move(wake)) {
    thread_ = std::thread([this] { work(); });
}

Thumbnails::~Thumbnails() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
    for (auto& [id, e] : entries_) {
        if (e.tex.id) glDeleteTextures(1, &e.tex.id);
    }
}

Thumbnails::Texture Thumbnails::get(uint64_t clipId, const std::string& pngPath) {
    auto [it, inserted] = entries_.try_emplace(clipId);
    it->second.lastUse = frame_;
    if (inserted) {
        {
            std::lock_guard lock(mutex_);
            jobs_.emplace_front(clipId, pngPath);  // newest request first, that's what's on screen
        }
        cv_.notify_one();
    }
    return it->second.tex;
}

void Thumbnails::drop(uint64_t clipId) {
    auto it = entries_.find(clipId);
    if (it == entries_.end()) return;
    if (it->second.tex.id) glDeleteTextures(1, &it->second.tex.id);
    entries_.erase(it);
}

void Thumbnails::trim(size_t keep) {
    if (entries_.size() <= keep) return;
    std::vector<std::pair<uint64_t, uint64_t>> byAge;  // lastUse, id
    for (const auto& [id, e] : entries_) {
        if (!e.pending) byAge.emplace_back(e.lastUse, id);
    }
    std::sort(byAge.begin(), byAge.end());
    for (size_t i = 0; i < byAge.size() && entries_.size() > keep; ++i) drop(byAge[i].second);
}

void Thumbnails::upload() {
    ++frame_;
    std::vector<Decoded> done;
    {
        std::lock_guard lock(mutex_);
        done.swap(done_);
    }
    for (Decoded& d : done) {
        auto it = entries_.find(d.clipId);
        if (it == entries_.end()) continue;  // dropped while decoding
        it->second.pending = false;
        if (d.rgba.empty()) continue;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, d.width, d.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, d.rgba.data());
        it->second.tex = {tex, d.width, d.height};
    }
}

void Thumbnails::work() {
    for (;;) {
        std::pair<uint64_t, std::string> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Decoded d;
        d.clipId = job.first;
        int w, h, channels;
        if (uint8_t* px = stbi_load(job.second.c_str(), &w, &h, &channels, 4)) {
            const float scale = std::min(1.0f, static_cast<float>(kMaxSide) / std::max(w, h));
            d.width = std::max(1, static_cast<int>(w * scale));
            d.height = std::max(1, static_cast<int>(h * scale));
            if (scale < 1.0f)
                d.rgba = shrink(px, w, h, d.width, d.height);
            else
                d.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
            stbi_image_free(px);
        }
        {
            std::lock_guard lock(mutex_);
            done_.push_back(std::move(d));
        }
        if (wake_) wake_();
    }
}

}  // namespace clipper
