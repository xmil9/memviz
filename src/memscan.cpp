#include "memscan.h"

#include <algorithm>

#include <tlhelp32.h>
#include <psapi.h>

namespace memscan
{

bool enableSeDebug()
{
   HANDLE token = nullptr;
   if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                         &token))
   {
      return false;
   }

   LUID luid{};
   bool ok = false;
   if (LookupPrivilegeValue(nullptr, SE_DEBUG_NAME, &luid))
   {
      TOKEN_PRIVILEGES tp{};
      tp.PrivilegeCount = 1;
      tp.Privileges[0].Luid = luid;
      tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
      if (AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr))
      {
         ok = (GetLastError() == ERROR_SUCCESS);
      }
   }
   CloseHandle(token);
   return ok;
}

std::vector<ProcessInfo> listProcesses()
{
   std::vector<ProcessInfo> out;
   HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
   if (snap == INVALID_HANDLE_VALUE)
   {
      return out;
   }

   PROCESSENTRY32W pe{};
   pe.dwSize = sizeof(pe);
   if (Process32FirstW(snap, &pe))
   {
      do
      {
         ProcessInfo pi;
         pi.pid = pe.th32ProcessID;
         pi.name = pe.szExeFile;
         out.push_back(std::move(pi));
      } while (Process32NextW(snap, &pe));
   }
   CloseHandle(snap);

   std::sort(out.begin(), out.end(),
             [](const ProcessInfo& a, const ProcessInfo& b)
             {
                int c = _wcsicmp(a.name.c_str(), b.name.c_str());
                if (c != 0)
                {
                   return c < 0;
                }
                return a.pid < b.pid;
             });
   return out;
}

std::wstring processName(DWORD pid)
{
   HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
   if (snap == INVALID_HANDLE_VALUE)
   {
      return L"";
   }
   std::wstring name;
   PROCESSENTRY32W pe{};
   pe.dwSize = sizeof(pe);
   if (Process32FirstW(snap, &pe))
   {
      do
      {
         if (pe.th32ProcessID == pid)
         {
            name = pe.szExeFile;
            break;
         }
      } while (Process32NextW(snap, &pe));
   }
   CloseHandle(snap);
   return name;
}

HANDLE openProcess(DWORD pid)
{
   HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
   if (!h)
   {
      // Fall back to limited query rights (still lets us walk regions on some
      // targets even if reads are denied).
      h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
   }
   return h;
}

void appAddressRange(uintptr_t& lo, uintptr_t& hi, uint32_t& allocGranularity)
{
   SYSTEM_INFO si{};
   GetSystemInfo(&si);
   lo = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
   hi = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
   allocGranularity = si.dwAllocationGranularity;
}

static RegionState toState(DWORD s)
{
   if (s == MEM_COMMIT)
   {
      return RegionState::Committed;
   }
   if (s == MEM_RESERVE)
   {
      return RegionState::Reserved;
   }
   return RegionState::Free;
}

std::vector<Region> enumRegions(HANDLE h, uintptr_t lo, uintptr_t hi)
{
   std::vector<Region> out;
   if (!h)
   {
      return out;
   }

   MEMORY_BASIC_INFORMATION mbi{};
   uintptr_t addr = lo;
   while (addr < hi)
   {
      SIZE_T got = VirtualQueryEx(h, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi));
      if (got == 0)
      {
         // Skip ahead a page on transient failures; bail if we can't advance.
         uintptr_t next = addr + 0x1000;
         if (next <= addr)
         {
            break;
         }
         addr = next;
         continue;
      }

      Region r;
      r.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
      r.size = mbi.RegionSize;
      r.state = toState(mbi.State);
      r.protect = (mbi.State == MEM_COMMIT) ? mbi.Protect : 0;
      r.type = mbi.Type;
      out.push_back(std::move(r));

      uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      if (next <= addr)
      {
         break; // guard against wrap / zero size
      }
      addr = next;
   }
   return out;
}

