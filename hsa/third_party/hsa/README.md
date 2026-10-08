# HSA ABI headers

Unmodified core, AMD extension, AMD queue/signal layout, launch descriptor and loader extension headers from
https://github.com/iree-org/hsa-runtime-headers/tree/4285513114a70f7cf4830c89279c8cfa57b901bb
with upstream license notices and LICENSE.txt. This is the header revision
pinned by HRX System's MODULE.cmake.lock, which LSE builds against.

These declarations are an ABI reference and do not imply that every declared
function is implemented. No ROCr runtime implementation is vendored.
