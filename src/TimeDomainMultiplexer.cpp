// SPDX-License-Identifier: MIT
/*
 * Time Domain Multiplexer Implementation
 *
 * General-purpose module for multiplexing multiple analog channels through
 * a single ADC on the CH446Q crossbar (chip K). Not FakeGPIO-specific.
 *
 * See TimeDomainMultiplexer.h for API documentation.
 */

#include "TimeDomainMultiplexer.h"
#include "JumperlessDefines.h"
#include "States.h"
#include "CH446Q.h"
#include "Peripherals.h"
#include <Arduino.h>

// Forward declaration (defined at bottom of this file)
bool isAdcInUseByOtherConnections(int adcChannel);

// ============================================================================
// Lifecycle
// ============================================================================

static void clearChannelHops(TdmChannel& ch) {
    for (int i = 0; i < TDM_MAX_HOPS; i++) {
        ch.hopChip[i] = -1;
        ch.hopX[i] = -1;
        ch.hopY[i] = -1;
    }
    ch.numHops = 0;
}

void TimeDomainMultiplexer::init() {
    for (int i = 0; i < TDM_MAX_CHANNELS; i++) {
        channels[i].active = false;
        channels[i].userNode = -1;
        channels[i].chipKY = -1;
        channels[i].netIndex = -1;
        channels[i].lastVoltage = 0.0f;
        clearChannelHops(channels[i]);
    }
    adcChannel = -1;
    activeChannel = -1;
    lastPolledSlot = -1;
    activeCount = 0;
}

// ============================================================================
// Channel Management
// ============================================================================

int TimeDomainMultiplexer::addChannel(int userNode, int8_t chipKY, int netIndex,
                                      const int8_t* pathChips, const int8_t* pathX,
                                      const int8_t* pathY, int8_t numHops) {
    // Check if already exists
    int existing = findChannel(userNode);
    if (existing >= 0) {
        // Update existing channel
        channels[existing].chipKY = chipKY;
        channels[existing].netIndex = netIndex;
        if (pathChips && numHops > 0) {
            updateChannelPath(existing, pathChips, pathX, pathY, numHops);
        }
        return existing;
    }

    int slot = findFreeSlot();
    if (slot < 0) return -1;

    channels[slot].active = true;
    channels[slot].userNode = userNode;
    channels[slot].chipKY = chipKY;
    channels[slot].netIndex = netIndex;
    channels[slot].lastVoltage = 0.0f;
    clearChannelHops(channels[slot]);

    // Store full path hops if provided
    if (pathChips && pathX && pathY && numHops > 0) {
        int8_t n = (numHops > TDM_MAX_HOPS) ? TDM_MAX_HOPS : numHops;
        for (int i = 0; i < n; i++) {
            channels[slot].hopChip[i] = pathChips[i];
            channels[slot].hopX[i] = pathX[i];
            channels[slot].hopY[i] = pathY[i];
        }
        channels[slot].numHops = n;
    }

    activeCount++;
    return slot;
}

void TimeDomainMultiplexer::updateChannelPath(int slot, const int8_t* pathChips,
                                               const int8_t* pathX, const int8_t* pathY,
                                               int8_t numHops) {
    if (slot < 0 || slot >= TDM_MAX_CHANNELS) return;
    if (!channels[slot].active) return;

    clearChannelHops(channels[slot]);
    int8_t n = (numHops > TDM_MAX_HOPS) ? TDM_MAX_HOPS : numHops;
    for (int i = 0; i < n; i++) {
        channels[slot].hopChip[i] = pathChips[i];
        channels[slot].hopX[i] = pathX[i];
        channels[slot].hopY[i] = pathY[i];
    }
    channels[slot].numHops = n;
}

int TimeDomainMultiplexer::removeChannel(int userNode) {
    int slot = findChannel(userNode);
    if (slot < 0) return 0;

    // If this channel is currently connected, disconnect it first
    if (activeChannel == slot) {
        disconnectActive();
    }

    channels[slot].active = false;
    channels[slot].userNode = -1;
    channels[slot].chipKY = -1;
    channels[slot].netIndex = -1;
    channels[slot].lastVoltage = 0.0f;

    clearChannelHops(channels[slot]);
    activeCount--;

    // Release ADC if no channels remain
    if (activeCount <= 0) {
        activeCount = 0;
        releaseAdc();
    }

    return 1;
}

int TimeDomainMultiplexer::findChannel(int userNode) {
    for (int i = 0; i < TDM_MAX_CHANNELS; i++) {
        if (channels[i].active && channels[i].userNode == userNode) {
            return i;
        }
    }
    return -1;
}

