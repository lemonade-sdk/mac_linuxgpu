"""A MacLinuxGPU selector called the way the driver runs every call that can
sleep (dext/sources/session_state.h, "Calls that never sleep, and every other
call"): IOConnectCallAsyncMethod, the completion awaited on this thread, and
the structure output fetched with OWNER_RESULT. The Python twin of
host/owner_call.h, for the scripts.

    call = OwnerCall(connection)
    result, scalars, data = call(selector, scalars, data=b"", outputs=0, capacity=0)

result is the selector's IOReturn (0 on success)."""
import ctypes as c
import struct
import sys
import time

OWNER_RESULT = 88       # session_state.h MLG_SELECTOR_OWNER_RESULT
OWNER_HEADER = 4        # MLG_OWNER_ASYNC_HEADER
OWNER_SCALARS = 12      # MLG_OWNER_ASYNC_SCALARS
OWNER_MAX_SCALARS = 16  # MLG_OWNER_ASYNC_MAX_SCALARS
PING = 0
RUNTIME_BUILD = 43
SESSION_CALLS_ASYNC_BUILD = 243   # MLG_SESSION_CALLS_ASYNC_BUILD
CALLOUT_FUNC, CALLOUT_REFCON, CALLOUT_COUNT = 1, 2, 3   # IOKit's kIOAsyncCallout* indices
MACH_RCV_MSG, MACH_RCV_TIMEOUT, MACH_RCV_TIMED_OUT = 0x2, 0x100, 0x10004003
NOT_ATTACHED, IPC_ERROR = 0xe00002d9, 0xe00002bf  # kIOReturnNotAttached, kIOReturnIPCError
NO_SPACE = 0xe00002db  # kIOReturnNoSpace
TIMEOUT = 0xe00002d6    # kIOReturnTimeout
# host/owner_call.h's bounds: InitDevice, ShutdownGPU and DrmSelftest the longer.
BOUND_S, LONG_BOUND_S, LONG_SELECTORS = 120, 300, (9, 42, 82)

_CALLBACK = c.CFUNCTYPE(None, c.c_void_p, c.c_int, c.POINTER(c.c_void_p), c.c_uint32)


