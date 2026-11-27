// tree_walk.cpp - the shared directory walk (tree_walk.hpp).
#include "tree_walk.hpp"

#include <algorithm>
#include <exception>
#include <system_error>
#include <utility>
#include <vector>

namespace slrrc {
namespace tree {

namespace fs = std::filesystem;

namespace {

using Native = fs::path::string_type;
constexpr fs::path::value_type SEP = fs::path::preferred_separator;

bool to_utf8(const fs::path& path, std::string& out) {
    try {
        out = path.u8string();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// For messages only: a name that is not UTF-8 shows '?' for what cannot be written.
std::string shown(const fs::path& path) {
    try {
        return path.generic_u8string();
    } catch (const std::exception&) {
        std::string out;
        for (const auto ch : path.native()) {
            out += ch == '\\' ? '/' : (ch > 0 && ch < 0x80 ? static_cast<char>(ch) : '?');
        }
        return out;
    }
}

bool is_link(const fs::file_status& status) {
#if defined(_MSC_VER)
    if (status.type() == fs::file_type::junction) {
        return true;
    }
#endif
    return status.type() == fs::file_type::symlink;
}

// The comparison key of a real directory: native form, ASCII case folded where
// the file system ignores case, ending with a separator, so that a prefix test
// is an ancestor test.
Native real_key(Native text) {
    for (auto& ch : text) {
#if defined(_WIN32)
        if (ch == '/') {
            ch = SEP;
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<fs::path::value_type>(ch - 'A' + 'a');
        }
#endif
    }
    if (text.empty() || text.back() != SEP) {
        text += SEP;
    }
    return text;
}

struct Item {
    std::string name;
    fs::path path;
    bool directory = false;
    bool link = false;
};

struct Walker {
    const std::function<bool(const Entry&)>& visit;
    const std::function<void(const Problem&)>& problem;
    std::vector<Native> chain;      // real keys of the directories on the current path

    void report(Problem::Kind kind, const fs::path& path, std::string what = {}) {
        if (problem) {
            problem(Problem{kind, path, std::move(what)});
        }
    }

    void directory(const fs::path& dir, const std::string& rel, int depth) {
        std::vector<Item> files;
        std::vector<Item> dirs;
        std::error_code ec;
        fs::directory_iterator it(dir, ec);
        if (ec) {
            report(Problem::Unlistable, dir, ec.message());
            return;
        }
        for (const fs::directory_iterator end; it != end;) {
            Item item;
            item.path = it->path();
            if (!to_utf8(item.path.filename(), item.name)) {
                // One report per rejected entry, which covers its subtree.
                report(Problem::Unencodable, dir);
            } else {
                std::error_code status_error;
                const fs::file_status own = it->symlink_status(status_error);
                if (!status_error && is_link(own)) {
                    item.link = true;
                    const fs::file_status target = fs::status(item.path, status_error);
                    if (status_error || !fs::exists(target)) {
                        report(Problem::BrokenLink, item.path,
                               status_error ? status_error.message() : "its target does not exist");
                        item.name.clear();
                    } else {
                        item.directory = fs::is_directory(target);
                    }
                } else if (!status_error) {
                    item.directory = fs::is_directory(own);
                }
                if (!item.name.empty()) {
                    (item.directory ? dirs : files).push_back(std::move(item));
                }
            }
            it.increment(ec);
            if (ec) {
                report(Problem::Unlistable, dir, ec.message());
                break;
            }
        }
        const auto by_name = [](const Item& a, const Item& b) { return a.name < b.name; };
        std::sort(files.begin(), files.end(), by_name);
        std::sort(dirs.begin(), dirs.end(), by_name);
        const auto rel_of = [&](const std::string& name) { return rel.empty() ? name : rel + '/' + name; };
        for (Item& f : files) {
            Entry entry{std::move(f.path), f.name, rel_of(f.name), depth, false, f.link};
            visit(entry);
        }
        for (Item& d : dirs) {
            Native real;
            if (d.link) {
                // Only links are resolved: below a real directory, the real
                // path of a child is its parent's followed by its name.
                std::error_code link_error;
                const fs::path target = fs::canonical(d.path, link_error);
                if (link_error) {
                    report(Problem::BrokenLink, d.path, link_error.message());
                    continue;
                }
                real = real_key(target.native());
                const bool cycle = std::any_of(chain.begin(), chain.end(), [&](const Native& open) {
                    return open.compare(0, real.size(), real) == 0;
                });
                if (cycle) {
                    report(Problem::Cycle, d.path);
                    continue;
                }
            } else {
                real = real_key(chain.back() + d.path.filename().native());
            }
            const Entry entry{d.path, d.name, rel_of(d.name), depth, true, d.link};
            if (!visit(entry)) {
                continue;
            }
            chain.push_back(std::move(real));
            directory(d.path, entry.rel, depth + 1);
            chain.pop_back();
        }
    }
};

}  // namespace

std::string describe(const Problem& problem) {
    const std::string path = shown(problem.path);
    switch (problem.kind) {
    case Problem::Unencodable:
        return "skipped a name not representable in UTF-8 under " + path + " (with anything below it)";
    case Problem::Unlistable:
        return "cannot list directory " + path + (problem.what.empty() ? "" : ": " + problem.what);
    case Problem::Cycle:
        return "skipped directory link " + path + ": it leads back to a directory that encloses it";
    case Problem::BrokenLink:
        return "skipped link " + path + (problem.what.empty() ? "" : ": " + problem.what);
    }
    return path;
}

void walk(const fs::path& root, const std::function<bool(const Entry&)>& visit,
          const std::function<void(const Problem&)>& problem) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return;
    }
    Walker walker{visit, problem, {}};
    fs::path real = fs::canonical(root, ec);
    if (ec) {
        real = fs::absolute(root, ec).lexically_normal();
    }
    walker.chain.push_back(real_key(real.native()));
    walker.directory(root, std::string(), 0);
}

}  // namespace tree
}  // namespace slrrc
