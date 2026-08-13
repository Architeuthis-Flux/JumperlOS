/*!
 *  @file MCP4728.cpp
 *
 *  Async I2C Driver for the MCP4728 4-Channel 12-Bit I2C DAC
 *  Based on Adafruit_MCP4728 but using Earle's async Wire API
 */

#include "MCP4728.h"
#include "JumperlessDefines.h"  // For LDAC pin definition
#include "hardware/i2c.h"       // For low-level I2C control

// ============================================================================
// Minimal Software I2C for MCP4728 address programming
// This is needed because address change requires toggling LDAC between bytes,
// which hardware I2C cannot do precisely.
// ============================================================================
#define SOFT_SDA_PIN 4
#define SOFT_SCL_PIN 5

static inline void softI2C_delay() { delayMicroseconds(3); }  // ~166kHz

static inline void softI2C_sdaHigh() { 
  pinMode(SOFT_SDA_PIN, INPUT_PULLUP); 
}
static inline void softI2C_sdaLow() { 
  pinMode(SOFT_SDA_PIN, OUTPUT); 
  digitalWrite(SOFT_SDA_PIN, LOW); 
}
static inline void softI2C_sclHigh() {
  pinMode(SOFT_SCL_PIN, INPUT_PULLUP);
  // Clock stretching, DEADLINE-BOUNDED. Pins 4/5 are also hardware I2C0
  // (INA219s, OLED type 2, wavegen's DAC streaming from the other core) -
  // an unbounded wait here spins this core forever if the bus is held low
  // by a device mid-transaction with someone else. The MCP4728 doesn't
  // stretch anywhere near 5ms; a low SCL past that means the bus is not
  // ours and the transfer is already lost - proceed and let the ACK fail.
  unsigned long start = micros();
  while (digitalRead(SOFT_SCL_PIN) == LOW) {
    if (micros() - start > 5000) break;
  }
}
static inline void softI2C_sclLow() { 
  pinMode(SOFT_SCL_PIN, OUTPUT); 
  digitalWrite(SOFT_SCL_PIN, LOW); 
}
static inline bool softI2C_sdaRead() { 
  pinMode(SOFT_SDA_PIN, INPUT_PULLUP);
  return digitalRead(SOFT_SDA_PIN); 
}

static void softI2C_start() {
  softI2C_sdaHigh(); softI2C_sclHigh(); softI2C_delay();
  softI2C_sdaLow(); softI2C_delay();
  softI2C_sclLow(); softI2C_delay();
}

static void softI2C_stop() {
  softI2C_sdaLow(); softI2C_delay();
  softI2C_sclHigh(); softI2C_delay();
  softI2C_sdaHigh(); softI2C_delay();
}

static bool softI2C_writeByte(uint8_t data) {
  for (int i = 7; i >= 0; i--) {
    if (data & (1 << i)) softI2C_sdaHigh(); else softI2C_sdaLow();
    softI2C_delay();
    softI2C_sclHigh(); softI2C_delay();
    softI2C_sclLow(); softI2C_delay();
  }
  // Read ACK
  softI2C_sdaHigh(); softI2C_delay();
  softI2C_sclHigh(); softI2C_delay();
  bool ack = !softI2C_sdaRead();  // ACK = SDA low
  softI2C_sclLow(); softI2C_delay();
  return ack;
}

// Write byte with LDAC toggle during the 8th clock's negative pulse
// Per datasheet: LDAC must go LOW during the negative pulse of the 8th clock
static bool softI2C_writeByteWithLDAC(uint8_t data, uint8_t ldacPin) {
  for (int i = 7; i >= 0; i--) {
    if (data & (1 << i)) softI2C_sdaHigh(); else softI2C_sdaLow();
    softI2C_delay();
    softI2C_sclHigh(); softI2C_delay();
    softI2C_sclLow();
    // Toggle LDAC LOW during the 8th clock's negative pulse (i == 0 is the 8th bit)
    if (i == 0) {
      digitalWrite(ldacPin, LOW);
    }
    softI2C_delay();
  }
  // Read ACK (LDAC is now LOW)
  softI2C_sdaHigh(); softI2C_delay();
  softI2C_sclHigh(); softI2C_delay();
  bool ack = !softI2C_sdaRead();
  softI2C_sclLow(); softI2C_delay();
  return ack;
}

/*!
 *    @brief  Instantiates a new MCP4728 class
 */
MCP4728::MCP4728(void) : _initialized(false), _i2c_address(0), _wire(nullptr) {}

