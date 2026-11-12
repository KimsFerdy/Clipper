#include <cctype>

#include "platform/GlobalShortcut.h"

namespace clipper {

std::string prettyAccel(const std::string& accel) {
    std::string out;
    size_t i = 0;
    while (i < accel.size() && accel[i] == '<') {
        const size_t close = accel.find('>', i);
        if (close == std::string::npos) break;
        std::string mod = accel.substr(i + 1, close - i - 1);
        if (mod == "Primary") mod = "Ctrl";
        if (mod == "Control") mod = "Ctrl";
#ifdef _WIN32
        if (mod == "Super") mod = "Win";
#endif
        out += mod + "+";
        i = close + 1;
    }
    std::string key = accel.substr(i);
    if (key.size() == 1) key[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    if (key == "space") key = "Space";
    return out + key;
}

}  // namespace clipper
