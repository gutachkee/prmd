// Call-tree profiler behind PRMD_TIMED (include/prmd/profile.h).

#include "prmd/profile.h"

#ifdef PRMD_PROFILE

#include <chrono>
#include <cstring>
#include <memory>

namespace prmd::profile {
namespace {

struct Node {
  const char* name = "";
  Node* parent = nullptr;
  std::vector<std::unique_ptr<Node>> children;  // first-seen order
  uint64_t calls = 0;
  int64_t ns = 0;

  Node* child(const char* n) {
    for (auto& c : children) {
      // Literals with the same text are usually merged, so the pointer test
      // almost always decides; strcmp covers the rest.
      if (c->name == n || std::strcmp(c->name, n) == 0) {
        return c.get();
      }
    }
    children.push_back(std::make_unique<Node>());
    Node* c = children.back().get();
    c->name = n;
    c->parent = this;
    return c;
  }
};

struct Tree {
  Node root;
  Node* current = &root;
};

Tree& tree() {
  thread_local Tree t;
  return t;
}

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void walk(const Node& n,
          const std::string& prefix,
          int depth,
          std::vector<Entry>& out) {
  for (const auto& c : n.children) {
    Entry e;
    e.name = c->name;
    e.path = prefix.empty() ? e.name : prefix + "/" + e.name;
    e.depth = depth;
    e.calls = c->calls;
    e.ms = double(c->ns) / 1e6;
    out.push_back(e);
    walk(*c, e.path, depth + 1, out);
  }
}

}  // namespace

Scope::Scope(const char* name) {
  Tree& t = tree();
  Node* n = t.current->child(name);
  t.current = n;
  node_ = n;
  start_ns_ = now_ns();
}

Scope::~Scope() {
  const int64_t end = now_ns();
  Node* n = static_cast<Node*>(node_);
  n->ns += end - start_ns_;
  ++n->calls;
  tree().current = n->parent;
}

void reset() {
  Tree& t = tree();
  t.root.children.clear();
  t.current = &t.root;
}

std::vector<Entry> snapshot() {
  std::vector<Entry> out;
  walk(tree().root, "", 0, out);
  return out;
}

}  // namespace prmd::profile

#else  // profiling compiled out

namespace prmd::profile {

void reset() {}

std::vector<Entry> snapshot() {
  return {};
}

}  // namespace prmd::profile

#endif