struct last_settings last_settings;


volatile bool started = false;
volatile bool ended = false;

/*!
 *    @brief  Sets up the hardware and initializes I2C
 *    @param  i2c_address The I2C address to be used
 *    @param  wire The Wire object to be used for I2C connections
 *    @return True if initialization was successful, otherwise false
 */
bool MCP4728::begin(uint8_t i2c_address, TwoWire *wire) {
  _i2c_address = i2c_address;
  _wire = wire;
  
  if (!_wire) {
    return false;
  }
  _wire->setClock(1700000); //I have no clue why this shaves off ~8us of the transfer time, but it does
  _clock_hz = 1000000;//the rp2350 has a max of 1000000 for i2c
  
  // Test communication with a simple read
  _wire->beginTransmission(_i2c_address);
  uint8_t error = _wire->endTransmission();
  
  if (error == 0) {
    _initialized = true;
    return true;
  }
  
  return false;
}

/*!
 *    @brief  End the I2C connection and free resources
 */
void MCP4728::end() {
  if (_initialized) {
    _wire->abortAsync();
    _initialized = false;
  }
}

bool MCP4728::sendI2Cstart() {
  _wire->beginTransmission(_i2c_address);
  started = true;
  return true;
}

bool MCP4728::sendI2Cend(bool sendStop) {
  ended = true;
  // endTransmission(false) issues a repeated START instead of STOP
  return _wire->endTransmission(sendStop) == 0;
}

bool MCP4728::sendI2Cdata(uint8_t *data, size_t length) {
  return _wire->write(data, length) == length;
}

/*!
 *    @brief  Write new I2C address bits to the MCP4728 EEPROM
 *    @param  newAddressBits The new address bits (0-7, only lower 3 bits used)
 *    @return True if the write was successful
 *    
 *    Based on: changeDeviceID.pde from neurostar's MCP4728 library
 *    Uses software I2C for precise LDAC timing control.
 *    
 *    Wire format (from datasheet Figure 5-11):
 *      i2c.start(0xC0 | (oldAddress << 1))        // 1100 0000 | old addr shifted
 *      i2c.ldacwrite(0x61 | (oldAddress << 2))    // Write + toggle LDAC
 *      i2c.write(0x62 | (newAddress << 2))
 *      i2c.write(0x63 | (newAddress << 2))
 *      i2c.stop()
 */
bool MCP4728::setMCPAddressBits(uint8_t newAddressBits, bool debug) {
  if (debug) { Serial.println("MCP4728::setMCPAddressBits() called"); Serial.flush(); }
  
  if (!_initialized || !_wire) {
    if (debug) { Serial.println("Not initialized, returning false"); }
    return false;
  }
  
  // Extract current address bits (0-7)
  uint8_t currentAddressBits = (_i2c_address - MCP4728_I2CADDR_DEFAULT) & 0x07;
  newAddressBits &= 0x07;
  
  // Build bytes per changeDeviceID.pde / datasheet Figure 5-11
  uint8_t addrByte = 0xC0 | (currentAddressBits << 1);  // 1100 0000 | old << 1 | W
  uint8_t byte1 = 0x61 | (currentAddressBits << 2);     // 0110 0001 | old << 2
  uint8_t byte2 = 0x62 | (newAddressBits << 2);         // 0110 0010 | new << 2
  uint8_t byte3 = 0x63 | (newAddressBits << 2);         // 0110 0011 | new << 2
  
  if (debug) {
    Serial.printf("Old bits: %d, New bits: %d, Bytes: 0x%02X 0x%02X 0x%02X 0x%02X\n", 
                  currentAddressBits, newAddressBits, addrByte, byte1, byte2, byte3);
  }
  
  // Stop hardware I2C so we can use software I2C
  _wire->end();
  
  // Set LDAC HIGH before starting
  pinMode(LDAC, OUTPUT);
  digitalWrite(LDAC, HIGH);
  delayMicroseconds(10);
  
  bool success = true;
  
  // Software I2C sequence (matches changeDeviceID.pde writeAddress())
  softI2C_start();
  
  // Address byte
  if (!softI2C_writeByte(addrByte)) {
    if (debug) { Serial.println("NACK on address"); }
    success = false;
  }
  
  // Byte1 with LDAC toggle during 8th clock
  if (success && !softI2C_writeByteWithLDAC(byte1, LDAC)) {
    if (debug) { Serial.println("NACK on byte1"); }
    success = false;
  }
  
  // Byte2
  if (success && !softI2C_writeByte(byte2)) {
    if (debug) { Serial.println("NACK on byte2"); }
    success = false;
  }
  
  // Byte3
  if (success && !softI2C_writeByte(byte3)) {
    if (debug) { Serial.println("NACK on byte3"); }
    success = false;
  }
  
  softI2C_stop();
  
  // Wait for EEPROM write (25-50ms per datasheet)
  delay(100);
  
  // Restore LDAC HIGH
  digitalWrite(LDAC, HIGH);
  
  // Restore hardware I2C
  _wire->setSDA(SOFT_SDA_PIN);
  _wire->setSCL(SOFT_SCL_PIN);
  _wire->setClock(_clock_hz);
  _wire->begin();
  
  if (success) {
    _i2c_address = MCP4728_I2CADDR_DEFAULT | newAddressBits;
    _mcp_address_bits = newAddressBits;
    if (debug) { Serial.printf("Success! New address: 0x%02X\n", _i2c_address); }
  } else {
    if (debug) { Serial.println("Failed to set address"); }
  }
  
  return success;
}

