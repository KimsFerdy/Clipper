#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/ClipStore.h"

namespace clipper {

struct ClipFilter {
    enum class View { History, Pinned, Folder };
    enum class Type { Any, Text, Image, Files };

    std::string query;  // space separated words, all have to match (title, text, file names, source app)
    View view = View::History;
    uint32_t folder = 0;  // View::Folder
    Type type = Type::Any;
};

// matching ids, most recently used first
std::vector<uint64_t> filterClips(const ClipStore& store, const ClipFilter& filter);

// one line for lists: whitespace collapsed, at most maxChars characters (utf-8 safe, "…" when cut)
std::string previewLine(const Clip& clip, size_t maxChars);

// local paths of a files clip
std::vector<std::string> clipPaths(const Clip& clip);

// "a.txt" for "/home/x/a.txt", "x" for "/home/x/"
std::string baseName(const std::string& path);

}  // namespace clipper
