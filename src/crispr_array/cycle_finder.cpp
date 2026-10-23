#include "crispr_array/cycle_finder.h"
#include "crispr_array/bounded_cycle_search.h"
#include <algorithm>
#include <limits>
#include <climits>
#include <chrono>
#include <type_traits>
#include "crispr_array/filters.h"
#include "core/settings.h"
#include <stdexcept>
#include <fstream>
#include <stack>

// Parallel hashmap for better performance in DLS
#include <parallel_hashmap/phmap.h>

/**
 * @file cycle_finder.cpp
 * @brief Implementation of functions for cycle detection and analysis in a sequence graph.
 * - Checking if any incoming edge of a node is not equal to the node itself.
 * - Performing a background check on a neighbor node to determine if it meets certain criteria.
 * - Getting the outgoing edges of a node that pass the background check.
 * - Retrieving the incoming edges of a node that pass the background check.
 * - Finding cycles in the graph.
 * - Performing a depth-limited search to determine if a path exists between two nodes within a certain depth.
 * - Collecting tips (nodes with no outgoing edges) in the graph.
 * - Performing recursive reduction to invalidate edges.
 * - Invalidating nodes with edge multiplicity of one.
 * - Chunking start nodes based on their multiplicity for parallel processing.
 * - Finding all cycles in the graph by iterating over chunked start nodes and utilizing parallel processing.
 */


/**
 * @brief Checks if any incoming edge of a node is not equal to the node itself.
 */
bool CycleFinder::_IncomingNotEqualToCurrentNode(uint64_t node, size_t edge_indegree) {
    uint64_t incomings[edge_indegree];
    this->settings.sdbg->IncomingEdges(node, incomings);
    for (const auto& incoming : incomings)
        if (node==incoming)
            return true;
    return false;
}
/**
 * @brief Performs a background check on a neighbor node to determine if it meets certain criteria.
 */
bool CycleFinder::_BackgroundCheck(uint64_t original_node, size_t repeat_multiplicity, uint64_t neighbor_node) {
    auto neighbor_node_multiplicity = this->settings.sdbg->EdgeMultiplicity(neighbor_node);
    // Use lock-free visited test (atomic bitset) to avoid contention
    size_t idx = neighbor_node >> 6;
    uint64_t mask = 1ULL << (neighbor_node & 63);
    int tid = omp_get_thread_num();
    if ((per_thread_visited[tid][idx] & mask) != 0) {
        return false;
    }
    if (repeat_multiplicity / neighbor_node_multiplicity > 500) {
        return false;
    }
    if (original_node == neighbor_node) {
        return false;
    }
    return true; 
}


/**
 * @brief Gets the outgoing edges of a node that pass the background check.
 */
void CycleFinder::_GetOutgoings(uint64_t node, SmallNodeSet& outgoings_set, size_t repeat_multiplicity) {
   
    int edge_outdegree = this->settings.sdbg->EdgeOutdegree(node);
    if (edge_outdegree == 0 || !this->settings.sdbg->IsValidEdge(node)) {
        return;
    }
     uint64_t outgoings[edge_outdegree];
    int flag = this->settings.sdbg->OutgoingEdges(node, outgoings);
    if(flag!=-1)    
        for (const auto& outgoing : outgoings)
            if (this->_BackgroundCheck(node, repeat_multiplicity, outgoing) && this->settings.sdbg->IsValidEdge(outgoing))
                outgoings_set.insert(outgoing);
    
    
}
/**
 * @brief Retrieves the incoming edges of a node that pass the background check.
 */
void CycleFinder::_GetIncomings(uint64_t node, SmallNodeSet& incomings_set, size_t repeat_multiplicity) {
  
    int edge_indegree = this->settings.sdbg->EdgeIndegree(node);
    if (edge_indegree == 0 || !this->settings.sdbg->IsValidEdge(node)) {
        return;
    }
    uint64_t incomings[edge_indegree];
    int flag = this->settings.sdbg->IncomingEdges(node, incomings);
    if (flag!=-1)
        for (const auto& incoming : incomings)
            if (this->_BackgroundCheck(node, repeat_multiplicity, incoming) && this->settings.sdbg->IsValidEdge(incoming))
                incomings_set.insert(incoming);
}
// ## START: HELPER FUNCTIONS FOR DLS ##
/**
 * @brief Gets the outgoing edges of a node that pass the background check.
 */
