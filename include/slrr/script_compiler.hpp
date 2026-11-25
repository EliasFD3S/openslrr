#pragma once
#include <functional>
#include <string>

namespace slrr {
struct ScriptCompileReport {
  int attempted = 0, succeeded = 0, failed = 0, classes = 0;
};
// How float literals become bits (third_party/slrrc/floatlit.hpp). B is the
// Build 940 exe's own conversion and the one System.compileAll uses. The
// stock MWM classes were compiled with x87 forms of the same loop: data
// classes (cars, engines, running gear) with D, core scripts with A. Auto
// picks, per source unit, the setting whose literal bits appear in the
// existing class (its .openslrr.bak when there is one); B when nothing tells
// them apart (#420).
enum class FloatLiteralMode : char { A = 'A', B = 'B', C = 'C', D = 'D', Auto = '*' };
struct ScriptCompileOptions {
  FloatLiteralMode float_mode = FloatLiteralMode::B;
};
// Compile each source unit under directory to its sibling TUFA container.
// All classes declared by one .java remain in one .class. Existing loaded VM
// classes are not replaced while executing; restart to use regenerated code.
ScriptCompileReport script_compile_all(const std::string& game_root,
    const std::string& directory, const std::function<void()>& checkpoint = {});
ScriptCompileReport script_compile_all(const std::string& game_root,
    const std::string& directory, const ScriptCompileOptions& options,
    const std::function<void()>& checkpoint = {});
}
