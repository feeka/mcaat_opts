#ifndef MCAAT_BOUNDED_CYCLE_SEARCH_H
#define MCAAT_BOUNDED_CYCLE_SEARCH_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace mcaat {
namespace cycle_search {

struct RangeStats {
    uint64_t shorter_cycle_roots = 0;
    uint64_t rejected_small_region = 0;
    uint64_t confirmed_roots = 0;
    uint64_t needs_enumeration = 0;
};

enum class CandidateStatus { Rejected, Confirmed, NeedsEnumeration };

// One owner per scratch object. The graph must not mutate during a search.
// BackwardSet is an exact set with clear/count/insert (flat_hash_set in production).
template <class NodeId, class BackwardSet>
struct Scratch {
    static_assert(std::is_unsigned<NodeId>::value, "unsigned graph IDs required");
    using WordId = typename std::conditional<sizeof(NodeId) == 4,
                                             uint32_t, uint64_t>::type;
    std::vector<uint64_t> forward_bits;
    std::vector<WordId> dirty_words;
    BackwardSet backward_seen;
    std::vector<NodeId> forward, backward, next;

    void reset(uint64_t node_count) {
        if (node_count && node_count - 1 > std::numeric_limits<NodeId>::max())
            throw std::length_error("BFS node ID type is too narrow");
        const uint64_t words = node_count / 64 + (node_count % 64 != 0);
        if (words != forward_bits.size()) {
            // Called by the owning worker: first-touch placement of its bitmap.
            forward_bits.assign(words, 0);
        } else {
            for (WordId word : dirty_words) forward_bits[word] = 0;
        }
        dirty_words.clear();
        backward_seen.clear();
        forward.clear();
        backward.clear();
        next.clear();
    }

    bool forward_contains(uint64_t id) const {
        return (forward_bits[id >> 6] & (uint64_t{1} << (id & 63))) != 0;
    }

