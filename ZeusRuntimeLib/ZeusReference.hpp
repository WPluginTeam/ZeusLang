#pragma once

#include <cstdint>
#include <string>

namespace ZeusType {

    // Borrowed address used by dynamic execution. "cell" is a VM storage
    // location; native scalar references carry their declared Zeus type.
    // This handle never owns the referent.
    struct ReferenceHandle {
        void* address = nullptr;
        std::string typeName;
        std::uint64_t generation = 0;
    };

}
