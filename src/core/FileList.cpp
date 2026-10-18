#include "core/FileList.h"

#include <cctype>

namespace clipper {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string pathToUri(std::string_view path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file://";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string uriToPath(std::string_view uri) {
    constexpr std::string_view prefix = "file://";
    if (uri.substr(0, prefix.size()) != prefix) return {};
    uri.remove_prefix(prefix.size());
    // file://hostname/path is allowed by the spec, only localhost / empty is ours
    const size_t slash = uri.find('/');
    if (slash == std::string_view::npos) return {};
    const std::string_view host = uri.substr(0, slash);
    if (!host.empty() && host != "localhost") return {};
    uri.remove_prefix(slash);
    std::string out;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const int hi = hexValue(uri[i + 1]), lo = hexValue(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += uri[i];
    }
    return out;
}

std::vector<std::string> uriLines(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (!line.empty() && line[0] != '#') out.emplace_back(line);
    }
    return out;
}

bool parseCopiedFiles(std::string_view data, bool* cut, std::string* uris) {
    std::vector<std::string> lines = uriLines(data);
    if (lines.size() < 2 || (lines[0] != "copy" && lines[0] != "cut")) return false;
    *cut = lines[0] == "cut";
    uris->clear();
    for (size_t i = 1; i < lines.size(); ++i) *uris += (i > 1 ? "\n" : "") + lines[i];
    return true;
}

std::string formatCopiedFiles(bool cut, const std::string& uris) {
    std::string out = cut ? "cut" : "copy";
    for (const std::string& u : uriLines(uris)) out += "\n" + u;
    return out;
}

std::string formatUriList(const std::string& uris) {
    std::string out;
    for (const std::string& u : uriLines(uris)) out += u + "\r\n";
    return out;
}

bool allFileUris(std::string_view text) {
    std::vector<std::string> lines = uriLines(text);
    if (lines.empty()) return false;
    for (const std::string& l : lines) {
        if (uriToPath(l).empty()) return false;
    }
    return true;
}

}  // namespace clipper
