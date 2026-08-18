// SPDX-License-Identifier: MIT
/**
 * @file SharedBuffer.cpp
 * @brief Implementation of pre-allocated shared buffer for context data transfer
 * 
 * This provides an 8KB static buffer that allows Ekilo, Python REPL, and other
 * contexts to pass content directly without saving to flash or dynamic allocation.
 */

#include "SharedBuffer.h"
#include "externVars.h"  // For core_sync_acquire/release
#include "PsramArena.h"  // PSRAM-backed allocation when available

// Static instance pointer (set on first call to getInstance)
SharedBuffer* SharedBuffer::instance = nullptr;

/**
 * @brief Private constructor - lazily allocates the body buffer.
 *
 * Tries PSRAM first; falls back to malloc on SRAM. If both fail (boot too
 * early, OOM), buffer stays null and write/append fail gracefully via the
 * null-check in setLength().
 */
SharedBuffer::SharedBuffer()
    : buffer(nullptr)
    , contentLen(0)
    , contentType(SharedBufferContentType::UNKNOWN)
    , isReadyFlag(false)
    , sourceContext(0)
{
    filename[0] = '\0';
    ensureBuffer();  // best-effort; retried lazily if it fails here
}

/**
 * @brief Lazily (re)allocate the backing buffer.
 *
 * Tries PSRAM first, then the SRAM heap. On units without PSRAM the
 * constructor's early allocation can fail when the heap is fragmented or
 * already claimed (e.g. by the MicroPython GC heap). Retrying here lets a
 * later call succeed once memory frees up, instead of leaving `buffer`
 * permanently null - which previously caused reads/writes against a null
 * pointer (garbage content, "buffer overflow" on save).
 */
// Write/read-back validation that a freshly allocated block is real,
// coherent RAM. PSRAM detection can misfire on boards where the chip is
// absent or dead, in which case psram_alloc() hands back a pointer into the
// 0x11xxxxxx window where writes are dropped and reads return ROM/flash
// garbage. eKilo loads files into this buffer, so a bad pointer shows up as
// garbled text and a bogus "buffer overflow" on save. Verifying here lets us
// reject the PSRAM block and fall back to SRAM regardless of detection.
static bool sharedBufferRoundtripOk(char* buf, size_t size) {
    if (!buf || size < 64) return false;

    const size_t off[] = { 0, size / 4, size / 2, (size / 4) * 3, size - 1 };
    constexpr int N = 5;
    char saved[N];
    for (int i = 0; i < N; i++) saved[i] = buf[off[i]];

    // Distinct values per probe so an aliasing window also fails the check.
    for (int i = 0; i < N; i++) buf[off[i]] = (char)(0xA5 ^ (i * 37 + 1));
    __asm volatile("" ::: "memory");

    bool ok = true;
    for (int i = 0; i < N; i++) {
        if (buf[off[i]] != (char)(0xA5 ^ (i * 37 + 1))) { ok = false; break; }
    }

    for (int i = 0; i < N; i++) buf[off[i]] = saved[i];
    return ok;
}

bool SharedBuffer::ensureBuffer() {
    if (buffer) return true;

    // Prefer PSRAM to keep 24KB off the SRAM heap, but only if it's real,
    // working memory. Otherwise give the block back and use the SRAM heap.
    char* psbuf = (char*)psram_alloc(SHARED_BUFFER_SIZE_PSRAM);
    if (psbuf && sharedBufferRoundtripOk(psbuf, SHARED_BUFFER_SIZE)) {
        buffer = psbuf;
        sharedBufferSize = SHARED_BUFFER_SIZE_PSRAM;
    } else {
        if (psbuf) {
            psram_free(psbuf);
            Serial.println("SharedBuffer: PSRAM block failed verification - using SRAM");
        }
        buffer = (char*)malloc(SHARED_BUFFER_SIZE);
        sharedBufferSize = SHARED_BUFFER_SIZE;
    }

    if (buffer) {
        buffer[0] = '\0';
        contentLen = 0;
    }
    return buffer != nullptr;
}

char* SharedBuffer::rawBuffer() {
    ensureBuffer();
    return buffer;
}

/**
 * @brief Get the singleton instance
 * 
 * Uses a function-local static - allocated in BSS, not heap.
 * This avoids heap fragmentation from the 24KB buffer.
 */
SharedBuffer& SharedBuffer::getInstance() {
    // Function-local static - Meyer's Singleton pattern
    static SharedBuffer localInstance;
    instance = &localInstance;
    return localInstance;
}

// ============================================================================
// Writing to the buffer
// ============================================================================

/**
 * @brief Clear all content and metadata
 */
void SharedBuffer::clear() {
    // NOTE: clear() must NOT allocate. It is called on REPL cleanup paths
    // where forcing a 24KB malloc would be wasteful (and dangerous on tight
    // no-PSRAM heaps). Allocation happens lazily in the write paths instead.
    core_sync_acquire();
    
    contentLen = 0;
    if (buffer) buffer[0] = '\0';
    filename[0] = '\0';
    contentType = SharedBufferContentType::UNKNOWN;
    isReadyFlag = false;
    sourceContext = 0;
    
    core_sync_release();
}

/**
 * @brief Write data to the buffer (overwrites existing content)
 */
bool SharedBuffer::write(const uint8_t* data, size_t len) {
    if (data == nullptr || !ensureBuffer()) {
        return false;
    }
    
    // Check if it fits (leave room for null terminator)
    if (len >= sharedBufferSize) {
        Serial.print("SharedBuffer: Data too large (");
        Serial.print(len);
        Serial.print(" > ");
        Serial.print(sharedBufferSize - 1);
        Serial.println(")");
        return false;
    }
    
    core_sync_acquire();
    
    memcpy(buffer, data, len);
    contentLen = len;
    buffer[contentLen] = '\0';  // Null terminate
    
    core_sync_release();
    
    return true;
}

