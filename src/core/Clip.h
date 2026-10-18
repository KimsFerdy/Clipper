#pragma once

#include <cstdint>
#include <string>

namespace clipper {

enum class ClipKind : uint8_t { Text = 0, Image = 1, Files = 2 };

// one entry of the history
struct Clip {
    uint64_t id = 0;
    ClipKind kind = ClipKind::Text;
    int64_t created = 0;   // unix ms, first time it was copied
    int64_t lastUsed = 0;  // unix ms, copied again or pasted from clipper
    uint64_t seq = 0;      // bumped on every use, newest on top (ms can tie)
    uint32_t useCount = 1;
    uint32_t folder = 0;  // 0 = not in a folder
    bool pinned = false;
    bool cut = false;  // files: cut in the file manager, not copied
    uint64_t hash = 0;
    uint32_t width = 0, height = 0;  // images
    uint64_t bytes = 0;              // payload size (text bytes / png size)
    std::string title;               // set by the user, "" = none
    std::string source;              // WM_CLASS of the app it came from
    std::string text;                // text clips: the text. files: file:// uris, one per line. images: ""

    // pinned and foldered clips are never pruned or cleared with the history
    bool kept() const { return pinned || folder != 0; }
};

struct Folder {
    uint32_t id = 0;
    std::string name;
};

const char* kindName(ClipKind kind);

}  // namespace clipper
