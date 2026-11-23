// Emitter member definitions: elision of redundant class liveness checks; included inside class Emitter.
//
// Every member access on a managed class value is guarded by
// zeus_class_ref_require_live, which takes a lock and a hash lookup so that a
// use after `del` still raises. Two optimizations reduce these checks:
// 1. A class-typed function parameter is an SSA value that never changes, and
//    the callee holds a reference to it from the prologue. When nothing in
//    the reachable program can destroy a class object behind that reference,
//    one check in the prologue proves liveness for the whole body (including
//    non-reassigned loop phis).
// 2. Within a single basic block (between control-flow merges and potentially
//    destroying calls), the same SSA pointer only needs to be checked once.

    // True when the reachable program contains an operation that can destroy a
    // class object while a reference to it is still held: an explicit `del`
    // (static or dynamic) or a raw-memory class slot store/clear, which destroys
    // the object that previously occupied the slot.
    bool programMayDestroyClassObjects() const {
        if (!programChunks) return true;
        for (size_t ci = 0; ci < programChunks->size(); ++ci) {
            const auto& chunk = (*programChunks)[ci];
            const bool isFunction =
                !chunk.empty() && chunk.front().op == VMOperator::FUNC_BEGIN;
            if (isFunction &&
                reachableFunctionChunks.find(ci) == reachableFunctionChunks.end()) {
                continue;
            }
            for (const auto& instruction : chunk) {
                switch (instruction.op) {
                    case VMOperator::DELETE_VAR:
                        return true;
                    case VMOperator::LLVM_FADDR:
                    case VMOperator::LLVM_FSTORE:
                        // Only the slot forms hold class objects; plain int,
                        // string and bytes accesses cannot destroy one.
                        if (instruction.args.size() > 1 &&
                            std::holds_alternative<std::string>(instruction.args[1])) {
                            const std::string& mode =
                                std::get<std::string>(instruction.args[1]);
                            if (mode == "slot" || mode == "slot_clear") return true;
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        return false;
    }

    // Decide once per program, after reachability is known.
    void prescanLivenessElision() {
        elideParameterLivenessChecks = !mcuTarget && !programMayDestroyClassObjects();
    }

    // A parameter verified in the prologue (or a pointer already verified in
    // the current straight-line block) needs no further check.
    bool needsLivenessCheck(const NativeValue& value) const {
        if (!isManagedClassValue(value)) return false;
        if (verifiedLiveParameters.find(value.value) !=
            verifiedLiveParameters.end()) {
            return false;
        }
        return blockVerifiedLiveValues.find(value.value) ==
            blockVerifiedLiveValues.end();
    }

    template <typename EmitLine>
    void emitRequireLiveIfNeeded(const NativeValue& value, EmitLine&& emitLine) {
        if (!needsLivenessCheck(value)) return;
        emitLine("call void @zeus_class_ref_require_live(ptr " +
                 value.value + ")");
        blockVerifiedLiveValues.insert(value.value);
    }

    // Prologue hook: verify a retained class parameter once and remember it.
    // Must run right after the retain, where an exception unwinds the same way
    // the first in-body check would have.
    template <typename EmitLine>
    void verifyParameterLiveOnce(const std::string& parameterValue, EmitLine&& emitLine) {
        if (!elideParameterLivenessChecks) return;
        emitLine("call void @zeus_class_ref_require_live(ptr " + parameterValue + ")");
        verifiedLiveParameters.insert(parameterValue);
    }

    // If a merged local phi (at an if/else join or a loop header where the
    // local is never written in the loop) only receives verified parameters,
    // the phi value itself is guaranteed live throughout its scope.
    void propagateVerifiedPhi(
        const std::string& phi,
        const std::vector<const NativeValue*>& incomingValues,
        bool writtenInLoop
    ) {
        if (!elideParameterLivenessChecks || writtenInLoop ||
            incomingValues.empty()) {
            return;
        }
        for (const NativeValue* value : incomingValues) {
            if (value == nullptr ||
                verifiedLiveParameters.find(value->value) ==
                    verifiedLiveParameters.end()) {
                return;
            }
        }
        verifiedLiveParameters.insert(phi);
    }

    // Scope guard: a function body starts with empty verified sets and the
    // enclosing function's sets are restored afterwards.
    struct LivenessScope {
        std::set<std::string>& paramTarget;
        std::set<std::string>& blockTarget;
        std::set<std::string> savedParam;
        std::set<std::string> savedBlock;
        LivenessScope(
            std::set<std::string>& paramSet,
            std::set<std::string>& blockSet
        )
            : paramTarget(paramSet),
              blockTarget(blockSet),
              savedParam(std::move(paramSet)),
              savedBlock(std::move(blockSet)) {
            paramTarget.clear();
            blockTarget.clear();
        }
        ~LivenessScope() {
            paramTarget = std::move(savedParam);
            blockTarget = std::move(savedBlock);
        }
    };
