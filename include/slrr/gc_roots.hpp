#pragma once
#include <functional>

namespace slrr {
// Native registries can contain non-Java keys. The collector checks heap
// membership before following any address supplied by a provider.
using GcRootVisitor = std::function<void(const void*)>;
using GcRootProvider = std::function<void(const GcRootVisitor&)>;
void gameref_visit_gc_roots(const GcRootVisitor& visit);
void part_visit_gc_roots(const GcRootVisitor& visit);
void osd_visit_gc_roots(const GcRootVisitor& visit);
void font_visit_gc_roots(const GcRootVisitor& visit);
void mainloop_visit_gc_roots(const GcRootVisitor& visit);
void dyno_visit_gc_roots(const GcRootVisitor& visit);
void scene_visit_gc_roots(const GcRootVisitor& visit);
}
