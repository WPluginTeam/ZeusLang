#pragma once

#include "ZeusABI.hpp"

#ifdef __cplusplus
extern "C" {
#endif

ZeusNativeBytes zeus_crypto_random_bytes(int64_t length);
ZeusNativeBytes zeus_crypto_sha1(ZeusNativeBytes data);
ZeusNativeString zeus_crypto_base64_encode(ZeusNativeBytes data);
ZeusNativeBytes zeus_crypto_base64_decode(ZeusNativeString data);

ZeusNativeBytes zeus_websocket_mask(
    ZeusNativeBytes payload,
    ZeusNativeBytes mask
);
ZeusNativeBytes zeus_websocket_join(const ZeusValue* chunks);

ZeusNativeString zeus_protocol_last_error(void);

#ifdef __cplusplus
}
#endif
