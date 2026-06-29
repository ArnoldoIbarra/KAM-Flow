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
    std::atomic<bool> g_isSecurityShutdown{false};

    /// Monotonically increasing generation counter. Bumped on every Shutdown()
    /// so that thread-local key caches auto-invalidate on reconnect cycles.
    /// Without this, threads that survive across reconnect (heartbeat, hook)
    /// would silently encrypt with the OLD key, causing decryption failures
    /// that look like "replay attacks" on the remote end.
    std::atomic<uint32_t> g_keyGeneration{0};

    /// Counter-based nonce state. AES-GCM only requires nonces to be unique,
    /// not cryptographically random (NIST SP 800-38D, Section 8.2.1).
    /// Using a 4-byte random session prefix + 8-byte atomic counter eliminates
    /// the per-packet BCryptGenRandom() kernel call that was causing ~1000
    /// kernel-mode CSPRNG invocations per second at 1000Hz mouse polling.
    uint8_t g_noncePrefix[4] = {0};
    std::atomic<uint64_t> g_nonceCounter{0};

    struct ThreadLocalKey {
        BCRYPT_KEY_HANDLE hKey = NULL;
        std::vector<uint8_t> keyObjectBuffer;
        uint32_t generation = 0; // Tracks which key generation this TLS copy belongs to
        
        ~ThreadLocalKey() {
            if (hKey) {
                BCryptDestroyKey(hKey);
                hKey = NULL;
            }
            if (!keyObjectBuffer.empty()) {
                SecureZeroMemory(keyObjectBuffer.data(), keyObjectBuffer.size());
            }
        }
    };
    
    thread_local ThreadLocalKey tls_key;

    BCRYPT_KEY_HANDLE GetThreadLocalKey() {
        if (g_isSecurityShutdown.load(std::memory_order_acquire)) return NULL;

        uint32_t currentGen = g_keyGeneration.load(std::memory_order_acquire);

        // If the TLS key exists but belongs to an old generation (pre-reconnect),
        // destroy it so we re-duplicate from the new master key below.
        if (tls_key.hKey != NULL && tls_key.generation != currentGen) {
            BCryptDestroyKey(tls_key.hKey);
            tls_key.hKey = NULL;
            SecureZeroMemory(tls_key.keyObjectBuffer.data(), tls_key.keyObjectBuffer.size());
            tls_key.keyObjectBuffer.clear();
        }

        if (tls_key.hKey != NULL) return tls_key.hKey;
        if (hKey == NULL) return NULL;

        DWORD cbKeyObject = 0, cbData = 0;
        BCryptGetProperty(hAesAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbKeyObject, sizeof(DWORD), &cbData, 0);
        tls_key.keyObjectBuffer.resize(cbKeyObject);
        
        if (BCryptDuplicateKey(hKey, &tls_key.hKey, tls_key.keyObjectBuffer.data(), cbKeyObject, 0) != STATUS_SUCCESS) {
            return NULL;
        }
        tls_key.generation = currentGen;
        return tls_key.hKey;
    }

    /**
     * @brief Initializes CNG, sets the AES-GCM chaining mode, and hashes the PIN to a 256-bit key.
     * @param pin The 8-digit security PIN string.
     * @return true if initialization and key derivation succeeded, false otherwise.
     */
    bool Initialize(const char* pin) {
        if (hAesAlg != NULL) return true;
        g_isSecurityShutdown.store(false, std::memory_order_release);

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

        // Seed the counter-based nonce with a 4-byte random session prefix.
        // This single BCryptGenRandom call replaces the per-packet calls that
        // were causing 1000+ kernel-mode CSPRNG invocations per second.
        BCryptGenRandom(NULL, g_noncePrefix, sizeof(g_noncePrefix), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        g_nonceCounter.store(0, std::memory_order_relaxed);
        
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
        BCRYPT_KEY_HANDLE localKey = GetThreadLocalKey();
        if (!localKey) return false;

        // Build a deterministic 12-byte nonce: [4-byte session prefix][8-byte counter]
        // NIST SP 800-38D Section 8.2.1 allows deterministic nonce construction
        // as long as the (key, nonce) pair is never reused. The session prefix
        // is random per Initialize(), and the counter is monotonically increasing.
        // This replaces the per-packet BCryptGenRandom() kernel call that was
        // causing mouse micro-freezes at high polling rates (1000Hz = 1000
        // kernel-mode CSPRNG calls/second).
        uint8_t iv[12];
        memcpy(iv, g_noncePrefix, 4);
        uint64_t counterVal = g_nonceCounter.fetch_add(1, std::memory_order_relaxed);
        memcpy(iv + 4, &counterVal, 8);

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

        if (BCryptEncrypt(localKey, ptBuffer, (ULONG)plainSize, &authInfo, ivTemp, sizeof(ivTemp), 
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
        BCRYPT_KEY_HANDLE localKey = GetThreadLocalKey();
        if (!localKey || cipherSize < 28) return false; // 12 (IV) + 16 (Tag) minimum

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

        DWORD cbResult = 0;

        uint8_t ivTemp[12];
        // Preserve the original IV since BCryptDecrypt mutates the passed buffer during execution.
        memcpy(ivTemp, iv, sizeof(iv));
        
        outPlaintext.resize(actualCipherSize);
        PUCHAR ctBuffer = actualCipherSize > 0 ? (PUCHAR)actualCiphertext : NULL;
        PUCHAR ptBuffer = actualCipherSize > 0 ? outPlaintext.data() : NULL;

        if (BCryptDecrypt(localKey, ctBuffer, (ULONG)actualCipherSize, &authInfo, ivTemp, sizeof(ivTemp), 
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
        g_isSecurityShutdown.store(true, std::memory_order_release);
        // Bump the generation counter so all thread-local key caches
        // auto-invalidate on the next GetThreadLocalKey() call. Without this,
        // threads surviving across reconnect cycles would encrypt with the
        // old key, causing decryption failures on the remote end.
        g_keyGeneration.fetch_add(1, std::memory_order_release);
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
        // Zero the nonce prefix so it doesn't leak into a future session
        SecureZeroMemory(g_noncePrefix, sizeof(g_noncePrefix));
    }
}