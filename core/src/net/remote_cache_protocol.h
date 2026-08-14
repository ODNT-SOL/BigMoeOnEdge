// Protocol for the F3 dual-node RoCE expert cache.
//
// Phase 3a uses plain TCP sockets over the RoCE subnet for bootstrapping and correctness
// validation. Phase 3b will swap the transport to RDMA (rdma_cm + ibverbs) while keeping the
// same message framing.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bmoe::net {

// A unique expert tensor identifier.
struct ExpertKey {
    uint32_t layer = 0;
    uint32_t expert = 0;
    uint32_t proj = 0; // 0 = gate, 1 = up, 2 = down (recipe-specific)

    bool operator==(const ExpertKey & o) const {
        return layer == o.layer && expert == o.expert && proj == o.proj;
    }
};

// Message types.
enum class MsgType : uint8_t {
    Invalid = 0,
    GetExpert = 1,   // client -> server
    ExpertData = 2,  // server -> client
    NotFound = 3,    // server -> client: model or expert unavailable
    Error = 4,       // server -> client: generic error
};

// Fixed-size request header. The model_id follows as a length-prefixed UTF-8 string.
struct GetExpertRequest {
    ExpertKey key;
    uint32_t model_id_len = 0; // bytes of model_id that follow this header
};

// Response header. For ExpertData a byte payload follows; for Error a message string follows.
struct ExpertResponse {
    MsgType type = MsgType::Invalid;
    uint64_t payload_len = 0;
};

// Simple little-endian wire encoding.
inline uint64_t htole64(uint64_t x) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return x;
#else
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) {
        r |= ((x >> (i * 8)) & 0xff) << ((7 - i) * 8);
    }
    return r;
#endif
}
inline uint32_t htole32(uint32_t x) { /* similar */ return x; }
inline uint16_t htole16(uint16_t x) { return x; }

inline void encode_u64(uint8_t * p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t((v >> (i * 8)) & 0xff);
}
inline uint64_t decode_u64(const uint8_t * p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (i * 8);
    return v;
}
inline void encode_u32(uint8_t * p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t((v >> (i * 8)) & 0xff);
}
inline uint32_t decode_u32(const uint8_t * p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (i * 8);
    return v;
}

} // namespace bmoe::net
