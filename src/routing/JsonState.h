/*
 * JsonState.h
 * 
 * Generates JSON representation of the current Jumperless state
 * for LLM and external tool consumption.
 */

#ifndef JSONSTATE_H
#define JSONSTATE_H

#include <Arduino.h>

struct PowerState;

/**
 * JSON-escape a string, bounded to maxLen, dropping anything outside printable
 * ASCII. Use this for every string that reaches the state JSON: the names involved
 * live in fixed char[32] buffers, and get_state() passes the finished document
 * through MicroPython's mp_obj_new_str(), which rejects the whole thing if any byte
 * is not valid UTF-8.
 */
String escapeJson(const char* s, size_t maxLen);
String escapeJson(String s);

class JsonState {
public:
    /** Full state if section is nullptr or empty; else single section: "power", "nets", "gpio", "overlays". */
    static String getJumperlessStateJSON(const char* section = nullptr);
};



class JsonStateParser {
public:
    // Main entry point - parses JSON and applies state
    // Returns true on success, false on parse error
    static bool applyJSONState(const String& json, bool clearFirst = true);
    
    // Get last error message
    static const char* getLastError();

    // JSON helpers (public for section-only output in JsonState)
    static String extractArray(const String& json, const char* key);
    static String extractObject(const String& json, const char* key);
    
private:
    static String lastError;
    
    // Section parsers
    static bool parseNetsSection(const String& json, const struct PowerState& oldPower);
    static bool parsePowerSection(const String& json);
    static bool parseGpioSection(const String& json);
    static bool parseOverlaysSection(const String& json);
    
    // JSON helpers (internal)
    static String extractString(const String& json, const char* key);
    static float extractFloat(const String& json, const char* key, float defaultVal = 0.0f);
    static int extractInt(const String& json, const char* key, int defaultVal = -1);
    
    // Get next object from array (for iteration)
    static String getNextArrayElement(const String& array, int& startPos);
};

#endif // JSON_STATE_PARSER_H

