// SPDX-License-Identifier: MIT

// Debug flag for relayed command buffer tracing
// Set to 1 to see buffer contents when extracting relayed commands
#define DEBUG_RELAYED_COMMANDS 0

#include "Jerial.h"
#include "ArduinoStuff.h"
#include "ReadingDisplay.h" // resetLastShown() when Enter scrolls the pinned rows
#include "Python_Proper.h" // For ScriptHistory
#include "SingleCharCommands.h"

// External USB CDC instances (defined in ArduinoStuff.cpp)
extern Adafruit_USBD_CDC USBSer1;
extern Adafruit_USBD_CDC USBSer2;
extern Adafruit_USBD_CDC USBSer3;

// External OLED instance
extern class oled oled;

// Serial1 is a global Arduino object, available by default

// Global variable for interactive mode tracking
int termInInteractiveMode = 0;

// --- Line-buffering / interactive-mode control (single source of truth) ------
// See Jerial.h for the contract. All SO/SI (0x0E/0x0F) emission and every
// mutation of termInInteractiveMode lives here so the app can never be forced
// on/off redundantly and drift out of sync.

void pushLineBufferingToApp( ) {
    extern struct config jumperlessConfig;
    int target = ( jumperlessConfig.display.terminal_line_buffering == 1 ) ? 1 : 0;
    if ( termInInteractiveMode != target ) {
        Serial.write( target ? 0x0E : 0x0F );
        Serial.flush( );
        termInInteractiveMode = target;
    }
}

bool setTerminalLineBuffering( bool enabled ) {
    extern struct config jumperlessConfig;
    bool previous = ( jumperlessConfig.display.terminal_line_buffering == 1 );
    jumperlessConfig.display.terminal_line_buffering = enabled ? 1 : 0;
    pushLineBufferingToApp( );
    return previous;
}

void acknowledgeAppLineBuffering( bool enabled ) {
    extern struct config jumperlessConfig;
    int target = enabled ? 1 : 0;
    jumperlessConfig.display.terminal_line_buffering = target;
    termInInteractiveMode = target; // app already knows; do not echo SO/SI back
}

// Bulk machine input (Wokwi diagram JSON, netlist blobs sent by the app or
// pasted whole) should never enter the recallable command history: it is large,
// single-use, and would evict real commands from the small ring buffer. A line
// counts as "bulk" only if it contains a bracket AND is either the Wokwi command
// ('W') or longer than a normal command line.
// ponytail: simple prefix+length heuristic, ceiling = it won't historize a
// legitimately-long bracketed command; upgrade path is an explicit opt-out flag
// on the command if that ever matters.
static bool isBulkMachineLine( const char* line, int len ) {
    if ( !line || len <= 0 ) {
        return false;
    }
    bool hasBracket = false;
    for ( int i = 0; i < len; i++ ) {
        if ( line[i] == '{' || line[i] == '[' ) {
            hasBracket = true;
            break;
        }
    }
    if ( !hasBracket ) {
        return false;
    }
    return ( line[0] == 'W' || len > 512 );
}

// ============================================================================
// TermControl Implementation (Internal Class for USB Serial Endpoints)
// ============================================================================

TermControl::TermControl( Stream* underlying_stream, bool create_own_history )
    : stream( underlying_stream ),
      history( nullptr ),
      owns_history( create_own_history ),
      history_initialized( false ),
      line_length( 0 ),
      cursor_position( 0 ),
      line_ready( false ),
      echo_enabled( true ),
      syntax_highlighter( underlying_stream ),
      ansi_state( ANSI_NORMAL ),
      brace_depth( 0 ),
      queue_head( 0 ),
      queue_tail( 0 ),
      queue_count( 0 ),
      last_response_target( nullptr ) {

    // Initialize response targets array
    for ( int i = 0; i < COMMAND_QUEUE_SIZE; i++ ) {
        response_targets[i] = nullptr;
    }

    // Create our own history instance if requested
    if ( create_own_history ) {
        history = new ScriptHistory( );
        if ( history ) {
            history->initFilesystem( );
        }
    }

    // Initialize current line buffer and display buffer
    memset( current_line, 0, sizeof( current_line ) );
    memset( display_buffer, 0, sizeof( display_buffer ) );
    display_length = 0;
}

TermControl::~TermControl( ) {
    // Clean up history if we own it
    if ( owns_history && history ) {
        delete history;
        history = nullptr;
    }
}

// Stream Interface Implementation
int TermControl::available( ) {
    if ( !stream )
        return 0;
    return stream->available( );
}

#define ECHO_TERMINAL_INPUT 0

int TermControl::read( ) {
    if ( !stream )
        return -1;
    
    int c = stream->read();
    #if ECHO_TERMINAL_INPUT == 1
    if (c >= 0) {
        Serial.print((char)c);
        Serial.flush();
    }
    #endif
    return c;
}

int TermControl::peek( ) {
    if ( !stream )
        return -1;
    return stream->peek( );
}

void TermControl::flush( ) {
    if ( stream ) {
        stream->flush( );
    }
}

size_t TermControl::write( uint8_t byte ) {
    if ( !stream )
        return 0;
    return stream->write( byte );
}

size_t TermControl::write( const uint8_t* buffer, size_t size ) {
    if ( !stream )
        return 0;
    return stream->write( buffer, size );
}

// Terminal-specific functionality
bool TermControl::service( ) {
    if ( !stream ) {

        return false;
    }

    bool line_was_ready_before = line_ready;
    
    int avail = stream->available();
    if (avail > 0) {
        #if DEBUG_JERIAL == 1
        Serial.printf("TermControl::service(): stream->available() = %d\n", avail);
        Serial.flush();
        #endif
        
    }

    while ( stream->available( ) > 0 ) {
        char c = (char)stream->read( );
        #if DEBUG_JERIAL == 1
        Serial.printf("TermControl read: '%c' (%d)\n", c, (int)c);
        Serial.flush();
        #endif
        // Note: Tag filtering now happens in RelayBufferStream
        // User-typed input doesn't have tags, so no filtering needed here

        // Line-buffering sync protocol: a bare SO (0x0E) enables / SI (0x0F)
        // disables line buffering — the same single characters the firmware
        // sends the app to announce the state, so the protocol is symmetric.
        // Handled here (before the ANSI/normal-char path) so the app can switch
        // buffering OFF even while it is currently ON — handleNormalChar drops
        // bare control chars, which would otherwise make this a no-op in buffered
        // mode. The raw (unbuffered) input path handles these via registered
        // single-char commands (cmd_toggleLineBufferingQuiet). This is app-
        // initiated, so adopt the state silently (no echo back).
        if ( c == 0x0E ) {
            acknowledgeAppLineBuffering( true );
            continue;
        }
        if ( c == 0x0F ) {
            acknowledgeAppLineBuffering( false );
            // CRITICAL: stop reading here. The app sends SI immediately before a
            // bulk machine message body (Wokwi JSON, netlist) so the firmware skips the
            // line editor for it. If we kept draining this while() loop, every
            // following byte would still go through handleNormalChar (echo,
            // per-keystroke syntax-highlight re-render, history) — exactly the
            // mangling SI is meant to prevent. Returning hands the remaining bytes
            // to the raw input path: the main busy loop recomputes useLineBuffering
            // (now false), sees bytes available, and reads them char-by-char into
            // the relevant command handler (e.g. cmd_parseWokwi).
            return false;
        }

        // DC4 (0x14): Arduino presence query from the app. Answer it here so it
        // works in buffered mode too. Otherwise the line editor swallows it as a
        // bare control char, the app gets no "Y,Y" reply, reads the Arduino as
        // "not present", and pops a blocking "Flash anyway?" prompt (the "have to
        // press enter to flash" symptom). replyWithSerialInfo() on core 2 also
        // answers DC4 by peeking Serial; whichever side consumes the byte first
        // replies, and both produce the same response.
        if ( c == 0x14 ) {
            Serial.print( checkIfArduinoIsConnected( ) ? "Y," : "n," );
            Serial.println( checkArduinoPresence( ) ? "Y" : "n" );
            Serial.flush( );
            continue;
        }

        // Machine-command fast-path (buffered mode only). The app pushes Wokwi
        // netlists as bare machine commands with NO trailing newline and NO
        // separator between the 'Q' slot-query and the 'W <slot> {json}' body.
        // The interactive line editor would mash them into one un-submittable
        // "QW 0 {json}" line — echoed and syntax-highlight re-rendered on every
        // byte (the garbage you see) — and it could never dispatch as two
        // commands anyway. When such a command appears at the start of an empty
        // line, submit just the trigger char (no echo, no history) so its handler
        // runs immediately and reads any message body straight from the stream, exactly
        // as it does when line buffering is off. This makes app-pushed netlists
        // work even when the app isn't using the SI/SO line-buffering handshake.
        //   'Q' : slot query, no interactive argument form -> always fast-path.
        //   'W' : Wokwi command; has human forms ("W 2", "W file [slot]"), so only
        //         fast-path when its message body is already streaming in (a burst),
        //         which only happens for an app/paste push, never a keypress.
        if ( line_length == 0 && cursor_position == 0 &&
             ( c == 'Q' || ( c == 'W' && stream->available( ) > 8 ) ) ) {
            completed_line = String( c );
            line_ready = true;
            clearCurrentLine( );
            return true;
        }

        // Handle ANSI escape sequences
        switch ( ansi_state ) {
        case ANSI_NORMAL:
            if ( c == '\x1b' ) { // ESC
                ansi_state = ANSI_ESCAPE;
            } else {
                handleNormalChar( c );
            }
            break;

        case ANSI_ESCAPE:
            if ( c == '[' ) {
                ansi_state = ANSI_BRACKET;
            } else {
                ansi_state = ANSI_NORMAL;
                // Handle other escape sequences if needed
            }
            break;

        case ANSI_BRACKET:
            ansi_state = ANSI_NORMAL;
            switch ( c ) {
            case 'A':
                handleArrowUp( );
                break;
            case 'B':
                handleArrowDown( );
                break;
            case 'C':
                handleArrowRight( );
                break;
            case 'D':
                handleArrowLeft( );
                break;
                // Ignore other bracket sequences
            }
            break;
        
        case ANSI_MAIN_SERIAL_ENQ:
            // Handle ENQ state if needed
            ansi_state = ANSI_NORMAL;
            break;
        }

        // Return immediately if a line became ready
        if ( !line_was_ready_before && line_ready ) {
            return true;
        }
    }

    return false; // No line ready
}

