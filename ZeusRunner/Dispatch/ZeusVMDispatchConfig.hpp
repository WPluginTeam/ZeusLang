#pragma once

// This boundary is required even with whole-program optimization. Combining
// opcode temporaries produces a huge native frame for every interpreted call.
#if defined(_MSC_VER)
#define ZEUS_VM_NOINLINE __declspec(noinline)
#else
#define ZEUS_VM_NOINLINE __attribute__((noinline))
#endif