void CycleFinder::_GetOutgoings(uint64_t node, SmallNodeSet& outgoings_set) {
   
    int edge_outdegree = this->settings.sdbg->EdgeOutdegree(node);
    if (edge_outdegree == 0 || !this->settings.sdbg->IsValidEdge(node)) {
        return;
    }
    uint64_t outgoings[edge_outdegree];
    int flag = this->settings.sdbg->OutgoingEdges(node, outgoings);
    if(flag!=-1)
        for (const auto& outgoing : outgoings)
            if (this->settings.sdbg->IsValidEdge(outgoing) && outgoing != node && !(this->settings.sdbg->EdgeMultiplicity(node) / this->settings.sdbg->EdgeMultiplicity(outgoing) > 500))
                outgoings_set.insert(outgoing);
    
    
}
/**
 * @brief Retrieves the incoming edges of a node that pass the background check.
 */
void CycleFinder::_GetIncomings(uint64_t node, SmallNodeSet& incomings_set) {
  
    int edge_indegree = this->settings.sdbg->EdgeIndegree(node);
    if (edge_indegree == 0 || !this->settings.sdbg->IsValidEdge(node)) {
        return;
    }
    uint64_t incomings[edge_indegree];
    int flag = this->settings.sdbg->IncomingEdges(node, incomings);
    if (flag!=-1)
        for (const auto& incoming : incomings)
            if (this->settings.sdbg->IsValidEdge(incoming) && incoming != node && !(this->settings.sdbg->EdgeMultiplicity(node) / this->settings.sdbg->EdgeMultiplicity(incoming) > 500))
                incomings_set.insert(incoming);
}
// ## END: HELPER FUNCTIONS FOR DLS ##

/**
 * @brief Finds the cycles in the graph using settings configuration.
 * 
 * @param settings The application Settings containing SDBG pointer and cycle finder configuration.
 */
CycleFinder::CycleFinder(Settings& settings)
    : settings(settings), cluster_bounds(500) {
    // Ensure settings.sdbg is valid; this should be set by caller before constructing CycleFinder
    if (this->settings.sdbg == nullptr) {
        throw std::runtime_error("CycleFinder requires settings.sdbg to be set to a valid SDBG instance");
    }
    if (settings.threads == 0 || settings.threads > static_cast<size_t>(INT_MAX)) {
        throw std::invalid_argument("CycleFinder requires a valid positive thread count");
    }
    this->FindApproximateCRISPRArrays();
}