int TimeDomainMultiplexer::findFreeSlot() {
    for (int i = 0; i < TDM_MAX_CHANNELS; i++) {
        if (!channels[i].active) {
            return i;
        }
    }
    return -1;
}

void TimeDomainMultiplexer::updateChannelChipKY(int slot, int8_t newChipKY) {
    if (slot < 0 || slot >= TDM_MAX_CHANNELS) return;
    if (!channels[slot].active) return;

    int8_t oldY = channels[slot].chipKY;
    channels[slot].chipKY = newChipKY;

    // If this channel is currently connected and Y changed, reconnect
    if (activeChannel == slot && oldY != newChipKY) {
        // Disconnect entire old path + ADC
        disconnectActive();
        // Reconnect with new Y (switchTo re-enables all hops + ADC)
        if (newChipKY >= 0 && newChipKY < 8) {
            switchTo(slot);
        }
    }
}

// ============================================================================
// Channel Switching (Full-Path)
// ============================================================================
//
// Multiple TDM inputs can share the same chip K Y position (the router reuses
// Y lines when different breadboard nodes route through the same Y). If we only
// toggle the ADC X↔Y crosspoint, the breadboard-side hops stay permanently
// connected, shorting all nodes that share that Y bus together.
//
// Fix: connect/disconnect the ENTIRE path (all hops from breadboard to chip K)
// for each channel switch. Only one channel's full signal path is live at a time.

// Helper: connect or disconnect all hops of a channel (excluding the ADC crosspoint)
static void setChannelPath(TdmChannel& ch, int connect) {
    for (int i = 0; i < ch.numHops; i++) {
        if (ch.hopChip[i] < 0 || ch.hopX[i] < 0 || ch.hopY[i] < 0) continue;
        sendXYrawUnchecked(ch.hopChip[i], ch.hopX[i], ch.hopY[i], connect);
        if (connect) {
            lastChipXY[ch.hopChip[i]].connected[ch.hopY[i]] |= (1 << ch.hopX[i]);
        } else {
            lastChipXY[ch.hopChip[i]].connected[ch.hopY[i]] &= ~(1 << ch.hopX[i]);
        }
    }
}

bool TimeDomainMultiplexer::switchTo(int slot) {
    if (slot < 0 || slot >= TDM_MAX_CHANNELS) return false;
    if (!channels[slot].active) return false;
    if (channels[slot].chipKY < 0 || channels[slot].chipKY >= 8) return false;
    if (adcChannel < 0) return false;

    // Already connected to this channel
    if (activeChannel == slot) return true;

    int8_t adcX = getChipKX();

    // Disconnect old channel: ALL hops + ADC crosspoint
    if (activeChannel >= 0 && activeChannel < TDM_MAX_CHANNELS) {
        TdmChannel& old = channels[activeChannel];
        // Disconnect ADC X from old Y
        if (old.chipKY >= 0 && old.chipKY < 8) {
            sendXYrawUnchecked(CHIP_K, adcX, old.chipKY, 0);
            lastChipXY[CHIP_K].connected[old.chipKY] &= ~(1 << adcX);
        }
        // Disconnect breadboard-side hops
        if (old.numHops > 0) {
            setChannelPath(old, 0);
        }
    }

    TdmChannel& ch = channels[slot];

    // Connect new channel: breadboard-side hops first, then ADC crosspoint
    if (ch.numHops > 0) {
        setChannelPath(ch, 1);
    }
    sendXYrawUnchecked(CHIP_K, adcX, ch.chipKY, 1);
    lastChipXY[CHIP_K].connected[ch.chipKY] |= (1 << adcX);
    activeChannel = slot;

    return true;
}

void TimeDomainMultiplexer::disconnectActive() {
    if (activeChannel < 0 || adcChannel < 0) {
        activeChannel = -1;
        return;
    }

    if (activeChannel < TDM_MAX_CHANNELS && channels[activeChannel].active) {
        TdmChannel& ch = channels[activeChannel];
        int8_t adcX = getChipKX();

        // Disconnect ADC X from Y
        if (ch.chipKY >= 0 && ch.chipKY < 8) {
            sendXYrawUnchecked(CHIP_K, adcX, ch.chipKY, 0);
            lastChipXY[CHIP_K].connected[ch.chipKY] &= ~(1 << adcX);
        }
        // Disconnect breadboard-side hops
        if (ch.numHops > 0) {
            setChannelPath(ch, 0);
        }
    }

    activeChannel = -1;
}

// ============================================================================
// Reading
// ============================================================================

float TimeDomainMultiplexer::readActive(int samples) {
    if (activeChannel < 0 || adcChannel < 0) return 0.0f;

    float voltage = readAdcVoltage(adcChannel, samples);
    channels[activeChannel].lastVoltage = voltage;
    return voltage;
}

