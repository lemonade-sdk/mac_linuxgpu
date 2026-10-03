#include "shim_init_diagnostics.h"
#include <cassert>
#include <functional>
#include <string>
#include <vector>
using namespace mac_hsa;

struct CachedRPC {
    std::function<uint32_t(uint64_t, uint64_t *, uint32_t *)> log;
    unsigned probes = 0, chunks = 0;
    bool closed = false;
    uint32_t probeError = 0;
    uint32_t query(const uint64_t *input, uint32_t inputs, uint64_t *output, uint32_t *count) {
        assert(!closed);
        if (inputs == 1 && input[0] == kCachedProbeStatus) {
            assert(!probes++ && *count == 5);
            if (probeError) return probeError;
            output[0] = 1; output[1] = 0; output[2] = uint64_t(-12ll);
            output[3] = 2; output[4] = 0xa4;
            return 0;
        }
        assert(inputs == 2 && input[0] == kCachedKernelLog && *count == 16);
        chunks++;
        return log(input[1], output, count);
    }
    std::string dump() {
        std::string output;
        dumpCachedShimDiagnostics([&](auto... args) { return query(args...); },
            [&](std::string_view bytes) { output.append(bytes); });
        closed = true;
        return output;
    }
};

static void reply(uint64_t end, uint64_t start, std::string_view bytes,
                   uint64_t *output, uint32_t *count) {
    assert(bytes.size() <= 104);
    output[0] = end; output[1] = start + bytes.size(); output[2] = bytes.size();
    for (size_t i = 0; i < bytes.size(); i++)
        output[3 + i / 8] |= uint64_t(static_cast<unsigned char>(bytes[i])) << ((i % 8) * 8);
    *count = 3 + (bytes.size() + 7) / 8;
}

int main() {
    {CachedRPC rpc;
     rpc.log = [](uint64_t cursor, uint64_t *out, uint32_t *count) {
         if (!cursor) reply(150, 0, std::string(104, 'A'), out, count);
         else { assert(cursor == 104); reply(190, 104, std::string(46, 'B') + std::string(40, 'C'), out, count); }
         return 0;
     };
     auto text = rpc.dump();
     assert(rpc.probes == 1 && rpc.chunks == 2);
     assert(text.find("probe_result=-12") != std::string::npos);
     assert(text.find(std::string(104, 'A') + std::string(46, 'B')) != std::string::npos);
     assert(text.find(std::string(40, 'C')) == std::string::npos);
     assert(text.find("next_cursor=150") != std::string::npos);}
    {CachedRPC rpc;
     rpc.log = [](uint64_t cursor, uint64_t *out, uint32_t *count) {
         assert(!cursor); reply(20000, 19996, "tail", out, count); return 0;
     };
     auto text = rpc.dump();
     assert(text.find("cursor clamped: 0 -> 19996") != std::string::npos);
     assert(text.find("tail\nmac_linuxgpu: cached kernel log end") != std::string::npos);}
    {CachedRPC rpc;
     rpc.log = [](uint64_t cursor, uint64_t *out, uint32_t *count) {
         if (!cursor) reply(150, 0, std::string(104, 'A'), out, count);
         else { assert(cursor == 104); reply(20000, 19996, "tail", out, count); }
         return 0;
     };
     auto text = rpc.dump();
     assert(text.find("tail") == std::string::npos && text.find("next_cursor=150") != std::string::npos);}
    {CachedRPC rpc;
     rpc.log = [](uint64_t cursor, uint64_t *out, uint32_t *count) {
         if (!cursor) reply(150, 0, std::string(104, 'A'), out, count);
         else reply(50, 50, "", out, count);
         return 0;
     };
     assert(rpc.dump().find("next_cursor=50") != std::string::npos);}
    {CachedRPC rpc;
     rpc.log = [](uint64_t cursor, uint64_t *out, uint32_t *count) {
         assert(!cursor); reply(0, 0, "", out, count); return 0;
     };
     assert(rpc.dump().find("next_cursor=0") != std::string::npos && rpc.chunks == 1);}
    for (unsigned bad = 0; bad < 7; bad++) {
        CachedRPC rpc;
        rpc.log = [bad](uint64_t, uint64_t *out, uint32_t *count) {
            reply(10, 0, "12345678", out, count);
            switch (bad) {
            case 0: *count = 2; break;
            case 1: *count = 17; break;
            case 2: out[2] = 105; break;
            case 3: out[2] = 9; break;
            case 4: out[1] = 3; break;
            case 5: out[0] = 7; break;
            case 6: out[2] = 0; break;
            }
            return 0;
        };
        auto text = rpc.dump();
        assert(text.find("unavailable") != std::string::npos || text.find("invalid cached log") != std::string::npos);
        assert(rpc.chunks == 1);
    }
    {CachedRPC rpc; rpc.probeError = 0xe00002d9;
     rpc.log = [](uint64_t, uint64_t *, uint32_t *) { return 0xe00002d9u; };
     auto text = rpc.dump();
     assert(text.find("cached probe unavailable") != std::string::npos);
     assert(text.find("cached log unavailable") != std::string::npos && rpc.chunks == 1);}
    {CachedRPC rpc;
     rpc.log = [](uint64_t cursor, uint64_t *out, uint32_t *count) {
         reply(UINT64_MAX, cursor, "X", out, count); return 0;
     };
     assert(rpc.dump().find("exceeded bounded snapshot") != std::string::npos && rpc.chunks == 512);}
    {std::string text;
     const ShimInitializationFailure failure{ShimInitializationOperation::Scalar, 9, 0, false,
        HSA_STATUS_ERROR_OUT_OF_RESOURCES};
     reportShimInitializationFailure(failure, HSA_STATUS_ERROR,
        [&](std::string_view bytes) { text.append(bytes); });
     assert(text.find("selector=9") != std::string::npos && text.find("first_hsa=") != std::string::npos);}
    std::puts("Cached shim diagnostics: bounded fake RPC, wrap, append, malformed replies, and failure context passed");
}
