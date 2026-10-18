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

namespace clipper {

// image clips as gl textures, at most kMaxSide px on the long side.
// pngs get decoded + shrunk on a worker thread (a 4k screenshot takes ~80 ms), uploaded on the ui thread
class Thumbnails {
public:
    static constexpr int kMaxSide = 640;

    struct Texture {
        unsigned int id = 0;  // 0 = still loading or broken
        int width = 0, height = 0;
    };

    explicit Thumbnails(std::function<void()> wake);
    ~Thumbnails();  // needs the gl context to still be around
    Thumbnails(const Thumbnails&) = delete;
    Thumbnails& operator=(const Thumbnails&) = delete;

    // starts loading the first time, empty texture until it's ready
    Texture get(uint64_t clipId, const std::string& pngPath);
    void drop(uint64_t clipId);
    void upload();  // once a frame: decoded pixels -> textures
    void trim(size_t keep);  // drop the least recently used ones beyond that

private:
    struct Decoded {
        uint64_t clipId;
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    struct Entry {
        Texture tex;
        uint64_t lastUse = 0;
        bool pending = true;
    };

    void work();

    std::function<void()> wake_;
    std::unordered_map<uint64_t, Entry> entries_;  // ui thread
    uint64_t frame_ = 0;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<uint64_t, std::string>> jobs_;
    std::vector<Decoded> done_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace clipper
