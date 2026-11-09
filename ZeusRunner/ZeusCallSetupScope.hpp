#pragma once

#include <utility>

namespace ZeusVMDetail {

// Environment ownership transfers to CallFrame only after its insertion.
// Before then every exit must release the provisional callee environment.
template <typename Rollback>
class CallSetupScope {
    private:
        Rollback action;
        bool active = true;

    public:
        explicit CallSetupScope(Rollback rollback)
            : action(std::move(rollback)) {}

        CallSetupScope(const CallSetupScope&) = delete;
        CallSetupScope& operator=(const CallSetupScope&) = delete;

        ~CallSetupScope() {
            rollback();
        }

        void rollback() {
            if (!active) return;
            active = false;
            action();
        }

        void commit() noexcept {
            active = false;
        }
};

} // namespace ZeusVMDetail
