#include "platform/Shell.h"

#include <filesystem>

#include "platform/Process.h"

namespace clipper {

bool showInFileManager(const std::string& path) {
    const std::string parent = std::filesystem::path(path).parent_path().string();
    return posix::spawnDetached({"xdg-open", parent});
}

}  // namespace clipper