std::vector<ModuleInfo> enumModules(HANDLE h)
{
   std::vector<ModuleInfo> out;
   if (!h)
   {
      return out;
   }

   std::vector<HMODULE> mods(1024);
   DWORD needed = 0;
   if (!EnumProcessModulesEx(h, mods.data(),
                             static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed,
                             LIST_MODULES_ALL))
   {
      return out;
   }
   DWORD count = needed / sizeof(HMODULE);
   if (count > mods.size())
   {
      mods.resize(count);
      if (!EnumProcessModulesEx(h, mods.data(),
                                static_cast<DWORD>(mods.size() * sizeof(HMODULE)),
                                &needed, LIST_MODULES_ALL))
      {
         return out;
      }
      count = needed / sizeof(HMODULE);
   }

   for (DWORD i = 0; i < count; ++i)
   {
      MODULEINFO mi{};
      if (!GetModuleInformation(h, mods[i], &mi, sizeof(mi)))
      {
         continue;
      }
      wchar_t name[MAX_PATH] = {};
      ModuleInfo info;
      info.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
      info.size = mi.SizeOfImage;
      if (GetModuleBaseNameW(h, mods[i], name, MAX_PATH) > 0)
      {
         info.name = name;
      }
      out.push_back(std::move(info));
   }

   std::sort(out.begin(), out.end(),
             [](const ModuleInfo& a, const ModuleInfo& b) { return a.base < b.base; });
   return out;
}

void annotateModules(std::vector<Region>& regions, const std::vector<ModuleInfo>& modules)
{
   if (modules.empty())
   {
      return;
   }
   for (auto& r : regions)
   {
      if (r.type != MEM_IMAGE)
      {
         continue;
      }
      // Binary search: last module whose base <= region base.
      auto it = std::upper_bound(modules.begin(), modules.end(), r.base,
                                 [](uintptr_t addr, const ModuleInfo& m)
                                 { return addr < m.base; });
      if (it == modules.begin())
      {
         continue;
      }
      --it;
      if (r.base >= it->base && r.base < it->base + it->size)
      {
         r.module = it->name;
      }
   }
}

size_t readMemory(HANDLE h, uintptr_t addr, void* out, size_t size)
{
   if (!h || size == 0)
   {
      return 0;
   }
   SIZE_T read = 0;
   if (ReadProcessMemory(h, reinterpret_cast<LPCVOID>(addr), out, size, &read))
   {
      return read;
   }
   // ReadProcessMemory can fail yet still report a partial read for the leading
   // valid pages.
   return read;
}

const char* stateStr(RegionState s)
{
   switch (s)
   {
   case RegionState::Committed:
      return "Committed";
   case RegionState::Reserved:
      return "Reserved";
   default:
      return "Free";
   }
}

std::string protectStr(DWORD protect)
{
   if (protect == 0)
   {
      return "---";
   }
   DWORD base = protect & 0xFF;
   std::string s;
   switch (base)
   {
   case PAGE_NOACCESS:
      s = "---";
      break;
   case PAGE_READONLY:
      s = "R--";
      break;
   case PAGE_READWRITE:
      s = "RW-";
      break;
   case PAGE_WRITECOPY:
      s = "RC-";
      break;
   case PAGE_EXECUTE:
      s = "--X";
      break;
   case PAGE_EXECUTE_READ:
      s = "R-X";
      break;
   case PAGE_EXECUTE_READWRITE:
      s = "RWX";
      break;
   case PAGE_EXECUTE_WRITECOPY:
      s = "RCX";
      break;
   default:
      s = "???";
      break;
   }
   if (protect & PAGE_GUARD)
   {
      s += " G";
   }
   if (protect & PAGE_NOCACHE)
   {
      s += " N";
   }
   return s;
}

std::string typeStr(DWORD type)
{
   switch (type)
   {
   case MEM_IMAGE:
      return "Image";
   case MEM_MAPPED:
      return "Mapped";
   case MEM_PRIVATE:
      return "Private";
   default:
      return "-";
   }
}

} // namespace memscan
