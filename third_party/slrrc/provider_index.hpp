// provider_index.hpp - which class container provides each fully qualified
// class name, shared by the script compiler (install.cpp) and the engine's mod
// overlay (src/jvm/mod_overlay.cpp), so that both pick the same file.
// Standard C++17 only.
//
// A container is a class file.  The caller reads it (each with its own reader)
// and hands over the names it declares; a container that declares nothing
// (damaged, empty, not a class file) never provides anything.  A file path
// only identifies a container: a name is resolved by the name, never by the
// path of a file.
//
// PRIORITY.  Containers compete in layers: a smaller layer wins (mods in load
// order, then the base game).  Within a layer, in class order
// (class_order.hpp: directory, then file name, ASCII case folded, then the
// path bytes), the first container that declares a name provides it, except
// that a later file named exactly after the class replaces a provider that is
// not (named_file_replaces).
#ifndef SLRRC_PROVIDER_INDEX_HPP
#define SLRRC_PROVIDER_INDEX_HPP

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace slrrc {

struct Container {
    size_t id = 0;                      // the caller's handle
    int layer = 0;                      // smaller wins
    std::string rel;                    // path in its layer, '/' separators: order and file name
    std::vector<std::string> classes;   // declared names, file order; empty: provides nothing
};

class ProviderIndex {
public:
    ProviderIndex() = default;
    // The views into the containers stay valid when the index moves.
    ProviderIndex(const ProviderIndex&) = delete;
    ProviderIndex& operator=(const ProviderIndex&) = delete;
    ProviderIndex(ProviderIndex&&) = default;
    ProviderIndex& operator=(ProviderIndex&&) = default;

    // Replaces the index.
    void build(std::vector<Container> containers);

    // In priority order.
    const std::vector<Container>& containers() const { return containers_; }

    // The container that provides the name, nullptr when none declares it.
    const Container* provider(std::string_view fqn) const;

    // Names that several containers declare: the provider first, then the
    // others in priority order.
    const std::map<std::string_view, std::vector<const Container*>>& shared() const { return shared_; }

    // The container with this path (ASCII case folded) in the preferred layer
    // when that one declares something, otherwise the first in priority order
    // that does; nullptr when there is none.
    const Container* at(std::string_view rel, int preferred_layer) const;

    // The one provided name whose simple name (the part behind the last '.')
    // is this; empty when there is none or, setting *ambiguous, several.
    std::string_view unique_simple(std::string_view simple, bool* ambiguous = nullptr) const;

    // The packages of the provided names.
    const std::set<std::string_view>& packages() const { return packages_; }

private:
    struct Simple {
        std::string_view fqn;
        bool ambiguous = false;
    };
    std::vector<Container> containers_;
    std::map<std::string_view, const Container*> providers_;
    std::map<std::string_view, std::vector<const Container*>> shared_;
    std::vector<const Container*> by_rel_;      // containers that declare something, by folded path
    std::map<std::string_view, Simple> simple_;
    std::set<std::string_view> packages_;
};

}  // namespace slrrc

#endif
