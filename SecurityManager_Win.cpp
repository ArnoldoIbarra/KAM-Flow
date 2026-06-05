// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// SecurityManager Windows Implementation using CNG
// =============================================================================

/**
 * @file SecurityManager_Win.cpp
 * @brief Implementation of the SecurityManager using Windows CNG.
 * Provides zero-overhead AES-GCM encryption with authenticated headers (AAD).
 */

#include "SecurityManager.h"
#include "StateManager.h"

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <bcrypt.h>
#include <iostream>
#include <mutex>
#include "UIManager.h"

namespace Security {

    BCRYPT_ALG_HANDLE hAesAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    std::vector<uint8_t> keyObjectBuffer;
    
    /// Mutex to prevent AES-GCM internal state corruption during heavy bi-directional data flow.
    std::mutex cryptoMutex;

    /**
     * @brief Initializes CNG, sets the AES-GCM chaining mode, and hashes the PIN to a 256-bit key.
     * @param pin The 8-digit security PIN string.
     * @return true if initialization and key derivation succeeded, false otherwise.
     */
    bool Initialize(const char* pin) {
        if (hAesAlg != NULL) return true;

        if (BCryptOpenAlgorithmProvider(&hAesAlg, BCRYPT_AES_ALGORITHM, NULL, 0) != STATUS_SUCCESS) {
            if (State::globalDebugMode) UI::LogDebug("[Security] Failed to open CNG AES Provider.");
            return false;
        }

        if (BCryptSetProperty(hAesAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != STATUS_SUCCESS) {
            return false;
        }

        uint8_t derivedKey[32];
        BCRYPT_ALG_HANDLE hHashAlg = NULL;
        BCryptOpenAlgorithmProvider(&hHashAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
        
        BCRYPT_HASH_HANDLE hHash = NULL;
        BCryptCreateHash(hHashAlg, &hHash, NULL, 0, NULL, 0, 0);
        BCryptHashData(hHash, (PUCHAR)pin, (ULONG)strlen(pin), 0);
        BCryptFinishHash(hHash, derivedKey, sizeof(derivedKey), 0);
        
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hHashAlg, 0);

        DWORD cbKeyObject = 0, cbData = 0;
        BCryptGetProperty(hAesAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbKeyObject, sizeof(DWORD), &cbData, 0);
        keyObjectBuffer.resize(cbKeyObject);

        if (BCryptGenerateSymmetricKey(hAesAlg, &hKey, keyObjectBuffer.data(), cbKeyObject, derivedKey, sizeof(derivedKey), 0) != STATUS_SUCCESS) {
            if (State::globalDebugMode) UI::LogDebug("[Security] Failed to generate Symmetric Key.");
            return false;
        }

        SecureZeroMemory(derivedKey, sizeof(derivedKey));
        
        if (State::globalDebugMode) UI::LogDebug("[Security] Hardware AES-GCM Provider Initialized.");
        return true;
    }

    /**
     * @brief Encrypts plaintext data. Prepends a 12-byte random Nonce and appends a 16-byte Auth Tag.
     * @param plaintext Pointer to the raw data buffer.
     * @param plainSize Size of the plaintext buffer.
     * @param aad Pointer to the unencrypted header to authenticate.
     * @param aadSize Size of the AAD buffer.
     * @param outCiphertext Output vector containing [12-Byte Nonce] + [Encrypted Data] + [16-byte Auth Tag].
     * @return true if successful.
     */
    bool EncryptPayload(const void* plaintext, size_t plainSize, const void* aad, size_t aadSize, std::vector<uint8_t>& outCiphertext) {
        std::lock_guard<std::mutex> lock(cryptoMutex);
        if (!hKey) return false;

        uint8_t iv[12];
        BCryptGenRandom(NULL, iv, sizeof(iv), BCRYPT_USE_SYSTEM_PREFERRED_RNG);

        uint8_t authTag[16];
        DWORD cbResult = 0;
        
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = iv;
        authInfo.cbNonce = sizeof(iv);
        authInfo.pbAuthData = (PUCHAR)aad;
        authInfo.cbAuthData = (ULONG)aadSize;
        authInfo.pbTag = authTag;
        authInfo.cbTag = sizeof(authTag);

        std::vector<uint8_t> tempCiphertext(plainSize);
        uint8_t ivTemp[12];
        // Preserve the original IV since BCryptEncrypt mutates the passed buffer during execution.
        memcpy(ivTemp, iv, sizeof(iv));

        PUCHAR ptBuffer = plainSize > 0 ? (PUCHAR)plaintext : NULL;
        PUCHAR ctBuffer = plainSize > 0 ? tempCiphertext.data() : NULL;

        if (BCryptEncrypt(hKey, ptBuffer, (ULONG)plainSize, &authInfo, ivTemp, sizeof(ivTemp), 
                          ctBuffer, (ULONG)tempCiphertext.size(), &cbResult, 0) != STATUS_SUCCESS) {
            outCiphertext.clear();
            return false;
        }

        outCiphertext.resize(sizeof(iv) + cbResult + sizeof(authTag));
        memcpy(outCiphertext.data(), iv, sizeof(iv));
        if (cbResult > 0) {
            memcpy(outCiphertext.data() + sizeof(iv), tempCiphertext.data(), cbResult);
        }
        memcpy(outCiphertext.data() + sizeof(iv) + cbResult, authTag, sizeof(authTag));

        return true;
    }

    /**
     * @brief Decrypts data by extracting the prepended 12-byte Nonce and appended 16-byte Auth Tag.
     * @param ciphertext Pointer to the raw encrypted buffer [12-Byte Nonce] + [Encrypted Data] + [16-Byte Tag].
     * @param cipherSize Size of the ciphertext buffer.
     * @param aad Pointer to the exact header transmitted over the wire to verify integrity.
     * @param aadSize Size of the AAD buffer.
     * @param outPlaintext Output vector containing the exact decrypted original payload.
     * @return true if decryption succeeds.
     */
    bool DecryptPayload(const void* ciphertext, size_t cipherSize, const void* aad, size_t aadSize, std::vector<uint8_t>& outPlaintext) {
        std::lock_guard<std::mutex> lock(cryptoMutex);
        if (!hKey || cipherSize < 28) return false; // 12 (IV) + 16 (Tag) minimum

        const uint8_t* cipherData = static_cast<const uint8_t*>(ciphertext);

        uint8_t iv[12];
        memcpy(iv, cipherData, 12);

        size_t actualCipherSize = cipherSize - 28;
        const uint8_t* actualCiphertext = cipherData + 12;
        
        uint8_t authTag[16];
        memcpy(authTag, cipherData + 12 + actualCipherSize, 16);

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = iv;
        authInfo.cbNonce = sizeof(iv);
        authInfo.pbAuthData = (PUCHAR)aad;
        authInfo.cbAuthData = (ULONG)aadSize;
        authInfo.pbTag = authTag;
        authInfo.cbTag = sizeof(authTag);

        outPlaintext.resize(actualCipherSize);
        DWORD cbResult = 0;

        uint8_t ivTemp[12];
        // Preserve the original IV since BCryptDecrypt mutates the passed buffer during execution.
        memcpy(ivTemp, iv, sizeof(iv));
        
        PUCHAR ctBuffer = actualCipherSize > 0 ? (PUCHAR)actualCiphertext : NULL;
        PUCHAR ptBuffer = actualCipherSize > 0 ? outPlaintext.data() : NULL;

        if (BCryptDecrypt(hKey, ctBuffer, (ULONG)actualCipherSize, &authInfo, ivTemp, sizeof(ivTemp), 
                          ptBuffer, (ULONG)actualCipherSize, &cbResult, 0) != STATUS_SUCCESS) {
            outPlaintext.clear();
            return false;
        }

        outPlaintext.resize(cbResult);

        return true;
    }

    /**
     * @brief Cleans up cryptography handles and zeros memory containing the key object.
     * @return void
     */
    void Shutdown() {
        if (hKey) {
            BCryptDestroyKey(hKey);
            hKey = NULL;
        }
        if (hAesAlg) {
            BCryptCloseAlgorithmProvider(hAesAlg, 0);
            hAesAlg = NULL;
        }
        SecureZeroMemory(keyObjectBuffer.data(), keyObjectBuffer.size());
        keyObjectBuffer.clear();
    }
}