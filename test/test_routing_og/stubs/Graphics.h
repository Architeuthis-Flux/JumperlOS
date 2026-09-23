#pragma once
#include <stdint.h>
extern uint32_t gpioReadingColors[50];
#include <Arduino.h>
void changeTerminalColor(int termColor = -1, bool flush = true, Stream *stream = &Serial);
