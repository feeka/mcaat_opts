#ifndef MCAAT_BOUNDED_CYCLE_SEARCH_H
#define MCAAT_BOUNDED_CYCLE_SEARCH_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace mcaat {
namespace cycle_search {

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

// Detect a nonempty simple cycle through start with length <= limit.
// Does not enforce a minimum cycle length. A and P are outgoing/incoming
// neighbors of start; find an A-to-P path of length <= limit - 2 in G-start.
// Optional incomings must be an unfiltered IncomingEdges result for this root
// from the same immutable graph snapshot.
template <class Graph, class Predicate, class NodeId, class Set>
bool exists(const Graph& graph, uint64_t start, int limit,
            const Predicate& allowed, Scratch<NodeId, Set>& scratch,
            const uint64_t* incomings = nullptr, int incoming_count = 0) {
    if (limit < 1 || start >= graph.size() || !graph.IsValidEdge(start) ||
        !allowed(start)) return false;
    uint64_t neighbors[4];
    const int outgoing_count = graph.OutgoingEdges(start, neighbors);
    if (outgoing_count <= 0) return false;
    for (int i = 0; i < outgoing_count; ++i)
        if (neighbors[i] == start) return true;
    if (limit == 1) return false;

    scratch.reset(graph.size());
    for (int i = 0; i < outgoing_count; ++i) {
        const uint64_t nb = neighbors[i];
        if (allowed(nb) && scratch.forward_insert(nb))
            scratch.forward.push_back(static_cast<NodeId>(nb));
    }
    if (scratch.forward.empty()) return false;
    if (incomings == nullptr) {
        incoming_count = graph.IncomingEdges(start, neighbors);
        incomings = neighbors;
    }
    for (int i = 0; i < incoming_count; ++i) {
        const uint64_t nb = incomings[i];
        if (nb == start || !allowed(nb)) continue;
        if (scratch.forward_contains(nb)) return true;
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
        if (found) return true;
        if (final_layer) return false;
        ++radius_sum;
    }
    return false;
}

}  // namespace cycle_search
}  // namespace mcaat
#endif