bool TermControl::hasCompletedLine( ) const {
    // Check both legacy single slot and command queue
    return line_ready || queue_count > 0;
}

String TermControl::getCompletedLine( ) {
    // First check queue (relayed commands have priority)
    if ( queue_count > 0 ) {
        String result = command_queue[queue_tail];
        
        // CRITICAL: Capture response target at the SAME TIME as getting command
        last_response_target = response_targets[queue_tail];
        response_targets[queue_tail] = nullptr;  // Clear slot
        
        #if DEBUG_RELAYED_COMMANDS
        Serial.printf("TermControl::getCompletedLine(): from queue[%d]: '%s', captured response_target=%p\n",
                     queue_tail, result.c_str(), last_response_target);
        Serial.flush();
        #endif
        
        queue_tail = (queue_tail + 1) % COMMAND_QUEUE_SIZE;
        queue_count--;
        return result;
    }
    
    // Fall back to legacy single slot (user typed input - no response target)
    if ( !line_ready )
        return "";

    last_response_target = nullptr;  // User input has no specific target
    String result = completed_line;
    completed_line = "";
    line_ready = false;
    return result;
}

Stream* TermControl::getResponseTarget( ) {
    // Simply return the last captured response target
    Stream* target = last_response_target;
    last_response_target = nullptr;  // Clear after retrieval
    
    #if DEBUG_RELAYED_COMMANDS
    Serial.printf("TermControl::getResponseTarget(): returning %p\n", target);
    Serial.flush();
    #endif
    
    return target;
}

String TermControl::peekCompletedLine( ) const {
    if ( !line_ready )
        return "";
    return completed_line;
}

void TermControl::clearCompletedLine( ) {
    completed_line = "";
    line_ready = false;
}

void TermControl::relayCompletedLine( const char* line, Stream* response_target ) {
    if ( !line ) {
        return;
    }
    
    // CRITICAL: NON-BLOCKING queue-based relay
    // Add to circular buffer - drops oldest if full (better than blocking)
    
    if ( queue_count >= COMMAND_QUEUE_SIZE ) {
        // Queue full - drop this command with warning
        if ( stream ) {
            // stream->print( "⚠️  Command queue full (" );
            // stream->print( COMMAND_QUEUE_SIZE );
            // stream->print( "), dropped: '" );
            // stream->print( line );
            // stream->println( "'" );
            // stream->flush(); // Blocking flush causes latency in AsyncPassthrough!
        }
        return;
    }
    
    // Add to queue with response target
    command_queue[queue_head] = String( line );
    response_targets[queue_head] = response_target;
    queue_head = (queue_head + 1) % COMMAND_QUEUE_SIZE;
    queue_count++;
    
    // Optionally add to history (skip bulk machine input — see isBulkMachineLine)
    if ( history && strlen(line) > 0 && !isBulkMachineLine( line, (int)strlen(line) ) ) {
        history->addToHistory( String( line ) );
    }
}


const char* TermControl::getCurrentLineBuffer( ) {
    return current_line;
}

void TermControl::setPrompt( const char* prompt ) {
    if ( prompt ) {
        prompt_text = String( prompt );
        prompt_visible_cols = (int) strlen( prompt );
    } else {
        prompt_text = "";
        prompt_visible_cols = 0;
    }
}

void TermControl::enableEcho( bool enabled ) {
    echo_enabled = enabled;
}

void TermControl::setColoredPrompt( const char* prompt, int color_code ) {
    if ( prompt ) {
        char colored_prompt[ 128 ];
        snprintf( colored_prompt, sizeof( colored_prompt ), "\x1b[38;5;%dm%s\x1b[0m", color_code, prompt );
        prompt_text = String( colored_prompt );
        prompt_visible_cols = (int) strlen( prompt );   // the SGR wrapper takes no columns
    } else {
        prompt_text = "";
        prompt_visible_cols = 0;
    }
}

// Character and Line Handling
void TermControl::handleNormalChar( char c ) {
    switch ( c ) {
    case '\r':
    case '\n':
        handleEnter( );
        break;

    case '\b': // Backspace
    case 0x7F: // DEL
        handleBackspace( );
        break;

    case 0x15: // Ctrl+U - clear line
        handleCtrlU( );
        break;

    case '\t': // Tab
        handleTab( );
        break;

    case '?':
        // Version query ONLY on an empty line. A mid-line '?' must insert
        // normally or command arguments like 'i?' (RouteSafety self-check)
        // can never be typed - the old unconditional capture silently
        // turned 'i?' into 'i' + a version banner.
        if ( line_length == 0 ) {
            if (stream) {
                extern const char firmwareVersion[]; // Assume this is defined somewhere
                stream->print( "Jumperless firmware version: " );
                stream->println( firmwareVersion );
                stream->flush( );
            }
            break;
        }
        insertCharAtCursor( c );
        if ( echo_enabled && !currentLineIsMultiline( ) ) {
            renderCurrentLine( );
        }
        break;

    default:
        if ( c >= 32 && c <= 126 ) { // Printable characters
            if ( c == '{' || c == '[' || c == '(' ) {
                brace_depth++;
            } else if ( c == '}' || c == ']' || c == ')' ) {
                if ( brace_depth > 0 ) {
                    brace_depth--;
                }
            }

            insertCharAtCursor( c );
            if ( echo_enabled ) {
                // Re-render on every keystroke. The single-line redraw model
                // (\r ... \x1b[K) only breaks when the buffer actually spans
                // multiple lines (the multi-line JSON entry path that inserts
                // a literal '\n' in handleEnter). An open bracket on a single
                // line is fine to echo normally — suppressing echo there froze
                // the display until the bracket closed.
                if ( !currentLineIsMultiline( ) ) {
                    renderCurrentLine( );
                }
            }
        }
        break;
    }
}

void TermControl::handleBackspace( ) {
    if ( cursor_position > 0 ) {
        cursor_position--;

        // Keep brace_depth in sync with the buffer contents so the multi-line
        // submit heuristic in handleEnter doesn't get stuck if a bracket is
        // edited out of the line.
        char removed = current_line[ cursor_position ];
        if ( removed == '{' || removed == '[' || removed == '(' ) {
            if ( brace_depth > 0 ) {
                brace_depth--;
            }
        } else if ( removed == '}' || removed == ']' || removed == ')' ) {
            brace_depth++;
        }

        if ( cursor_position < line_length ) {
            memmove( &current_line[ cursor_position ],
                     &current_line[ cursor_position + 1 ],
                     line_length - cursor_position - 1 );
        }
        line_length--;
        display_length--;
        current_line[ line_length ] = '\0';

        if ( echo_enabled && !currentLineIsMultiline( ) ) {
            renderCurrentLine( );
        }
    }
}

