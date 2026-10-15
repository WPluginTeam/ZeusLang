#include "ZeusThreadABI.hpp"
#include "ZeusDynamicABI.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {

struct ScopedZeusValue {
    ZeusValue value{};

    ScopedZeusValue() {
        zeus_value_init_nil(&value);
    }

    ~ScopedZeusValue() {
        zeus_value_release(&value);
    }

    ScopedZeusValue(const ScopedZeusValue&) = delete;
    ScopedZeusValue& operator=(const ScopedZeusValue&) = delete;
};

struct AotClosureTask {
    ZeusValue closure;
    std::vector<ZeusValue> arguments;

    AotClosureTask() {
        zeus_value_init_nil(&closure);
    }

    ~AotClosureTask() {
        zeus_value_release(&closure);
        for (auto& argument : arguments) zeus_value_release(&argument);
    }
};

void runAotClosure(void* rawContext) {
    auto* context = static_cast<AotClosureTask*>(rawContext);
    ZeusValue result;
    zeus_value_init_nil(&result);
    try {
        std::vector<const ZeusValue*> argumentPointers;
        argumentPointers.reserve(context->arguments.size());
        for (const auto& argument : context->arguments) {
            argumentPointers.push_back(&argument);
        }
        zeus_closure_call(
            &result,
            &context->closure,
            static_cast<int32_t>(argumentPointers.size()),
            argumentPointers.empty() ? nullptr : argumentPointers.data(),
            nullptr
        );
    } catch (...) {
        zeus_value_release(&result);
        throw;
    }
    zeus_value_release(&result);
}

bool copyThreadArguments(AotClosureTask& context, const ZeusValue* arguments) {
    if (!arguments || arguments->tag == ZEUS_VALUE_TAG_NIL) return true;
    if (!zeus_value_is_array(arguments)) return false;

    ScopedZeusValue length;
    zeus_value_array_length(&length.value, arguments);
    if (length.value.tag != ZEUS_VALUE_TAG_INT64 || length.value.int64_payload < 0 ||
        length.value.int64_payload > std::numeric_limits<int32_t>::max()) {
        return false;
    }

    const int64_t argumentCount = length.value.int64_payload;
    context.arguments.reserve(static_cast<size_t>(argumentCount));
    for (int64_t indexValue = 0; indexValue < argumentCount; ++indexValue) {
        ScopedZeusValue index;
        zeus_value_init_i64(&index.value, indexValue);
        context.arguments.emplace_back();
        zeus_value_init_nil(&context.arguments.back());
        zeus_value_array_load(
            &context.arguments.back(), arguments, &index.value);
    }
    return true;
}

void destroyAotClosure(void* rawContext) {
    delete static_cast<AotClosureTask*>(rawContext);
}

} // namespace

extern "C" bool zeus_thread_start_closure(
    int64_t thread_id,
    const ZeusValue* closure,
    const ZeusValue* arguments
) {
    if (!closure) return false;
    std::unique_ptr<AotClosureTask> context(new (std::nothrow) AotClosureTask{});
    if (!context) return false;

    try {
        zeus_value_clone(&context->closure, closure);
        if (!copyThreadArguments(*context, arguments)) return false;
    } catch (...) {
        return false;
    }

    AotClosureTask* rawContext = context.get();
    if (!zeus_thread_start_task(
            thread_id, runAotClosure, rawContext, destroyAotClosure)) {
        return false;
    }
    context.release();
    return true;
}
