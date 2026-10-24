#include "crispr_array/bounded_cycle_search.h"

#include <iostream>
#include <unordered_set>

namespace search = mcaat::cycle_search;

struct Graph {
    std::vector<std::vector<uint64_t>> edges;
    std::vector<bool> valid;
    explicit Graph(size_t n) : edges(n), valid(n, true) {}
    uint64_t size() const { return edges.size(); }
    bool IsValidEdge(uint64_t v) const { return valid.at(v); }
    int OutgoingEdges(uint64_t v, uint64_t* out) const {
        if (!valid.at(v)) return -1;
        int count = 0;
        for (uint64_t w : edges.at(v)) if (valid.at(w)) out[count++] = w;
        return count;
    }
    int IncomingEdges(uint64_t v, uint64_t* out) const {
        if (!valid.at(v)) return -1;
        int count = 0;
        for (uint64_t u = 0; u < size(); ++u)
            if (valid[u]) for (uint64_t w : edges[u]) if (w == v) out[count++] = u;
        return count;
    }
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Independent oracle: enumerate simple prefixes, with no bidirectional state.
template <class Allowed>
bool oracle(const Graph& g, uint64_t root, int limit, const Allowed& allowed,
            int minimum = 1) {
    if (limit < 1 || root >= g.size() || !g.valid[root] || !allowed(root)) return false;
    std::vector<bool> on_path(g.size(), false);
    on_path[root] = true;
    auto visit = [&](auto&& self, uint64_t v, int depth) -> bool {
        if (depth == limit) return false;
        for (uint64_t w : g.edges[v]) {
            if (!g.valid[w] || !allowed(w)) continue;
            if (w == root) {
                if (depth + 1 >= minimum) return true;
                continue;
            }
            if (!on_path[w]) {
                on_path[w] = true;
                if (self(self, w, depth + 1)) return true;
                on_path[w] = false;
            }
        }
        return false;
    };
    return visit(visit, root, 0);
}

template <class Id, class Allowed>
void check(const Graph& g, uint64_t root, int limit, const Allowed& allowed,
           search::Scratch<Id, std::unordered_set<Id>>& scratch) {
    const bool expected = oracle(g, root, limit, allowed);
    require(search::exists(g, root, limit, allowed, scratch) == expected,
            "BFS disagrees with simple-path oracle");
    int shortest = 0;
    for (int length = 1; length <= limit; ++length) {
        if (oracle(g, root, length, allowed)) {
            shortest = length;
            break;
        }
    }
    require(search::shortest_cycle_length(g, root, limit, allowed, scratch) == shortest,
            "Shortest cycle length disagrees with oracle");
    for (int minimum = 0; minimum <= std::max(1, limit + 1); ++minimum)
        require(search::exists_in_range(g, root, minimum, limit, allowed, scratch) ==
                    oracle(g, root, limit, allowed, minimum),
                "Range detector disagrees with simple-path oracle");
    if (root < g.size() && g.valid[root]) {
        uint64_t incoming[4];
        const int count = g.IncomingEdges(root, incoming);
        require(search::exists(g, root, limit, allowed, scratch, incoming, count) == expected,
                "Cached incoming-neighbor path disagrees with oracle");
        for (int minimum = 0; minimum <= std::max(1, limit + 1); ++minimum)
            require(search::exists_in_range(g, root, minimum, limit, allowed,
                        scratch, incoming, count) ==
                        oracle(g, root, limit, allowed, minimum),
                    "Cached range detector disagrees with oracle");
    }
}

template <class Id>
void run() {
    search::Scratch<Id, std::unordered_set<Id>> scratch;
    auto all = [](uint64_t) { return true; };

    // Every loop-free directed graph on four vertices, every root and bound.
    for (unsigned mask = 0; mask < (1u << 12); ++mask) {
        Graph g(4);
        unsigned bit = 0;
        for (uint64_t u = 0; u < 4; ++u)
            for (uint64_t v = 0; v < 4; ++v) if (u != v) {
                if (mask & (1u << bit)) g.edges[u].push_back(v);
                ++bit;
            }
        for (uint64_t root = 0; root < 4; ++root)
            for (int limit = 0; limit <= 5; ++limit)
                check(g, root, limit, all, scratch);
    }

    // Exercise both expansion directions, filtering, early returns and reused state.
    Graph branching(6);
    branching.edges = {{1, 2, 3}, {4}, {4}, {5}, {0, 5}, {0}};
    for (unsigned mask = 0; mask < 64; ++mask) {
        auto allowed = [mask](uint64_t id) { return (mask & (1u << id)) != 0; };
        for (uint64_t root = 0; root < 6; ++root)
            for (int limit = 1; limit <= 7; ++limit)
                check(branching, root, limit, allowed, scratch);
    }
    branching.valid[4] = false;
    for (uint64_t root = 0; root < 6; ++root)
        check(branching, root, 7, all, scratch);

    // Odd/even strict upper bounds, bitmap word boundaries and graph resizing.
    for (size_t n : {size_t{3}, size_t{6}, size_t{65}, size_t{130}, size_t{3}}) {
        Graph ring(n);
        for (size_t v = 0; v < n; ++v) ring.edges[v].push_back((v + 1) % n);
        for (int delta : {-1, 0, 1})
            check(ring, 0, static_cast<int>(n) + delta, all, scratch);
    }
    Graph self_loop(1);
    self_loop.edges[0] = {0};
    for (int limit : {-1, 0, 1, 2}) check(self_loop, 0, limit, all, scratch);
    auto none = [](uint64_t) { return false; };
    check(self_loop, 0, 2, none, scratch);
    Graph empty(0);
    check(empty, 0, 2, all, scratch);

    // The root has no cycle even though a reachable region does.
    Graph trap(3);
    trap.edges = {{1}, {2}, {1}};
    check(trap, 0, 77, all, scratch);

    // Repeated laps of a short cycle must not count as a longer SIMPLE cycle.
    Graph short_only(80);
    short_only.edges[0] = {1};
    short_only.edges[1] = {0};
    require(!search::exists_in_range(short_only, 0, 27, 77, all, scratch),
            "A two-cycle cannot qualify by taking repeated laps");

    // Large outgoing reachability does not mean there are enough vertices
    // that can return to the root (and conversely for the reversed graph).
    Graph tail(80);
    tail.edges[0] = {1, 2};
    tail.edges[1] = {0};
    for (uint64_t v = 2; v < 79; ++v) tail.edges[v] = {v + 1};
    require(!search::exists_in_range(tail, 0, 27, 77, all, scratch),
            "A long outgoing tail must not inflate the cycle length");
    Graph reverse_tail(80);
    for (uint64_t v = 0; v < tail.size(); ++v)
        for (uint64_t w : tail.edges[v]) reverse_tail.edges[w].push_back(v);
    require(!search::exists_in_range(reverse_tail, 0, 27, 77, all, scratch),
            "A long incoming tail must not inflate the cycle length");

    // Rejecting a root just because its shortest cycle is too short is unsound.
    // This root belongs to both a two-cycle and a 27-cycle.
    Graph mixed(80);
    mixed.edges[0] = {1, 2};
    mixed.edges[1] = {0};
    for (uint64_t v = 2; v < 27; ++v) mixed.edges[v] = {v + 1};
    mixed.edges[27] = {0};
    require(search::exists_in_range(mixed, 0, 27, 77, all, scratch),
            "A short cycle must not hide a qualifying longer cycle");
    require(!search::exists_in_range(mixed, 0, 28, 77, all, scratch),
            "Neither cycle meets a minimum above 27");
    auto blocked_long_path = [](uint64_t id) { return id != 14; };
    require(!search::exists_in_range(mixed, 0, 27, 77, blocked_long_path, scratch),
            "The range fallback must apply the multiplicity predicate");
    mixed.valid[14] = false;
    require(!search::exists_in_range(mixed, 0, 27, 77, all, scratch),
            "The range fallback must respect graph validity");
}

int main() {
    try {
        run<uint32_t>();
        run<uint64_t>();
        search::Scratch<uint32_t, std::unordered_set<uint32_t>> narrow;
        bool rejected = false;
        try { narrow.reset((uint64_t{1} << 32) + 1); }
        catch (const std::length_error&) { rejected = true; }
        require(rejected, "Narrow IDs must be rejected before allocating a bitmap");
        std::cout << "Bounded-cycle search checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
