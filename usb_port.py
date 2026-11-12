"""USB CDC I/O that never configures or toggles DTR/RTS.

pySerial.open() calls SetCommState with enabled modem control flags even when
the caller never uses setDTR/setRTS. The L2 USB Serial/JTAG must be opened
without those changes. Baud settings are irrelevant for this USB endpoint.
"""
import ctypes
from ctypes import wintypes

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                              ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel.CreateFileW.restype = wintypes.HANDLE
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.CloseHandle.restype = wintypes.BOOL
kernel.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                           ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel.ReadFile.restype = wintypes.BOOL
kernel.WriteFile.argtypes = kernel.ReadFile.argtypes
kernel.WriteFile.restype = wintypes.BOOL

class Timeouts(ctypes.Structure):
    _fields_ = [(name, wintypes.DWORD) for name in (
        "ReadIntervalTimeout", "ReadTotalTimeoutMultiplier", "ReadTotalTimeoutConstant",
        "WriteTotalTimeoutMultiplier", "WriteTotalTimeoutConstant")]

kernel.SetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.POINTER(Timeouts)]
kernel.SetCommTimeouts.restype = wintypes.BOOL

class UsbPort:
    def __init__(self, port="COM83", timeout=0.3):
        self.handle = kernel.CreateFileW("\\\\.\\" + port, 0xC0000000, 0, None, 3, 0, None)
        if self.handle == wintypes.HANDLE(-1).value:
            self.handle = None
            raise ctypes.WinError(ctypes.get_last_error())
        # These only control I/O wait times; there are no modem line writes.
        waits = Timeouts(0xFFFFFFFF, 0, max(1,int(timeout*1000)), 0, 2000)
        if not kernel.SetCommTimeouts(self.handle, ctypes.byref(waits)):
            error = ctypes.WinError(ctypes.get_last_error())
            self.close()
            raise error
    def read(self, count=2048):
        data = ctypes.create_string_buffer(count)
        done = wintypes.DWORD()
        if not kernel.ReadFile(self.handle, data, count, ctypes.byref(done), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return data.raw[:done.value]
    def write(self, data):
        done = wintypes.DWORD()
        buf = ctypes.create_string_buffer(data)
        if not kernel.WriteFile(self.handle, buf, len(data), ctypes.byref(done), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return done.value
    def close(self):
        if self.handle is not None:
            kernel.CloseHandle(self.handle)
            self.handle = None
    def __enter__(self): return self
    def __exit__(self, *_): self.close()
