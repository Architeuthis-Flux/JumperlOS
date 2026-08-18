// SPDX-License-Identifier: MIT
/**
 * CommandBuffer implementation
 * See CommandBuffer.h for architecture documentation
 */

#include "CommandBuffer.h"
#include "Adafruit_USBD_CDC.h"
#include <cstring>

// Singleton instance
static CommandBuffer* s_instance = nullptr;

CommandBuffer& CommandBuffer::getInstance() {
    if (s_instance == nullptr) {
        s_instance = new CommandBuffer();
    }
    return *s_instance;
}

// Global accessor for convenience
CommandBuffer& cmdBuffer = CommandBuffer::getInstance();

CommandBuffer::CommandBuffer()
    : cmd_q_head(0)
    , cmd_q_tail(0)
    , cmd_q_count(0)
    , is_python_command(false)
    , respond_to_uart(false)
    , uart_out_read(0)
    , uart_out_write(0)
    , commands_received(0)
    , commands_processed(0)
    , bytes_queued_to_uart(0)
    , bytes_sent_to_uart(0)
{
    memset(cmd_queue, 0, sizeof(cmd_queue));
    memset(uart_out_buffer, 0, sizeof(uart_out_buffer));
}

// =========================================================================
// INCOMING: Pending commands (ring queue)
// =========================================================================

bool CommandBuffer::setPendingJCommand(const char* cmd) {
    if (cmd_q_count >= CMD_QUEUE_DEPTH) {
        // Queue full - drop cleanly (caller resets its parser state).
        return false;
    }
    if (!cmd || strlen(cmd) == 0) {
        return false;
    }

    PendingEntry& e = cmd_queue[cmd_q_tail];

    // Copy command (truncate if too long)
    size_t len = strlen(cmd);
    if (len >= CMD_BUFFER_SIZE) {
        len = CMD_BUFFER_SIZE - 1;
    }
    memcpy(e.cmd, cmd, len);
    e.cmd[len] = '\0';
    e.is_python = false;
    e.respond_to_uart = true;  // Commands from UART get response to UART

    cmd_q_tail = (cmd_q_tail + 1) % CMD_QUEUE_DEPTH;
    commands_received++;
    cmd_q_count++;  // publish LAST
    return true;
}

bool CommandBuffer::setPendingPCommand(const char* cmd) {
    if (cmd_q_count >= CMD_QUEUE_DEPTH) {
        return false;
    }
    if (!cmd) {
        return false;
    }

    // Skip leading whitespace
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }

    PendingEntry& e = cmd_queue[cmd_q_tail];

    // Build command with '>' prefix if not already present
    size_t len = strlen(cmd);
    size_t offset = 0;
    if (cmd[0] != '>') {
        e.cmd[0] = '>';
        offset = 1;
    }
    if (len + offset >= CMD_BUFFER_SIZE) {
        len = CMD_BUFFER_SIZE - offset - 1;
    }
    memcpy(e.cmd + offset, cmd, len);
    e.cmd[offset + len] = '\0';
    e.is_python = true;
    e.respond_to_uart = true;

    cmd_q_tail = (cmd_q_tail + 1) % CMD_QUEUE_DEPTH;
    commands_received++;
    cmd_q_count++;  // publish LAST
    return true;
}

String CommandBuffer::consumePendingCommand() {
    if (cmd_q_count == 0) {
        return String();
    }

    PendingEntry& e = cmd_queue[cmd_q_head];
    String result(e.cmd);

    // Latch per-command flags for the command now being executed.
    is_python_command = e.is_python;
    respond_to_uart = e.respond_to_uart;  // stays set during execution for UART capture

    cmd_q_head = (cmd_q_head + 1) % CMD_QUEUE_DEPTH;
    cmd_q_count--;
    return result;
}

const char* CommandBuffer::consumePendingCommandPtr() {
    if (cmd_q_count == 0) {
        return nullptr;
    }

    uint8_t idx = cmd_q_head;
    PendingEntry& e = cmd_queue[idx];

    // Latch per-command flags for the command now being executed.
    is_python_command = e.is_python;
    respond_to_uart = e.respond_to_uart;

    // Advance head/count, but the entry's buffer stays intact until the producer
    // wraps all the way around (CMD_QUEUE_DEPTH more pushes) - far longer than a
    // single command execution - so the returned pointer is safe to use.
    cmd_q_head = (cmd_q_head + 1) % CMD_QUEUE_DEPTH;
    cmd_q_count--;
    return e.cmd;
}

void CommandBuffer::clearPendingCommand() {
    cmd_q_head = 0;
    cmd_q_tail = 0;
    cmd_q_count = 0;
    is_python_command = false;
    respond_to_uart = false;  // Only clear respond_to_uart when explicitly called
}

// =========================================================================
// OUTGOING: UART response buffer
// =========================================================================

size_t CommandBuffer::queueForUART(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return 0;
    }
    
    size_t queued = 0;
    for (size_t i = 0; i < len; i++) {
        uint16_t next_write = (uart_out_write + 1) % UART_OUT_BUFFER_SIZE;
        
        if (next_write == uart_out_read) {
            // Buffer full
            break;
        }
        
        uart_out_buffer[uart_out_write] = data[i];
        uart_out_write = next_write;
        queued++;
    }
    
    bytes_queued_to_uart += queued;
    return queued;
}

size_t CommandBuffer::queueForUART(const char* str) {
    if (!str) return 0;
    return queueForUART((const uint8_t*)str, strlen(str));
}

size_t CommandBuffer::queueForUART(const String& str) {
    if (str.length() == 0) return 0;
    return queueForUART((const uint8_t*)str.c_str(), str.length());
}

bool CommandBuffer::hasOutgoingData() const {
    return uart_out_read != uart_out_write;
}

size_t CommandBuffer::outgoingAvailable() const {
    if (uart_out_write >= uart_out_read) {
        return uart_out_write - uart_out_read;
    }
    return UART_OUT_BUFFER_SIZE - uart_out_read + uart_out_write;
}

int CommandBuffer::getOutgoingByte() {
    if (uart_out_read == uart_out_write) {
        return -1;  // Empty
    }
    
    uint8_t byte = uart_out_buffer[uart_out_read];
    uart_out_read = (uart_out_read + 1) % UART_OUT_BUFFER_SIZE;
    bytes_sent_to_uart++;
    return byte;
}

int CommandBuffer::peekOutgoingByte() const {
    if (uart_out_read == uart_out_write) {
        return -1;
    }
    return uart_out_buffer[uart_out_read];
}

void CommandBuffer::clearOutgoing() {
    uart_out_read = 0;
    uart_out_write = 0;
}