vector<vector<uint64_t>> CycleFinder::FindCycle(uint64_t start_node, vector<uint64_t>& path, phmap::flat_hash_map<uint64_t,int>& lock, vector<SmallNodeSet>& stack, 
                                        vector<int>& backtrack_lengths) {
    int counter = 0;
    uint64_t current_node = start_node;
    vector<vector<uint64_t>> cycles;
    int steps_counter = 0;

    while (!stack.empty()) {
        steps_counter += 1;
        if (steps_counter > 10000000) {
            break;
        }
        
        SmallNodeSet neighbors = stack.back();
        bool flag = true;
        for (auto neighbor : neighbors) {
            current_node = neighbor;
            if (current_node == start_node ) {
                backtrack_lengths.back() = 1;
                if (path.size() > static_cast<size_t>(this->settings.cycle_finder_settings.cycle_min_length)) {
                    cycles.push_back(path);
                    counter += 1;
                    if (counter >= this->cluster_bounds) {
                        cycles.clear();
                        flag=false;
                    }
                }
            } 
            else if (static_cast<int>(path.size()) < lock.try_emplace(neighbor, this->settings.cycle_finder_settings.cycle_max_length).first->second) {
                neighbors.erase(neighbor);
                path.push_back(neighbor);
                backtrack_lengths.push_back(this->settings.cycle_finder_settings.cycle_max_length);
                lock[neighbor] = path.size();
                stack.back().erase(neighbor);
                SmallNodeSet outgoings;
                this->_GetOutgoings(neighbor, outgoings, this->settings.sdbg->EdgeMultiplicity(start_node));
                stack.push_back(outgoings);
                flag = false;
                break;
            }
        }
        if (flag) {
            stack.pop_back();
            uint64_t v = path.back();
            path.pop_back();
            int backtrack_length = backtrack_lengths.back();
            backtrack_lengths.pop_back();

            if (!backtrack_lengths.empty()) {
                backtrack_lengths.back() = min(backtrack_lengths.back(), backtrack_length);
            }
            if (backtrack_length < this->settings.cycle_finder_settings.cycle_max_length) {
                vector<pair<int, int>> relax_stack;
                relax_stack.push_back(make_pair(backtrack_length, v));

                phmap::flat_hash_set<uint64_t> path_set(path.begin(), path.end());

                while (!relax_stack.empty()) {
                    int bl = relax_stack.back().first;
                    int u = relax_stack.back().second;
                    relax_stack.pop_back();
                    if (lock.try_emplace(u, this->settings.cycle_finder_settings.cycle_max_length).first->second < this->settings.cycle_finder_settings.cycle_max_length - bl + 1) {
                        lock[u] = this->settings.cycle_finder_settings.cycle_max_length - bl + 1;
                        SmallNodeSet incomings;
                        this->_GetIncomings(u, incomings, this->settings.sdbg->EdgeMultiplicity(start_node));
                        for (auto w : incomings)
                            if (path_set.find(w) == path_set.end())
                                relax_stack.push_back(make_pair(bl + 1, w));
                    }
                }
            }
        }
    }
    
    
    if (cycles.empty()) return {};

    // Mark visited nodes atomically (no global critical section)
    for (const auto& cycle : cycles)
        for (const auto& node : cycle) {
            size_t idx = node >> 6;
            uint64_t mask = 1ULL << (node & 63);
            int tid = omp_get_thread_num();
            per_thread_visited[tid][idx] |= mask;
        }

    return cycles;
}


/**
 * @brief Utility function to initialize and start the cycle finding process from a given node.
 */
vector<vector<uint64_t>> CycleFinder::FindCycleUtil(uint64_t start_node) {
    vector<uint64_t> path;
    phmap::flat_hash_map<uint64_t, int> lock;
    vector<SmallNodeSet> stack;
    vector<int> backtrack_lengths;
    path.push_back(start_node);
    lock[start_node] = 0;
    SmallNodeSet outgoings;
    this->_GetOutgoings(start_node, outgoings, this->settings.sdbg->EdgeMultiplicity(start_node));
    stack.push_back(outgoings);
    backtrack_lengths.push_back(this->settings.cycle_finder_settings.cycle_max_length);
    return FindCycle(start_node, path, lock, stack, backtrack_lengths);
}
/**
 * @brief Performs a depth-limited search to determine if a path exists between two nodes within a certain depth.
 * Optimized using megahit's graph traversal patterns.
 */
