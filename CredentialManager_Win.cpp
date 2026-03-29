// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Credential Manager Windows Implementation
// =============================================================================

/**
 * @file CredentialManager_Win.cpp
 * @brief Implementation of the CredentialManager using Windows Credential Manager API.
 */

#include "CredentialManager.h"
#include "StateManager.h"
#include <windows.h>
#include <wincred.h>
#include <iostream>

namespace CredentialManager {

    /**
     * @brief Saves a plaintext secret to the OS Credential Vault.
     * @param targetName The unique identifier for the credential.
     * @param secret The plaintext string to securely store.
     * @return true if successfully saved, false otherwise.
     */
    bool SaveSecret(const std::string& targetName, const std::string& secret) {
        CREDENTIALA cred = { 0 };
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = (LPSTR)targetName.c_str();
        cred.CredentialBlobSize = (DWORD)secret.size();
        cred.CredentialBlob = (LPBYTE)secret.data();
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

        if (!CredWriteA(&cred, 0)) {
            if (State::globalDebugMode) std::cerr << "[CredManager] Failed to save credential for " << targetName << "\n";
            return false;
        }
        return true;
    }

    /**
     * @brief Retrieves a plaintext secret from the OS Credential Vault.
     * @param targetName The unique identifier for the credential to load.
     * @param outSecret Reference to a string to populate with the retrieved secret.
     * @return true if successfully loaded, false if the credential does not exist.
     */
    bool LoadSecret(const std::string& targetName, std::string& outSecret) {
        PCREDENTIALA pCred;
        if (CredReadA(targetName.c_str(), CRED_TYPE_GENERIC, 0, &pCred)) {
            outSecret.assign((char*)pCred->CredentialBlob, pCred->CredentialBlobSize);
            CredFree(pCred);
            return true;
        }
        return false;
    }

    /**
     * @brief Deletes a secret from the OS Credential Vault.
     * @param targetName The unique identifier for the credential to delete.
     * @return true if successfully deleted, false otherwise.
     */
    bool DeleteSecret(const std::string& targetName) {
        return CredDeleteA(targetName.c_str(), CRED_TYPE_GENERIC, 0) != 0;
    }

    /**
     * @brief Retrieves a list of all saved target names matching a specific prefix.
     * @param prefix The string prefix to filter saved credentials by.
     * @return A vector containing the names of all matching credentials.
     */
    std::vector<std::string> GetSavedTargets(const std::string& prefix) {
        std::vector<std::string> targets;
        DWORD count;
        PCREDENTIALA* pCreds;
        std::string filter = prefix + "*";
        if (CredEnumerateA(filter.c_str(), 0, &count, &pCreds)) {
            for (DWORD i = 0; i < count; ++i) targets.push_back(pCreds[i]->TargetName);
            CredFree(pCreds);
        }
        return targets;
    }
}