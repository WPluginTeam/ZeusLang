#include "ZeusGC.hpp"

#include "ZeusABIInternal.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace ZeusGC {
namespace {

// Collections are rare; growth between two of them is cheap to keep.
constexpr std::size_t kMinCollectNodes = 1024;
// Allocating code registers nodes far more often than it allocates boxes, so
// the allocation trigger counts registrations instead of walking the node table.
constexpr std::size_t kCollectInterval = 1024;

struct NodeEntry {
    std::weak_ptr<void> owner;
    TraceFunction trace = nullptr;
    BreakCyclesFunction breakCycles = nullptr;
};

class Collector {
public:
    std::recursive_mutex& mutex() { return mutex_; }
    RootLink*& rootHead() { return rootHead_; }

    void registerNode(
        const std::shared_ptr<void>& node,
        TraceFunction trace,
        BreakCyclesFunction breakCycles
    ) {
        if (!node || !trace || !breakCycles) {
            throw std::invalid_argument("managed GC nodes require an owner and callbacks");
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        nodes_[node.get()] = NodeEntry{ node, trace, breakCycles };
        pendingRegistrations_.fetch_add(1, std::memory_order_relaxed);
        if ((nodes_.size() & 63u) == 0) pruneExpired();
    }

    void rootNode(const std::shared_ptr<void>& node) {
        if (!node) return;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        ++nodeRoots_[node.get()];
    }

    void unrootNode(const std::shared_ptr<void>& node) {
        if (!node) return;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = nodeRoots_.find(node.get());
        if (it == nodeRoots_.end()) return;
        if (it->second <= 1) nodeRoots_.erase(it);
        else --it->second;
    }

    // Copies the current root set so marking can run without walking a list
    // that traced values are allowed to mutate.
    std::vector<const ZeusType::UniversalVariant*> snapshotRoots() const {
        return RootRegistry::snapshot();
    }

    void collect(const std::vector<std::shared_ptr<void>>& additionalRoots) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::unordered_set<const void*> marked;
        std::function<void(const ZeusType::UniversalVariant&)> markVariant;
        std::function<void(const ZeusValue&)> markValue;
        std::function<void(const std::shared_ptr<void>&)> markNode;

        Visitor visitor;
        MarkState state{ *this, marked, visitor, markVariant, markValue, markNode };
        visitor.context = &state;
        visitor.value = [](void* context, const ZeusValue* value) {
            auto* state = static_cast<MarkState*>(context);
            state->markValue(*value);
        };
        visitor.variant = [](void* context, const ZeusType::UniversalVariant* value) {
            auto* state = static_cast<MarkState*>(context);
            state->markVariant(*value);
        };
        visitor.node = [](void* context, const std::shared_ptr<void>* node) {
            auto* state = static_cast<MarkState*>(context);
            state->markNode(*node);
        };

        state.markVariant = [&](const ZeusType::UniversalVariant& value) {
            if (const auto* named = std::get_if<std::shared_ptr<ZeusType::NamedArg>>(&value)) {
                if (*named) state.markVariant((*named)->value);
                return;
            }
            if (const auto* native = std::get_if<ZeusType::NativeClosureHandle>(&value)) {
                if (native->state) state.markNode(native->state);
                return;
            }
            if (const auto* wrapped = std::get_if<ZeusType::Variant>(&value)) {
                std::visit([&](const auto& inner) {
                    using T = std::decay_t<decltype(inner)>;
                    if constexpr (std::is_same_v<T, ZeusType::Array>) {
                        if (!inner.impl) return;
                        for (const auto& item : inner.impl->data) state.markVariant(item);
                    } else if constexpr (std::is_same_v<T, ZeusType::Object>) {
                        if (!inner.impl) return;
                        for (const auto& item : inner.impl->data) state.markVariant(item.second);
                    }
                }, *wrapped);
            }
        };
        state.markValue = [&](const ZeusValue& value) {
            if (value.flags != ZEUS_VALUE_FLAG_OWNS_BOX || value.payload == 0) return;
            const auto* box = reinterpret_cast<const ZeusBox*>(
                static_cast<uintptr_t>(value.payload));
            state.markVariant(box->v);
        };
        state.markNode = [&](const std::shared_ptr<void>& node) {
            if (!node || !marked.insert(node.get()).second) return;
            const auto entry = nodes_.find(node.get());
            if (entry == nodes_.end()) return;
            const auto keepAlive = entry->second.owner.lock();
            if (!keepAlive) return;
            entry->second.trace(keepAlive.get(), state.visitor);
        };

        for (const ZeusType::UniversalVariant* root : snapshotRoots()) {
            state.markVariant(*root);
        }
        for (const auto& root : nodeRoots_) {
            const auto entry = nodes_.find(root.first);
            if (entry == nodes_.end()) continue;
            const auto keepAlive = entry->second.owner.lock();
            if (keepAlive) state.markNode(keepAlive);
        }
        for (const auto& root : additionalRoots) state.markNode(root);

        std::vector<std::pair<std::shared_ptr<void>, BreakCyclesFunction>> unreachable;
        unreachable.reserve(nodes_.size());
        for (const auto& item : nodes_) {
            if (marked.find(item.first) != marked.end()) continue;
            auto keepAlive = item.second.owner.lock();
            if (keepAlive) unreachable.emplace_back(std::move(keepAlive), item.second.breakCycles);
        }

        for (const auto& item : unreachable) {
            item.second(item.first.get());
        }
        pruneExpired();
        pendingRegistrations_.store(0, std::memory_order_relaxed);
        nextThreshold_.store(
            std::max(kMinCollectNodes, nodes_.size() * 2),
            std::memory_order_relaxed
        );
    }

