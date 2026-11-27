// The order in which class files of one tree compete, shared by the script
// compiler (install.cpp) and the engine's mod overlay (src/jvm/mod_overlay.cpp)
// so that both pick the same file when one class is declared more than once.
// Header-only, standard C++17 only.
#ifndef SLRRC_CLASS_ORDER_HPP
#define SLRRC_CLASS_ORDER_HPP

#include <string>
#include <string_view>
#include <utility>

namespace slrrc {

inline std::string class_order_lower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

// The order of the index: directory, then name, both lower case. The
// directory uses '/' separators; the name has no extension.
typedef std::pair<std::string, std::string> ClassOrderKey;

inline ClassOrderKey class_order_key(std::string_view class_dir, std::string_view name) {
    return ClassOrderKey(class_order_lower(class_dir), class_order_lower(name));
}

// Break ties by original relative path bytes, not filesystem enumeration order.
inline bool class_order_less(const ClassOrderKey& a, std::string_view a_path,
                             const ClassOrderKey& b, std::string_view b_path) {
    return a == b ? a_path < b_path : a < b;
}

// Within equal priority, a later file replaces the class's current provider
// only when it is named after the class (file_name == simple) and the current
// provider is a different file that is not.
inline bool named_file_replaces(std::string_view file_name, std::string_view old_file_name,
                                std::string_view simple, bool same_file) {
    return file_name == simple && !same_file && old_file_name != simple;
}

}  // namespace slrrc

#endif