void TermControl::handleEnter( ) {
    // Get pending response target from Jerial (set by AsyncPassthrough)
    Stream* pending_target = Jerial.getPendingResponseTarget();
    
    #if DEBUG_RELAYED_COMMANDS
    Serial.printf("TermControl::handleEnter(): line_length=%d, pending_response_target=%p\n",
                 line_length, pending_target);
    Serial.flush();
    #endif
    
    // Decide whether this Enter should HOLD the current line for multi-line
    // continuation instead of submitting it. This only applies to raw,
    // non-command input that still has an unclosed bracket while line buffering
    // is enabled (e.g. pasting a multi-line JSON object). Lines that start with
    // a command letter (like "W{...}" or Python "print(") always submit, even
    // with an open bracket, so they are processed / reported rather than
    // silently swallowed.
    extern struct config jumperlessConfig;
    bool holdForContinuation = false;
    if ( pending_target == nullptr &&
         jumperlessConfig.display.terminal_line_buffering == 1 &&
         brace_depth > 0 && line_length > 0 &&
         line_length < JERIAL_MAX_LINE_LENGTH - 1 ) {
        // Only hold for multi-line continuation when the line is a raw
        // bracketed structure being typed/pasted (a JSON object/array), e.g.
        // pasting a diagram.json that begins with '{'. EVERY command-prefixed
        // line submits on Enter instead — both letter commands ("W{...}") and
        // the Python prefix (">print("). Previously the test was "first char
        // is not a letter", which wrongly swept ">"-prefixed Python into the
        // multi-line buffer, so an unbalanced "print(" was never executed.
        char firstChar = current_line[0];
        if ( firstChar == '{' || firstChar == '[' ) {
            holdForContinuation = true;
        }
    }

    // Echo the line break that moves the cursor off the input line before any
    // command output (or the next prompt) is printed. Suppress it only while
    // holding the line for multi-line continuation, since the user is still
    // composing the same logical input. Previously this was gated on
    // brace_depth == 0, so a submitted-but-unbalanced command line (e.g.
    // "print(") printed its output glued to the end of the input line.
    if ( echo_enabled && stream && !holdForContinuation ) {
        // This newline scrolls the terminal; the port-1 linefeed counter in
        // ReadingDisplay sees it and the live reading re-pins fresh rows below
        // the command output on its next paint. The dedupe reset just makes
        // that next paint happen even if the value hasn't changed.
        ReadingDisplay::resetLastShown( );
        stream->print( JERIAL_NEWLINE_OUT );
        stream->flush( );
    }

    // Add to history if we have a history manager and non-empty line, but skip
    // bulk machine input (Wokwi JSON / netlist pastes) so it doesn't flush the
    // user's real command history out of the small ring buffer.
    if ( history && line_length > 0 ) {
        current_line[ line_length ] = '\0';
        if ( !isBulkMachineLine( current_line, line_length ) ) {
            history->addToHistory( String( current_line ) );
        }
    }

    // If there's a pending response target, queue this line with the target
    // (This happens when line is assembled from character-by-character relay)
    if ( pending_target != nullptr && line_length > 0 ) {
        current_line[ line_length ] = '\0';
        
        // Add to queue with response target instead of using completed_line
        if ( queue_count < COMMAND_QUEUE_SIZE ) {
            #if DEBUG_RELAYED_COMMANDS
            Serial.printf("TermControl::handleEnter(): Queuing '%s' with response_target=%p (queue slot %d)\n",
                         current_line, pending_target, queue_head);
            Serial.flush();
            #endif
            
            command_queue[queue_head] = String( current_line );
            response_targets[queue_head] = pending_target;
            queue_head = (queue_head + 1) % COMMAND_QUEUE_SIZE;
            queue_count++;
        } else {
            #if DEBUG_RELAYED_COMMANDS
            Serial.printf("⚠️  TermControl::handleEnter(): Queue full, dropping '%s'\n", current_line);
            Serial.flush();
            #endif
        }
        
        // Clear the pending target in Jerial
        Jerial.clearPendingResponseTarget();
        clearCurrentLine( );
        return;
    }

    // Hold the line for multi-line continuation (raw bracketed input).
    if ( holdForContinuation ) {
        insertCharAtCursor( '\n' );
        return;
    }

    // Make line available for parsing (normal user input)
    completed_line = String( current_line );
    line_ready = true;

    // Reset current line
    clearCurrentLine( );
}

void TermControl::handleArrowUp( ) {
    if ( !history )
        return;

    String prev = history->getPreviousCommand( );
    if ( prev.length( ) > 0 && prev.length( ) < JERIAL_MAX_LINE_LENGTH ) {
        strcpy( current_line, prev.c_str( ) );
        line_length = prev.length( );
        cursor_position = line_length;
        if ( echo_enabled ) {
            renderCurrentLine( );
        }
    }
}

void TermControl::handleArrowDown( ) {
    if ( !history )
        return;

    String next = history->getNextCommand( );
    if ( next.length( ) == 0 ) {
        clearCurrentLine( );
    } else if ( next.length( ) < JERIAL_MAX_LINE_LENGTH ) {
        strcpy( current_line, next.c_str( ) );
        line_length = next.length( );
        cursor_position = line_length;
    }

    if ( echo_enabled ) {
        renderCurrentLine( );
    }
}

void TermControl::handleArrowLeft( ) {
    if ( cursor_position > 0 ) {
        cursor_position--;
        if ( echo_enabled ) {
            moveCursorTo( cursor_position );
        }
    }
}

void TermControl::handleArrowRight( ) {
    if ( cursor_position < line_length ) {
        cursor_position++;
        if ( echo_enabled ) {
            moveCursorTo( cursor_position );
        }
    }
}

void TermControl::handleCtrlU( ) {
    clearCurrentLine( );
    if ( echo_enabled ) {
        renderCurrentLine( );
    }
}

void TermControl::handleTab( ) {
    // Could implement tab completion here in the future
    // For now, just ignore
}

void TermControl::handleMainSerialENQ( ) {
    replyWithSerialInfo( 1 );
}

// Terminal Rendering and Cursor Management
void TermControl::renderCurrentLine( ) {
    if ( !stream || !echo_enabled )
        return;

    // Create clean string from current line buffer
    char clean_line[ JERIAL_MAX_LINE_LENGTH + 1 ];
    memcpy( clean_line, current_line, line_length );
    clean_line[ line_length ] = '\0';

    // Get highlighted version from SyntaxHighlighting
    syntax_highlighter.setStream( stream );
    char* highlighted = syntax_highlighter.highlightString( clean_line, SYNTAX_JUMPERLESS_CONNECTIONS );

    // Store in display buffer for cursor calculations
    if ( highlighted ) {
        strncpy( display_buffer, highlighted, sizeof( display_buffer ) - 1 );
        display_buffer[ sizeof( display_buffer ) - 1 ] = '\0';
    } else {
        strncpy( display_buffer, clean_line, sizeof( display_buffer ) - 1 );
        display_buffer[ sizeof( display_buffer ) - 1 ] = '\0';
    }

    // Render the line
    stream->print( '\r' );

    // Print prompt if we have one
    if ( prompt_text.length( ) > 0 ) {
        stream->print( prompt_text );
    }

    // Print the content (either highlighted or clean)
    if ( line_length > 0 ) {
        if ( highlighted && strlen( highlighted ) > 0 ) {
            stream->print( highlighted );
            display_length = calculateVisualLength( String( highlighted ) );
        } else {
            stream->print( clean_line );
            display_length = line_length;
        }
    } else {
        display_length = 0;
    }

    // Clear to end of line to erase old characters
    stream->print( "\x1b[K" );

    // Position cursor correctly using display length
    moveCursorToPosition( cursor_position );

    stream->flush( );
}

void TermControl::clearCurrentLine( ) {
    memset( current_line, 0, sizeof( current_line ) );
    memset( display_buffer, 0, sizeof( display_buffer ) );
    line_length = 0;
    display_length = 0;
    cursor_position = 0;
    brace_depth = 0;
}

void TermControl::moveCursorTo( int position ) {
    if ( !stream || !echo_enabled )
        return;

    int visual_prompt_len = calculateVisualLength( prompt_text );
    int target_column = visual_prompt_len + position + 1;

    stream->print( '\r' );
    if ( target_column > 1 ) {
        stream->print( "\x1b[" );
        stream->print( target_column );
        stream->print( 'G' );
    }
    stream->flush( );
}

bool TermControl::currentLineIsMultiline( ) const {
    for ( int i = 0; i < line_length; i++ ) {
        if ( current_line[ i ] == '\n' ) {
            return true;
        }
    }
    return false;
}

int TermControl::calculateVisualLength( const String& text ) {
    int visual_len = 0;
    bool in_ansi = false;

    for ( int i = 0; i < text.length( ); i++ ) {
        char c = text.charAt( i );
        if ( c == '\x1b' ) {
            in_ansi = true;
        } else if ( in_ansi && c == 'm' ) {
            in_ansi = false;
        } else if ( !in_ansi ) {
            visual_len++;
        }
    }

    return visual_len;
}