bool CycleFinder::DepthLevelSearch(uint64_t start, uint64_t target, int limit, int& reached_depth) {
    struct StackEntry {
        uint64_t node;
        int depth;
    };

    static thread_local std::vector<StackEntry> dls_stack_pool;
    dls_stack_pool.clear();
    if (dls_stack_pool.capacity() == 0)
        dls_stack_pool.reserve(64);
    auto& dls_stack = dls_stack_pool;

    // Reuse per_thread_visited (allocated before ChunkStartNodes — no extra memory).
    // Dirty list clears only touched words: O(visited/64) per call, not O(N).
    const int tid = omp_get_thread_num();
    auto& vis_bits  = per_thread_visited[tid];
    auto& vis_dirty = per_thread_dirty[tid];
    for (uint32_t w : vis_dirty) vis_bits[w] = 0ULL;
    vis_dirty.clear();
    auto vis_test = [&](uint64_t n) -> bool { return (vis_bits[n >> 6] >> (n & 63)) & 1ULL; };
    auto vis_mark = [&](uint64_t n) {
        uint32_t w = static_cast<uint32_t>(n >> 6);
        vis_bits[w] |= 1ULL << (n & 63);
        vis_dirty.push_back(w);
    };

    // Cache values for faster comparison
    const uint64_t target_node = target;
    const uint64_t start_node = start;

    dls_stack.push_back({start_node, 0});
    reached_depth = 0;

    while (!dls_stack.empty()) {
        StackEntry current = dls_stack.back();
        dls_stack.pop_back();
        uint64_t v = current.node;
        int depth = current.depth;

        // Check if the current node is valid
        if (!this->settings.sdbg->IsValidEdge(v)) {
            continue;
        }

        // Update reached depth
        reached_depth = depth;

        // Get neighbors using SDBG API directly (megahit's pattern) - moved up for better branch prediction
        // Use megahit's efficient zero-degree check for early termination
        if (__builtin_expect(this->settings.sdbg->EdgeOutdegreeZero(v), 0)) {
            continue;
        }
        
        int outdegree = this->settings.sdbg->EdgeOutdegree(v);

        // Use fixed-size array for neighbors (megahit's pattern)
        uint64_t neighbors[MAX_EDGE_COUNT];
        int flag = this->settings.sdbg->OutgoingEdges(v, neighbors);

        if (__builtin_expect(flag == -1, 0)) {
            continue;
        }

        // Prefetch neighbors for better cache performance
        __builtin_prefetch(&neighbors[0], 0, 1);

        // Exceeded depth limit - check after we know we have neighbors
        if (__builtin_expect(depth >= limit, 0)) {
            continue;
        }

        // Process all neighbors to maintain correctness (removed faulty simple path optimization)
        // Process neighbors in forward order (SIMD-friendly pattern from megahit)
        // Unroll loop for small outdegrees to reduce overhead
            // Unrolled loop for common case (de Bruijn graph max degree = 4)
            for (int i = 0; i < outdegree; ++i) {
                uint64_t neighbor = neighbors[i];
                if (!this->settings.sdbg->IsValidEdge(neighbor)) {
                    continue;
                }
                // Multiplicity pruning: skip paths into low-coverage branches
                if (this->settings.sdbg->EdgeMultiplicity(start_node) /
                    this->settings.sdbg->EdgeMultiplicity(neighbor) > 500) {
                    continue;
                }
                // Cycle found: start_node is reachable from itself
                if (neighbor == start_node) {
                    return true;
                }
                if (!vis_test(neighbor)) {
                    vis_mark(neighbor);
                    dls_stack.push_back({neighbor, depth + 1});
                }
            }
    }

    return false;
}


namespace {
template <class NodeId>
using CycleSearchScratch = mcaat::cycle_search::Scratch<
    NodeId, phmap::flat_hash_set<NodeId>>;

// mul_t is uint16_t in the pinned MEGAHIT dependency: ceil(log2(m)) is 0..16.
unsigned MultiplicityBucket(mul_t multiplicity) {
    unsigned bucket = 0;
    for (unsigned remaining = multiplicity > 0 ? multiplicity - 1 : 0;
         remaining != 0; remaining >>= 1) ++bucket;
    return bucket;
}
}

bool CycleFinder::BidirectionalBFS(uint64_t start, int limit) {
    const SDBG& graph = *settings.sdbg;
    if (limit < 1 || start >= graph.size() || !graph.IsValidEdge(start))
        return false;
    const mul_t minimum = graph.EdgeMultiplicity(start) / 501u + 1u;
    auto dispatch = [&](auto id_tag, auto storage_tag) {
        using NodeId = decltype(id_tag);
        constexpr bool full = decltype(storage_tag)::value;
        // Standalone API calls reuse scratch without sharing enumeration state.
        static thread_local CycleSearchScratch<NodeId> scratch;
        auto allowed = [&](uint64_t id) {
            return graph.CycleMultiplicityAtLeast<full>(id, minimum);
        };
        return mcaat::cycle_search::exists(graph, start, limit, allowed, scratch);
    };
    const bool small_ids = graph.size() == 0 || graph.size() - 1 <= UINT32_MAX;
    if (graph.HasFullMultiplicityStorage()) {
        return small_ids ? dispatch(uint32_t{}, std::true_type{})
                         : dispatch(uint64_t{}, std::true_type{});
    }
    return small_ids ? dispatch(uint32_t{}, std::false_type{})
                     : dispatch(uint64_t{}, std::false_type{});
}

