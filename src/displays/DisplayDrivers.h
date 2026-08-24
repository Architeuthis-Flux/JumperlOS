// SPDX-License-Identifier: MIT
#ifndef DISPLAY_DRIVERS_H
#define DISPLAY_DRIVERS_H

#include "DisplayTypes.h"

struct PartDefinition;

// The driver registry (rodata descriptors + family ops).
// Key is the partdb driverKey, case-insensitive ASCII.
const DisplayDriverDesc* findDisplayDriver(const char* key);

// TEMPORARY resolver until the parts DB lands (B-M1): matches a part's
// partId against the known driver keys/aliases. partdbResolveDriver()
// replaces this at integration - the precedence rules live there.
const DisplayDriverDesc* displayResolveForPart(const PartDefinition& p);

#endif // DISPLAY_DRIVERS_H
