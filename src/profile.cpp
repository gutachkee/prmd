// Call-tree profiler behind PRMD_TIMED (include/prmd/profile.h).

#include "prmd/profile.h"

#ifdef PRMD_PROFILE

#include "prmd/internal/profile_threads.h"

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
  // Part of calls / ns merged in from pool worker threads.
  uint64_t worker_calls = 0;
  int64_t worker_ns = 0;

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
    e.worker_calls = c->worker_calls;
    e.worker_ms = double(c->worker_ns) / 1e6;
    out.push_back(e);
    walk(*c, e.path, depth + 1, out);
  }
}

// Adds every node below `src` to the matching (by name) node below `dst`,
// counting it as worker time.
void merge(Node& dst, const Node& src) {
  for (const auto& c : src.children) {
    Node* d = dst.child(c->name);
    d->calls += c->calls;
    d->ns += c->ns;
    d->worker_calls += c->calls;
    d->worker_ns += c->ns;
    merge(*d, *c);
  }
}

}  // namespace

namespace detail {

struct Subtree {
  Node root;
};

void SubtreeDeleter::operator()(Subtree* t) const {
  delete t;
}

void* current_scope() {
  return tree().current;
}

void begin_worker() {
  reset();
}

SubtreePtr end_worker() {
  Tree& t = tree();
  if (t.root.children.empty()) {
    return nullptr;
  }
  SubtreePtr out(new Subtree);
  out->root.children = std::move(t.root.children);
  reset();
  return out;
}

void merge_worker(void* scope, Subtree& t) {
  merge(*static_cast<Node*>(scope), t.root);
}

}  // namespace detail

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
