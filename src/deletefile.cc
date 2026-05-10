#include "deletefile.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "config.h"
#include "detours.h"
#include "utils.h"

namespace {

// Check if a path contains wildcard characters (* or ?)
bool HasWildcard(const std::wstring& path) {
  return (path.find(L'*') != std::wstring::npos) ||
         (path.find(L'?') != std::wstring::npos);
}

// Case-insensitive substring search
bool ContainsIgnoreCase(const std::wstring& str, const std::wstring& substr) {
  if (substr.empty()) return false;
  
  auto it = str.begin();
  for (; it <= std::distance(str.begin(), str.end() - substr.size()); ++it) {
    bool match = true;
    for (size_t j = 0; j < substr.size(); ++j) {
      if (std::towlower(*it) != std::towlower(*(it + j))) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

// Delete a single file
bool DeleteSingleFile(const std::wstring& path) {
  // Check if file exists
  if (!std::filesystem::exists(path)) {
    return true;  // Consider non-existent files as "deleted"
  }

  // Remove read-only attribute if set
  DWORD attrs = ::GetFileAttributesW(path.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES &&
      (attrs & FILE_ATTRIBUTE_READONLY)) {
    ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
  }

  return ::DeleteFileW(path.c_str()) != FALSE;
}

// Delete a directory recursively
bool DeleteDirectoryRecursively(const std::wstring& path) {
  // Check if directory exists
  if (!std::filesystem::exists(path)) {
    return true;  // Consider non-existent directories as "deleted"
  }

  if (!std::filesystem::is_directory(path)) {
    // It's a file, delete it directly
    return DeleteSingleFile(path);
  }

  // First, remove read-only attributes from all files in the directory
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           path, std::filesystem::directory_options::skip_permission_denied)) {
    const auto& file_path = entry.path();
    DWORD attrs = ::GetFileAttributesW(file_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_READONLY)) {
      ::SetFileAttributesW(file_path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
  }

  // Remove the directory
  return ::RemoveDirectoryW(path.c_str()) != FALSE ||
         std::filesystem::remove_all(path, ec) > 0;
}

// Delete files matching a pattern (e.g., "User Data\*.log")
bool DeleteFilesByPattern(const std::wstring& pattern) {
  // Parse directory and pattern from the full path
  size_t last_backslash = pattern.find_last_of(L'\\');
  if (last_backslash == std::wstring::npos) {
    return false;  // No directory specified
  }

  std::wstring dir = pattern.substr(0, last_backslash);
  std::wstring file_pattern = pattern.substr(last_backslash + 1);

  // Resolve to absolute path
  std::wstring abs_dir = dir.starts_with(L"\\")
                             ? GetAppDir() + dir
                             : GetAppDir() + L"\\" + dir;
  abs_dir = CanonicalizePath(abs_dir);

  std::wstring search_path = abs_dir + L"\\" + file_pattern;

  bool success = true;
  WIN32_FIND_DATAW find_data;
  HANDLE find_handle = ::FindFirstFileW(search_path.c_str(), &find_data);

  if (find_handle == INVALID_HANDLE_VALUE) {
    return true;  // No files found, not an error
  }

  do {
    if (wcscmp(find_data.cFileName, L".") != 0 &&
        wcscmp(find_data.cFileName, L"..") != 0) {
      std::wstring file_path = abs_dir + L"\\" + find_data.cFileName;

      if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        // Delete directory recursively
        DeleteDirectoryRecursively(file_path);
      } else {
        // Delete file
        if (!DeleteSingleFile(file_path)) {
          success = false;
        }
      }
    }
  } while (::FindNextFileW(find_handle, &find_data));

  ::FindClose(find_handle);
  return success;
}

// Check if a directory path should be blocked
bool IsDirBlocked(const std::wstring& path) {
  const auto& block_dirs = config.GetBlockDirs();
  
  for (const auto& blocked_dir : block_dirs) {
    // Normalize both paths for comparison
    std::wstring normalized_path = path;
    std::wstring normalized_blocked = blocked_dir;
    
    // Remove trailing backslashes for consistent comparison
    while (normalized_path.size() > 1 && normalized_path.back() == L'\\') {
      normalized_path.pop_back();
    }
    while (normalized_blocked.size() > 1 && normalized_blocked.back() == L'\\') {
      normalized_blocked.pop_back();
    }
    
    // Check if the path contains the blocked directory (substring match like another-lib)
    if (ContainsIgnoreCase(normalized_path, normalized_blocked)) {
      return true;
    }
  }
  
  // Also check for BrowserMetrics specifically (hardcoded like in another-lib)
  if (ContainsIgnoreCase(path, L"BrowserMetrics")) {
    return true;
  }
  
  return false;
}

}  // namespace

// ============================================================
// CreateDirectory Hook Implementation
// ============================================================

// Original function pointer
typedef BOOL (WINAPI *CreateDirectoryWOriginal)(
    LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);

static bool g_dir_block_hooks_installed = false;
static CreateDirectoryWOriginal g_pfnCreateDirectoryW = nullptr;

// Hooked CreateDirectoryW function
BOOL WINAPI CreateDirectoryWHooked(
    LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
  
  std::wstring path(lpPathName);
  
  // Check if this directory should be blocked
  if (IsDirBlocked(path)) {
    DebugLog(L"Blocking directory creation: {}", path);
    // Return TRUE to indicate success without actually creating the directory
    // This prevents Chrome from knowing the directory was blocked
    return TRUE;
  }
  
  // Call original function
  if (g_pfnCreateDirectoryW != nullptr) {
    return g_pfnCreateDirectoryW(lpPathName, lpSecurityAttributes);
  }
  
  // Fallback: try to create anyway
  return ::CreateDirectoryW(lpPathName, lpSecurityAttributes);
}

void InitializeDirBlock() {
  if (g_dir_block_hooks_installed) {
    return;
  }
  
  const auto& block_dirs = config.GetBlockDirs();
  if (block_dirs.empty()) {
    DebugLog(L"No directories to block");
    return;
  }
  
  DebugLog(L"Initializing directory blocking with {} directories", block_dirs.size());
  for (const auto& dir : block_dirs) {
    DebugLog(L"  - Blocking: {}", dir);
  }
  
  // Hook CreateDirectoryW using Detours
  g_pfnCreateDirectoryW = ::CreateDirectoryW;
  
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(&reinterpret_cast<LPVOID&>(g_pfnCreateDirectoryW),
               CreateDirectoryWHooked);
  auto status = DetourTransactionCommit();
  
  if (status == NO_ERROR) {
    g_dir_block_hooks_installed = true;
    DebugLog(L"Directory blocking hooks installed successfully");
  } else {
    DebugLog(L"Failed to install directory blocking hooks: {}", status);
  }
}

void UninstallDirBlock() {
  if (!g_dir_block_hooks_installed) {
    return;
  }
  
  if (g_pfnCreateDirectoryW != nullptr) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&reinterpret_cast<LPVOID&>(g_pfnCreateDirectoryW),
                 CreateDirectoryWHooked);
    DetourTransactionCommit();
  }
  
  g_dir_block_hooks_installed = false;
  DebugLog(L"Directory blocking hooks uninstalled");
}

