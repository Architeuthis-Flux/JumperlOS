// SPDX-License-Identifier: MIT
/**
 * @file SharedBuffer.h
 * @brief Pre-allocated shared buffer for zero-copy data transfer between contexts
 *
 * This provides a 24KB transfer buffer that can be shared between different
 * contexts (Ekilo editor, Python REPL, File Manager, etc.) without flash
 * writes. When PSRAM is installed, the body lives in PSRAM (frees 24KB SRAM);
 * otherwise it's allocated from SRAM heap on first access. Either way, only
 * a small descriptor object lives in BSS.
 * 
 * Usage:
 *   // Producer (e.g., Ekilo editor)
 *   SharedBuffer& buf = SharedBuffer::getInstance();
 *   buf.clear();
 *   buf.setFilename("script.py");
 *   for each row:
 *       buf.appendLine(row.chars, row.size);
 *   buf.setReady(true);
 *   
 *   // Consumer (e.g., Python REPL)
 *   SharedBuffer& buf = SharedBuffer::getInstance();
 *   if (buf.isReady()) {
 *       const char* content = buf.data();
 *       size_t len = buf.length();
 *       // use content...
 *       buf.clear();  // Done consuming
 *   }
 */

#ifndef SHARED_BUFFER_H
#define SHARED_BUFFER_H

#include <Arduino.h>

// Size of the shared transfer buffer (24KB)
// This is the primary working buffer for eKilo editor file content
// Files are loaded here, parsed into line pointers, and serialized back on save
constexpr size_t SHARED_BUFFER_SIZE = 24 * 1024;
constexpr size_t SHARED_BUFFER_SIZE_PSRAM = 48 * 1024;

// Maximum filename length
constexpr size_t SHARED_BUFFER_FILENAME_SIZE = 128;

/**
 * @brief Content type hints for the shared buffer
 * 
 * Helps consumers know what kind of content is in the buffer
 */
enum class SharedBufferContentType : uint8_t {
    UNKNOWN = 0,    // Unspecified content
    TEXT,           // Plain text
    PYTHON_SCRIPT,  // Python source code
    NETLIST,        // Jumperless netlist format
    JSON,           // JSON data
    BINARY          // Binary data (use with caution)
};

/**
 * @brief Singleton class managing the shared buffer
 * 
 * Thread-safety: Uses existing core_sync_acquire/release for mutex
 * Memory: 24KB + ~200 bytes metadata, statically allocated
 */
class SharedBuffer {
public:
    /**
     * @brief Get the singleton instance
     */
    static SharedBuffer& getInstance();

    int sharedBufferSize; // the size of the shared buffer actually allocated (PSRAM or SRAM)
    
    // =========================================================================
    // Writing to the buffer
    // =========================================================================
    
    /**
     * @brief Clear all content and metadata
     * 
     * Call this before starting to write new content.
     */
    void clear();
    
    /**
     * @brief Write data to the buffer (overwrites existing content)
     * 
     * @param data Pointer to data to write
     * @param len Length in bytes
     * @return true if written successfully, false if buffer would overflow
     */
    bool write(const uint8_t* data, size_t len);
    bool write(const char* str);
    bool write(const char* data, size_t len);
    
    /**
     * @brief Append data to existing content
     * 
     * @param data Pointer to data to append
     * @param len Length in bytes
     * @return true if appended successfully, false if buffer would overflow
     */
    bool append(const uint8_t* data, size_t len);
    bool append(const char* str);
    bool append(const char* data, size_t len);
    
    /**
     * @brief Append a line (adds newline at end)
     * 
     * Convenience method for building text content line by line.
     * 
     * @param line Line content (without newline)
     * @param len Length of line content
     * @return true if appended successfully
     */
    bool appendLine(const char* line, size_t len);
    bool appendLine(const char* line);  // null-terminated version
    
    /**
     * @brief Append a character
     */
    bool appendChar(char c);
    
    // =========================================================================
    // Reading from the buffer
    // =========================================================================
    
    /**
     * @brief Get pointer to buffer content (read-only)
     * 
     * Content is null-terminated for convenience with string functions.
     * 
     * @return Pointer to buffer data (always valid, may be empty)
     */
    const char* data() const { return buffer ? buffer : ""; }
    
    /**
     * @brief Get raw writable pointer to buffer for direct file reads
     * 
     * WARNING: After writing directly to this buffer, you MUST call
     * setLength() to update the content length, and the null terminator
     * is YOUR responsibility.
     * 
     * Lazily (re)allocates the backing buffer if it isn't allocated yet.
     * May still return nullptr if allocation fails (e.g. no PSRAM and the
     * SRAM heap can't satisfy the request); callers MUST null-check.
     * 
     * @return Pointer to raw buffer (writable), or nullptr if unavailable
     */
    char* rawBuffer();

