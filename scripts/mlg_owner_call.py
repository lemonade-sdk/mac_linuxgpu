"""A MacLinuxGPU selector called the way the driver runs every call that can
sleep (dext/sources/session_state.h, "Calls that never sleep, and every other
call"): IOConnectCallAsyncMethod, the completion awaited on this thread, and
the structure output fetched with OWNER_RESULT. The Python twin of
host/owner_call.h, for the scripts.

    call = OwnerCall(connection)
    result, scalars, data = call(selector, scalars, data=b"", outputs=0, capacity=0)

result is the selector's IOReturn (0 on success)."""
import ctypes as c

OWNER_RESULT = 88       # session_state.h MLG_SELECTOR_OWNER_RESULT
OWNER_HEADER = 4        # MLG_OWNER_ASYNC_HEADER
OWNER_SCALARS = 12      # MLG_OWNER_ASYNC_SCALARS
PING = 0
CALLOUT_FUNC, CALLOUT_REFCON, CALLOUT_COUNT = 1, 2, 3   # IOKit's kIOAsyncCallout* indices
MACH_RCV_MSG, MACH_RCV_TIMEOUT, MACH_RCV_TIMED_OUT = 0x2, 0x100, 0x10004003
NOT_ATTACHED, IPC_ERROR = 0xe00002d8, 0xe00002c3

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
            immediate = (c.c_uint64 * OWNER_SCALARS)()
            count = c.c_uint(min(outputs, OWNER_SCALARS))
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
            while "status" not in done:
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
            if len(args) < OWNER_HEADER:
                return IPC_ERROR, [], b""
            code = args[1] & 0xffffffff
            values = args[OWNER_HEADER:OWNER_HEADER + args[2]][:outputs]
            blob = b""
            if code == 0 and args[3]:
                token = (c.c_uint64 * 1)(args[0])
                out = c.create_string_buffer(max(1, capacity))
                size = c.c_size_t(capacity)
                none = c.c_uint(0)
                fetched = io.IOConnectCallMethod(self.connection, OWNER_RESULT, token, 1, None, 0,
                                                 None, c.byref(none), out, c.byref(size))
                if fetched:
                    return fetched & 0xffffffff, [], b""
                blob = out.raw[:size.value]
            return code, values, blob
        finally:
            io.IONotificationPortDestroy(port)
