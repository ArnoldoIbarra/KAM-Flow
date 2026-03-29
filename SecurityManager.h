// =============================================================================
// KAM-Flow - © Arnoldo Ibarra - Proprietary and Confidential
// =============================================================================
// Security Manager Interface
// =============================================================================

/// @file SecurityManager.h
/// Defines the abstract security interface for payload encryption and decryption.

#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

/// Namespace managing cryptographic operations and secure key derivation.
namespace Security {
    
    /// Initializes the cryptography provider and derives the 256-bit session key from the provided PIN.
    bool Initialize(const char* pin);

    /// Encrypts a plaintext payload using AES-GCM. Uses the provided AAD to authenticate the data.
    bool EncryptPayload(const void* plaintext, size_t plainSize, const void* aad, size_t aadSize, std::vector<uint8_t>& outCiphertext);

    /// Decrypts an AES-GCM payload by extracting the 12-byte Nonce and 16-byte Auth Tag, verifying against the AAD.
    bool DecryptPayload(const void* ciphertext, size_t cipherSize, const void* aad, size_t aadSize, std::vector<uint8_t>& outPlaintext);

    /// Cleans up hardware cryptography handles and safely zeroes sensitive memory.
    void Shutdown();
}