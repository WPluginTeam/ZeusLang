#pragma once

#include "ZeusABI.hpp"

// C ABI wrappers used by ZeusBuiltins/string.zeu. Each argument and the
// result use the dynamic ZeusValue representation, keeping the builtin
// semantics shared with the VM implementation.
extern "C" {

void zeus_string_builtin_str(ZeusValue* out, const ZeusValue* value);
void zeus_string_builtin_type(ZeusValue* out, const ZeusValue* value);
void zeus_string_builtin_len(ZeusValue* out, const ZeusValue* value);
void zeus_string_get(ZeusValue* out, const ZeusValue* value, const ZeusValue* index);
void zeus_string_slice(ZeusValue* out, const ZeusValue* value, const ZeusValue* start, const ZeusValue* end);
void zeus_string_index(ZeusValue* out, const ZeusValue* value, const ZeusValue* substring);
void zeus_string_rindex(ZeusValue* out, const ZeusValue* value, const ZeusValue* substring);
void zeus_string_replace(ZeusValue* out, const ZeusValue* value, const ZeusValue* oldValue, const ZeusValue* replacement);
void zeus_string_split(ZeusValue* out, const ZeusValue* value, const ZeusValue* separator);
void zeus_string_startswith(ZeusValue* out, const ZeusValue* value, const ZeusValue* prefix);
void zeus_string_endswith(ZeusValue* out, const ZeusValue* value, const ZeusValue* suffix);
void zeus_string_lower(ZeusValue* out, const ZeusValue* value);
void zeus_string_upper(ZeusValue* out, const ZeusValue* value);
void zeus_string_count(ZeusValue* out, const ZeusValue* value, const ZeusValue* substring);

}