    void traceNodeValues(
        const std::shared_ptr<void>& root,
        const std::function<void(const ZeusType::UniversalVariant&)>& callback
    ) {
        if (!root || !callback) return;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        traceNodes({ root }, callback);
    }

    void traceRoots(
        const std::function<void(const ZeusType::UniversalVariant&)>& callback
    ) {
        if (!callback) return;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const ZeusType::UniversalVariant* value : snapshotRoots()) {
            callback(*value);
        }

        std::vector<std::shared_ptr<void>> roots;
        roots.reserve(nodeRoots_.size());
        for (const auto& root : nodeRoots_) {
            const auto entry = nodes_.find(root.first);
            if (entry == nodes_.end()) continue;
            if (auto keepAlive = entry->second.owner.lock()) roots.push_back(std::move(keepAlive));
        }
        traceNodes(roots, callback);
    }

    std::size_t liveNodeCount() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pruneExpired();
        return nodes_.size();
    }

    void enterAllocation() {
        allocationDepth_.fetch_add(1, std::memory_order_relaxed);
    }

    // Runs at the outermost allocation exit, where every node built during the
    // scope is already rooted and can be marked.
    void leaveAllocation() {
        if (allocationDepth_.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        if (pendingRegistrations_.load(std::memory_order_relaxed) < kCollectInterval) return;
        collect({});
    }

private:
    struct MarkState {
        Collector& owner;
        std::unordered_set<const void*>& marked;
        Visitor& visitor;
        std::function<void(const ZeusType::UniversalVariant&)>& markVariant;
        std::function<void(const ZeusValue&)>& markValue;
        std::function<void(const std::shared_ptr<void>&)>& markNode;
    };

    struct TraceState {
        Collector& owner;
        const std::function<void(const ZeusType::UniversalVariant&)>& callback;
        std::unordered_set<const void*> visited;
        std::vector<std::shared_ptr<void>> pending;
    };

    static void traceValue(const ZeusValue& value,
        const std::function<void(const ZeusType::UniversalVariant&)>& callback) {
        if (value.flags != ZEUS_VALUE_FLAG_OWNS_BOX || value.payload == 0) return;
        const auto* box = reinterpret_cast<const ZeusBox*>(
            static_cast<uintptr_t>(value.payload));
        callback(box->v);
    }

    static void traceValueCallback(void* context, const ZeusValue* value) {
        auto* state = static_cast<TraceState*>(context);
        if (value) traceValue(*value, state->callback);
    }

    static void traceVariantCallback(void* context, const ZeusType::UniversalVariant* value) {
        auto* state = static_cast<TraceState*>(context);
        if (value) state->callback(*value);
    }

    static void traceNodeCallback(void* context, const std::shared_ptr<void>* node) {
        auto* state = static_cast<TraceState*>(context);
        if (!node || !*node || !state->visited.insert(node->get()).second) return;
        state->pending.push_back(*node);
    }

    void traceNodes(
        const std::vector<std::shared_ptr<void>>& roots,
        const std::function<void(const ZeusType::UniversalVariant&)>& callback
    ) {
        TraceState state{ *this, callback, {}, {} };
        Visitor visitor{ &state, traceValueCallback, traceVariantCallback, traceNodeCallback };
        for (const auto& root : roots) {
            if (root && state.visited.insert(root.get()).second) state.pending.push_back(root);
        }
        for (std::size_t cursor = 0; cursor < state.pending.size(); ++cursor) {
            const auto& node = state.pending[cursor];
            const auto entry = nodes_.find(node.get());
            if (entry == nodes_.end()) continue;
            const auto keepAlive = entry->second.owner.lock();
            if (keepAlive) entry->second.trace(keepAlive.get(), visitor);
        }
    }

    void pruneExpired() {
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            if (it->second.owner.expired()) it = nodes_.erase(it);
            else ++it;
        }
        for (auto it = nodeRoots_.begin(); it != nodeRoots_.end();) {
            if (nodes_.find(it->first) == nodes_.end()) it = nodeRoots_.erase(it);
            else ++it;
        }
    }

    std::recursive_mutex mutex_;
    RootLink* rootHead_ = nullptr;
    std::unordered_map<const void*, NodeEntry> nodes_;
    std::unordered_map<const void*, std::size_t> nodeRoots_;
    std::atomic<std::size_t> nextThreshold_{ kMinCollectNodes };
    std::atomic<std::size_t> pendingRegistrations_{ 0 };
    std::atomic<std::size_t> allocationDepth_{ 0 };
};

