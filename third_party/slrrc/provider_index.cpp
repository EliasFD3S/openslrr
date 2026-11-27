// provider_index.cpp - the shared class provider index (provider_index.hpp).
#include "provider_index.hpp"

#include <algorithm>
#include <utility>

#include "class_order.hpp"

namespace slrrc {

namespace {

std::string_view file_name(std::string_view rel) {
    const size_t slash = rel.rfind('/');
    return slash == std::string_view::npos ? rel : rel.substr(slash + 1);
}

// The file name without its extension.
std::string_view stem(std::string_view rel) {
    const std::string_view name = file_name(rel);
    const size_t dot = name.rfind('.');
    return dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string_view directory(std::string_view rel) {
    const size_t slash = rel.rfind('/');
    return slash == std::string_view::npos ? std::string_view() : rel.substr(0, slash);
}

std::string_view simple_name(std::string_view fqn) {
    const size_t dot = fqn.rfind('.');
    return dot == std::string_view::npos ? fqn : fqn.substr(dot + 1);
}

char fold(char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

int compare_folded(std::string_view a, std::string_view b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const char x = fold(a[i]);
        const char y = fold(b[i]);
        if (x != y) {
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
        }
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

}  // namespace

void ProviderIndex::build(std::vector<Container> containers) {
    providers_.clear();
    shared_.clear();
    by_rel_.clear();
    simple_.clear();
    packages_.clear();
    {
        // The keys are computed once, outside the sort.
        struct Keyed {
            ClassOrderKey key;
            Container container;
        };
        std::vector<Keyed> keyed;
        keyed.reserve(containers.size());
        for (Container& c : containers) {
            ClassOrderKey key = class_order_key(directory(c.rel), stem(c.rel));
            keyed.push_back(Keyed{std::move(key), std::move(c)});
        }
        std::sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
            if (a.container.layer != b.container.layer) {
                return a.container.layer < b.container.layer;
            }
            return class_order_less(a.key, a.container.rel, b.key, b.container.rel);
        });
        containers_.clear();
        containers_.reserve(keyed.size());
        for (Keyed& k : keyed) {
            containers_.push_back(std::move(k.container));
        }
    }
    // From here on containers_ does not change: views into it stay valid.
    for (const Container& c : containers_) {
        if (c.classes.empty()) {
            continue;
        }
        by_rel_.push_back(&c);
        for (const std::string& name : c.classes) {
            const auto [it, fresh] = providers_.emplace(name, &c);
            if (fresh || it->second == &c) {
                continue;
            }
            const Container* old = it->second;
            auto& list = shared_[it->first];
            if (list.empty()) {
                list.push_back(old);
            }
            list.push_back(&c);
            if (old->layer == c.layer &&
                named_file_replaces(stem(c.rel), stem(old->rel), simple_name(name), false)) {
                it->second = &c;
            }
        }
    }
    for (auto& [name, list] : shared_) {
        const auto winner = std::find(list.begin(), list.end(), providers_.at(name));
        std::rotate(list.begin(), winner, winner + 1);
    }
    std::stable_sort(by_rel_.begin(), by_rel_.end(), [](const Container* a, const Container* b) {
        return compare_folded(a->rel, b->rel) < 0;
    });
    for (const auto& [name, container] : providers_) {
        const auto [it, fresh] = simple_.emplace(simple_name(name), Simple{name, false});
        if (!fresh) {
            it->second.ambiguous = true;
        }
        const size_t dot = name.rfind('.');
        if (dot != std::string_view::npos) {
            packages_.insert(name.substr(0, dot));
        }
    }
}

const Container* ProviderIndex::provider(std::string_view fqn) const {
    const auto it = providers_.find(fqn);
    return it == providers_.end() ? nullptr : it->second;
}

const Container* ProviderIndex::at(std::string_view rel, int preferred_layer) const {
    auto it = std::lower_bound(by_rel_.begin(), by_rel_.end(), rel, [](const Container* c, std::string_view want) {
        return compare_folded(c->rel, want) < 0;
    });
    const Container* first = nullptr;
    for (; it != by_rel_.end() && compare_folded((*it)->rel, rel) == 0; ++it) {
        if ((*it)->layer == preferred_layer) {
            return *it;
        }
        if (!first) {
            first = *it;
        }
    }
    return first;
}

std::string_view ProviderIndex::unique_simple(std::string_view simple, bool* ambiguous) const {
    const auto it = simple_.find(simple);
    if (ambiguous) {
        *ambiguous = it != simple_.end() && it->second.ambiguous;
    }
    return it == simple_.end() || it->second.ambiguous ? std::string_view() : it->second.fqn;
}

}  // namespace slrrc
