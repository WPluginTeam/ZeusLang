#pragma once

#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>
#include "../ZeusRuntimeLib/ZeusGC.hpp"

namespace ZeusVMRoots {
    // Dense VM object ids make a stamped set cheaper than allocating a map
    // node for every retain barrier. Reset preserves storage between opcodes.
    class ObjectRoots {
        public:
            bool add(const ZeusType::ObjectHandle& handle) {
                if (handle.id < 0) return false;
                const size_t index = static_cast<size_t>(handle.id);
                if (index >= seen.size()) seen.resize(index + 1);
                auto& slot = seen[index];
                if (slot.stamp == stamp && slot.generation == handle.generation) return false;
                slot = Seen{stamp, handle.generation};
                values.push_back(handle);
                return true;
            }
            void clear() {
                values.clear();
                if (++stamp == 0) {
                    for (auto& slot : seen) slot.stamp = 0;
                    stamp = 1;
                }
            }
            auto begin() const { return values.begin(); }
            auto end() const { return values.end(); }
        private:
            struct Seen { uint64_t stamp = 0; int generation = 0; };
            uint64_t stamp = 1;
            std::vector<Seen> seen;
            std::vector<ZeusType::ObjectHandle> values;
    };

    // Values and table identities only; no collector dereferences another VM.
    struct Snapshot {
        const void* objectTable = nullptr;
        ObjectRoots objects;
        std::set<std::pair<const void*, int>> closures;
        std::map<const void*, std::weak_ptr<void>> nativeNodes;

        void reset(const void* table) {
            objectTable = table;
            objects.clear();
            closures.clear();
            nativeNodes.clear();
        }
        void merge(const Snapshot& incoming) {
            for (const auto& handle : incoming.objects) objects.add(handle);
            closures.insert(incoming.closures.begin(), incoming.closures.end());
            for (const auto& native : incoming.nativeNodes) nativeNodes[native.first] = native.second;
        }
    };

    class Registry {
        struct Domain {
            const void* table;
            size_t virtualMachines = 0;
            size_t pendingPeers = 0;
        };

        class PendingPeer {
            public:
                explicit PendingPeer(std::shared_ptr<Domain> domain)
                    : domain(std::move(domain)) { ++this->domain->pendingPeers; }
                ~PendingPeer() {
                    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                    --domain->pendingPeers;
                    retireDomainIfIdle(domain);
                }
            private:
                std::shared_ptr<Domain> domain;
        };

        public:
            static void syncDomain(const void* owner, const void* table) {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                syncDomainEntry(entries()[owner], table);
            }

            static bool hasPeers(const void* owner, const void* table) {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                auto& entry = entries().at(owner);
                syncDomainEntry(entry, table);
                return hasPeers(entry);
            }

            static std::shared_ptr<void> reservePeer(const void* owner, const void* table) {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                auto& entry = entries().at(owner);
                syncDomainEntry(entry, table);
                return std::make_shared<PendingPeer>(entry.domain);
            }

            static Snapshot takeSnapshotStorage(const void* owner) {
                // The owner holds graphMutex until publish restores this entry.
                return std::move(entries().at(owner).current);
            }

            static void publish(const void* owner, Snapshot snapshot) {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                auto& entry = entries()[owner];
                syncDomainEntry(entry, snapshot.objectTable);
                entry.current = std::move(snapshot);
            }

            static void pinObject(const void* owner, const void* table,
                                  const ZeusType::ObjectHandle& handle) {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                auto& entry = entries().at(owner);
                syncDomainEntry(entry, table);
                if (hasPeers(entry)) entry.current.objects.add(handle);
                if (entry.depth) entry.active[entry.depth - 1].objects.add(handle);
            }

            static void pin(const void* owner, Snapshot snapshot) {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                auto& entry = entries().at(owner);
                syncDomainEntry(entry, snapshot.objectTable);
                if (hasPeers(entry)) entry.current.merge(snapshot);
                if (entry.depth) entry.active[entry.depth - 1].merge(snapshot);
            }

            static void beginInstruction(const void* owner) {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                auto& entry = entries().at(owner);
                if (entry.depth == entry.active.size()) entry.active.emplace_back();
                entry.active[entry.depth++].reset(entry.current.objectTable);
            }

            static void endInstruction(const void* owner) {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                auto& entry = entries().at(owner);
                entry.active[--entry.depth].reset(entry.current.objectTable);
            }

            static void remove(const void* owner) {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                const auto found = entries().find(owner);
                if (found == entries().end()) return;
                if (found->second.domain) {
                    --found->second.domain->virtualMachines;
                    retireDomainIfIdle(found->second.domain);
                }
                entries().erase(found);
            }

            template <typename Visitor>
            static void visit(const void* objectTable, const void* excludedOwner, Visitor&& visitor) {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                for (const auto& entry : entries()) {
                    if (entry.first != excludedOwner && entry.second.current.objectTable == objectTable) {
                        visitor(entry.second.current);
                    }
                    for (size_t index = 0; index < entry.second.depth; ++index) {
                        const auto& active = entry.second.active[index];
                        if (active.objectTable == objectTable) visitor(active);
                    }
                }
            }

        private:
            struct Entry {
                std::shared_ptr<Domain> domain;
                Snapshot current;
                std::vector<Snapshot> active;
                size_t depth = 0;
            };
            static bool hasPeers(const Entry& entry) {
                return entry.domain->virtualMachines > 1 || entry.domain->pendingPeers != 0;
            }

            static void syncDomainEntry(Entry& entry, const void* table) {
                if (entry.domain && entry.domain->table == table) return;
                if (entry.depth != 0) {
                    throw std::logic_error("VM object table cannot change during instruction execution");
                }
                auto& existing = domains()[table];
                auto domain = existing.lock();
                if (!domain) {
                    domain = std::make_shared<Domain>(Domain{table});
                    existing = domain;
                }
                if (entry.domain) {
                    --entry.domain->virtualMachines;
                    retireDomainIfIdle(entry.domain);
                }
                entry.domain = std::move(domain);
                ++entry.domain->virtualMachines;
                entry.current.reset(table);
            }

            static void retireDomainIfIdle(const std::shared_ptr<Domain>& domain) {
                if (domain->virtualMachines != 0 || domain->pendingPeers != 0) return;
                const auto found = domains().find(domain->table);
                if (found != domains().end() && found->second.lock() == domain) {
                    domains().erase(found);
                }
            }

            static std::map<const void*, std::weak_ptr<Domain>>& domains() {
                static std::map<const void*, std::weak_ptr<Domain>> domains;
                return domains;
            }

            static std::map<const void*, Entry>& entries() {
                static std::map<const void*, Entry> registry;
                return registry;
            }
    };

    class InstructionScope {
        public:
            explicit InstructionScope(const void* owner) : owner(owner) {
                Registry::beginInstruction(owner);
            }
            ~InstructionScope() { Registry::endInstruction(owner); }
            InstructionScope(const InstructionScope&) = delete;
            InstructionScope& operator=(const InstructionScope&) = delete;
        private:
            const void* owner;
    };
}
