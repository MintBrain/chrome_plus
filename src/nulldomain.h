#ifndef CHROME_PLUS_SRC_NULLDOMAIN_H_
#define CHROME_PLUS_SRC_NULLDOMAIN_H_

// Initialize NullDomain hooks to block specified domains
// This must be called after config is loaded
void InitializeNullDomain();

// Uninstall NullDomain hooks (called on DLL detach)
void UninstallNullDomain();

#endif  // CHROME_PLUS_SRC_NULLDOMAIN_H_
