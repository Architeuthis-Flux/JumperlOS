// SPDX-License-Identifier: MIT
//
// InfraPaths - system-owned connections and resource arbitration.
//
// The firmware needs crossbar connections of its own: probe buffer power
// (a DAC or a routable GPIO feeding ROUTABLE_BUFFER_IN), the OLED's I2C
// lock bridges, the serial_1 UART lock bridges. Historically each of these
// grew its own machinery (handleLockedConnections + clones, the GPIO claim
// in routableBufferPower, scrubStaleGpioBufferBridges, the setDacXvoltage
// probe-power swap, findUnusedRoutableGpio's inline skip list). This module
// replaces all of that with one table:
//
//   InfraFunction: a system need (probe_power, oled_i2c, serial_1) with an
//   ORDERED list of candidates. Each candidate resolves its node pair(s)
//   from live config, reports whether its resources are free of user
//   claims, and carries idempotent hardware setup/teardown callbacks.
//
// infraEvaluate() runs at the head of every hardware-applying netlist
// rebuild (inside the core1busy window, before loadBridgesFromState) and
// converges every function onto its most-preferred viable candidate. The
// user claiming a resource (bridging DAC1, taking a GPIO in MicroPython,
// setting a DAC outside the probe-power window) makes that candidate
// non-viable, and the function transparently relocates; freeing the
// resource switches it back on the next rebuild. Physical crosspoints only
// change via sendPaths at the rebuild tail, so rebuild-head evaluation is
// exactly aligned with when user claims become real.
//
// Infra bridges are invisible to the rest of the system by construction:
//   - never serialized into slots (serializeBridges skips them),
//   - never recorded in undo history (undoRecord* skips them),
//   - never given duplicate paths (added with dup=0, and fillUnusedPaths
//     skips them regardless) - so their resistance is a known
//     crosspoint-count times calibration.crosspoint_resistance,
//   - scrubbed out of loaded slots from older firmware
//     (infraScrubLoadedBridges).

#pragma once

#include <Arduino.h>
#include <stdint.h>

// One node pair an infra function has live in the bridge array.
struct InfraPair {
    int16_t a, b;
};

// ---------------------------------------------------------------------------
// Evaluation + queries
// ---------------------------------------------------------------------------

// Converge every infra function onto its most-preferred viable candidate.
// MUST be called only from the head of a hardware-applying rebuild
// (refreshConnections / refreshLocalConnections / fastRefresh), inside the
// core1busy window, before loadBridgesFromState(). Never from slot preview.
void infraEvaluate(void);

// True if (n1,n2) is an active infra pair (order-agnostic). O(active pairs).
bool infraIsBridge(int n1, int n2);

// True if any active infra pair touches this node.
bool infraOwnsNode(int node);

// ---------------------------------------------------------------------------
// Probe buffer power
// ---------------------------------------------------------------------------

// The node currently feeding ROUTABLE_BUFFER_IN (DAC1 / DAC0 / RP_GPIO_x),
// or -1 when probe power is off / no candidate was viable.
int infraProbePowerSource(void);

// gpioDef index of the active GPIO power claim, or -1 when probe power is
// DAC-fed or off. Replaces probeGpioPowerClaimIdx().
int infraProbePowerGpioIdx(void);

// Turn the probe-power function on/off (the routableBufferPower wrapper and
// config appliers call this). Does not refresh by itself.
void infraSetProbePowerEnabled(bool on);
bool infraProbePowerWanted(void);

// Total source resistance of the GPIO-powered buffer feed for the droop
// current model: calibration.probe_droop_ohms when calibrated (> 0), else
// probe_pad_ohms + N x crosspoint_resistance where N is counted from the
// ACTUAL routed feed path - exact because infra bridges are never
// duplicated. Falls back to pad + 4 crosspoints when the path isn't routed
// yet (the standard GPIO->L->K shape).
float infraProbeDroopOhms(void);

// ---------------------------------------------------------------------------
// Nudges
// ---------------------------------------------------------------------------

