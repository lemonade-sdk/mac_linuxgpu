#pragma once
#include "device_init.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

namespace mac_hsa {
constexpr uint64_t kCachedProbeStatus = 0x4c50524f;
constexpr uint64_t kCachedKernelLog = 0x4c4c4f47;

template <class Emit>
void reportShimInitializationFailure(const ShimInitializationFailure &failure,
                                     hsa_status_t returnedStatus, Emit emit) {
    const char *operation = "unknown";
    switch (failure.operation) {
    case ShimInitializationOperation::Scalar: operation = "scalar"; break;
    case ShimInitializationOperation::Validation: operation = "response validation"; break;
    case ShimInitializationOperation::ReserveHostWindow: operation = "reserve host window"; break;
    case ShimInitializationOperation::ReleaseHostWindow: operation = "release host window"; break;
    case ShimInitializationOperation::None: break;
    }
    char text[256];
    std::snprintf(text, sizeof(text),
        "mac_linuxgpu: initialization failed: operation=%s selector=%u tag=%#llx has_tag=%u first_hsa=%#x returned_hsa=%#x\n",
        operation, failure.selector, static_cast<unsigned long long>(failure.tag),
        unsigned(failure.hasTag), unsigned(failure.status), unsigned(returnedStatus));
    emit(std::string_view(text));
    if (failure.selector == 21 && failure.hasTag && failure.tag == kComputeSessionQueryTag)
        emit(std::string_view("mac_linuxgpu: the driver gave this client no compute session "
                              "(QueryInfo tag 12); the driver log says why\n"));
}

/* query uses an existing connection and only QueryInfo's two cached tags.
 * The supplied callback must never open a client or initialize a session. */
template <class Query, class Emit>
void dumpCachedShimDiagnostics(Query query, Emit emit) {
    char text[256];
    std::array<uint64_t, 5> probe{};
    uint32_t count = probe.size();
    uint64_t tag = kCachedProbeStatus;
    uint32_t result = query(&tag, 1, probe.data(), &count);
    if (result || count != probe.size()) {
        std::snprintf(text, sizeof(text),
            "mac_linuxgpu: cached probe unavailable: IOReturn=%#x count=%u\n", result, count);
    } else {
        std::snprintf(text, sizeof(text),
            "mac_linuxgpu: cached probe: attempted=%llu modules_running=%llu probe_result=%lld transport_fault=%llu fault_offset=%#llx\n",
            static_cast<unsigned long long>(probe[0]), static_cast<unsigned long long>(probe[1]),
            static_cast<long long>(static_cast<int64_t>(probe[2])),
            static_cast<unsigned long long>(probe[3]), static_cast<unsigned long long>(probe[4]));
    }
    emit(std::string_view(text));
    emit(std::string_view("mac_linuxgpu: cached kernel log begin\n"));
    uint64_t cursor = 0, target = 0;
    bool first = true;
    for (unsigned chunk = 0; chunk < 512; ++chunk) {
        const uint64_t input[] = {kCachedKernelLog, cursor};
        std::array<uint64_t, 16> output{};
        count = output.size();
        result = query(input, 2, output.data(), &count);
        if (result || count < 3 || count > output.size()) {
            std::snprintf(text, sizeof(text),
                "\nmac_linuxgpu: cached log unavailable: IOReturn=%#x count=%u\n", result, count);
            emit(std::string_view(text));
            return;
        }
        const uint64_t end = output[0], next = output[1], bytes = output[2];
        if (bytes > (count - 3) * sizeof(uint64_t) || bytes > 104 || bytes > next) {
            emit(std::string_view("\nmac_linuxgpu: invalid cached log byte count\n"));
            return;
        }
        const uint64_t start = next - bytes;
        if (end < next || (start < cursor && cursor <= end) || (!bytes && next != end)) {
            emit(std::string_view("\nmac_linuxgpu: invalid cached log cursor\n"));
            return;
        }
        if (first) { target = end; first = false; }
        if (start != cursor) {
            std::snprintf(text, sizeof(text),
                "\nmac_linuxgpu: cached log cursor clamped: %llu -> %llu\n",
                static_cast<unsigned long long>(cursor), static_cast<unsigned long long>(start));
            emit(std::string_view(text));
        }
        const size_t copied = start < target ? std::min<uint64_t>(bytes, target - start) : 0;
        char data[104];
        for (size_t i = 0; i < copied; ++i)
            data[i] = static_cast<char>(output[3 + i / 8] >> ((i % 8) * 8));
        if (copied) emit(std::string_view(data, copied));
        cursor = std::min(next, target);
        if (next >= target || !bytes) {
            std::snprintf(text, sizeof(text),
                "\nmac_linuxgpu: cached kernel log end; next_cursor=%llu\n",
                static_cast<unsigned long long>(cursor));
            emit(std::string_view(text));
            return;
        }
    }
    emit(std::string_view("\nmac_linuxgpu: cached log exceeded bounded snapshot\n"));
}
}