// ============================================================
// Public API implementations
// ============================================================

bool HasWildcard(const std::wstring& path) {
  return ::HasWildcard(path);
}

bool DeleteSingleFile(const std::wstring& path) {
  return ::DeleteSingleFile(path);
}

bool DeleteDirectoryRecursively(const std::wstring& path) {
  return ::DeleteDirectoryRecursively(path);
}

bool DeleteFilesByPattern(const std::wstring& pattern) {
  return ::DeleteFilesByPattern(pattern);
}

void PerformCleanup() {
  const auto& app_dir = GetAppDir();

  // Process delete_dir entries
  for (const auto& dir_entry : config.GetDeleteDirs()) {
    std::wstring full_path;

    if (dir_entry.starts_with(L"\\") || dir_entry.find(L":") != std::wstring::npos) {
      // Absolute path or path starting with backslash
      if (dir_entry.starts_with(L"\\")) {
        full_path = app_dir + dir_entry;
      } else {
        full_path = dir_entry;
      }
    } else {
      // Relative path
      full_path = app_dir + L"\\" + dir_entry;
    }

    full_path = CanonicalizePath(full_path);
    DebugLog(L"Deleting directory: {}", full_path);

    if (!DeleteDirectoryRecursively(full_path)) {
      DebugLog(L"Failed to delete directory: {}, error: {}", full_path,
               GetLastError());
    }
  }

  // Process delete_file entries
  for (const auto& file_entry : config.GetDeleteFiles()) {
    std::wstring full_path;

    if (file_entry.starts_with(L"\\") || file_entry.find(L":") != std::wstring::npos) {
      // Absolute path or path starting with backslash
      if (file_entry.starts_with(L"\\")) {
        full_path = app_dir + file_entry;
      } else {
        full_path = file_entry;
      }
    } else {
      // Relative path
      full_path = app_dir + L"\\" + file_entry;
    }

    full_path = CanonicalizePath(full_path);

    if (HasWildcard(file_entry)) {
      DebugLog(L"Deleting files by pattern: {}", full_path);
      if (!DeleteFilesByPattern(full_path)) {
        DebugLog(L"Failed to delete files by pattern: {}, error: {}", full_path,
                 GetLastError());
      }
    } else {
      DebugLog(L"Deleting file: {}", full_path);
      if (!DeleteSingleFile(full_path)) {
        DebugLog(L"Failed to delete file: {}, error: {}", full_path,
                 GetLastError());
      }
    }
  }
}