vector<uint64_t> CycleFinder::CollectTips() {
    size_t n = this->settings.sdbg->size();
    int threads = static_cast<int>(this->settings.threads);
    vector<vector<uint64_t>> local_tips(threads);

    #pragma omp parallel num_threads(threads)
    {
        int tid = omp_get_thread_num();
        #pragma omp for schedule(static)
        for (uint64_t node = 0; node < n; node++) {
            if (this->settings.sdbg->EdgeOutdegree(node) == 0 && this->settings.sdbg->IsValidEdge(node)) {
                local_tips[tid].push_back(node);
            }
        }
    }

    // Merge results while keeping memory usage minimal
    size_t total = 0;
    for (auto &v : local_tips) total += v.size();
    vector<uint64_t> tips;
    tips.reserve(total);
    for (auto &v : local_tips) {
        tips.insert(tips.end(), v.begin(), v.end());
    }
    return tips;
} 

void CycleFinder::RecursiveReduction(uint64_t tip) {
    std::stack<uint64_t> work;
    work.push(tip);
    while (!work.empty()) {
        uint64_t node = work.top();
        work.pop();
        if (this->settings.sdbg->EdgeOutdegree(node) > 0)
            continue;
        if (!this->settings.sdbg->IsValidEdge(node))
            continue;
        SmallNodeSet parents;
        this->_GetIncomings(node, parents);
        this->settings.sdbg->SetInvalidEdge(node);
        for (uint64_t parent : parents)
            if (this->settings.sdbg->IsValidEdge(parent))
                work.push(parent);
    }
}
void CycleFinder::InvalidateMultiplicityOneNodes() {
    uint64_t invalidated = 0;
    #pragma omp parallel for reduction(+:invalidated)
    for (uint64_t node = 0; node < this->settings.sdbg->size(); node++) {
        if (this->settings.sdbg->EdgeMultiplicity(node) <=1) {
            this->settings.sdbg->SetInvalidEdge(node);
            invalidated += 1;
        }
    }
    std::cout << "  ▸ Pre-filter: removed " << invalidated << " low-multiplicity edges" << std::endl;
}

/**
 * @brief Chunks the start nodes based on their multiplicity for parallel processing.
 */
size_t CycleFinder::ChunkStartNodes(
    map<int, vector<uint64_t>, greater<int>>& start_nodes_chunked) {
    if (settings.threads == 0 || settings.threads > static_cast<size_t>(INT_MAX))
        throw std::invalid_argument("Cycle search requires a valid positive thread count");
    if (!settings.cycle_finder_settings.low_abundance)
        InvalidateMultiplicityOneNodes();

    // All graph mutation is complete before this region. The adjacency API
    // filters invalid IDs, and cached seed neighbors remain valid throughout it.
    const SDBG& graph = *settings.sdbg;
    const uint64_t n = graph.size();
    const int threads = static_cast<int>(settings.threads);
    const int limit = settings.cycle_finder_settings.cycle_max_length;
    const uint64_t threshold = settings.cycle_finder_settings.threshold_multiplicity;
    if (limit < 1 || n == 0) return 0;

    const auto detection_started = std::chrono::steady_clock::now();
    struct alignas(64) WorkerResults {
        array<vector<uint64_t>, 17> buckets;
        uint64_t candidates = 0;
    };
    vector<WorkerResults> workers(threads);
    // Restore the original continuous worksharing schedule. A slow root holds
    // its worker, not a global window barrier that prevents all other progress.
    constexpr int scan_chunk = 20000;
    auto dispatch = [&](auto id_tag, auto storage_tag) {
        using NodeId = decltype(id_tag);
        constexpr bool full = decltype(storage_tag)::value;
        #pragma omp parallel num_threads(threads)
        {
            const int tid = omp_get_thread_num();
            auto& worker = workers[tid];
            CycleSearchScratch<NodeId> scratch;
            #pragma omp for schedule(dynamic, scan_chunk)
            for (uint64_t id = 0; id < n; ++id) {
                if (!graph.IsValidEdge(id)) continue;
                const mul_t multiplicity = graph.EdgeMultiplicity(id);
                if (multiplicity <= threshold) continue;
                uint64_t incomings[4];
                const int incoming_count = graph.IncomingEdges(id, incomings);
                if (incoming_count < 2) continue;
                bool self_loop = false;
                for (int i = 0; i < incoming_count; ++i)
                    self_loop |= incomings[i] == id;
                if (self_loop) continue;
                ++worker.candidates;
                const mul_t minimum = multiplicity / 501u + 1u;
                auto allowed = [&](uint64_t neighbor) {
                    return graph.CycleMultiplicityAtLeast<full>(neighbor, minimum);
                };
                if (mcaat::cycle_search::exists(graph, id, limit, allowed,
                        scratch, incomings, incoming_count))
                    worker.buckets[MultiplicityBucket(multiplicity)].push_back(id);
            }
        }
    };
    const bool small_ids = n - 1 <= UINT32_MAX;
    if (graph.HasFullMultiplicityStorage()) {
        if (small_ids) dispatch(uint32_t{}, std::true_type{});
        else dispatch(uint64_t{}, std::true_type{});
    } else {
        if (small_ids) dispatch(uint32_t{}, std::false_type{});
        else dispatch(uint64_t{}, std::false_type{});
    }

    size_t total = 0;
    for (unsigned bucket = 0; bucket < 17; ++bucket) {
        size_t count = 0;
        for (const auto& worker : workers) count += worker.buckets[bucket].size();
        if (count == 0) continue;
        auto& out = start_nodes_chunked[static_cast<int>(bucket)];
        out.reserve(out.size() + count);
        for (auto& worker : workers)
            out.insert(out.end(), worker.buckets[bucket].begin(),
                       worker.buckets[bucket].end());
    }
    for (const auto& entry : start_nodes_chunked) {
        total += entry.second.size();
        std::cout << "  ▸ Bucket log2=" << entry.first << ": "
                  << entry.second.size() << " nodes" << std::endl;
    }
    uint64_t candidates = 0;
    for (const auto& worker : workers) candidates += worker.candidates;
    const double detection_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - detection_started).count();
    std::cout << "  ▸ Candidate scan + BFS + merge: " << detection_seconds
              << " s; tested " << candidates << "; accepted " << total << std::endl;
    return total;
}

