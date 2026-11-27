// tree_walk.hpp - the one directory walk of a game tree, shared by the
// script compiler (install.cpp) and the engine (mod overlay, class loader), so
// that both see the same files.  Standard C++17 only.
//
// WHAT IS WALKED.  Top down from a root.  In each directory its files come
// first, then its subdirectories, each group in the byte order of the UTF-8
// names; a directory is followed by its whole subtree before its next sibling.
//
// LINKS.  Symbolic links to directories and NTFS junctions are followed, so a
// private copy of a game or a mod folder may link to shared content.  A link
// whose resolved target is the directory it sits in or one of the real
// directories that enclose it on the current path would recurse for ever: it is
// reported once (Problem::Cycle) and not entered.  Two links to one directory
// that do not enclose each other are both walked: each is a path of its own.
// A link that cannot be resolved is reported (Problem::BrokenLink) and skipped.
//
// PROBLEMS never stop the walk and are never thrown.  A name that cannot be
// written in UTF-8 (an unpaired UTF-16 surrogate on NTFS) is reported once per
// such entry and skipped with everything below it; a directory that cannot be listed is
// reported once.  A missing root is an empty tree and is not reported.
#ifndef SLRRC_TREE_WALK_HPP
#define SLRRC_TREE_WALK_HPP

#include <filesystem>
#include <functional>
#include <string>

namespace slrrc {
namespace tree {

struct Entry {
    std::filesystem::path path;     // as reached from the root; links are not resolved
    std::string name;               // file name, UTF-8
    std::string rel;                // relative to the root, UTF-8, '/' separators
    int depth = 0;                  // 0: directly in the root
    bool directory = false;
    bool link = false;              // reached through a symbolic link or junction
};

struct Problem {
    enum Kind { Unencodable, Unlistable, Cycle, BrokenLink };
    Kind kind = Unlistable;
    std::filesystem::path path;     // Unencodable: the directory holding the name; else the entry
    std::string what;               // the system's reason, when there is one
};

// One line for a log, without a prefix or a newline.
std::string describe(const Problem& problem);

// visit() is called for every file and directory; for a directory, false skips
// its subtree (the value is ignored for files).  problem() may be empty.
void walk(const std::filesystem::path& root, const std::function<bool(const Entry&)>& visit,
          const std::function<void(const Problem&)>& problem = {});

}  // namespace tree
}  // namespace slrrc

#endif
