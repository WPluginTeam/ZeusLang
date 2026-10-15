#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../ZeusRuntimeLib/ZeusGC.hpp"
#include "../ZeusRuntimeLib/ZeusObjectPayload.hpp"

namespace ZeusVMCopy {

    // Initial references keep every new node alive while the graph is assembled.
    // Stored fields and the returned value acquire their own references.
    template <typename VMType>
    class Context {
        public:
            explicit Context(VMType& vm) : vm(vm) {}

            ZeusType::UniversalVariant copy(
                const ZeusType::UniversalVariant& value, bool skipHook = false
            ) {
                const auto* handle = std::get_if<ZeusType::ObjectHandle>(&value);
                if (!handle) {
                    ZeusGC::retainVariantDeep(vm, value);
                    return value;
                }

                const auto identity = std::make_pair(handle->id, handle->generation);
                const auto found = copies.find(identity);
                if (found != copies.end()) {
                    ZeusGC::retainVariantDeep(vm, found->second);
                    return found->second;
                }

                const auto* source = vm.getValidObject(*handle);
                if (!source) throw std::runtime_error("class copy received a stale object");
                const auto* type = vm.getClass(source->classId);
                if (!type) throw std::runtime_error("class copy received an invalid class");
                const auto hook = type->operatorMethods.find("[@=]");
                if (!skipHook && hook != type->operatorMethods.end()) {
                    auto result = invokeHook(*handle, hook->second);
                    try {
                        copies.emplace(identity, result);
                        hookResults.push_back(result);
                    } catch (...) {
                        copies.erase(identity);
                        ZeusGC::releaseVariantDeep(vm, result);
                        throw;
                    }
                    ZeusGC::retainVariantDeep(vm, result);
                    return result;
                }

                // Allocating another node can relocate the VM object vector.
                // Snapshot source fields and type metadata before allocating.
                const auto fields = ZeusType::snapshotObjectFields(*source, *type);
                const auto referenceTypes = type->explicitFieldTypes;
                ZeusType::ObjectInstance instance;
                instance.classId = source->classId;
                instance.flags = source->flags;
                const auto target = vm.allocObject(std::move(instance));
                try {
                    allocated.push_back(target);
                } catch (...) {
                    vm.getValidObject(target)->destructorCalled = true;
                    vm.releaseObject(target);
                    throw;
                }
                copies.emplace(identity, target);

                for (const auto& field : fields) {
                    const auto fieldType = referenceTypes.find(field.first);
                    const bool isReference = fieldType != referenceTypes.end() &&
                        !fieldType->second.empty() && fieldType->second.front() == '&';
                    auto fieldValue = field.second;
                    if (isReference) ZeusGC::retainVariantDeep(vm, fieldValue);
                    else fieldValue = copy(field.second);
                    try {
                        auto* targetObject = vm.getValidObject(target);
                        if (!targetObject) {
                            throw std::runtime_error("class copy target became stale");
                        }
                        const auto* targetType = vm.getClass(targetObject->classId);
                        if (!targetType) {
                            throw std::runtime_error("class copy target has an invalid class");
                        }
                        ZeusType::writeObjectField(
                            *targetObject, *targetType, field.first, fieldValue);
                    } catch (...) {
                        ZeusGC::releaseVariantDeep(vm, fieldValue);
                        throw;
                    }
                }
                vm.retainObject(target);
                return target;
            }

            bool beginHook(
                const ZeusType::UniversalVariant& value,
                ZeusType::ObjectHandle& sourceHandle,
                int& prototypeId
            ) {
                const auto* handle = std::get_if<ZeusType::ObjectHandle>(&value);
                if (!handle) return false;

                const auto identity = std::make_pair(handle->id, handle->generation);
                if (copies.find(identity) != copies.end()) return false;

                const auto* source = vm.getValidObject(*handle);
                if (!source) throw std::runtime_error("class copy received a stale object");
                const auto* type = vm.getClass(source->classId);
                if (!type) throw std::runtime_error("class copy received an invalid class");
                const auto hook = type->operatorMethods.find("[@=]");
                if (hook == type->operatorMethods.end()) return false;

                const auto* prototype = vm.getPrototype(hook->second);
                if (!prototype || prototype->paramCount != 1) {
                    throw std::runtime_error("operator[@=] requires no explicit arguments");
                }

                pendingHookIdentity = identity;
                sourceHandle = *handle;
                prototypeId = hook->second;
                return true;
            }