    /**
     * @brief Ensure the backing buffer is allocated.
     *
     * The body is allocated lazily (PSRAM first, then SRAM heap). The
     * original allocation happens in the constructor, but on units without
     * PSRAM that early malloc can fail if the heap is fragmented/claimed.
     * This retries on demand so a later call (when more memory is free) can
     * succeed instead of leaving the buffer null forever.
     *
     * @return true if the buffer is available after the call
     */
    bool ensureBuffer();
    
    /**
     * @brief Set the content length after direct buffer writes
     * 
     * Call this after writing directly to rawBuffer().
     * This also ensures null termination.
     * 
     * @param len New content length (must be < SHARED_BUFFER_SIZE)
     */
    void setLength(size_t len) {
        if (!buffer) return;
        if (len >= sharedBufferSize) len = sharedBufferSize - 1;
        contentLen = len;
        buffer[contentLen] = '\0';
    }
    
    /**
     * @brief Get current content length in bytes
     */
    size_t length() const { return contentLen; }
    
    /**
     * @brief Check if buffer has any content
     */
    bool hasContent() const { return contentLen > 0; }
    
    /**
     * @brief Get remaining space in buffer
     */
    size_t remaining() const { return sharedBufferSize - contentLen - 1; }  // -1 for null terminator
    
    // =========================================================================
    // Metadata
    // =========================================================================
    
    /**
     * @brief Set the filename associated with this content
     * 
     * This allows consumers to know what file the content came from
     * or should be saved to.
     * 
     * @param filename Path (max 127 chars, will be truncated if longer)
     */
    void setFilename(const char* filename);
    
    /**
     * @brief Get the filename associated with this content
     * @return Filename or empty string if not set
     */
    const char* getFilename() const { return filename; }
    
    /**
     * @brief Check if filename is set
     */
    bool hasFilename() const { return filename[0] != '\0'; }
    
    /**
     * @brief Set the content type
     */
    void setContentType(SharedBufferContentType type) { contentType = type; }
    
    /**
     * @brief Get the content type
     */
    SharedBufferContentType getContentType() const { return contentType; }
    
    /**
     * @brief Set ready flag (indicates content is complete and ready for consumption)
     * 
     * Producers should set this to true after finishing writing.
     * Consumers should check this before reading.
     * 
     * @param ready true if content is ready for consumption
     */
    void setReady(bool ready) { isReadyFlag = ready; }
    
    /**
     * @brief Check if content is ready for consumption
     */
    bool isReady() const { return isReadyFlag; }
    
    /**
     * @brief Set the source context type (who produced this content)
     */
    void setSourceContext(uint8_t ctx) { sourceContext = ctx; }
    
    /**
     * @brief Get the source context type
     */
    uint8_t getSourceContext() const { return sourceContext; }
    
    // =========================================================================
    // Debug
    // =========================================================================
    
    /**
     * @brief Print full buffer status to Serial (use sparingly)
     */
    void printStatus() const;
    
private:
    SharedBuffer();
    ~SharedBuffer() = default;
    
    // Prevent copying
    SharedBuffer(const SharedBuffer&) = delete;
    SharedBuffer& operator=(const SharedBuffer&) = delete;
    
    static SharedBuffer* instance;

    // The shared buffer - 24KB allocated lazily from PSRAM if available,
    // SRAM heap otherwise. Was BSS before the PSRAM cache layer existed.
    char* buffer;
    size_t contentLen;
    
    // Metadata
    char filename[SHARED_BUFFER_FILENAME_SIZE];
    SharedBufferContentType contentType;
    bool isReadyFlag;
    uint8_t sourceContext;  // ContextType enum value cast to uint8_t
};

// ============================================================================
// Convenience functions (can be called without getting singleton)
// ============================================================================

/**
 * @brief Quick check if shared buffer has ready content
 */
inline bool sharedBufferHasContent() {
    return SharedBuffer::getInstance().isReady() && SharedBuffer::getInstance().hasContent();
}

/**
 * @brief Quick access to shared buffer data
 */
inline const char* sharedBufferData() {
    return SharedBuffer::getInstance().data();
}

/**
 * @brief Quick access to shared buffer length
 */
inline size_t sharedBufferLength() {
    return SharedBuffer::getInstance().length();
}

/**
 * @brief Quick clear of shared buffer
 */
inline void sharedBufferClear() {
    SharedBuffer::getInstance().clear();
}

#endif // SHARED_BUFFER_H