    bool forward_insert(uint64_t id) {
        auto& word = forward_bits[id >> 6];
        const uint64_t mask = uint64_t{1} << (id & 63);
        const uint64_t old = word;
        if (old & mask) return false;
        if (old == 0) dirty_words.push_back(static_cast<WordId>(id >> 6));
        word = old | mask;
        return true;
    }
};

template <bool Forward, class Graph, class Predicate, class NodeId, class Set>
bool expand_layer(const Graph& graph, uint64_t start, const Predicate& allowed,
                  Scratch<NodeId, Set>& scratch, bool final_layer) {
    auto& frontier = Forward ? scratch.forward : scratch.backward;
    scratch.next.clear();
    uint64_t neighbors[4];
    for (NodeId v : frontier) {
        // SDBG returns only valid neighbors, or -1 for an invalid source.
        const int count = Forward ? graph.OutgoingEdges(v, neighbors)
                                  : graph.IncomingEdges(v, neighbors);
        for (int i = 0; i < count; ++i) {
            const uint64_t nb = neighbors[i];
            if (nb == start) continue;  // Search in G minus the root.
            const NodeId id = static_cast<NodeId>(nb);
            if constexpr (Forward) {
                if (scratch.forward_contains(nb)) continue;
                if (scratch.backward_seen.count(id)) return true;
                if (!final_layer && allowed(nb) && scratch.forward_insert(nb))
                    scratch.next.push_back(id);
            } else {
                // Opposite-side membership proves admissibility already.
                if (scratch.forward_contains(nb)) return true;
                if (final_layer || scratch.backward_seen.count(id)) continue;
                if (allowed(nb) && scratch.backward_seen.insert(id).second)
                    scratch.next.push_back(id);
            }
        }
    }
    if (!final_layer) frontier.swap(scratch.next);
    return false;
}

// Return the shortest simple-cycle length through start, or zero if none is
// within limit. Before a first intersection, the two visited balls are disjoint:
// every path of length <= their radius sum would already have intersected.
// Does not enforce a minimum cycle length. A and P are outgoing/incoming
// neighbors of start; find an A-to-P path of length <= limit - 2 in G-start.
// Optional incomings must be an unfiltered IncomingEdges result for this root
// from the same immutable graph snapshot.
template <class Graph, class Predicate, class NodeId, class Set>
int shortest_cycle_length(const Graph& graph, uint64_t start, int limit,
                          const Predicate& allowed, Scratch<NodeId, Set>& scratch,
                          const uint64_t* incomings = nullptr, int incoming_count = 0) {
    if (limit < 1 || start >= graph.size() || !graph.IsValidEdge(start) ||
        !allowed(start)) return 0;
    uint64_t neighbors[4];
    const int outgoing_count = graph.OutgoingEdges(start, neighbors);
    if (outgoing_count <= 0) return 0;
    for (int i = 0; i < outgoing_count; ++i)
        if (neighbors[i] == start) return 1;
    if (limit == 1) return 0;

    scratch.reset(graph.size());
    for (int i = 0; i < outgoing_count; ++i) {
        const uint64_t nb = neighbors[i];
        if (allowed(nb) && scratch.forward_insert(nb))
            scratch.forward.push_back(static_cast<NodeId>(nb));
    }
    if (scratch.forward.empty()) return 0;
    if (incomings == nullptr) {
        incoming_count = graph.IncomingEdges(start, neighbors);
        incomings = neighbors;
    }
    for (int i = 0; i < incoming_count; ++i) {
        const uint64_t nb = incomings[i];
        if (nb == start || !allowed(nb)) continue;
        if (scratch.forward_contains(nb)) return 2;
        if (scratch.backward_seen.insert(static_cast<NodeId>(nb)).second)
            scratch.backward.push_back(static_cast<NodeId>(nb));
    }

    const int budget = limit - 2;
    int radius_sum = 0;
    while (!scratch.forward.empty() && !scratch.backward.empty() &&
           radius_sum < budget) {
        const bool final_layer = radius_sum + 1 == budget;
        const bool found = scratch.forward.size() <= scratch.backward.size()
            ? expand_layer<true>(graph, start, allowed, scratch, final_layer)
            : expand_layer<false>(graph, start, allowed, scratch, final_layer);
        if (found) return radius_sum + 3;
        if (final_layer) return 0;
        ++radius_sum;
    }
    return 0;
}

// Preserve the upper-bound-only public kernel for callers needing that contract.
template <class Graph, class Predicate, class NodeId, class Set>
bool exists(const Graph& graph, uint64_t start, int limit,
            const Predicate& allowed, Scratch<NodeId, Set>& scratch,
            const uint64_t* incomings = nullptr, int incoming_count = 0) {
    return shortest_cycle_length(graph, start, limit, allowed, scratch,
                                 incomings, incoming_count) != 0;
}

// Every vertex of a qualifying simple cycle must be reachable in both directions.
// Stop as soon as required distinct vertices are found. Exhaustion below required
// proves that no long-enough simple cycle exists. This visits at most required-1
// adjacency lists, rather than constructing a whole component of the graph.
template <bool Forward, class Graph, class Predicate, class NodeId, class Set>
bool enough_reachable_nodes(const Graph& graph, uint64_t start, std::size_t required,
                            const Predicate& allowed, Scratch<NodeId, Set>& scratch) {
    scratch.reset(graph.size());
    scratch.forward_insert(start);
    scratch.forward.push_back(static_cast<NodeId>(start));
    if (required <= 1) return true;
    uint64_t neighbors[4];
    for (std::size_t i = 0; i < scratch.forward.size(); ++i) {
        const NodeId node = scratch.forward[i];
        const int count = Forward ? graph.OutgoingEdges(node, neighbors)
                                  : graph.IncomingEdges(node, neighbors);
        for (int j = 0; j < count; ++j) {
            const uint64_t nb = neighbors[j];
            if (scratch.forward_contains(nb) || !allowed(nb)) continue;
            scratch.forward_insert(nb);
            scratch.forward.push_back(static_cast<NodeId>(nb));
            if (scratch.forward.size() >= required) return true;
        }
    }
    return false;
}

// Conservative candidate classification, NOT an exact range-existence decision.
// A too-short cycle neither qualifies nor rules out a longer valid cycle.
// After BFS, perform at most 2*(minimum-1) additional adjacency calls. If these
// bounded necessary conditions cannot reject the root, defer it to enumeration.
// Never enumerate simple paths in this scan or interpret unresolved as rejected.
template <class Graph, class Predicate, class NodeId, class Set>
CandidateStatus classify_candidate(const Graph& graph, uint64_t start,
                                    int minimum, int maximum,
                                    const Predicate& allowed, Scratch<NodeId, Set>& scratch,
                                    const uint64_t* incomings = nullptr,
                                    int incoming_count = 0, RangeStats* statistics = nullptr) {
    minimum = std::max(1, minimum);
    if (maximum < minimum || graph.size() < static_cast<uint64_t>(minimum))
        return CandidateStatus::Rejected;
    // A simple cycle cannot contain more vertices than the graph.
    maximum = static_cast<int>(std::min(static_cast<uint64_t>(maximum), graph.size()));
    const int shortest = shortest_cycle_length(graph, start, maximum, allowed,
                                               scratch, incomings, incoming_count);
    if (shortest == 0) return CandidateStatus::Rejected;
    if (shortest >= minimum) {
        if (statistics) ++statistics->confirmed_roots;
        return CandidateStatus::Confirmed;
    }
    if (statistics) ++statistics->shorter_cycle_roots;

    if (!enough_reachable_nodes<true>(graph, start, static_cast<std::size_t>(minimum),
                                      allowed, scratch) ||
        !enough_reachable_nodes<false>(graph, start, static_cast<std::size_t>(minimum),
                                       allowed, scratch)) {
        if (statistics) ++statistics->rejected_small_region;
        return CandidateStatus::Rejected;
    }

    if (statistics) ++statistics->needs_enumeration;
    return CandidateStatus::NeedsEnumeration;
}

}  // namespace cycle_search
}  // namespace mcaat
#endif
