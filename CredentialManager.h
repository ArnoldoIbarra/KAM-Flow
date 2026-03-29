// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Credential Manager Interface
// =============================================================================

/// @file CredentialManager.h
/// Defines the interface for secure storage and retrieval of session PINs.

#pragma once
#include <string>
#include <vector>

namespace CredentialManager {
    /// Saves a plaintext secret to the OS Credential Vault.
    bool SaveSecret(const std::string& targetName, const std::string& secret);

    /// Retrieves a plaintext secret from the OS Credential Vault.
    bool LoadSecret(const std::string& targetName, std::string& outSecret);

    /// Deletes a secret from the OS Credential Vault.
    bool DeleteSecret(const std::string& targetName);

    /// Retrieves a list of all saved target names matching a specific prefix.
    std::vector<std::string> GetSavedTargets(const std::string& prefix);
}