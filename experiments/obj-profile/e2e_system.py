"""Low-rate Windows diagnostic counters; values are not CPU hardware PMCs."""
import ctypes
from ctypes import wintypes


class FormattedValue(ctypes.Structure):
    _fields_ = [("status", wintypes.DWORD), ("value", ctypes.c_double)]


class FormattedItem(ctypes.Structure):
    _fields_ = [("name", wintypes.LPWSTR), ("counter", FormattedValue)]


def open_counters():
    pdh = ctypes.WinDLL("pdh")
    pdh.PdhOpenQueryW.argtypes = [wintypes.LPCWSTR, ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p)]
    pdh.PdhAddEnglishCounterW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p)]
    pdh.PdhCollectQueryData.argtypes = [ctypes.c_void_p]
    pdh.PdhGetFormattedCounterValue.argtypes = [ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(FormattedValue)]
    pdh.PdhGetFormattedCounterArrayW.argtypes = [ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    pdh.PdhCloseQuery.argtypes = [ctypes.c_void_p]
    for name in ("PdhOpenQueryW", "PdhAddEnglishCounterW", "PdhCollectQueryData", "PdhGetFormattedCounterValue", "PdhGetFormattedCounterArrayW", "PdhCloseQuery"):
        getattr(pdh, name).restype = wintypes.DWORD
    handle = ctypes.c_void_p()
    status = pdh.PdhOpenQueryW(None, 0, ctypes.byref(handle))
    if status:
        return {"error": f"PdhOpenQueryW: 0x{status:08x}"}
    paths = {
        "cpu_frequency_MHz": r"\Processor Information(_Total)\Processor Frequency",
        "cpu_performance_percent": r"\Processor Information(_Total)\% Processor Performance",
        "cpu_utility_percent": r"\Processor Information(_Total)\% Processor Utility",
        "cpu_queue_length": r"\System\Processor Queue Length",
        "disk_read_bytes_per_second": r"\PhysicalDisk(_Total)\Disk Read Bytes/sec",
        "disk_write_bytes_per_second": r"\PhysicalDisk(_Total)\Disk Write Bytes/sec",
        "disk_read_latency_seconds": r"\PhysicalDisk(_Total)\Avg. Disk sec/Read",
        "memory_page_reads_per_second": r"\Memory\Page Reads/sec",
        "memory_pages_input_per_second": r"\Memory\Pages Input/sec",
        "gpu_engines": r"\GPU Engine(*)\Utilization Percentage",
        "gpu_process_dedicated_bytes": r"\GPU Process Memory(*)\Dedicated Usage",
        "gpu_process_shared_bytes": r"\GPU Process Memory(*)\Shared Usage",
    }
    counters, errors = {}, {}
    for name, path in paths.items():
        counter = ctypes.c_void_p()
        status = pdh.PdhAddEnglishCounterW(handle, path, 0, ctypes.byref(counter))
        if status:
            errors[name] = f"0x{status:08x}"
        else:
            counters[name] = counter
    pdh.PdhCollectQueryData(handle)
    return {"pdh": pdh, "query": handle, "counters": counters, "errors": errors, "paths": paths}


def sample_counters(runtime, pid):
    if "pdh" not in runtime:
        return {"error": runtime.get("error")}
    pdh = runtime["pdh"]
    status = pdh.PdhCollectQueryData(runtime["query"])
    if status:
        return {"error": f"PdhCollectQueryData: 0x{status:08x}"}
    result = {}
    for name, handle in runtime["counters"].items():
        if name.startswith("gpu_"):
            size, count = wintypes.DWORD(), wintypes.DWORD()
            status = pdh.PdhGetFormattedCounterArrayW(handle, 0x200 | 0x8000, ctypes.byref(size), ctypes.byref(count), None)
            if status != 0x800007D2 or not size.value:
                result[name] = {"error": f"0x{status:08x}"}; continue
            buffer = ctypes.create_string_buffer(size.value)
            status = pdh.PdhGetFormattedCounterArrayW(handle, 0x200 | 0x8000, ctypes.byref(size), ctypes.byref(count), buffer)
            if status:
                result[name] = {"error": f"0x{status:08x}"}; continue
            values = ctypes.cast(buffer, ctypes.POINTER(FormattedItem))
            result[name] = [{"instance": values[i].name, "value": values[i].counter.value}
                            for i in range(count.value) if values[i].counter.status in (0, 1)
                            and (f"pid_{pid}_" in values[i].name or (name == "gpu_engines" and values[i].counter.value > 0))]
        else:
            value, counter_type = FormattedValue(), wintypes.DWORD()
            status = pdh.PdhGetFormattedCounterValue(handle, 0x200 | 0x8000, ctypes.byref(counter_type), ctypes.byref(value))
            result[name] = value.value if not status and value.status in (0, 1) else {"error": f"0x{status or value.status:08x}"}
    return result


def close_counters(runtime):
    if "pdh" in runtime:
        runtime["pdh"].PdhCloseQuery(runtime["query"])