/**
 * @brief Finds all cycles in the graph by iterating over chunked start nodes and utilizing parallel processing.
 */
int CycleFinder::FindApproximateCRISPRArrays()
 {
    
    vector<uint64_t> tips = this->CollectTips();
    std::cout << "  ▸ Graph: " << this->settings.sdbg->size() << " nodes, tips: " << tips.size() << std::endl;
    
    this->InvalidateMultiplicityOneNodes();
    for ( uint64_t tip : tips) {
        this->RecursiveReduction(tip);
    }
    uint64_t valid_edges = 0;
    #pragma omp parallel for reduction(+:valid_edges)
    for (uint64_t node = 0; node < this->settings.sdbg->size(); node++) {
        if (this->settings.sdbg->IsValidEdge(node)) {
            valid_edges += 1;
        }
    }

    tips = this->CollectTips();
    std::cout << "  ▸ After tip pruning: " << valid_edges << " valid edges, " << tips.size() << " tips remain" << std::endl;
    // struct mallinfo mem_info = mallinfo(); // deprecated
    // size_t graph_mem_info = mem_info.uordblks; // unused
    int cumulative = 0;
    std::cout << "  ▸ Params: max-len=" << this->settings.cycle_finder_settings.cycle_max_length
              << "  min-len=" << this->settings.cycle_finder_settings.cycle_min_length
              << "  threads=" << this->settings.threads << std::endl;
        std::unordered_map<uint64_t, std::vector<std::vector<uint64_t>>> all_cycles;

    map<int, vector<uint64_t>, greater<int>> start_nodes_chunked;
    const size_t start_nodes_amount = ChunkStartNodes(start_nodes_chunked);
    std::cout << "  ▸ Start nodes: " << start_nodes_amount << std::endl;

    const auto enumeration_started = std::chrono::steady_clock::now();
    // Enumeration has independent state; BFS worker scratch has been released.
    const int max_threads = static_cast<int>(settings.threads);
    const uint64_t node_count = settings.sdbg->size();
    const size_t words = node_count / 64 + (node_count % 64 != 0);
    per_thread_visited.resize(max_threads);
    per_thread_dirty.resize(max_threads);
    #pragma omp parallel for num_threads(max_threads) schedule(static, 1)
    for (int t = 0; t < max_threads; ++t) {
        per_thread_visited[t].assign(words, 0);
        per_thread_dirty[t].clear();
    }
    size_t counter = 0;
    for (auto nodes_iterator = start_nodes_chunked.begin(); nodes_iterator != start_nodes_chunked.end(); nodes_iterator++) {
        // Reset visited bitmaps so nodes from a higher-multiplicity bucket don't
        // block traversal in lower-multiplicity buckets.
        #pragma omp parallel for num_threads(max_threads) schedule(static, 1)
        for (int t = 0; t < max_threads; ++t)
            std::fill(per_thread_visited[t].begin(), per_thread_visited[t].end(), 0);
        size_t cumulative_at_bucket_start = cumulative;
        auto thread_count = static_cast<int>(this->settings.threads);
        if (static_cast<int>(nodes_iterator->second.size()) < thread_count)
            thread_count = nodes_iterator->second.size();

        // Per-thread results to avoid concurrent modification of shared unordered_map
        vector<unordered_map<uint64_t, vector<vector<uint64_t>>>> local_results(thread_count);
        vector<size_t> local_cycle_counts(thread_count, 0);
        vector<size_t> local_processed(thread_count, 0);

        #pragma omp parallel num_threads(thread_count)
        {
            int tid = omp_get_thread_num();
            #pragma omp for schedule(static)
            for (uint64_t start_node_index = 0; start_node_index < nodes_iterator->second.size(); start_node_index++) {
                uint64_t start_node = nodes_iterator->second[start_node_index];
                vector<vector<uint64_t>> cycles = this->FindCycleUtil(start_node);
                local_cycle_counts[tid] += cycles.size();
                local_processed[tid] += 1;
                if (!cycles.empty()) {
                    local_results[tid][start_node] = std::move(cycles);
                }
            }
        }

        // Merge local results into shared structures (serial merge)
        for (int t = 0; t < thread_count; ++t) {
            for (auto &entry : local_results[t]) {
                this->results[entry.first] = std::move(entry.second);
            }
            cumulative += local_cycle_counts[t];
            counter += local_processed[t];
        }

        malloc_trim(0);
        // Summarize work performed for this multiplicity bucket and avoid printing too often
        size_t cycles_in_bucket = cumulative - cumulative_at_bucket_start;
        std::cout << "  ▸ Bucket log2=" << nodes_iterator->first << ": "
                  << nodes_iterator->second.size() << " nodes, "
                  << cycles_in_bucket << " cycles (total " << cumulative << ")" << std::endl;
    }
        // Completed cycle enumeration
        std::cout << "  ▸ Total: " << cumulative << " cycles across "
              << this->results.size() << " start nodes" << std::endl;

    std::cout << "  ▸ Enumeration including scratch initialization: "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                               enumeration_started).count()
              << " s" << std::endl;
    const auto output_started = std::chrono::steady_clock::now();
    const std::string cycles_path = this->settings.output_folder + "/cycles.txt";
    this->writeMapToFile(this->results, cycles_path);
    this->settings.cycles = this->results;
    std::cout << "  ▸ Cycle serialization + result copy: "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                               output_started).count()
              << " s" << std::endl;

    return cumulative;
}

void CycleFinder::writeMapToFile(
    const std::unordered_map<uint64_t, std::vector<std::vector<uint64_t>>>& cycles,
    const std::string& filename)
{
    std::ofstream out(filename);
    if (!out.is_open()) {
        std::cerr << "Error: Cannot write cycles to: " << filename << std::endl;
        return;
    }
    const size_t k = this->settings.sdbg->k();
    std::vector<uint8_t> seq(k);
    for (const auto& [start_node, cycle_vec] : cycles) {
        for (const auto& cycle : cycle_vec) {
            for (size_t i = 0; i < cycle.size(); ++i) {
                seq.assign(k, 0);
                this->settings.sdbg->GetLabel(cycle[i], seq.data());
                std::string label(k, 'N');
                for (size_t j = 0; j < k; ++j) {
                    uint8_t c = seq[j];
                    label[j] = (c >= 1 && c <= 4) ? "ACGT"[c - 1] : 'N';
                }
                if (i > 0) out << ' ';
                out << label;
            }
            out << '\n';
        }
    }
    std::cout << "  ▸ Cycles saved: " << filename << std::endl;
}

CycleFinder::~CycleFinder() {}
