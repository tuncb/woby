"""Read a Windows process address map without changing its memory or working set."""
from collections import defaultdict
import ctypes
import mmap
import struct
import time


MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_FREE = 0x10000
MEM_PRIVATE = 0x20000
MEM_MAPPED = 0x40000
MEM_IMAGE = 0x1000000
TYPE_NAMES = {MEM_PRIVATE: "private", MEM_MAPPED: "mapped", MEM_IMAGE: "image"}
# Explicit Windows x64 layout. This avoids host-dependent DWORD/alignment sizes.
MBI64 = struct.Struct("<QQI4xQIII4x")


def summarize_regions(regions):
    totals = defaultdict(int)
    allocations = {}
    for region in regions:
        state, kind, size = region["state"], region["type"], region["size"]
        if state == MEM_FREE:
            continue
        name = TYPE_NAMES.get(kind, f"type_{kind:x}")
        status = "committed" if state == MEM_COMMIT else "reserved" if state == MEM_RESERVE else f"state_{state:x}"
        totals[f"{name}_{status}_bytes"] += size
        base = region["allocation_base"]
        allocation = allocations.setdefault(base, {"base": base, "committed_bytes": 0,
            "reserved_bytes": 0, "region_count": 0, "types": set(), "protections": set(), "mapped_file": ""})
        allocation["region_count"] += 1
        allocation["types"].add(name)
        if state == MEM_COMMIT:
            allocation["committed_bytes"] += size
            allocation["protections"].add(region["protect"])
        elif state == MEM_RESERVE:
            allocation["reserved_bytes"] += size
        if region.get("mapped_file"):
            allocation["mapped_file"] = region["mapped_file"]
    for allocation in allocations.values():
        allocation["types"] = sorted(allocation["types"])
        allocation["protections"] = sorted(allocation["protections"])
    return {"totals": dict(totals), "allocations": sorted(allocations.values(),
            key=lambda row: row["committed_bytes"], reverse=True), "region_count": len(regions)}


def query_region_pages(query, handle, region):
    page_size = mmap.PAGESIZE
    pages = region['size'] // page_size
    result = {'queried_pages': pages, 'valid_pages': 0, 'shared_valid_pages': 0, 'large_valid_pages': 0}
    for begin in range(0, pages, 32768):
        count = min(32768, pages-begin)
        entries = (ctypes.c_size_t * (2*count))()
        for index in range(count):
            entries[2*index] = region['base'] + (begin+index)*page_size
        if not query(handle, entries, ctypes.sizeof(entries)):
            raise ctypes.WinError(ctypes.get_last_error())
        for index in range(count):
            flags = entries[2*index+1]
            if flags & 1:
                result['valid_pages'] += 1
                result['shared_valid_pages'] += bool(flags & (1 << 15))
                result['large_valid_pages'] += bool(flags & (1 << 23))
    return result


def read_process_map(pid, resident=False):
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        raise RuntimeError("The memory-map probe requires a Windows x64 Python process")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel.CloseHandle.restype = ctypes.c_int
    kernel.VirtualQueryEx.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t)
    kernel.VirtualQueryEx.restype = ctypes.c_size_t
    psapi.GetMappedFileNameW.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32)
    psapi.GetMappedFileNameW.restype = ctypes.c_uint32
    psapi.QueryWorkingSetEx.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32)
    psapi.QueryWorkingSetEx.restype = ctypes.c_int
    handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    started = time.perf_counter()
    regions, names = [], {}
    address = 0
    buffer = ctypes.create_string_buffer(MBI64.size)
    try:
        while True:
            size = kernel.VirtualQueryEx(handle, ctypes.c_void_p(address), buffer, len(buffer))
            if not size:
                error = ctypes.get_last_error()
                # The query above the maximum user address returns INVALID_PARAMETER.
                if error == 87 and regions:
                    break
                raise ctypes.WinError(error)
            if size != MBI64.size:
                raise RuntimeError(f"Unexpected MEMORY_BASIC_INFORMATION size: {size}")
            base, allocation, allocation_protect, count, state, protect, kind = MBI64.unpack(buffer.raw)
            if not count or base + count <= address:
                raise RuntimeError("Memory enumeration did not advance")
            region = {"base": base, "allocation_base": allocation, "size": count,
                      "state": state, "type": kind, "protect": protect,
                      "allocation_protect": allocation_protect}
            if state == MEM_COMMIT and kind in (MEM_IMAGE, MEM_MAPPED):
                if allocation not in names:
                    filename = ctypes.create_unicode_buffer(32768)
                    names[allocation] = filename.value if psapi.GetMappedFileNameW(
                        handle, ctypes.c_void_p(base), filename, len(filename)) else ""
                region["mapped_file"] = names[allocation]
            regions.append(region)
            address = base + count
        summary = summarize_regions(regions)
        if resident:
            allocations = {row['base']: row for row in summary['allocations']}
            totals = defaultdict(int)
            by_protection = defaultdict(lambda: defaultdict(int))
            for region in regions:
                allocation = allocations.get(region['allocation_base'])
                if region['state'] != MEM_COMMIT or not allocation or allocation['committed_bytes'] < 1024*1024:
                    continue
                pages = query_region_pages(psapi.QueryWorkingSetEx, handle, region)
                for key, count in pages.items():
                    allocation[key] = allocation.get(key, 0) + count
                    totals[key] += count
                    by_protection[str(region['protect'])][key] += count
            summary['page_query'] = {'page_bytes': mmap.PAGESIZE, 'minimum_allocation_bytes': 1024*1024,
                                    'totals': dict(totals), 'by_protection': {key: dict(value) for key, value in by_protection.items()},
                                    'method': 'QueryWorkingSetEx for every committed page in allocations >=1 MiB. '
                                              'Valid is a CPU virtual-page property; it does not measure GPU VRAM residency. '
                                              'The query does not read, touch or lock target pages.'}
    finally:
        kernel.CloseHandle(handle)
    return {**summary, "pid": pid, "duration_ms": (time.perf_counter()-started)*1000,
            "regions": regions,
            "method": "VirtualQueryEx: page states/types grouped by AllocationBase; GetMappedFileNameW for section paths. "
                      "These are virtual regions, not C++ allocation identities or a physical-residency measurement. "
                      "Copy-on-write pages retain MEM_IMAGE/MEM_MAPPED types. Enumeration is not an atomic snapshot."}