/*!
 *    @brief  Fast write - sends all 4 channels simultaneously (synchronous)
 *    @param  channel_a_value Value for channel A (0-4095)
 *    @param  channel_b_value Value for channel B (0-4095)
 *    @param  channel_c_value Value for channel C (0-4095)
 *    @param  channel_d_value Value for channel D (0-4095)
 *    @return True if operation completed successfully
 */
bool MCP4728::fastWriteAsync(uint16_t channel_a_value, uint16_t channel_b_value,
                                  uint16_t channel_c_value, uint16_t channel_d_value) {
    // Serial.println("MCP4728::fastWriteAsync() called (now synchronous)");
    // Serial.flush();
    
    if (!_initialized) {
        Serial.println("Not initialized, returning false");
        Serial.flush();
        return false;
    }
    
    // For synchronous operations, we don't need to check async state
    
    // Serial.print("Values: A=");
    // Serial.print(channel_a_value);
    // Serial.print(" B=");
    // Serial.print(channel_b_value);
    // Serial.print(" C=");
    // Serial.print(channel_c_value);
    // Serial.print(" D=");
    // Serial.println(channel_d_value);
    // Serial.flush();
    
    // Prepare the 8-byte buffer for fast write
    // Format: [A_MSB, A_LSB, B_MSB, B_LSB, C_MSB, C_LSB, D_MSB, D_LSB]
    _async_buffer[0] = channel_a_value >> 8;
    _async_buffer[1] = channel_a_value & 0xFF;
    
    _async_buffer[2] = channel_b_value >> 8;
    _async_buffer[3] = channel_b_value & 0xFF;
    
    _async_buffer[4] = channel_c_value >> 8;
    _async_buffer[5] = channel_c_value & 0xFF;
    
    _async_buffer[6] = channel_d_value >> 8;
    _async_buffer[7] = channel_d_value & 0xFF;
    
    // For single value updates, use synchronous write (not async)
    // writeAsync is meant for large buffer transfers, not single commands
    // Serial.println("About to call _wire->write() (synchronous)");
    // Serial.flush();
    
    _wire->beginTransmission(_i2c_address);
    _wire->write(_async_buffer, 8);
    int result = _wire->endTransmission();
    //int result = 0;
    
    // Serial.print("_wire->endTransmission() result: ");
    // Serial.println(result);
    // Serial.flush();
    
    return (result == 0); // Convert to bool: 0 = success, non-zero = error
}

/*!
 *    @brief  Check if async operation is complete
 *    @return True if operation is finished
 */
bool MCP4728::finishedAsync() {
  if (!_initialized) {
    return true; // Not initialized, so "finished"
  }
  
  return _wire->finishedAsync();
}

/*!
 *    @brief  Set callback for when async operation completes
 *    @param  function Callback function to call when operation finishes
 */
void MCP4728::onFinishedAsync(void(*function)(void)) {
  if (_initialized) {
    _wire->onFinishedAsync(function);
  }
}

/*!
 *    @brief  Abort any pending async operation
 */
void MCP4728::abortAsync() {
  if (_initialized) {
    _wire->abortAsync();
  }
}

/*!
 *    @brief  Set individual channel value (synchronous)
 *    @param  channel The channel to update
 *    @param  new_value The new value to assign (0-4095)
 *    @param  new_vref Reference voltage setting
 *    @param  new_gain Gain setting
 *    @param  new_pd_mode Power down mode setting
 *    @param  udac UDAC setting (false = immediate latch)
 *    @return True if the write was successful
 */