void TermControl::moveCursorToPosition( int position ) {
    if ( !stream || !echo_enabled )
        return;

    int visual_prompt_len = calculateVisualLength( prompt_text );
    int visual_position = position;
    int target_column = visual_prompt_len + visual_position + 1;

    stream->print( '\r' );
    if ( target_column > 1 ) {
        stream->print( "\x1b[" );
        stream->print( target_column );
        stream->print( 'G' );
    }
    stream->flush( );
}

void TermControl::insertCharAtCursor( char c ) {
    if ( line_length >= JERIAL_MAX_LINE_LENGTH - 1 ) {
        if ( stream && echo_enabled ) {
            stream->print( "\x07" ); // Bell sound
            stream->flush( );
        }
        return;
    }

    if ( cursor_position < line_length ) {
        memmove( &current_line[ cursor_position + 1 ],
                 &current_line[ cursor_position ],
                 line_length - cursor_position );
    }

    current_line[ cursor_position ] = c;
    cursor_position++;
    line_length++;
    display_length++;
}

void TermControl::deleteCharAtCursor( ) {
    if ( cursor_position >= line_length ) {
        return;
    }

    memmove( &current_line[ cursor_position ],
             &current_line[ cursor_position + 1 ],
             line_length - cursor_position - 1 );

    line_length--;
    display_length--;
    current_line[ line_length ] = '\0';
}

// ============================================================================
// JerialClass Implementation
// ============================================================================

// Global Jerial instance
JerialClass Jerial;

JerialClass::JerialClass()
    : output_count(0),
      broadcast_enabled(true),
      input_stream(nullptr),
      input_source_count(0),
      term_control(nullptr),
      term_control_active(false),
      pending_response_target(nullptr),
      current_response_target(nullptr),
      relay_read_pos(0),
      relay_write_pos(0),
      tag_buffer_pos(0),
      in_tag(false),
      relay_stream(nullptr),
      mux_stream(nullptr) {
    // Initialize arrays
    for (int i = 0; i < JERIAL_MAX_OUTPUTS; i++) {
        output_streams[i] = nullptr;
        input_sources[i] = nullptr;
    }
    memset(relay_buffer, 0, sizeof(relay_buffer));
    memset(tag_buffer, 0, sizeof(tag_buffer));
    
    // Create relay stream wrapper (with tag filtering)
    relay_stream = new RelayBufferStream(
        (uint8_t*)relay_buffer,
        sizeof(relay_buffer),
        &relay_read_pos,
        &relay_write_pos
    );
    
    // Debug: Confirm relay stream created
    // Note: Can't Serial.println here - Serial not initialized yet!
}

JerialClass::~JerialClass() {
    destroyTermControl();
    if (relay_stream) {
        delete relay_stream;
        relay_stream = nullptr;
    }
    if (mux_stream) {
        delete mux_stream;
        mux_stream = nullptr;
    }
}

// ============================================================================
// Stream Interface Implementation
// ============================================================================
#define DEBUG_JERIAL 0
int JerialClass::available() {
    // CRITICAL: Check RelayBufferStream first (with tag filtering!)
    // This works regardless of line buffering mode
    if (relay_stream && relay_stream->available() > 0) {
        int avail = relay_stream->available();
        #if DEBUG_JERIAL == 1
        Serial.printf("Jerial.available(): relay_stream has %d chars\n", avail);
        Serial.flush();
        #endif
        return avail;
    }
    
    // If terminal control is active, check it
    if (term_control_active && term_control) {
        return term_control->available();
    }
    
    // Check actual input stream
    if (!input_stream) {
        return 0;
    }
    return input_stream->available();
}

int JerialClass::read() {
    // CRITICAL: Read from RelayBufferStream first (with tag filtering!)
    // This works regardless of line buffering mode
    if (relay_stream && relay_stream->available() > 0) {
        int c = relay_stream->read();
        if (c >= 0) {
            #if DEBUG_JERIAL == 1
            Serial.printf("Jerial.read(): got '%c' (%d) from relay_stream\n", (char)c, c);
            Serial.flush();
            #endif
            return c;
        }
    }
    
    // If terminal control is active, read through it
    if (term_control_active && term_control) {
        return term_control->read();
    }
    
    // Read from actual input stream
    // User-typed input doesn't have tags
    if (!input_stream) {
        return -1;
    }
    
    return input_stream->read();
}

int JerialClass::peek() {
    // Peek at relay buffer first
    if (relay_read_pos != relay_write_pos) {
        return relay_buffer[relay_read_pos];
    }
    
    // If terminal control is active, peek through it
    if (term_control_active && term_control) {
        return term_control->peek();
    }
    
    // Peek at actual input stream
    if (!input_stream) {
        return -1;
    }
    return input_stream->peek();
}

void JerialClass::flush() {
    // CRITICAL FIX: Don't call output_streams[i]->flush() directly!
    // For USB CDC streams, flush() can block INDEFINITELY if the USB host
    // stops reading (which happens during rapid command processing when
    // the host's USB buffers fill up). This causes Core 0 to freeze.
    //
    // Instead, we service USB non-blocking with tud_task() which sends
    // any pending data without blocking. The USB protocol guarantees
    // eventual delivery without requiring blocking waits.
    //#ifdef USE_TINYUSB
    // (raw tud_task() is never called from here; the mutex-guarded pump is yield())
    // Service USB multiple times to give it a chance to transmit
    // for (int i = 0; i < 3; i++) {
    //     tud_task();
    //     delayMicroseconds(50);  // Small yield between calls
    // }

    // Serial.flush();
    
    // tud_task();
    // #else
    // Non-TinyUSB fallback: call flush() but this path is rarely used
    for (int i = 0; i < output_count; i++) {
        if (output_streams[i]) {
            output_streams[i]->flush();
        }
    }
    //#endif
}

size_t JerialClass::write(uint8_t byte) {
    return writeToOutputs(byte);
}

size_t JerialClass::write(const uint8_t *buffer, size_t size) {
    return writeToOutputs(buffer, size);
}

int JerialClass::availableForWrite() {
    if (output_count == 0) {
        return 0;
    }
    
    int min_available = INT_MAX;
    for (int i = 0; i < output_count; i++) {
        if (output_streams[i]) {
            int avail = 128; // Conservative default
            
            // Check if it's a known CDC endpoint
            if (output_streams[i] == &Serial || 
                output_streams[i] == &USBSer1 || 
                output_streams[i] == &USBSer2 || 
                output_streams[i] == &USBSer3) {
                Adafruit_USBD_CDC* cdc = static_cast<Adafruit_USBD_CDC*>(output_streams[i]);
                avail = cdc->availableForWrite();
            }
            
            if (avail < min_available) {
                min_available = avail;
            }
        }
    }
    
    return (min_available == INT_MAX) ? 0 : min_available;
}

// ============================================================================
// Terminal Control Integration
// ============================================================================

bool JerialClass::service() {
    // CRITICAL: If there's an relayed command pending, DON'T process it here!
    // Let RelayedCommandService handle it directly for faster execution.
    // This prevents both paths from trying to consume the same relay buffer.
    // if (hasRelayedCommand) {
    //     // Don't process relay buffer - RelayedCommandService will handle it
    //     // Only process regular user input from Serial
    //     if (term_control_active && term_control && input_stream && input_stream->available() > 0) {
    //         // There's user input, process just that (not relay buffer)
    //         // But TermControl reads from MultiSourceStream which includes relay...
    //         // So we skip entirely when relay is pending
    //     }
    //     return false;  // Signal that RelayedCommandService should handle this
    // }
    
    // TermControl reads from MultiSourceStream which automatically checks:
    // 1. RelayBufferStream (priority) - for AsyncPassthrough commands
    // 2. Real input stream (Serial) - for user input
    // No manual switching needed!

    static uint32_t last_debug = 0;
    #if DEBUG_JERIAL == 1
    if (millis() - last_debug > 1000) {
        last_debug = millis();
        Serial.printf("Jerial.service(): term_control_active=%d, term_control=%p, relay_stream=%p, mux_stream=%p\n",
                      term_control_active, term_control, relay_stream, mux_stream);
        Serial.printf("  Relay buffer: r=%d w=%d (has data: %d)\n",
                      relay_read_pos, relay_write_pos, relay_read_pos != relay_write_pos);
        Serial.flush();
    }
    #endif
    // USBSer3 backchannel - handled by SingleCharCommands
    singleCharCommands.serviceUSBSer3();

    if (term_control_active && term_control && jumperlessConfig.display.terminal_line_buffering == 1) {
        return term_control->service();
    }
    #if DEBUG_JERIAL == 1
    Serial.println("⚠️  TermControl not active!");
    Serial.flush();
    #endif
    return false;
}

bool JerialClass::hasCompletedLine() const {
    if (term_control_active && term_control) {
        return term_control->hasCompletedLine();
    }
    return false;
}

String JerialClass::getCompletedLine() {
    if (term_control_active && term_control) {
        return term_control->getCompletedLine();
    }
    return "";
}

