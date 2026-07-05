#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

// Native Win32 access layer: process enumeration, attaching, walking the
// virtual address space, reading memory, and mapping image regions to modules.
namespace memscan {

struct ProcessInfo {
    DWORD        pid = 0;
    std::wstring name;
};

enum class RegionState { Free, Reserved, Committed };

struct Region {
    uintptr_t    base = 0;
    size_t       size = 0;
    RegionState  state = RegionState::Free;
    DWORD        protect = 0;  // raw PAGE_* flags (0 for free/reserved w/o access)
    DWORD        type = 0;     // MEM_PRIVATE / MEM_MAPPED / MEM_IMAGE
    std::wstring module;       // base name of owning module (image regions)
};

struct ModuleInfo {
    uintptr_t    base = 0;
    size_t       size = 0;
    std::wstring name;
};

// Try to enable SeDebugPrivilege for the current process (needs admin to fully
// take effect). Returns true if the adjustment call succeeded.
bool enableSeDebug();

// Snapshot of all running processes (pid + image name).
std::vector<ProcessInfo> listProcesses();

// Look up a single process image name by pid (empty on failure).
std::wstring processName(DWORD pid);

// Open a process for querying + reading memory. Returns NULL on failure;
// call GetLastError() for details.
HANDLE openProcess(DWORD pid);

// Usable user-mode application address range and allocation granularity.
void appAddressRange(uintptr_t& lo, uintptr_t& hi, uint32_t& allocGranularity);

// Walk the address space in [lo, hi) via VirtualQueryEx.
std::vector<Region> enumRegions(HANDLE h, uintptr_t lo, uintptr_t hi);

// Loaded modules of the target (for annotating MEM_IMAGE regions).
std::vector<ModuleInfo> enumModules(HANDLE h);

// Fill Region::module for image regions using the given module list.
void annotateModules(std::vector<Region>& regions,
                      const std::vector<ModuleInfo>& modules);

// Read up to `size` bytes at `addr` into `out`. Returns bytes actually read
// (0 on failure). Partial reads across unreadable pages are handled by the
// caller by chunking.
size_t readMemory(HANDLE h, uintptr_t addr, void* out, size_t size);

const char*  stateStr(RegionState s);
std::string  protectStr(DWORD protect);  // e.g. "RW-", "R-X", "---"
std::string  typeStr(DWORD type);        // "Private" / "Mapped" / "Image"

}  // namespace memscan