bool MCP4728::setChannelValue(MCP4728_channel_t channel, uint16_t new_value,
                                   MCP4728_vref_t new_vref, MCP4728_gain_t new_gain,
                                   MCP4728_pd_mode_t new_pd_mode, bool udac) {
  if (!_initialized) {
    return false;
  }
  
  uint8_t output_buffer[3];
  last_settings.channel = channel;
  last_settings.value = new_value;
  last_settings.vref = new_vref;
  last_settings.gain = new_gain;
  last_settings.pd_mode = new_pd_mode;

  // Build the setter header/ "address"
  // 0 1 0 0 0 DAC1 DAC0 UDAC[A]
  uint8_t sequential_write_cmd = 0x40; // MCP4728_MULTI_IR_CMD
  sequential_write_cmd |= (channel << 1);
  sequential_write_cmd |= udac;
  
  output_buffer[0] = sequential_write_cmd;
  
  // VREF PD1 PD0 Gx D11 D10 D9 D8 [A] D7 D6 D5 D4 D3 D2 D1 D0 [A]
  new_value |= (new_vref << 15);
  new_value |= (new_pd_mode << 13);
  new_value |= (new_gain << 12);
  
  output_buffer[1] = new_value >> 8;
  output_buffer[2] = new_value & 0xFF;
  
  _wire->beginTransmission(_i2c_address);
  _wire->write(output_buffer, 3);
  uint8_t error = _wire->endTransmission();
  
  return error == 0;
}


/*!
 *    @brief  Set individual channel value (synchronous)
 *    @param  channel The channel to update
 *    @param  new_value The new value to assign (0-4095)
 *    @param  sendStart True if the start command should be sent
 *    @param  sendEnd True if the end command should be sent
 *    @return True if the write was successful
 */
 bool MCP4728::quickSetChannelValue(MCP4728_channel_t channel, uint16_t new_value, bool sendStart, bool sendEnd) {
if (!_initialized) {
return false;
}

uint8_t output_buffer[3];

// Build the setter header/ "address"
// 0 1 0 0 0 DAC1 DAC0 UDAC[A]
uint8_t sequential_write_cmd = 0x40; // MCP4728_MULTI_IR_CMD
sequential_write_cmd |= (channel << 1);
sequential_write_cmd |= 0;

output_buffer[0] = sequential_write_cmd;

// VREF PD1 PD0 Gx D11 D10 D9 D8 [A] D7 D6 D5 D4 D3 D2 D1 D0 [A]
new_value |= (last_settings.vref << 15);
new_value |= (last_settings.pd_mode << 13);
new_value |= (last_settings.gain << 12);

output_buffer[1] = new_value >> 8;
output_buffer[2] = new_value & 0xFF;

if (sendStart) {
  _wire->beginTransmission(_i2c_address);
}
size_t bytes_written = _wire->write(output_buffer, 3);

// If TX buffer is saturated, write may return < length.
// Report failure so caller can endTransmission() and retry.
if (bytes_written != 3) {
  if (sendEnd) {
    (void)_wire->endTransmission();
  }
  return false;
}

if (sendEnd) {
  uint8_t error = _wire->endTransmission();
  return error == 0;
}

return true;
}

// Optimized single sample write: beginTransmission + 3 bytes + endTransmission(false)
// Uses last_settings for vref/gain/pd bits, UDAC=0 immediate update.
bool MCP4728::writeSampleRepeatedStart(MCP4728_channel_t channel, uint16_t value) {
  if (!_initialized) {
    return false;
  }
  uint8_t b0 = (uint8_t)(0x40 | ((channel << 1) & 0x06));
  uint16_t packed = value;
  packed |= (last_settings.vref << 15);
  packed |= (last_settings.pd_mode << 13);
  packed |= (last_settings.gain << 12);
  uint8_t b1 = (uint8_t)(packed >> 8);
  uint8_t b2 = (uint8_t)(packed & 0xFF);

  _wire->beginTransmission(_i2c_address);
  size_t n = _wire->write(&b0, 1);
  n += _wire->write(&b1, 1);
  n += _wire->write(&b2, 1);
  if (n != 3) {
    (void)_wire->endTransmission(true);
    return false;
  }
  // repeated START to chain the next sample immediately
  uint8_t err = _wire->endTransmission(false);
  return err == 0;
}