// Request re-evaluation: sets a sticky flag and starts a local refresh now
// if none is in progress. Safe from any core-0 context outside a rebuild.
// (The sticky flag exists because a nudge landing MID-refresh was already
// missed by that refresh's evaluation - infraServiceTick retries it.)
void infraNudge(void);

// Periodic (~500ms) retry of pending nudges + viability re-check of active
// candidates (catches claims that never touch the bridge array, e.g.
// MicroPython taking the claim pin or PWM being enabled on it mid-claim).
//
// KNOWN GAP (behavior parity with the old handleLockedConnections): toggling
// top_oled/serial_1 lock_connection at runtime changes enabled() but nothing
// nudges a rebuild, so the lock appears/disappears at the next refresh from
// any source - same latency the old clear/load-only mechanism had. Wire an
// infraNudge() into the config appliers if that ever matters.
void infraServiceTick(void);

// ---------------------------------------------------------------------------
// Slot-load migration
// ---------------------------------------------------------------------------

// Drop bridges from a just-deserialized slot that belong to the system, not
// the user: feed-shaped BUFFER_IN pairs (DAC/routable-GPIO to BUFFER_IN -
// stale power claims from firmware that persisted them), plus active
// lock-function pairs (re-added fresh by evaluation). Other BUFFER_IN
// bridges are deliberate user data and survive. Also restores pin config
// for stale GPIO claims. Called from the state load sanitizer.
void infraScrubLoadedBridges(void);

// ---------------------------------------------------------------------------
// Test / calibration override
// ---------------------------------------------------------------------------

// Pin a function to one candidate index (bypasses preference order but NOT
// viability), or -1 to return to automatic. Returns 0 on success. Used by
// the self test and the dual calibration. Function names: "probe_power",
// "oled_i2c", "serial_1".
int infraForceCandidate(const char* fnName, int candIdx);

// Call after writing DAC hardware with save=0 (state untouched) - e.g. the
// self test's normalize step and calibrateDacs' sweeps. The probe feed's
// skip-if-parked guard trusts globalState.power; the bump forces one
// unconditional hardware re-write on the next park so a blinded MCP4728
// can't hide behind an in-window state value.
void infraDacParkEpochBump(void);

// ---------------------------------------------------------------------------
// Ephemeral ADC resource pool
// ---------------------------------------------------------------------------
//
// The sibling of the persistent functions: system consumers that need AN adc
// - not a specific one - for transient taps. One ownership table + one
// user-claim predicate replaces the scattered per-consumer scans
// (pickScanAdc, isAdcInUseByOtherConnections, MeasureMode::findUnusedADC).
// A user bridging an ADC node claims it out of the pool the same way
// bridging DAC1 claims it away from probe power.

enum InfraAdcUser : uint8_t {
    INFRA_ADC_NONE = 0,
    INFRA_ADC_TDM,     // FakeGpio TimeDomainMultiplexer (core 1)
    INFRA_ADC_NVSCAN,  // net voltage scan taps (core 1, per-tap acquire)
    INFRA_ADC_MEASURE, // measure mode's ephemeral ADC bridge (core 0)
};

// Acquire a free ADC channel out of candidateMask (bit N = ADCN, ADC0-4).
// Skips channels claimed by a user bridge or owned by another pool user.
// Re-acquiring while already owning a still-viable channel returns it.
// allowSharedTdm: with no exclusive channel free, ride along on TDM's
// channel WITHOUT taking ownership (both consumers tap sequentially on
// core 1 and disconnect after each read - the old pickScanAdc fallback).
// Returns the channel, or -1.
int infraAcquireAdc(InfraAdcUser user, uint8_t candidateMask, bool allowSharedTdm);

// Release every channel this user owns. No-op for shared rides.
void infraReleaseAdc(InfraAdcUser user);

// Channels in candidateMask that are neither user-claimed nor pool-owned.
// For dry-run displays and the future parallel multi-ADC scan.
uint8_t infraFreeAdcMask(uint8_t candidateMask);

// The single user-claim predicate: a non-ephemeral, non-FakeGPIO-input
// bridge touches this ADC's node.
bool infraAdcUserClaimed(int adcChannel);

// Human-readable status dump (the `infra?` terminal command).
void infraPrintStatus(Stream* out);