bool SharedBuffer::write(const char* str) {
    if (str == nullptr) {
        return false;
    }
    return write((const uint8_t*)str, strlen(str));
}

bool SharedBuffer::write(const char* data, size_t len) {
    return write((const uint8_t*)data, len);
}

/**
 * @brief Append data to existing content
 */
bool SharedBuffer::append(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return true;  // Nothing to append is not an error
    }
    if (!ensureBuffer()) return false;
    
    // Check if it fits
    if (contentLen + len >= sharedBufferSize) {
        Serial.print("SharedBuffer: Append would overflow (");
        Serial.print(contentLen);
        Serial.print(" + ");
        Serial.print(len);
        Serial.print(" >= ");
        Serial.print(sharedBufferSize);
        Serial.println(")");
        return false;
    }
    
    core_sync_acquire();
    
    memcpy(buffer + contentLen, data, len);
    contentLen += len;
    buffer[contentLen] = '\0';  // Null terminate
    
    core_sync_release();
    
    return true;
}

bool SharedBuffer::append(const char* str) {
    if (str == nullptr) {
        return true;
    }
    return append((const uint8_t*)str, strlen(str));
}

bool SharedBuffer::append(const char* data, size_t len) {
    return append((const uint8_t*)data, len);
}

/**
 * @brief Append a line with newline at end
 */
bool SharedBuffer::appendLine(const char* line, size_t len) {
    if (!ensureBuffer()) return false;
    // Check if line + newline fits
    if (contentLen + len + 1 >= sharedBufferSize) {
        Serial.print("SharedBuffer: Line would overflow (");
        Serial.print(contentLen);
        Serial.print(" + ");
        Serial.print(len);
        Serial.println(" + 1)");
        return false;
    }
    
    core_sync_acquire();
    
    // Copy line content
    if (line != nullptr && len > 0) {
        memcpy(buffer + contentLen, line, len);
        contentLen += len;
    }
    
    // Add newline
    buffer[contentLen] = '\n';
    contentLen++;
    buffer[contentLen] = '\0';  // Null terminate
    
    core_sync_release();
    
    return true;
}

bool SharedBuffer::appendLine(const char* line) {
    if (line == nullptr) {
        return appendLine("", 0);  // Just add a newline
    }
    return appendLine(line, strlen(line));
}

/**
 * @brief Append a single character
 */
bool SharedBuffer::appendChar(char c) {
    if (!ensureBuffer()) return false;
    if (contentLen + 1 >= sharedBufferSize) {
        return false;
    }
    
    core_sync_acquire();
    
    buffer[contentLen] = c;
    contentLen++;
    buffer[contentLen] = '\0';
    
    core_sync_release();
    
    return true;
}

// ============================================================================
// Metadata
// ============================================================================

/**
 * @brief Set the filename associated with this content
 */
void SharedBuffer::setFilename(const char* name) {
    if (name == nullptr) {
        filename[0] = '\0';
        return;
    }
    
    size_t len = strlen(name);
    if (len >= SHARED_BUFFER_FILENAME_SIZE) {
        len = SHARED_BUFFER_FILENAME_SIZE - 1;
    }
    
    memcpy(filename, name, len);
    filename[len] = '\0';
}

// ============================================================================
// Debug
// ============================================================================

/**
 * @brief Print buffer status to Serial
 */
void SharedBuffer::printStatus() const {
    Serial.println("\n=== Shared Buffer Status ===");
    Serial.print("Content length: ");
    Serial.print(contentLen);
    Serial.print(" / ");
    Serial.print(sharedBufferSize);
    Serial.print(" (");
    Serial.print((contentLen * 100) / sharedBufferSize);
    Serial.println("% used)");
    
    Serial.print("Remaining: ");
    Serial.print(remaining());
    Serial.println(" bytes");
    
    if (hasFilename()) {
        Serial.print("Filename: ");
        Serial.println(filename);
    }
    
    Serial.print("Content type: ");
    switch (contentType) {
        case SharedBufferContentType::UNKNOWN: Serial.println("UNKNOWN"); break;
        case SharedBufferContentType::TEXT: Serial.println("TEXT"); break;
        case SharedBufferContentType::PYTHON_SCRIPT: Serial.println("PYTHON_SCRIPT"); break;
        case SharedBufferContentType::NETLIST: Serial.println("NETLIST"); break;
        case SharedBufferContentType::JSON: Serial.println("JSON"); break;
        case SharedBufferContentType::BINARY: Serial.println("BINARY"); break;
        default: Serial.println("(invalid)"); break;
    }
    
    Serial.print("Ready: ");
    Serial.println(isReadyFlag ? "YES" : "NO");
    
    Serial.print("Source context: ");
    Serial.println(sourceContext);
    
    // Show first 100 chars of content if any
    if (contentLen > 0) {
        Serial.print("Content preview: \"");
        size_t previewLen = contentLen < 100 ? contentLen : 100;
        for (size_t i = 0; i < previewLen; i++) {
            char c = buffer[i];
            if (c == '\n') {
                Serial.print("\\n");
            } else if (c == '\r') {
                Serial.print("\\r");
            } else if (c == '\t') {
                Serial.print("\\t");
            } else if (c >= 32 && c < 127) {
                Serial.print(c);
            } else {
                Serial.print('.');
            }
        }
        if (contentLen > 100) {
            Serial.print("...");
        }
        Serial.println("\"");
    }
    
    Serial.println("============================\n");
}

