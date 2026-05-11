#include "deletefile.h"

#include <windows.h>
#include <cwctype>          // for towlower

#include <filesystem>
#include <string>
#include <vector>

#include "config.h"
#include "detours.h"
#include "utils.h"

namespace {

// Internal helper functions (prefixed to avoid name collision with public API)

// Check if a path contains wildcard characters (* or ?)
bool InternalHasWildcard(const std::wstring& path) {
  return (path.find(L'*') != std::wstring::npos) ||
         (path.find(L'?') != std::wstring::npos);
}

// Case-insensitive substring search
bool ContainsIgnoreCase(const std::wstring& str, const std::wstring& substr) {
  if (substr.empty()) return false;
  if (substr.size() > str.size()) return false;

  for (size_t i = 0; i <= str.size() - substr.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < substr.size(); ++j) {
      if (towlower(static_cast<wint_t>(str[i + j])) !=
          towlower(static_cast<wint_t>(substr[j]))) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

// Delete a single file
bool InternalDeleteSingleFile(const std::wstring& path) {
  // Check if file exists
  if (!std::filesystem::exists(path)) {
    return true;  // Non-existent is as good as deleted
  }

  // Remove read-only attribute if set
  DWORD attrs = ::GetFileAttributesW(path.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
    ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
  }

  return ::DeleteFileW(path.c_str()) != FALSE;
}

// Delete a directory recursively
bool InternalDeleteDirectoryRecursively(const std::wstring& path) {
  // Check if directory exists
  if (!std::filesystem::exists(path)) {
    return true;  // Consider non-existent directories as "deleted"
  }

  if (!std::filesystem::is_directory(path)) {
    // It's a file, delete it directly
    return InternalDeleteSingleFile(path);
  }

  // First, remove read-only attributes from all files in the directory
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           path, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) break;
    const auto& file_path = entry.path();
    DWORD attrs = ::GetFileAttributesW(file_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
      ::SetFileAttributesW(file_path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
  }

  // Now remove the whole tree (works even if the directory is empty)
  return std::filesystem::remove_all(path, ec) > 0;
}

// Delete files matching a pattern (e.g., "User Data\*.log")
bool InternalDeleteFilesByPattern(const std::wstring& pattern) {
  // Parse directory and pattern from the full path
  size_t last_backslash = pattern.find_last_of(L'\\');
  if (last_backslash == std::wstring::npos) {
    return false;  // No directory part
  }

  std::wstring dir = pattern.substr(0, last_backslash);
  std::wstring file_pattern = pattern.substr(last_backslash + 1);

  // Determine the absolute directory
  std::wstring abs_dir;
  // Absolute if it contains a drive letter/UNC (simple check)
  bool is_absolute = (dir.find(L':') != std::wstring::npos);
  if (is_absolute) {
    abs_dir = CanonicalizePath(dir);   // already absolute
  } else {
    // Relative: root from application directory
    abs_dir = CanonicalizePath(GetAppDir() + L"\\" + dir);
  }

  std::wstring search_path = abs_dir + L"\\" + file_pattern;

  bool success = true;
  WIN32_FIND_DATAW find_data;
  HANDLE find_handle = ::FindFirstFileW(search_path.c_str(), &find_data);

  if (find_handle == INVALID_HANDLE_VALUE) {
    return true;  // No matching files – not an error
  }

  do {
    if (wcscmp(find_data.cFileName, L".") != 0 &&
        wcscmp(find_data.cFileName, L"..") != 0) {
      std::wstring file_path = abs_dir + L"\\" + find_data.cFileName;

      if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        // Delete directory recursively
        InternalDeleteDirectoryRecursively(file_path);
      } else {
        // Delete file
        if (!InternalDeleteSingleFile(file_path)) {
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

    // Remove trailing backslashes
    while (normalized_path.size() > 1 && normalized_path.back() == L'\\')
      normalized_path.pop_back();
    while (normalized_blocked.size() > 1 && normalized_blocked.back() == L'\\')
      normalized_blocked.pop_back();

    if (ContainsIgnoreCase(normalized_path, normalized_blocked)) {
      return true;
    }
  }

  // Hardcoded block for BrowserMetrics (should ideally be in config)
  if (ContainsIgnoreCase(path, L"BrowserMetrics")) {
    return true;
  }

  return false;
}

}  // namespace

// ============================================================
// CreateDirectory Hook Implementation
// ============================================================

typedef BOOL (WINAPI *CreateDirectoryWOriginal)(
    LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);

static bool g_dir_block_hooks_installed = false;
static CreateDirectoryWOriginal g_pfnCreateDirectoryW = nullptr;

// Hooked CreateDirectoryW function
BOOL WINAPI CreateDirectoryWHooked(
    LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
  
  std::wstring path(lpPathName);
  
  if (IsDirBlocked(path)) {
    DebugLog(L"Blocking directory creation: {}", path);
    return TRUE;   // Pretend success
  }
  
  if (g_pfnCreateDirectoryW != nullptr) {
    return g_pfnCreateDirectoryW(lpPathName, lpSecurityAttributes);
  }
  
  return ::CreateDirectoryW(lpPathName, lpSecurityAttributes);
}

void InitializeDirBlock() {
  if (g_dir_block_hooks_installed) return;

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
               reinterpret_cast<LPVOID>(CreateDirectoryWHooked));
  auto status = DetourTransactionCommit();

  if (status == NO_ERROR) {
    g_dir_block_hooks_installed = true;
    DebugLog(L"Directory blocking hooks installed successfully");
  } else {
    DebugLog(L"Failed to install directory blocking hooks: {}", status);
  }
}

void UninstallDirBlock() {
  if (!g_dir_block_hooks_installed) return;

  if (g_pfnCreateDirectoryW != nullptr) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&reinterpret_cast<LPVOID&>(g_pfnCreateDirectoryW),
                 reinterpret_cast<LPVOID>(CreateDirectoryWHooked));
    DetourTransactionCommit();
  }
  g_dir_block_hooks_installed = false;
  DebugLog(L"Directory blocking hooks uninstalled");
}

// ============================================================
// Public API implementations (delegate to internal helpers)
// ============================================================

bool HasWildcard(const std::wstring& path) {
  return InternalHasWildcard(path);
}

bool DeleteSingleFile(const std::wstring& path) {
  return InternalDeleteSingleFile(path);
}

bool DeleteDirectoryRecursively(const std::wstring& path) {
  return InternalDeleteDirectoryRecursively(path);
}

bool DeleteFilesByPattern(const std::wstring& pattern) {
  return InternalDeleteFilesByPattern(pattern);
}

void PerformCleanup() {
  const auto& app_dir = GetAppDir();

  // Process delete_dir entries
  for (const auto& dir_entry : config.GetDeleteDirs()) {
    std::wstring full_path;

    if (dir_entry.find(L":") != std::wstring::npos) {
      // Absolute path (contains drive letter)
      full_path = dir_entry;
    } else if (dir_entry.starts_with(L"\\")) {
      // Root‑relative – prepend app directory
      full_path = app_dir + L"\\" + dir_entry.substr(1);
    } else {
      // Plain relative path
      full_path = app_dir + L"\\" + dir_entry;
    }

    full_path = CanonicalizePath(full_path);
    DebugLog(L"Deleting directory: {}", full_path);

    if (!InternalDeleteDirectoryRecursively(full_path)) {
      DebugLog(L"Failed to delete directory: {}, error: {}", full_path,
               GetLastError());
    }
  }

  // Process delete_file entries
  for (const auto& file_entry : config.GetDeleteFiles()) {
    std::wstring full_path;

    if (file_entry.find(L":") != std::wstring::npos) {
      full_path = file_entry;
    } else if (file_entry.starts_with(L"\\")) {
      full_path = app_dir + L"\\" + file_entry.substr(1);
    } else {
      full_path = app_dir + L"\\" + file_entry;
    }

    full_path = CanonicalizePath(full_path);

    if (InternalHasWildcard(full_path)) {
      DebugLog(L"Deleting files by pattern: {}", full_path);
      if (!InternalDeleteFilesByPattern(full_path)) {   // full_path is now absolute
        DebugLog(L"Failed to delete files by pattern: {}, error: {}", full_path,
                 GetLastError());
      }
    } else {
      DebugLog(L"Deleting file: {}", full_path);
      if (!InternalDeleteSingleFile(full_path)) {
        DebugLog(L"Failed to delete file: {}, error: {}", full_path,
                 GetLastError());
      }
    }
  }
}