class OwnerCall:
    def __init__(self, connection):
        self.connection = connection
        io = self.io = c.CDLL("/System/Library/Frameworks/IOKit.framework/IOKit")
        system = self.system = c.CDLL("/usr/lib/libSystem.B.dylib")
        io.IONotificationPortCreate.argtypes = [c.c_uint]
        io.IONotificationPortCreate.restype = c.c_void_p
        io.IONotificationPortDestroy.argtypes = [c.c_void_p]
        io.IONotificationPortGetMachPort.argtypes = [c.c_void_p]
        io.IONotificationPortGetMachPort.restype = c.c_uint
        io.IODispatchCalloutFromMessage.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p]
        io.IOConnectCallAsyncMethod.argtypes = [
            c.c_uint, c.c_uint, c.c_uint, c.POINTER(c.c_uint64), c.c_uint,
            c.POINTER(c.c_uint64), c.c_uint, c.c_void_p, c.c_size_t,
            c.POINTER(c.c_uint64), c.POINTER(c.c_uint), c.c_void_p, c.POINTER(c.c_size_t)]
        io.IOConnectCallAsyncMethod.restype = c.c_int
        io.IOConnectCallMethod.argtypes = [
            c.c_uint, c.c_uint, c.POINTER(c.c_uint64), c.c_uint, c.c_void_p, c.c_size_t,
            c.POINTER(c.c_uint64), c.POINTER(c.c_uint), c.c_void_p, c.POINTER(c.c_size_t)]
        io.IOConnectCallMethod.restype = c.c_int
        system.mach_msg.argtypes = [c.c_void_p, c.c_int, c.c_uint, c.c_uint, c.c_uint,
                                    c.c_uint, c.c_uint]
        system.mach_msg.restype = c.c_int
        # An older driver answers such calls synchronously and never
        # completes them: refuse it now rather than wait forever.
        build = (c.c_uint64 * 4)()
        count = c.c_uint(4)
        result = io.IOConnectCallMethod(connection, RUNTIME_BUILD, None, 0, None, 0, build,
                                        c.byref(count), None, None)
        if result:
            raise RuntimeError(f"RuntimeBuild failed: {result & 0xffffffff:#x}")
        if count.value < 4 or build[3] < SESSION_CALLS_ASYNC_BUILD:
            raise RuntimeError("the installed MacLinuxGPU driver is older than build "
                               f"{SESSION_CALLS_ASYNC_BUILD}; install the matching driver")

    def __call__(self, selector, scalars, data=b"", outputs=0, capacity=0):
        io, system = self.io, self.system
        port = io.IONotificationPortCreate(0)
        if not port:
            raise MemoryError("no notification port")
        try:
            done = {}

            def completed(refcon, status, args, count):
                done["status"] = status
                done["args"] = [(args[i] or 0) for i in range(min(count, OWNER_HEADER + OWNER_SCALARS))]

            callback = _CALLBACK(completed)
            reference = (c.c_uint64 * 8)()
            reference[CALLOUT_FUNC] = c.cast(callback, c.c_void_p).value
            inputs = (c.c_uint64 * max(1, len(scalars)))(*scalars)
            immediate = (c.c_uint64 * OWNER_MAX_SCALARS)()
            want = min(outputs, OWNER_MAX_SCALARS)
            count = c.c_uint(want)
            room = c.c_size_t(capacity)
            probe = c.create_string_buffer(max(1, capacity))
            source = c.create_string_buffer(data, len(data)) if data else None
            result = io.IOConnectCallAsyncMethod(
                self.connection, selector, io.IONotificationPortGetMachPort(port), reference,
                CALLOUT_COUNT, inputs, len(scalars), source, len(data), immediate,
                c.byref(count), probe if capacity else None, c.byref(room) if capacity else None)
            if result:
                return result & 0xffffffff, [], b""
            message = c.create_string_buffer(4096)
            # As host/owner_call.h: never unbounded, even while Ping answers.
            bound = LONG_BOUND_S if selector in LONG_SELECTORS else BOUND_S
            deadline = time.monotonic() + bound
            while "status" not in done:
                if time.monotonic() >= deadline:
                    print(f"mac_linuxgpu: selector {selector}: no completion within {bound} s from a "
                          "driver that still answers Ping (kIOReturnTimeout)", file=sys.stderr)
                    return TIMEOUT, [], b""
                received = system.mach_msg(message, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 4096,
                                           io.IONotificationPortGetMachPort(port), 1000, 0)
                if received & 0xffffffff == MACH_RCV_TIMED_OUT:
                    pong = (c.c_uint64 * 1)()
                    n = c.c_uint(1)
                    if io.IOConnectCallMethod(self.connection, PING, None, 0, None, 0, pong,
                                              c.byref(n), None, None):
                        return NOT_ATTACHED, [], b""
                    continue
                if received:
                    return IPC_ERROR, [], b""
                io.IODispatchCalloutFromMessage(None, message, port)
            args = done["args"]
            if done["status"]:
                return done["status"] & 0xffffffff, [], b""
            if len(args) < OWNER_HEADER or args[2] > want:
                return IPC_ERROR, [], b""
            code = args[1] & 0xffffffff
            if code:
                return code, [], b""
            n = args[2]
            inline = n <= OWNER_SCALARS
            if inline and len(args) < OWNER_HEADER + n:
                return IPC_ERROR, [], b""
            values = args[OWNER_HEADER:OWNER_HEADER + n] if inline else []
            blob = b""
            # Kept until fetched (session_state.h): the scalars the completion
            # could not hold, then the structure output.
            kept = (0 if inline else 8 * n) + args[3]
            if kept:
                if args[3] > capacity:
                    return NO_SPACE, [], b""
                token = (c.c_uint64 * 1)(args[0])
                out = c.create_string_buffer(kept)
                size = c.c_size_t(kept)
                none = c.c_uint(0)
                fetched = io.IOConnectCallMethod(self.connection, OWNER_RESULT, token, 1, None, 0,
                                                 None, c.byref(none), out, c.byref(size))
                if fetched:
                    return fetched & 0xffffffff, [], b""
                if size.value != kept:
                    return IPC_ERROR, [], b""
                raw = out.raw[:kept]
                if not inline:
                    values = list(struct.unpack_from(f"<{n}Q", raw))
                    raw = raw[8 * n:]
                blob = raw
            return code, values, blob
        finally:
            io.IONotificationPortDestroy(port)
