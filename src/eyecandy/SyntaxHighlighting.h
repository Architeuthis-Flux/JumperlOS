#ifndef SYNTAXHIGHLIGHTING_H
#define SYNTAXHIGHLIGHTING_H

#include <Arduino.h>

#define TERM_COLOR_SUPPORT 256

#define HL_NORMAL 0
#define HL_COMMENT 2
#define HL_MLCOMMENT 3
#define HL_KEYWORD1 4 
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8
#define HL_JUMPERLESS_FUNC 9
#define HL_JUMPERLESS_TYPE 10
#define HL_JFS_FUNC 11
#define HL_TERMINAL_FUNC 12
#define HL_TERMINAL_NUMBER 13
#define HL_TERMINAL_NODE1 14
#define HL_TERMINAL_NODE2 15
#define HL_TERMINAL_STRING 16

enum SyntaxHighlightingType {
  SYNTAX_PYTHON,
  SYNTAX_JUMPERLESS,
  SYNTAX_JFS,
  SYNTAX_TERMINAL,
  SYNTAX_JUMPERLESS_CONNECTIONS,
  SYNTAX_DEFAULT,
};

// Python keyword classification
enum PythonKeywordType {
  KW_NONE,           // Not a keyword
  KW_PYTHON,         // Python keyword (if, for, etc.)
  KW_BUILTIN,        // Python built-in (len, str, etc.)
  KW_JUMPERLESS,     // Jumperless function (connect, dac_set, etc.)
  KW_JUMPERLESS_TYPE,// Jumperless constant (TOP_RAIL, GND, etc.)
  KW_JFS             // JFS function (open, read, etc.)
};

class SyntaxHighlighting {
  public:
    SyntaxHighlighting(Stream* stream);
    ~SyntaxHighlighting();
    void syntaxHighlighting();

    Stream* syntaxHighlightingStream;

    void syntaxHighlightingToColor(int hl);
    char* highlightString(const char* string, enum SyntaxHighlightingType type);
    void setStream(Stream* stream);
    String highlightJumperlessConnections(const String& input); // Smart connection highlighting
    
    // Python syntax highlighting - public for use in REPL and editors
    String highlightPythonCode(const String& code); // Full Python syntax highlighting
    void displayPythonWithHighlighting(const String& text, Stream* stream); // Stream output version
    PythonKeywordType classifyPythonKeyword(const String& word); // Classify a Python identifier
    
private:
    // Helper methods for smart highlighting
    bool isTerminalCommand(char c);
    String highlightSingleConnection(const String& connection);
    String getNodeFriendlyName(const String& input);
    bool isValidNodeName(const String& name);
    String processConnections(const String& connections); // Process comma-separated connections  
    String highlightConnectionsPreserveSpaces(const String& input); // Simple space-preserving highlighting
    
    // Python keyword checking
    bool isPythonKeyword(const String& word);
    bool isPythonBuiltin(const String& word);
    bool isJumperlessFunction(const String& word);
    bool isJumperlessType(const String& word);
    bool isJFSFunction(const String& word);
};

// Global helper function for backward compatibility
void displayStringWithSyntaxHighlighting(const String& text, Stream* stream);




















#endif