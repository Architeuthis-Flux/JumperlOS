// SPDX-License-Identifier: MIT
#ifndef PIO_REGISTRY_H
#define PIO_REGISTRY_H

#include <Arduino.h>
#include "hardware/pio.h"

// Every firmware PIO program load logs its placement here so the PIO panels
// (X's PIO block, the debug menu's "PIO Status") print measured truth instead
// of inference - task #30's tooling gap: the SDK doesn't expose per-block
// instruction usage, and the budget doc's block arithmetic was provably off
// by a word (the 1-instruction encoder sampler fit on a "32/32 full" PIO0).
//
// name must be a string LITERAL (stored by pointer). sm may be -1 when the
// program isn't tied to one SM (or the SM isn't known at the call site).
// Loads all happen on core 1's single-sequence init; the one runtime
// add/remove pair (the debug button analyzer, and the probe LED A/B flip)
// is rare enough that the unlocked table is fine for a diagnostics panel.

void pioRegistryLog(PIO pio, int sm, uint offset, uint length, const char* name);
void pioRegistryUnlog(PIO pio, uint offset, const char* name);
void pioRegistryPrint(Stream& target);

#endif
