#pragma once

#include <string>

namespace clipper {

// opens the folder a file is in, in the file manager. windows selects the file too
bool showInFileManager(const std::string& path);

}  // namespace clipper
