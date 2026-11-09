// VM member definitions. Only the owning thread reads its execution vectors.
    size_t instructionsUntilRootSnapshot = 0;

    const void* objectTableIdentity() const {
        return sharedObjects ? static_cast<const void*>(sharedObjects)
                             : static_cast<const void*>(&objects);
    }

    class RootSnapshotBuilder {
        public:
            explicit RootSnapshotBuilder(VM& owner, bool traverseObjects = true,
                                         bool traverseNodes = true,
                                         ZeusVMRoots::Snapshot storage = {})
                : snapshot(std::move(storage)), vm(owner),
                  traverseObjects(traverseObjects), traverseNodes(traverseNodes) {
                snapshot.reset(vm.objectTableIdentity());
            }

            void value(const ZeusType::UniversalVariant& source) {
                if (const auto* handle = std::get_if<ZeusType::ObjectHandle>(&source)) {
                    const auto* object = vm.getValidObject(*handle);
                    if (!object || !snapshot.objects.add(*handle)) return;
                    if (!traverseObjects) return;
                    for (const auto& field : vm.snapshotObjectFields(*object)) value(field.second);
                    return;
                }
                if (const auto* handle = std::get_if<ZeusType::ClosureHandle>(&source)) {
                    const auto* closure = vm.getClosure(handle->id);
                    if (!closure || closure->isFreed()) return;
                    const void* table = &vm.closures;
                    if (vm.sharedClosures && handle->id >= 0 &&
                        static_cast<size_t>(handle->id) < vm.sharedClosures->size() &&
                        closure == &(*vm.sharedClosures)[static_cast<size_t>(handle->id)]) {
                        table = vm.sharedClosures;
                    }
                    if (!snapshot.closures.emplace(table, handle->id).second) return;
                    for (auto* cell : closure->upvalues) if (cell) value(cell->readValue());
                    if (closure->boundThis) value(*closure->boundThis);
                    return;
                }
                if (const auto* native = std::get_if<ZeusType::NativeClosureHandle>(&source)) {
                    node(native->state);
                    return;
                }
                if (const auto* named = std::get_if<std::shared_ptr<ZeusType::NamedArg>>(&source)) {
                    if (*named && namedArguments.insert(named->get()).second) value((*named)->value);
                    return;
                }
                if (const auto* reference = std::get_if<ZeusType::ReferenceHandle>(&source)) {
                    if (reference->typeName == "cell" && reference->address &&
                        references.insert(reference->address).second) {
                        value(static_cast<const Cell*>(reference->address)->readValue());
                    } else if (reference->typeName == "shared" && reference->address &&
                               references.insert(reference->address).second) {
                        auto cell = ZeusShared::getGlobalHeap().findByAddress(
                            static_cast<const ZeusShared::SharedCell*>(reference->address),
                            reference->generation);
                        if (cell) value(cell->read());
                    }
                    return;
                }
                const auto* variant = std::get_if<ZeusType::Variant>(&source);
                if (!variant) return;
                if (const auto* array = std::get_if<ZeusType::Array>(variant)) node(array->impl);
                else if (const auto* object = std::get_if<ZeusType::Object>(variant)) node(object->impl);
            }

            void cells(const std::vector<Cell*>& sources) {
                for (auto* cell : sources) if (cell) value(cell->readValue());
            }

            ZeusVMRoots::Snapshot snapshot;

        private:
            VM& vm;
            bool traverseObjects;
            bool traverseNodes;
            std::set<const void*> references;
            std::set<const void*> namedArguments;

            void node(const std::shared_ptr<void>& source) {
                if (!source || !snapshot.nativeNodes.emplace(source.get(), source).second) return;
                if (traverseNodes) {
                    ZeusGC::traceNodeValues(source, [&](const ZeusType::UniversalVariant& child) {
                        value(child);
                    });
                }
            }
    };

    void syncVMRootDomain() {
        ZeusVMRoots::Registry::syncDomain(this, objectTableIdentity());
    }

    std::shared_ptr<void> reserveVMRootPeer() {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        auto lease = ZeusVMRoots::Registry::reservePeer(this, objectTableIdentity());
        publishVMRoots(true);
        return lease;
    }

    void publishVMRoots(bool force = false) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        instructionsUntilRootSnapshot = 64;
        if (!force && !ZeusVMRoots::Registry::hasPeers(this, objectTableIdentity())) return;
        RootSnapshotBuilder roots(*this, true, true,
            ZeusVMRoots::Registry::takeSnapshotStorage(this));
        for (size_t index = 0; index < stack.size(); ++index) roots.value(stack[index]);
        roots.cells(global);
        roots.cells(local);
        roots.cells(upvalue);
        for (const auto& frame : callFrames) {
            roots.cells(frame.savedLocal);
            roots.cells(frame.savedUpvalue);
            if (frame.constructorResult) roots.value(*frame.constructorResult);
        }
        roots.value(currentException);
        ZeusVMRoots::Registry::publish(this, std::move(roots.snapshot));
        // Bound stale roots during long pure-Zeus loops, without copying the
        // entire root graph on every instruction. Native calls publish sooner.
        instructionsUntilRootSnapshot = 64;
    }

    void pinVMObjectRoot(const ZeusType::ObjectHandle& handle) {
        ZeusVMRoots::Registry::pinObject(this, objectTableIdentity(), handle);
    }

    void pinVMClosureRoot(int closureId) {
        RootSnapshotBuilder roots(*this, false);
        roots.value(ZeusType::ClosureHandle{closureId});
        ZeusVMRoots::Registry::pin(this, std::move(roots.snapshot));
    }

    void pinVMTemporaryRoot(const ZeusType::UniversalVariant& value) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        RootSnapshotBuilder roots(*this, false, false);
        roots.value(value);
        ZeusVMRoots::Registry::pin(this, std::move(roots.snapshot));
    }

    void pinVMFrameRoots(const CallFrame& frame) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        RootSnapshotBuilder roots(*this, false);
        roots.cells(frame.savedLocal);
        roots.cells(frame.savedUpvalue);
        if (frame.constructorResult) roots.value(*frame.constructorResult);
        ZeusVMRoots::Registry::pin(this, std::move(roots.snapshot));
    }