String JerialClass::peekCompletedLine() const {
    if (term_control_active && term_control) {
        return term_control->peekCompletedLine();
    }
    return "";
}

void JerialClass::clearCompletedLine() {
    if (term_control_active && term_control) {
        term_control->clearCompletedLine();
    }
}

void JerialClass::relayCompletedLine(const char* line, Stream* response_target) {
    if (term_control_active && term_control) {
        term_control->relayCompletedLine(line, response_target);
    }
}

Stream* JerialClass::getResponseTarget() {
    // CRITICAL: Check current_response_target first (set by RelayedCommandService for fast-path)
    // This handles commands from UART that bypass TermControl's queue
    if (current_response_target != nullptr) {
        Stream* target = current_response_target;
        current_response_target = nullptr;  // Consume it
        
        #if DEBUG_RELAYED_COMMANDS
        Serial.printf("JerialClass::getResponseTarget(): returning current_response_target=%p\n", target);
        Serial.flush();
        #endif
        
        return target;
    }
    
    // Fall back to TermControl's queue-based response routing
    if (term_control_active && term_control) {
        return term_control->getResponseTarget();
    }
    return nullptr;
}

void JerialClass::setPendingResponseTarget(Stream* target) {
    #if DEBUG_RELAYED_COMMANDS
    Serial.printf("JerialClass::setPendingResponseTarget(%p)\n", target);
    Serial.flush();
    #endif
    
    pending_response_target = target;
}

Stream* JerialClass::getPendingResponseTarget() {
    return pending_response_target;
}

void JerialClass::clearPendingResponseTarget() {
    pending_response_target = nullptr;
}

void JerialClass::setCurrentResponseTarget(Stream* target) {
    #if DEBUG_RELAYED_COMMANDS
    Serial.printf("JerialClass::setCurrentResponseTarget(%p)\n", target);
    Serial.flush();
    #endif
    current_response_target = target;
}

void JerialClass::clearCurrentResponseTarget() {
    current_response_target = nullptr;
}

const char* JerialClass::getCurrentLineBuffer() {
    if (term_control_active && term_control) {
        return term_control->getCurrentLineBuffer();
    }
    return "";
}

void JerialClass::redrawInputLine() {
    if (term_control_active && term_control) {
        term_control->renderCurrentLine();
    }
}

int JerialClass::getInputLineColumns() {
    if (term_control_active && term_control) {
        return term_control->getPromptVisibleWidth() + term_control->getLineLength();
    }
    return 0;
}

void JerialClass::setPrompt(const char* prompt) {
    if (term_control_active && term_control) {
        term_control->setPrompt(prompt);
    }
}

void JerialClass::setColoredPrompt(const char* prompt, int color_code) {
    if (term_control_active && term_control) {
        term_control->setColoredPrompt(prompt, color_code);
    }
}

void JerialClass::enableEcho(bool enabled) {
    if (term_control_active && term_control) {
        term_control->enableEcho(enabled);
    }
}

ScriptHistory* JerialClass::getHistory() {
    if (term_control_active && term_control) {
        return term_control->getHistory();
    }
    return nullptr;
}

// ============================================================================
// Output Routing
// ============================================================================

bool JerialClass::addOutputStream(Stream* stream) {
    if (!stream) {
        return false;
    }
    
    // Check if already registered
    if (hasOutputStream(stream)) {
        return true;
    }
    
    // Check if we have space
    if (output_count >= JERIAL_MAX_OUTPUTS) {
        return false;
    }
    
    // Add to array
    output_streams[output_count++] = stream;
    return true;
}

bool JerialClass::addOutputStream(JerialEndpoint endpoint) {
    Stream* stream = getStreamForEndpoint(endpoint);
    if (!stream) {
        return false;
    }
    return addOutputStream(stream);
}

bool JerialClass::removeOutputStream(Stream* stream) {
    if (!stream) {
        return false;
    }
    
    bool found = false;
    for (int i = 0; i < output_count; i++) {
        if (output_streams[i] == stream) {
            found = true;
            for (int j = i; j < output_count - 1; j++) {
                output_streams[j] = output_streams[j + 1];
            }
            output_streams[output_count - 1] = nullptr;
            output_count--;
            break;
        }
    }
    
    return found;
}

bool JerialClass::removeOutputStream(JerialEndpoint endpoint) {
    Stream* stream = getStreamForEndpoint(endpoint);
    if (!stream) {
        return false;
    }
    return removeOutputStream(stream);
}

void JerialClass::clearOutputStreams() {
    for (int i = 0; i < JERIAL_MAX_OUTPUTS; i++) {
        output_streams[i] = nullptr;
    }
    output_count = 0;
}

void JerialClass::setOutputStream(Stream* stream) {
    clearOutputStreams();
    addOutputStream(stream);
}

void JerialClass::setOutputStream(JerialEndpoint endpoint) {
    clearOutputStreams();
    addOutputStream(endpoint);
}

