// Call-tree profiler behind PRMD_TIMED (include/prmd/profile.h).

#include "prmd/profile.h"

#ifdef PRMD_PROFILE

#include "prmd/internal/profile_threads.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <tuple>

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
  // Where a node was first created during a Pool::run: 1 + the begin index
  // of its chunk (0: before the run, or outside any run) and its creation
  // order inside that chunk. Only used, then cleared, by canonicalize().
  uint64_t key_chunk = 0;
  uint64_t key_seq = 0;

  Node* find(const char* n) {
    for (auto& c : children) {
      // Literals with the same text are usually merged, so the pointer test
      // almost always decides; strcmp covers the rest.
      if (c->name == n || std::strcmp(c->name, n) == 0) {
        return c.get();
      }
    }
    return nullptr;
  }

  Node* add(const char* n) {
    children.push_back(std::make_unique<Node>());
    Node* c = children.back().get();
    c->name = n;
    c->parent = this;
    return c;
  }

  Node* child(const char* n);
};

// The chunk this thread is running (Pool::run), 0 outside chunks.
struct ChunkKey {
  uint64_t chunk = 0;
  uint64_t seq = 0;
};
thread_local ChunkKey tl_chunk;

Node* Node::child(const char* n) {
  if (Node* c = find(n)) {
    return c;
  }
  Node* c = add(n);
  if (tl_chunk.chunk != 0) {
    c->key_chunk = tl_chunk.chunk;
    c->key_seq = tl_chunk.seq++;
  }
  return c;
}

bool key_less(const Node& a, const Node& b) {
  return std::tie(a.key_chunk, a.key_seq) < std::tie(b.key_chunk, b.key_seq);
}

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
    Node* d = dst.find(c->name);
    if (!d) {
      d = dst.add(c->name);
      d->key_chunk = c->key_chunk;
      d->key_seq = c->key_seq;
    } else if (key_less(*c, *d)) {
      d->key_chunk = c->key_chunk;
      d->key_seq = c->key_seq;
    }
    d->calls += c->calls;
    d->ns += c->ns;
    d->worker_calls += c->calls;
    d->worker_ns += c->ns;
    merge(*d, *c);
  }
}

// Stable: nodes with equal keys (0: created before the run) keep their
// order.
void sort_children(Node& n) {
  std::stable_sort(n.children.begin(), n.children.end(),
                   [](const std::unique_ptr<Node>& a,
                      const std::unique_ptr<Node>& b) {
                     return key_less(*a, *b);
                   });
  for (auto& c : n.children) {
    c->key_chunk = 0;
    c->key_seq = 0;
    sort_children(*c);
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

void begin_chunk(size_t b) {
  tl_chunk.chunk = uint64_t(b) + 1;
  tl_chunk.seq = 0;
}

void end_chunk() {
  tl_chunk = ChunkKey{};
}

void canonicalize(void* scope) {
  sort_children(*static_cast<Node*>(scope));
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
