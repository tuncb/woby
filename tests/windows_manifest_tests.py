"""Check the long-path setting in the built executables, not the source XML."""
import ctypes
from ctypes import wintypes
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
kernel32.LoadLibraryExW.restype = wintypes.HMODULE
kernel32.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
kernel32.FindResourceW.restype = wintypes.HANDLE
kernel32.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
kernel32.SizeofResource.restype = wintypes.DWORD
kernel32.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
kernel32.LoadResource.restype = wintypes.HANDLE
kernel32.LockResource.argtypes = [wintypes.HANDLE]
kernel32.LockResource.restype = ctypes.c_void_p
kernel32.FreeLibrary.argtypes = [wintypes.HMODULE]
kernel32.FreeLibrary.restype = wintypes.BOOL

for executable in sys.argv[1:]:
    # Read resources without executing code or resolving the executable's DLLs.
    module = kernel32.LoadLibraryExW(str(Path(executable).resolve()), None, 0x2 | 0x20)
    assert module, (executable, ctypes.get_last_error())
    try:
        resource = kernel32.FindResourceW(module, 1, 24)  # CREATEPROCESS_MANIFEST_RESOURCE_ID, RT_MANIFEST
        assert resource, (executable, ctypes.get_last_error())
        size = kernel32.SizeofResource(module, resource)
        handle = kernel32.LoadResource(module, resource)
        data = kernel32.LockResource(handle)
        assert size and handle and data, (executable, ctypes.get_last_error())
        manifest = ET.fromstring(ctypes.string_at(data, size))
        setting = manifest.find("{urn:schemas-microsoft-com:asm.v3}application/"
                                "{urn:schemas-microsoft-com:asm.v3}windowsSettings/"
                                "{http://schemas.microsoft.com/SMI/2016/WindowsSettings}longPathAware")
        assert setting is not None and (setting.text or "").strip() == "true", executable
    finally:
        assert kernel32.FreeLibrary(module)
print("Embedded Windows long-path manifests passed.")
