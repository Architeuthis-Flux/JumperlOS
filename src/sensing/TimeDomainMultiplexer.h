// SPDX-License-Identifier: MIT
#ifndef TIMEDOMAINMULTIPLEXER_H
#define TIMEDOMAINMULTIPLEXER_H

#include <stdint.h>

// ============================================================================
// Time Domain Multiplexer
// ============================================================================
//
// General-purpose module for multiplexing multiple analog channels through
// a single ADC on the CH446Q crossbar switch (chip K).
//
// Each channel maps a breadboard node to a chip K Y position. The shared
// ADC occupies a chip K X position (X8=ADC0, X9=ADC1, X10=ADC2, X11=ADC3).
// Only ONE channel is connected at a time. Switching disconnects the old
// channel's Y from the ADC X and connects the new channel's Y (2 SPI ops).
//
// The TDM module only reads raw voltages. Threshold/digital interpretation
// is the caller's responsibility.
//

#define TDM_MAX_CHANNELS 32

// Maximum number of hops stored per TDM channel (breadboard → chip K)
#define TDM_MAX_HOPS 4

// A single multiplexed channel
struct TdmChannel {
    bool active;            // Is this channel slot in use?
    int userNode;           // Breadboard node (1-60) that this channel reads
    int8_t chipKY;          // Chip K Y position for this channel (-1 if unknown)
    int netIndex;           // Net index for display purposes
    float lastVoltage;      // Last sampled voltage (raw, uninterpreted)

    // Full path hops for connect/disconnect during TDM switching.
    // When multiple inputs share a chip K Y position, just toggling the ADC
    // X↔Y connection isn't enough -- the breadboard-side hops still short
    // together through the shared Y bus. We store all hops so switchTo()
    // can connect/disconnect the ENTIRE path, isolating each channel.
    int8_t hopChip[TDM_MAX_HOPS]; // Chip ID for each hop (-1 = unused)
    int8_t hopX[TDM_MAX_HOPS];    // X position for each hop
    int8_t hopY[TDM_MAX_HOPS];    // Y position for each hop
    int8_t numHops;                // Number of valid hops (0 = path info not stored)
};

// The multiplexer instance
struct TimeDomainMultiplexer {
    TdmChannel channels[TDM_MAX_CHANNELS];
    int8_t adcChannel;      // Which ADC (0-3) this TDM uses, -1 if not assigned
    int activeChannel;      // Slot currently connected to ADC (-1 = none)
    int lastPolledSlot;     // Last slot read by pollNext() (for round-robin)
    int activeCount;        // Number of active channels (cached)

    // === Lifecycle ===
    void init();

    // === Channel Management ===
    // Add a channel with full path hops. Returns slot index (0-31) or -1 on failure.
    // pathChips/pathX/pathY: arrays of hop data, numHops entries.
    // The LAST hop should be the chip K connection (will be used for ADC switching).
    int addChannel(int userNode, int8_t chipKY, int netIndex,
                   const int8_t* pathChips = nullptr, const int8_t* pathX = nullptr,
                   const int8_t* pathY = nullptr, int8_t numHops = 0);
    // Remove a channel by user node. Returns 1 on success, 0 if not found.
    int removeChannel(int userNode);
    // Find channel slot by user node. Returns slot index or -1.
    int findChannel(int userNode);
    // Find a free slot. Returns slot index or -1.
    int findFreeSlot();
    // Update a channel's chip K Y position (after routing changes).
    void updateChannelChipKY(int slot, int8_t newChipKY);
    // Update a channel's full path hops (after re-routing).
    void updateChannelPath(int slot, const int8_t* pathChips, const int8_t* pathX,
                           const int8_t* pathY, int8_t numHops);

    // === Channel Switching ===
    // Connect a specific channel to the ADC (disconnects previous).
    // Returns true on success.
    bool switchTo(int slot);
    // Disconnect the currently active channel from ADC.
    void disconnectActive();

    // === Reading ===
    // Read the currently connected channel. Returns voltage.
    // Does NOT switch channels -- caller must switchTo() first.
    float readActive(int samples = 2);
    // Switch to channel + read + return voltage (convenience).
    float switchAndRead(int slot, int samples = 2);
    // Round-robin: switch to next active channel, read, store in lastVoltage.
    // Returns slot index that was read, or -1 if no active channels.
    int pollNext(int samples = 2);

    // === ADC Management ===
    // Get chip K X position for current ADC (ADC0=8, ADC1=9, ADC2=10, ADC3=11).
    int8_t getChipKX();
    // Find and assign a free ADC channel. Returns ADC channel (0-3) or -1.
    int assignFreeAdc();
    // Release the current ADC channel.
    void releaseAdc();
    // Check if our ADC was claimed by another connection.
    bool isAdcStillFree();
    // Find a new ADC if current one was taken. Returns new channel or -1.
    int reassignAdc();
};

#endif // TIMEDOMAINMULTIPLEXER_H
