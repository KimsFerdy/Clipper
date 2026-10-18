#include "core/Filter.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "core/FileList.h"

namespace clipper {

namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// ascii case-insensitive, no copies (texts can be megabytes)
bool containsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return lower(a) == b; });
    return it != hay.end();
}

std::vector<std::string> words(const std::string& query) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : query) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += lower(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

bool typeMatches(const Clip& c, ClipFilter::Type type) {
    switch (type) {
        case ClipFilter::Type::Any:
            return true;
        case ClipFilter::Type::Text:
            return c.kind == ClipKind::Text;
        case ClipFilter::Type::Image:
            return c.kind == ClipKind::Image;
        case ClipFilter::Type::Files:
            return c.kind == ClipKind::Files;
    }
    return true;
}

bool wordMatches(const Clip& c, const std::string& word) {
    if (containsNoCase(c.title, word) || containsNoCase(c.source, word)) return true;
    switch (c.kind) {
        case ClipKind::Text:
            return containsNoCase(c.text, word);
        case ClipKind::Image:
            return containsNoCase("image", word);
        case ClipKind::Files:
            for (const std::string& p : clipPaths(c)) {
                if (containsNoCase(p, word)) return true;
            }
            return false;
    }
    return false;
}

// bytes in the utf-8 sequence starting with c
size_t utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;  // stray continuation byte, step over it
}

std::string collapse(std::string_view text, size_t maxChars) {
    std::string out;
    size_t chars = 0;
    bool space = false;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isspace(c)) {
            space = !out.empty();
            ++i;
            continue;
        }
        if (space) {
            if (chars + 1 >= maxChars) return out + "…";
            out += ' ';
            ++chars;
            space = false;
        }
        if (chars >= maxChars) return out + "…";
        const size_t n = std::min(utf8Len(c), text.size() - i);
        out.append(text.data() + i, n);
        i += n;
        ++chars;
    }
    return out;
}

}  // namespace

std::vector<uint64_t> filterClips(const ClipStore& store, const ClipFilter& filter) {
    const std::vector<std::string> ws = words(filter.query);
    std::vector<const Clip*> hits;
    for (const Clip& c : store.clips()) {
        if (filter.view == ClipFilter::View::Pinned && !c.pinned) continue;
        if (filter.view == ClipFilter::View::Folder && c.folder != filter.folder) continue;
        if (!typeMatches(c, filter.type)) continue;
        if (!std::all_of(ws.begin(), ws.end(), [&](const std::string& w) { return wordMatches(c, w); })) continue;
        hits.push_back(&c);
    }
    std::sort(hits.begin(), hits.end(), [](const Clip* a, const Clip* b) { return a->seq > b->seq; });
    std::vector<uint64_t> ids;
    ids.reserve(hits.size());
    for (const Clip* c : hits) ids.push_back(c->id);
    return ids;
}

std::string baseName(const std::string& path) {
    std::string_view p = path;
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    const size_t slash = p.find_last_of('/');
    return std::string(slash == std::string_view::npos || p.size() == 1 ? p : p.substr(slash + 1));
}

std::vector<std::string> clipPaths(const Clip& clip) {
    std::vector<std::string> out;
    if (clip.kind != ClipKind::Files) return out;
    for (const std::string& uri : uriLines(clip.text)) {
        std::string path = uriToPath(uri);
        out.push_back(path.empty() ? uri : path);
    }
    return out;
}

std::string previewLine(const Clip& clip, size_t maxChars) {
    switch (clip.kind) {
        case ClipKind::Text:
            return collapse(clip.text, maxChars);
        case ClipKind::Image:
            return "Image " + std::to_string(clip.width) + " × " + std::to_string(clip.height);
        case ClipKind::Files: {
            const std::vector<std::string> paths = clipPaths(clip);
            std::string names;
            for (size_t i = 0; i < paths.size() && names.size() < maxChars * 4; ++i)
                names += (i ? ", " : "") + baseName(paths[i]);
            if (paths.size() > 1) names = std::to_string(paths.size()) + " items: " + names;
            return collapse(names, maxChars);
        }
    }
    return {};
}

}  // namespace clipper
