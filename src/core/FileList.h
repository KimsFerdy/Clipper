#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace clipper {

// copied files travel as file:// uris. files clips keep them one per line
std::string pathToUri(std::string_view path);  // "/a b" -> "file:///a%20b"
std::string uriToPath(std::string_view uri);   // "" if it isn't a local file:// uri
std::vector<std::string> uriLines(std::string_view text);  // non-empty lines, comments (#) and \r dropped

// x-special/gnome-copied-files: "copy\nfile:///a\nfile:///b" (or "cut\n...").
// false if it doesn't look like that
bool parseCopiedFiles(std::string_view data, bool* cut, std::string* uris);
std::string formatCopiedFiles(bool cut, const std::string& uris);

// text/uri-list wants \r\n line ends
std::string formatUriList(const std::string& uris);

// every line is a local file:// uri
bool allFileUris(std::string_view text);

}  // namespace clipper