            ZeusType::UniversalVariant finishHook(
                const ZeusType::UniversalVariant& value
            ) {
                if (!pendingHookIdentity) {
                    throw std::runtime_error("class copy hook continuation is missing");
                }
                if (!std::holds_alternative<ZeusType::ObjectHandle>(value)) {
                    throw std::runtime_error(
                        "operator[@=] must return a managed class value");
                }

                const auto identity = *pendingHookIdentity;
                ZeusGC::retainVariantDeep(vm, value);
                try {
                    const auto inserted = copies.emplace(identity, value);
                    if (!inserted.second) {
                        throw std::runtime_error("class copy hook completed more than once");
                    }
                    hookResults.push_back(value);
                } catch (...) {
                    copies.erase(identity);
                    ZeusGC::releaseVariantDeep(vm, value);
                    throw;
                }

                ZeusGC::retainVariantDeep(vm, value);
                pendingHookIdentity.reset();
                completed = true;
                return value;
            }

            void finish() {
                completed = true;
            }

            ~Context() noexcept {
                if (!completed) {
                    // Incomplete objects never run user finalizers. Drop graph
                    // edges first, while all nodes still have their initial hold.
                    for (const auto& handle : allocated) {
                        auto* object = vm.getValidObject(handle);
                        if (object) object->destructorCalled = true;
                    }
                    for (const auto& handle : allocated) {
                        auto* object = vm.getValidObject(handle);
                        if (!object) continue;
                        auto extensions = std::move(object->extensionValues);
                        auto declaredFields = std::move(object->declaredFields);
                        object->flags.clear();
                        for (const auto& field : extensions) {
                            ZeusGC::releaseVariantDeep(vm, field.second);
                        }
                        if (declaredFields) {
                            for (std::size_t index = 0;
                                 index < declaredFields->size(); ++index) {
                                const auto value = declaredFields->read(index);
                                ZeusGC::releaseVariantDeep(vm, value);
                            }
                        }
                    }
                }
                for (const auto& handle : allocated) vm.releaseObject(handle);
                for (const auto& result : hookResults) ZeusGC::releaseVariantDeep(vm, result);
            }

        private:
            VMType& vm;
            std::map<std::pair<int, int>, ZeusType::UniversalVariant> copies;
            std::vector<ZeusType::ObjectHandle> allocated;
            std::vector<ZeusType::UniversalVariant> hookResults;
            std::optional<std::pair<int, int>> pendingHookIdentity;
            bool completed = false;

            ZeusType::UniversalVariant invokeHook(
                const ZeusType::ObjectHandle& source, int prototypeId
            ) {
                const auto* prototype = vm.getPrototype(prototypeId);
                if (!prototype || prototype->paramCount != 1) {
                    throw std::runtime_error("operator[@=] requires no explicit arguments");
                }
                const size_t stackBase = vm.stack.size();
                typename VMType::Closure closure;
                closure.protoId = prototypeId;
                closure.boundThis = source;
                vm.retainObject(source);
                const int closureId = vm.allocClosure(std::move(closure));
                vm.retainClosure(closureId); // the stack owns an additional hold
                vm.stack.push_back(ZeusType::ClosureHandle{closureId});
                const bool success = vm.executeClosureSync(closureId, 0);
                vm.releaseClosure(closureId);
                if (!success || vm.stack.size() != stackBase + 1) {
                    vm.shrinkStackWithRelease(stackBase);
                    throw std::runtime_error("operator[@=] did not complete its copy result");
                }
                auto result = vm.popStackItem();
                if (!std::holds_alternative<ZeusType::ObjectHandle>(result.value)) {
                    vm.dropStackItem(result);
                    throw std::runtime_error(
                        "operator[@=] must return a managed class value");
                }
                if (!result.owner) ZeusGC::retainVariantDeep(vm, result.value);
                return result.value;
            }
    };

    template <typename VMType>
    ZeusType::UniversalVariant copy(
        VMType& vm, const ZeusType::UniversalVariant& source, bool skipHook = false
    ) {
        Context<VMType> context(vm);
        auto result = context.copy(source, skipHook);
        context.finish();
        return result;
    }
}
