#pragma once
#include <functional>
#include <string>

namespace slrr {
struct ScriptCompileReport {
  int attempted = 0, succeeded = 0, failed = 0, classes = 0;
};
// Compile each source unit under directory to its sibling TUFA container.
// All classes declared by one .java remain in one .class. Existing loaded VM
// classes are not replaced while executing; restart to use regenerated code.
ScriptCompileReport script_compile_all(const std::string& game_root,
    const std::string& directory, const std::function<void()>& checkpoint = {});
}
