#include "nulldomain.h"

// Include winsock2.h BEFORE windows.h to prevent winsock.h conflict
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <string>
#include <vector>

#include "config.h"
#include "detours.h"
#include "utils.h"

// Original function pointer for getaddrinfoW
typedef int (WSAAPI *GetaddrinfoWOriginal)(
    PCSTR nodename, PCSTR servname,
    const ADDRINFOW* hints, PADDRINFOW* result);

// Original function pointer for getaddrinfo
typedef int (WSAAPI *GetaddrinfoOriginal)(
    PCSTR nodename, PCSTR servname,
    const ADDRINFOA* hints, PADDRINFOA* result);

// Global flag to track if hooks are installed
static bool g_hooks_installed = false;

// Original function pointers
static GetaddrinfoWOriginal g_pfnGetaddrinfoW = nullptr;
static GetaddrinfoOriginal g_pfnGetaddrinfo = nullptr;

namespace {

// Check if a domain should be blocked
bool IsDomainBlocked(const std::string& hostname) {
  const auto& null_domains = config.GetNullDomains();
  
  for (const auto& domain : null_domains) {
    // Convert domain to string for comparison
    std::string domain_str(domain.begin(), domain.end());
    if (ContainsIgnoreCase(hostname, domain_str)) {
      return true;
    }
  }
  return false;
}

bool IsDomainBlocked(const std::wstring& hostname) {
  const auto& null_domains = config.GetNullDomains();
  
  for (const auto& domain : null_domains) {
    if (ContainsIgnoreCase(hostname, std::wstring(domain))) {
      return true;
    }
  }
  return false;
}

}  // namespace

// Hooked getaddrinfoW function (ANSI version)
int WSAAPI GetaddrinfoHooked(
    PCSTR nodename, PCSTR servname,
    const ADDRINFOA* hints, PADDRINFOA* result) {
  
  if (nodename != nullptr) {
    DebugLog(L"getaddrinfo called for: {}", nodename);
    if (IsDomainBlocked(std::string(nodename))) {
      DebugLog(L"Blocking DNS resolution for: {}", nodename);
      return EAI_FAIL;
    }
  }
  
  if (g_pfnGetaddrinfo != nullptr) {
    return g_pfnGetaddrinfo(nodename, servname, hints, result);
  }
  
  return EAI_FAIL;
}

// Hooked getaddrinfoW function (Wide version)
int WSAAPI GetaddrinfoWHooked(
    PCSTR nodename, PCSTR servname,
    const ADDRINFOW* hints, PADDRINFOW* result) {
  
  if (nodename != nullptr) {
    std::wstring hostname;
    hostname.assign(MultiByteToWideChar(CP_UTF8, 0, nodename, -1, nullptr, 0) - 1, 
                    MultiByteToWideChar(CP_UTF8, 0, nodename, -1, nullptr, 0));
    DebugLog(L"getaddrinfoW called for: {}", nodename);
    
    // Check if domain should be blocked
    if (g_pfnGetaddrinfoW != nullptr) {
      // Use the original to get the resolved hostname if needed
    }
    
    // Simple check: if nodename contains any blocked domain
    for (const auto& domain : config.GetNullDomains()) {
      std::string domain_str(domain.begin(), domain.end());
      if (ContainsIgnoreCase(std::string(nodename), domain_str)) {
        DebugLog(L"Blocking DNS resolution for: {}", nodename);
        return EAI_FAIL;
      }
    }
  }
  
  if (g_pfnGetaddrinfoW != nullptr) {
    return g_pfnGetaddrinfoW(nodename, servname, hints, result);
  }
  
  return EAI_FAIL;
}

void InitializeNullDomain() {
  if (g_hooks_installed) {
    return;
  }
  
  const auto& null_domains = config.GetNullDomains();
  if (null_domains.empty()) {
    DebugLog(L"No null domains configured");
    return;
  }
  
  DebugLog(L"Initializing NullDomain with {} domains", null_domains.size());
  for (const auto& domain : null_domains) {
    DebugLog(L"  - {}", domain);
  }
  
  // Load ws2_32.dll and get function addresses
  HMODULE hWs2 = GetModuleHandleW(L"ws2_32.dll");
  if (hWs2 == nullptr) {
    hWs2 = LoadLibraryW(L"ws2_32.dll");
  }
  
  if (hWs2 != nullptr) {
    g_pfnGetaddrinfo = reinterpret_cast<GetaddrinfoOriginal>(
        GetProcAddress(hWs2, "getaddrinfo"));
    g_pfnGetaddrinfoW = reinterpret_cast<GetaddrinfoWOriginal>(
        GetProcAddress(hWs2, "getaddrinfoW"));
    
    if (g_pfnGetaddrinfo != nullptr) {
      DebugLog(L"Hooking getaddrinfo");
      
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourAttach(&(LPVOID&)g_pfnGetaddrinfo,
                   reinterpret_cast<void*>(GetaddrinfoHooked));
      DetourTransactionCommit();
    }
    
    if (g_pfnGetaddrinfoW != nullptr) {
      DebugLog(L"Hooking getaddrinfoW");
      
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourAttach(&(LPVOID&)g_pfnGetaddrinfoW,
                   reinterpret_cast<void*>(GetaddrinfoWHooked));
      DetourTransactionCommit();
    }
    
    g_hooks_installed = true;
  } else {
    DebugLog(L"Failed to load ws2_32.dll");
  }
}

void UninstallNullDomain() {
  if (!g_hooks_installed) {
    return;
  }
  
  if (g_pfnGetaddrinfo != nullptr) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(LPVOID&)g_pfnGetaddrinfo,
                 reinterpret_cast<void*>(GetaddrinfoHooked));
    DetourTransactionCommit();
  }
  
  if (g_pfnGetaddrinfoW != nullptr) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(LPVOID&)g_pfnGetaddrinfoW,
                 reinterpret_cast<void*>(GetaddrinfoWHooked));
    DetourTransactionCommit();
  }
  
  g_hooks_installed = false;
}