float TimeDomainMultiplexer::switchAndRead(int slot, int samples) {
    if (!switchTo(slot)) return 0.0f;

    // Settling time for CH446Q analog switch after switching Y positions.
    // 30µs was too short and caused voltage readings from the previous channel
    // to leak into the new one. 80µs provides reliable isolation.
   // delayMicroseconds(80);

    return readActive(samples);
}

int TimeDomainMultiplexer::pollNext(int samples) {
    if (activeCount <= 0) return -1;

    // Find next active channel after lastPolledSlot (round-robin)
    int start = (lastPolledSlot + 1) % TDM_MAX_CHANNELS;
    for (int i = 0; i < TDM_MAX_CHANNELS; i++) {
        int slot = (start + i) % TDM_MAX_CHANNELS;
        if (channels[slot].active && channels[slot].chipKY >= 0) {
            switchAndRead(slot, samples);
            lastPolledSlot = slot;

            // Disconnect the ADC after reading so the TDM's ADC doesn't stay
            // connected to this channel between polls. Without this, the ADC
            // line floats at whatever voltage the last-polled channel had,
            // which can leak into the next switchAndRead() or into regular
            // ADC reads (showLEDmeasurements, adcReadings[], etc.).
            disconnectActive();

            return slot;
        }
    }

    return -1;  // No active channels with valid chip K Y
}

// ============================================================================
// ADC Management
// ============================================================================

int8_t TimeDomainMultiplexer::getChipKX() {
    // ADC0=X8, ADC1=X9, ADC2=X10, ADC3=X11
    if (adcChannel < 0 || adcChannel > 3) return 8;  // Default to ADC0
    return 8 + adcChannel;
}

int TimeDomainMultiplexer::assignFreeAdc() {
    // Check each ADC channel (0-3) for availability
    for (int adc = 0; adc < 4; adc++) {
        if (!isAdcInUseByOtherConnections(adc)) {
            adcChannel = adc;
            return adc;
        }
    }
    return -1;  // All ADCs in use
}

void TimeDomainMultiplexer::releaseAdc() {
    disconnectActive();
    adcChannel = -1;
}

bool TimeDomainMultiplexer::isAdcStillFree() {
    if (adcChannel < 0) return false;
    return !isAdcInUseByOtherConnections(adcChannel);
}

int TimeDomainMultiplexer::reassignAdc() {
    if (adcChannel < 0) return assignFreeAdc();

    // If current ADC is still free, keep it
    if (isAdcStillFree()) return adcChannel;

    // Current ADC was taken -- find a new one
    int8_t oldAdcX = getChipKX();
    int8_t oldY = -1;
    if (activeChannel >= 0 && activeChannel < TDM_MAX_CHANNELS) {
        oldY = channels[activeChannel].chipKY;
    }

    int newAdc = -1;
    for (int adc = 0; adc < 4; adc++) {
        if (adc == adcChannel) continue;  // Skip current (taken) one
        if (!isAdcInUseByOtherConnections(adc)) {
            newAdc = adc;
            break;
        }
    }

    if (newAdc < 0) return -1;  // No free ADC available

    // Disconnect from old ADC X, reconnect on new ADC X
    if (oldY >= 0 && oldY < 8) {
        sendXYrawUnchecked(CHIP_K, oldAdcX, oldY, 0);
        lastChipXY[CHIP_K].connected[oldY] &= ~(1 << oldAdcX);
    }

    adcChannel = newAdc;

    // Reconnect active channel on new ADC if there was one
    if (oldY >= 0 && oldY < 8 && activeChannel >= 0) {
        int8_t newAdcX = getChipKX();
        sendXYrawUnchecked(CHIP_K, newAdcX, oldY, 1);
        lastChipXY[CHIP_K].connected[oldY] |= (1 << newAdcX);
    }

    return newAdc;
}

// ============================================================================
// Helper: Check if ADC is used by non-TDM connections
// (Implemented here to keep TDM self-contained; reads bridge array directly)
// ============================================================================

bool isAdcInUseByOtherConnections(int adcChannel) {
    if (adcChannel < 0 || adcChannel > 3) return true;  // Invalid = occupied

    int adcNode = ADC0 + adcChannel;  // ADC0=110, ADC1=111, ADC2=112, ADC3=113

    for (int i = 0; i < globalState.connections.numBridges; i++) {
        int n1 = globalState.connections.bridges[i][0];
        int n2 = globalState.connections.bridges[i][1];

        // Skip FakeGPIO input bridges (they're expected to use this ADC)
        if (IS_FAKE_GP_IN(n1) || IS_FAKE_GP_IN(n2)) continue;

        if (n1 == adcNode || n2 == adcNode) return true;
    }

    return false;
}
