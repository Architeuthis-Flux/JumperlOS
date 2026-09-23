#pragma once
struct EEPROMClass { int read(int) { return 0; } void write(int, int) {} void commit() {} };
extern EEPROMClass EEPROM;