bool JerialClass::hasOutputStream(Stream* stream) const {
    for (int i = 0; i < output_count; i++) {
        if (output_streams[i] == stream) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Input Routing
// ============================================================================

void JerialClass::setInputStream(Stream* stream) {

    #if DEBUG_JERIAL == 1
    Serial.printf("setInputStream(%p) called\n", stream);
    Serial.flush();
    #endif
    input_stream = stream;
    
    // Create or destroy terminal control based on endpoint type
    JerialEndpoint endpoint = getEndpointType(stream);
    #if DEBUG_JERIAL == 1
        Serial.printf("  Endpoint type: %d, supports terminal: %d\n", (int)endpoint, supportsTerminalControl(endpoint));
        Serial.flush();
    #endif
    
    if (supportsTerminalControl(endpoint)) {
        createTermControlIfNeeded(stream);
        
        // If this is a secondary USB serial port, automatically set it as the response target
        // for any commands received on it. Main Serial (USB_SERIAL) typically broadcasts.
        if (endpoint == JerialEndpoint::USB_SER1 || 
            endpoint == JerialEndpoint::USB_SER2 || 
            endpoint == JerialEndpoint::USB_SER3) {
            setPendingResponseTarget(stream);
        } else if (endpoint == JerialEndpoint::USB_SERIAL) {
            clearPendingResponseTarget();
        }
    } else {
        destroyTermControl();
    }
}

void JerialClass::setInputStream(JerialEndpoint endpoint) {
    input_stream = getStreamForEndpoint(endpoint);
    
    // Create or destroy terminal control based on endpoint type
    if (supportsTerminalControl(endpoint)) {
        createTermControlIfNeeded(input_stream);
    } else {
        destroyTermControl();
    }
}

bool JerialClass::addInputSource(Stream* stream) {
    if (!stream) {
        return false;
    }
    
    // Check if already registered
    for (int i = 0; i < input_source_count; i++) {
        if (input_sources[i] == stream) {
            return true;
        }
    }
    
    // Check if we have space
    if (input_source_count >= JERIAL_MAX_OUTPUTS) {
        return false;
    }
    
    // Add to array
    input_sources[input_source_count++] = stream;
    return true;
}

bool JerialClass::addInputSource(JerialEndpoint endpoint) {
    Stream* stream = getStreamForEndpoint(endpoint);
    if (!stream) {
        return false;
    }
    return addInputSource(stream);
}

bool JerialClass::serviceInputs() {
    // Check all registered input sources for available data (in priority order)
    // Relay stream should be first in the list (added in main)
    for (int i = 0; i < input_source_count; i++) {
        if (input_sources[i] && input_sources[i]->available() > 0) {
            // Switch to this input source if it has data
            if (input_stream != input_sources[i]) {
                setInputStream(input_sources[i]);
            }
            return true;
        }
    }
    return false;
}

bool JerialClass::relayInput(const char* data, bool strip_tags) {
    // Note: strip_tags parameter is legacy - RelayBufferStream now handles tag filtering on read
    (void)strip_tags;  // Suppress unused parameter warning
    
    if (!data) {
        return false;
    }
    
    return relayInput((const uint8_t*)data, strlen(data), false);
}

bool JerialClass::relayInput(const uint8_t* data, size_t size, bool strip_tags) {
    // Note: strip_tags parameter is legacy - RelayBufferStream now handles tag filtering on read
    (void)strip_tags;  // Suppress unused parameter warning
    
    if (!data || size == 0) {
        return false;
    }
    
    // Write raw data to relay buffer - RelayBufferStream will filter tags on read
    for (size_t i = 0; i < size; i++) {
        uint16_t next_write = (relay_write_pos + 1) % sizeof(relay_buffer);
        
        if (next_write == relay_read_pos) {
            // Serial.printf("⚠️  Relay buffer full! Dropped %zu bytes\n", size - i);
            // Serial.flush();
            return false;  // Buffer full
        }
        
        relay_buffer[relay_write_pos] = data[i];
        
        relay_write_pos = next_write;
    }

    // Note: hasRelayedCommand flag is set by AsyncPassthrough when closing tag </j> is detected
    // This ensures the flag is only set when a complete command (with closing tag) is ready,
    // not on every character relay that happens to end with \n
    
    // Debug: Confirm relay (disabled for performance)
    // Serial.printf("✓ Relayed %zu chars (buffer: %d -> %d)\n", size, relay_read_pos, relay_write_pos);
    // Serial.flush();
    
    return true;
}

void JerialClass::clearRelayedInput() {
    relay_read_pos = 0;
    relay_write_pos = 0;
    memset(relay_buffer, 0, sizeof(relay_buffer));
}

/**
 * Fast, thread-safe check for complete line in relay buffer
 * Scans buffer for newline without consuming data
 */
bool JerialClass::hasRelayedCompleteLine() const {
    // Read positions once for thread safety
    uint16_t read_pos = relay_read_pos;
    uint16_t write_pos = relay_write_pos;
    
    if (read_pos == write_pos) {
        return false;  // Buffer empty
    }
    
    // Scan buffer for newline
    uint16_t pos = read_pos;
    while (pos != write_pos) {
        if (relay_buffer[pos] == '\n') {
            return true;  // Found complete line
        }
        pos = (pos + 1) % sizeof(relay_buffer);
    }
    
    return false;  // No newline found
}

/**
 * Extract complete line directly from relay buffer (fast path)
 * Bypasses slow TermControl processing for immediate command execution
 * Thread-safe: modifies read position atomically
 * 
 * NOTE: AsyncPassthrough already filters <j> and </j> tags before relay,
 * so we don't need tag filtering here. Just read characters directly.
 */
String JerialClass::getRelayedCompleteLine() {
    String line;
    line.reserve(128);  // Pre-allocate for performance
    
    // Read positions once for thread safety
    uint16_t read_pos = relay_read_pos;
    uint16_t write_pos = relay_write_pos;
    
    if (read_pos == write_pos) {
        return line;  // Buffer empty
    }
    
    #if DEBUG_RELAYED_COMMANDS
    // Debug: Show buffer contents
    Serial.printf("DEBUG getRelayedCompleteLine: read=%d write=%d\n", read_pos, write_pos);
    Serial.print("  Buffer contents (first 100 chars): [");
    uint16_t debug_pos = read_pos;
    int char_count = 0;
    while (debug_pos != write_pos && char_count < 100) {
        char c = relay_buffer[debug_pos];
        if (c >= 32 && c < 127) {
            Serial.print(c);
        } else if (c == '\n') {
            Serial.print("\\n");
        } else if (c == '\r') {
            Serial.print("\\r");
        } else {
            Serial.printf("<%02X>", (unsigned char)c);
        }
        debug_pos = (debug_pos + 1) % sizeof(relay_buffer);
        char_count++;
    }
    Serial.println("]");
    Serial.flush();
    #endif
    
    // Extract characters until newline
    // AsyncPassthrough has already filtered tags, so just read directly
    while (read_pos != write_pos) {
        char c = relay_buffer[read_pos];
        read_pos = (read_pos + 1) % sizeof(relay_buffer);
        
        // Check for newline (line complete)
        if (c == '\n') {
            // Update read position atomically
            relay_read_pos = read_pos;
            break;
        }
        
        // Add character to line (skip carriage returns)
        if (c != '\r') {
            line += c;
        }
    }
    
    #if DEBUG_RELAYED_COMMANDS
    Serial.printf("  Extracted line: \"%s\"\n", line.c_str());
    Serial.flush();
    #endif
    
    return line;
}

// ============================================================================
// Tag Stripping
// ============================================================================

// ============================================================================
// Internal TermControl Management
// ============================================================================

void JerialClass::createTermControlIfNeeded(Stream* stream) {
    if (!stream) {
        return;
    }
    
    // CRITICAL: Always recreate MultiSourceStream to ensure it wraps the current stream
    // Delete old mux_stream if it exists
    if (mux_stream) {
        delete mux_stream;
        mux_stream = nullptr;
    }
    
    // Create MultiSourceStream that prioritizes relay over the real stream
    mux_stream = new MultiSourceStream(relay_stream, stream);
    // Serial.println("✓ MultiSourceStream created (relay + Serial)");
    // Serial.flush();
    
    // CRITICAL: Recreate TermControl to point at the new mux_stream
    // Otherwise it keeps reading from the old stream!
    if (term_control) {
        // Serial.println("⚠️  Recreating TermControl with mux_stream");
        // Serial.flush();
        delete term_control;
        term_control = nullptr;
    }
    
    // Create terminal control with the multiplexed stream
    // TermControl reads from mux_stream, which checks relay first, then Serial
    term_control = new TermControl(mux_stream, true);
    term_control_active = true;
    
    // Serial.println("✓ TermControl created/updated with mux_stream");
    // Serial.flush();
}

void JerialClass::destroyTermControl() {
    if (term_control) {
        delete term_control;
        term_control = nullptr;
    }
    term_control_active = false;
}

// ============================================================================
// Static Helper Functions
// ============================================================================

JerialEndpoint JerialClass::getEndpointType(Stream* stream) {
    if (!stream) {
        return JerialEndpoint::NONE;
    }
    
    if (stream == &Serial) {
        return JerialEndpoint::USB_SERIAL;
    } else if (stream == &USBSer1) {
        return JerialEndpoint::USB_SER1;
    } else if (stream == &USBSer2) {
        return JerialEndpoint::USB_SER2;
    } else if (stream == &USBSer3) {
        return JerialEndpoint::USB_SER3;
    } else if (stream == &Serial1) {
        return JerialEndpoint::SERIAL1;
    } else if (stream == &OLEDOut) {
        return JerialEndpoint::OLED;
    }
    return JerialEndpoint::CUSTOM;
}

Stream* JerialClass::getStreamForEndpoint(JerialEndpoint endpoint) {
    switch (endpoint) {
        case JerialEndpoint::USB_SERIAL:
            return &Serial;
        case JerialEndpoint::USB_SER1:
            return &USBSer1;
        case JerialEndpoint::USB_SER2:
            return &USBSer2;
        case JerialEndpoint::USB_SER3:
            return &USBSer3;
        case JerialEndpoint::SERIAL1:
            return &Serial1;
        case JerialEndpoint::OLED:
            return &OLEDOut;
        case JerialEndpoint::UART0:
        case JerialEndpoint::UART1:
            return nullptr; // User must create UARTStream instances
        default:
            return nullptr;
    }
}

const char* JerialClass::endpointToString(JerialEndpoint endpoint) {
    switch (endpoint) {
        case JerialEndpoint::NONE:        return "NONE";
        case JerialEndpoint::USB_SERIAL:  return "USB_SERIAL";
        case JerialEndpoint::USB_SER1:    return "USB_SER1";
        case JerialEndpoint::USB_SER2:    return "USB_SER2";
        case JerialEndpoint::USB_SER3:    return "USB_SER3";
        case JerialEndpoint::SERIAL1:     return "SERIAL1";
        case JerialEndpoint::UART0:       return "UART0";
        case JerialEndpoint::UART1:       return "UART1";
        case JerialEndpoint::OLED:        return "OLED";
        case JerialEndpoint::CUSTOM:      return "CUSTOM";
        default:                          return "UNKNOWN";
    }
}

bool JerialClass::supportsTerminalControl(JerialEndpoint endpoint) {
    switch (endpoint) {
        case JerialEndpoint::USB_SERIAL:
        case JerialEndpoint::USB_SER1:
        case JerialEndpoint::USB_SER2:
        case JerialEndpoint::USB_SER3:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// Direct Print Methods - Override normal routing
// ============================================================================

size_t JerialClass::printTo(JerialEndpoint endpoint, const char* data) {
    Stream* stream = getStreamForEndpoint(endpoint);
    if (!stream || !data) {
        return 0;
    }
    return stream->print(data);
}

size_t JerialClass::printTo(JerialEndpoint endpoint, const String& data) {
    Stream* stream = getStreamForEndpoint(endpoint);
    if (!stream) {
        return 0;
    }
    return stream->print(data);
}

size_t JerialClass::printTo(JerialEndpoint endpoint, uint8_t byte) {
    Stream* stream = getStreamForEndpoint(endpoint);
    if (!stream) {
        return 0;
    }
    return stream->write(byte);
}

size_t JerialClass::printTo(JerialEndpoint endpoint, const uint8_t* buffer, size_t size) {
    Stream* stream = getStreamForEndpoint(endpoint);
    if (!stream || !buffer || size == 0) {
        return 0;
    }
    return stream->write(buffer, size);
}

size_t JerialClass::printTo(Stream* stream, const char* data) {
    if (!stream || !data) {
        return 0;
    }
    return stream->print(data);
}

size_t JerialClass::printTo(Stream* stream, const String& data) {
    if (!stream) {
        return 0;
    }
    return stream->print(data);
}

size_t JerialClass::printTo(Stream* stream, uint8_t byte) {
    if (!stream) {
        return 0;
    }
    return stream->write(byte);
}

size_t JerialClass::printTo(Stream* stream, const uint8_t* buffer, size_t size) {
    if (!stream || !buffer || size == 0) {
        return 0;
    }
    return stream->write(buffer, size);
}

size_t JerialClass::printlnTo(JerialEndpoint endpoint, const char* data) {
    Stream* stream = getStreamForEndpoint(endpoint);
    if (!stream) {
        return 0;
    }
    return stream->println(data);
}

size_t JerialClass::printlnTo(JerialEndpoint endpoint, const String& data) {
    Stream* stream = getStreamForEndpoint(endpoint);
    if (!stream) {
        return 0;
    }
    return stream->println(data);
}

size_t JerialClass::printlnTo(Stream* stream, const char* data) {
    if (!stream) {
        return 0;
    }
    return stream->println(data);
}

size_t JerialClass::printlnTo(Stream* stream, const String& data) {
    if (!stream) {
        return 0;
    }
    return stream->println(data);
}

// ============================================================================
// Internal Write Functions
// ============================================================================

size_t JerialClass::writeToOutputs(uint8_t byte) {
    if (output_count == 0) {
        return 0;
    }
    
    size_t written = 0;
    
    if (broadcast_enabled) {
        for (int i = 0; i < output_count; i++) {
            if (output_streams[i]) {
                size_t result = output_streams[i]->write(byte);
                if (i == 0) {
                    written = result;
                }
            }
        }
    } else {
        if (output_streams[0]) {
            written = output_streams[0]->write(byte);
        }
    }
    
    return written;
}

size_t JerialClass::writeToOutputs(const uint8_t *buffer, size_t size) {
    if (output_count == 0 || !buffer || size == 0) {
        return 0;
    }
    
    size_t written = 0;
    
    if (broadcast_enabled) {
        for (int i = 0; i < output_count; i++) {
            if (output_streams[i]) {
                size_t result = output_streams[i]->write(buffer, size);
                if (i == 0) {
                    written = result;
                }
            }
        }
    } else {
        if (output_streams[0]) {
            written = output_streams[0]->write(buffer, size);
        }
    }
    
    return written;
}

// ============================================================================
// Line Ending Normalization
// ============================================================================
// When terminal_line_buffering is enabled and the user connects via a terminal
// like Tabby (rather than the Jumperless App which auto-normalizes), bare \n
// characters don't produce proper line breaks. This converts \n to \r\n.

size_t JerialClass::printNormalized(const String& str) {
    return printNormalized(str.c_str());
}

size_t JerialClass::printNormalized(const char* str) {
    if (!str) return 0;
    
    size_t written = 0;
    char prev = 0;
    
    while (*str) {
        if (*str == '\n' && prev != '\r') {
            written += write((uint8_t)'\r');
        }
        written += write((uint8_t)*str);
        prev = *str;
        str++;
    }
    
    return written;
}


// ============================================================================

// Global instance
OLEDStream OLEDOut;

OLEDStream::OLEDStream() 
    : current_font(DEFAULT_SMALL_FONT),
      auto_update(true),  // CRITICAL: Must be true for print redirect to work
      scroll_enabled(true),
      in_ansi_escape(false),
      last_was_newline(false),
      max_lines(4) { // Default for 32px height
    recalculateMaxLines();
}

OLEDStream::~OLEDStream() {
    // Nothing to clean up
}

// Stream Interface Implementation
int OLEDStream::available() {
    return 0; // Write-only
}

int OLEDStream::read() {
    return -1; // Write-only
}

int OLEDStream::peek() {
    return -1; // Write-only
}

void OLEDStream::flush() {
    updateDisplay();
}

size_t OLEDStream::write(uint8_t byte) {
    if (!isConnected()) {
        return 0;
    }
    
    char c = (char)byte;
    
    // Handle ANSI escape sequences - filter them out
    if (c == '\x1b') { // ESC
        in_ansi_escape = true;
        return 1;
    }
    
    if (in_ansi_escape) {
        // Stay in escape mode until we see 'm' (color code end) or other terminators
        if (c == 'm' || c == 'H' || c == 'J' || c == 'K' || c == 'G') {
            in_ansi_escape = false;
        }
        return 1; // Character consumed, don't display
    }
    
    // Build current character/string to send
    char charBuf[5] = {0};
    int charLen = 0;
    
    // Handle special characters
    if (c == '\n' || c == '\r') {
        // Filter consecutive newlines - only process if last wasn't a newline
        if (!last_was_newline) {
            charBuf[charLen++] = '\n';
            last_was_newline = true;
        } else {
            return 1; // Skip consecutive newlines
        }
    } else if (c == '\t') {
        // Tab = 4 spaces
        charBuf[0] = ' ';
        charBuf[1] = ' ';
        charBuf[2] = ' ';
        charBuf[3] = ' ';
        charLen = 4;
        last_was_newline = false;
    } else if (c == '\b') {
        // Backspace - not easily supported with showMultiLineSmallText
        // Just ignore for now
        last_was_newline = false;
        return 1;
    } else if (c >= 32 && c <= 126) {
        // Printable ASCII character
        charBuf[charLen++] = c;
        last_was_newline = false;
    } else {
        // Ignore other control characters
        return 1;
    }
    
    // Send to showMultiLineSmallText (clear=false to append)
    if (charLen > 0 && auto_update) {
        charBuf[charLen] = '\0';
        oled.showMultiLineSmallText(charBuf, false, true);
    }
    
    return 1;
}

size_t OLEDStream::write(const uint8_t *buffer, size_t size) {
    if (!isConnected() || !buffer || size == 0) {
        return 0;
    }
    
    // Build a complete filtered string from the buffer, then send to
    // showMultiLineSmallText in one shot.  This avoids per-byte display
    // refreshes so that e.g. print(42) shows "42" atomically.
    char filtered[OLEDSTREAM_BUFFER_SIZE];
    int filteredLen = 0;
    
    for (size_t i = 0; i < size && filteredLen < OLEDSTREAM_BUFFER_SIZE - 1; i++) {
        char c = (char)buffer[i];
        
        // Handle ANSI escape sequences - filter them out
        if (c == '\x1b') {
            in_ansi_escape = true;
            continue;
        }
        if (in_ansi_escape) {
            if (c == 'm' || c == 'H' || c == 'J' || c == 'K' || c == 'G') {
                in_ansi_escape = false;
            }
            continue;
        }
        
        // Handle special characters
        if (c == '\n' || c == '\r') {
            if (!last_was_newline) {
                filtered[filteredLen++] = '\n';
                last_was_newline = true;
            }
        } else if (c == '\t') {
            int spaces = 4;
            if (filteredLen + spaces > OLEDSTREAM_BUFFER_SIZE - 1)
                spaces = OLEDSTREAM_BUFFER_SIZE - 1 - filteredLen;
            for (int j = 0; j < spaces; j++) filtered[filteredLen++] = ' ';
            last_was_newline = false;
        } else if (c == '\b') {
            last_was_newline = false;
        } else if (c >= 32 && c <= 126) {
            filtered[filteredLen++] = c;
            last_was_newline = false;
        }
        // Other control characters are silently ignored
    }
    
    // Send the complete filtered string to the display in one update
    if (filteredLen > 0 && auto_update) {
        filtered[filteredLen] = '\0';
        oled.showMultiLineSmallText(filtered, false, true);
    }
    
    return size;
}

// ============================================================================
// Print Methods - Convert values to ASCII (like Serial.print vs write)
// ============================================================================

// Helper: format a number into buf[] and return the length.
// Handles bases 2-16, negative values in base 10.
static size_t formatNumber(char* buf, size_t bufSize, long long value, int base, bool isSigned) {
    if (base < 2 || base > 16) base = 10;
    
    char tmp[8 * sizeof(long long) + 2]; // worst case: 64 binary digits + sign + NUL
    char* p = &tmp[sizeof(tmp) - 1];
    *p = '\0';
    
    bool negative = false;
    unsigned long long uval;
    if (isSigned && base == 10 && value < 0) {
        negative = true;
        uval = (unsigned long long)(-value);
    } else {
        uval = (unsigned long long)value;
    }
    
    do {
        char c = uval % base;
        uval /= base;
        *--p = c < 10 ? c + '0' : c + 'A' - 10;
    } while (uval);
    
    if (negative) *--p = '-';
    
    size_t len = strlen(p);
    if (len >= bufSize) len = bufSize - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return len;
}

static size_t formatFloat(char* buf, size_t bufSize, double value, int decimals) {
    if (decimals < 0) decimals = 2;
    if (isnan(value))  { strncpy(buf, "nan", bufSize); buf[bufSize-1]='\0'; return strlen(buf); }
    if (isinf(value))  { strncpy(buf, "inf", bufSize); buf[bufSize-1]='\0'; return strlen(buf); }
    if (value > 4294967040.0 || value < -4294967040.0) { strncpy(buf, "ovf", bufSize); buf[bufSize-1]='\0'; return strlen(buf); }
    int n = snprintf(buf, bufSize, "%.*f", decimals, value);
    if (n < 0) n = 0;
    if ((size_t)n >= bufSize) n = bufSize - 1;
    buf[n] = '\0';
    return (size_t)n;
}

size_t OLEDStream::print(int value, int base) {
    char buf[8 * sizeof(long long) + 2];
    size_t len = formatNumber(buf, sizeof(buf), (long long)value, base, true);
    return write((const uint8_t*)buf, len);
}

size_t OLEDStream::print(unsigned int value, int base) {
    char buf[8 * sizeof(long long) + 2];
    size_t len = formatNumber(buf, sizeof(buf), (long long)(unsigned long long)value, base, false);
    return write((const uint8_t*)buf, len);
}

size_t OLEDStream::print(long value, int base) {
    char buf[8 * sizeof(long long) + 2];
    size_t len = formatNumber(buf, sizeof(buf), (long long)value, base, true);
    return write((const uint8_t*)buf, len);
}

size_t OLEDStream::print(unsigned long value, int base) {
    char buf[8 * sizeof(long long) + 2];
    size_t len = formatNumber(buf, sizeof(buf), (long long)(unsigned long long)value, base, false);
    return write((const uint8_t*)buf, len);
}

size_t OLEDStream::print(long long value, int base) {
    char buf[8 * sizeof(long long) + 2];
    size_t len = formatNumber(buf, sizeof(buf), value, base, true);
    return write((const uint8_t*)buf, len);
}

size_t OLEDStream::print(unsigned long long value, int base) {
    char buf[8 * sizeof(long long) + 2];
    size_t len = formatNumber(buf, sizeof(buf), (long long)value, base, false);
    return write((const uint8_t*)buf, len);
}

size_t OLEDStream::print(double value, int decimals) {
    char buf[64];
    size_t len = formatFloat(buf, sizeof(buf), value, decimals);
    return write((const uint8_t*)buf, len);
}

size_t OLEDStream::println(int value, int base)           { size_t n = print(value, base); n += println(); return n; }
size_t OLEDStream::println(unsigned int value, int base)  { size_t n = print(value, base); n += println(); return n; }
size_t OLEDStream::println(long value, int base)          { size_t n = print(value, base); n += println(); return n; }
size_t OLEDStream::println(unsigned long value, int base) { size_t n = print(value, base); n += println(); return n; }
size_t OLEDStream::println(long long value, int base)     { size_t n = print(value, base); n += println(); return n; }
size_t OLEDStream::println(unsigned long long value, int base) { size_t n = print(value, base); n += println(); return n; }
size_t OLEDStream::println(double value, int decimals)    { size_t n = print(value, decimals); n += println(); return n; }
size_t OLEDStream::println(void)                          { return write((const uint8_t*)"\n", 1); }

// OLED-specific Functions
void OLEDStream::setSmallFont(SmallFont font) {
    current_font = font;
    if (auto_update) {
        updateDisplay();
    }
}

void OLEDStream::clear() {
    // Reset state
    last_was_newline = false;
    in_ansi_escape = false;
    
    // Clear OLED and reset showMultiLineSmallText buffer
    if (isConnected()) {
        oled.showMultiLineSmallText("", true, true);
    }
}

bool OLEDStream::isConnected() const {
    return oled.isConnected();
}

// Internal Helpers - simplified since showMultiLineSmallText handles buffering
// These are kept as stubs for compatibility but don't do anything
void OLEDStream::printChar(char c) {
    // No-op: showMultiLineSmallText handles buffering
}

void OLEDStream::newline() {
    // No-op: showMultiLineSmallText handles buffering
}

void OLEDStream::scrollUp() {
    // No-op: showMultiLineSmallText handles scrolling
}

void OLEDStream::updateDisplay() {
    // No-op: showMultiLineSmallText is called directly in write()
    // This is only here for manual flush() calls
    if (!isConnected()) {
        return;
    }
    
    // Just ensure the display is updated
    oled.flushFramebuffer();
}

void OLEDStream::recalculateMaxLines() {
    if (isConnected()) {
        // Get actual line height from current small font
        oled.setSmallFont(current_font);
        FontMetrics metrics = oled.getFontMetrics();
        int lineHeight = metrics.lineHeight > 0 ? metrics.lineHeight : 11; // Default to 11 if invalid
        
        max_lines = oled.displayHeight / lineHeight;
        
        // Ensure we don't exceed buffer size
        if (max_lines > OLEDSTREAM_MAX_POSSIBLE_LINES) {
            max_lines = OLEDSTREAM_MAX_POSSIBLE_LINES;
        }
        // Minimum of 1 line
        if (max_lines < 1) {
            max_lines = 1;
        }
        
        oled.restoreNormalFont();
    } else {
        max_lines = 4; // Default for 32px height
    }
}


// ============================================================================
// UARTStream Implementation
// ============================================================================

UARTStream::UARTStream(uart_inst_t* uart_inst, uint tx_pin_num, uint rx_pin_num)
    : uart(uart_inst),
      tx_pin(tx_pin_num),
      rx_pin(rx_pin_num),
      current_baud(115200),
      initialized(false),
      has_peeked(false),
      peek_byte(0) {
}

UARTStream::~UARTStream() {
    if (initialized) {
        end();
    }
}

// Initialization
void UARTStream::begin(uint32_t baud, uint data_bits, uint stop_bits, uart_parity_t parity) {
    if (initialized) {
        end();
    }
    
    gpio_set_function(tx_pin, GPIO_FUNC_UART);
    gpio_set_function(rx_pin, GPIO_FUNC_UART);
    
    uart_init(uart, baud);
    uart_set_format(uart, data_bits, stop_bits, parity);
    uart_set_fifo_enabled(uart, true);
    
    current_baud = baud;
    initialized = true;
    has_peeked = false;
}

void UARTStream::end() {
    if (!initialized) {
        return;
    }
    
    uart_tx_wait_blocking(uart);
    uart_deinit(uart);
    
    initialized = false;
}

void UARTStream::setBaudRate(uint32_t baud) {
    if (!initialized) {
        return;
    }
    
    uart_set_baudrate(uart, baud);
    current_baud = baud;
}

void UARTStream::setFlowControl(bool enable, uint cts_pin_num, uint rts_pin_num) {
    if (!initialized) {
        return;
    }
    
    if (enable) {
        uart_set_hw_flow(uart, true, true);
        gpio_set_function(cts_pin_num, GPIO_FUNC_UART);
        gpio_set_function(rts_pin_num, GPIO_FUNC_UART);
    } else {
        uart_set_hw_flow(uart, false, false);
    }
}

// Stream Interface Implementation
int UARTStream::available() {
    if (!initialized) {
        return 0;
    }
    
    int count = 0;
    
    if (has_peeked) {
        count++;
    }
    
    if (uart_is_readable(uart)) {
        count = 1;
    }
    
    return count + (has_peeked ? 1 : 0);
}

int UARTStream::read() {
    if (!initialized) {
        return -1;
    }
    
    if (has_peeked) {
        has_peeked = false;
        return peek_byte;
    }
    
    if (uart_is_readable(uart)) {
        return uart_getc(uart);
    }
    
    return -1;
}

int UARTStream::peek() {
    if (!initialized) {
        return -1;
    }
    
    if (has_peeked) {
        return peek_byte;
    }
    
    if (uart_is_readable(uart)) {
        peek_byte = uart_getc(uart);
        has_peeked = true;
        return peek_byte;
    }
    
    return -1;
}

void UARTStream::flush() {
    if (!initialized) {
        return;
    }
    
    uart_tx_wait_blocking(uart);
}

size_t UARTStream::write(uint8_t byte) {
    if (!initialized) {
        return 0;
    }
    
    uart_putc_raw(uart, byte);
    
    return 1;
}

size_t UARTStream::write(const uint8_t *buffer, size_t size) {
    if (!initialized || !buffer) {
        return 0;
    }
    
    for (size_t i = 0; i < size; i++) {
        uart_putc_raw(uart, buffer[i]);
    }
    
    return size;
}

int UARTStream::availableForWrite() {
    if (!initialized) {
        return 0;
    }
    
    if (uart_is_writable(uart)) {
        return 32;
    }
    
    return 0;
}

// UART-specific Functions
bool UARTStream::isWritable() const {
    if (!initialized) {
        return false;
    }
    
    return uart_is_writable(uart);
}

bool UARTStream::isReadable() const {
    if (!initialized) {
        return false;
    }
    
    return uart_is_readable(uart) || has_peeked;
}