Collector& collectorInstance() {
    static Collector instance;
    return instance;
}

} // namespace

void RootRegistry::attach(RootLink& link, const ZeusType::UniversalVariant* target) {
    if (!target) return;
    Collector& collector = collectorInstance();
    std::lock_guard<std::recursive_mutex> lock(collector.mutex());
    link.target_ = target;
    if (link.attached_) return;
    link.attached_ = true;
    link.next_ = collector.rootHead();
    collector.rootHead() = &link;
}

void RootRegistry::retract(RootLink& link) {
    Collector& collector = collectorInstance();
    std::lock_guard<std::recursive_mutex> lock(collector.mutex());
    if (!link.attached_) return;
    RootLink** slot = &collector.rootHead();
    while (*slot) {
        if (*slot == &link) {
            *slot = link.next_;
            break;
        }
        slot = &(*slot)->next_;
    }
    link.next_ = nullptr;
    link.target_ = nullptr;
    link.attached_ = false;
}

RootLink::~RootLink() noexcept {
    retract();
}

std::vector<const ZeusType::UniversalVariant*> RootRegistry::snapshot() {
    Collector& collector = collectorInstance();
    std::lock_guard<std::recursive_mutex> lock(collector.mutex());
    std::vector<const ZeusType::UniversalVariant*> roots;
    for (const RootLink* link = collector.rootHead(); link; link = link->next_) {
        if (link->attached_ && link->target_) roots.push_back(link->target_);
    }
    return roots;
}

void RootLink::attach(const ZeusType::UniversalVariant* target) {
    RootRegistry::attach(*this, target);
}

void RootLink::retract() noexcept {
    RootRegistry::retract(*this);
}

void registerSharedNode(
    const std::shared_ptr<void>& node,
    TraceFunction trace,
    BreakCyclesFunction breakCycles
) {
    collectorInstance().registerNode(node, trace, breakCycles);
}

void rootNode(const std::shared_ptr<void>& node) { collectorInstance().rootNode(node); }
void unrootNode(const std::shared_ptr<void>& node) { collectorInstance().unrootNode(node); }

void addPermanentRootVariant(const ZeusType::UniversalVariant* value) {
    if (!value) return;
    auto* link = new RootLink();
    link->attach(value);
}

AllocationScope::AllocationScope() noexcept {
    collectorInstance().enterAllocation();
}

AllocationScope::~AllocationScope() {
    collectorInstance().leaveAllocation();
}

std::recursive_mutex& graphMutex() { return collectorInstance().mutex(); }

void traceNodeValues(
    const std::shared_ptr<void>& root,
    const std::function<void(const ZeusType::UniversalVariant&)>& visitor
) {
    collectorInstance().traceNodeValues(root, visitor);
}

void traceRoots(const std::function<void(const ZeusType::UniversalVariant&)>& visitor) {
    collectorInstance().traceRoots(visitor);
}

void collect(const std::vector<std::shared_ptr<void>>& additionalRoots) {
    collectorInstance().collect(additionalRoots);
}

std::size_t liveNodeCount() { return collectorInstance().liveNodeCount(); }

} // namespace ZeusGC

extern "C" void zeus_gc_collect(void) {
    ZeusGC::collect();
}

extern "C" uint64_t zeus_gc_live_node_count(void) {
    return static_cast<uint64_t>(ZeusGC::liveNodeCount());
}
