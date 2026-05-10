#ifndef CHROME_PLUS_SRC_DELETEFILE_H_
#define CHROME_PLUS_SRC_DELETEFILE_H_

#include <string>
#include <vector>

// Perform cleanup of specified directories and files
// based on configuration from chrome++.ini [cleanup] section.
void PerformCleanup();

// Initialize directory blocking hooks to prevent creation of specified directories
void InitializeDirBlock();

// Uninstall directory blocking hooks
void UninstallDirBlock();

// Check if a path contains wildcard characters
bool HasWildcard(const std::wstring& path);

// Delete a single file
bool DeleteSingleFile(const std::wstring& path);

// Delete a directory recursively
bool DeleteDirectoryRecursively(const std::wstring& path);

// Delete files matching a pattern (e.g., "User Data\*.log")
bool DeleteFilesByPattern(const std::wstring& pattern);

#endif  // CHROME_PLUS_SRC_DELETEFILE_H_
