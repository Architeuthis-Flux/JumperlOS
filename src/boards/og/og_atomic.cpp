// SPDX-License-Identifier: MIT
//
// __atomic_test_and_set for the RP2040 (OG Jumperless).
//
// The cortex-m0plus has no native atomic (LDREX/STREX) instructions, so GCC
// lowers __atomic_test_and_set (used by the cross-core ADC lock in
// Peripherals.cpp readAdc()) to a libcall. This
// bare-metal toolchain ships no libatomic and libgcc for this multilib does
// not provide __atomic_test_and_set, so we provide it here. The M33-based V5
// inlines the instruction and never references the symbol, so this file is a
// no-op in the V5 build.
//
// Correctness: the ADC lock is contended BETWEEN THE TWO CORES, so masking
// interrupts is not enough - we use an RP2040 hardware spinlock to make the
// byte test-and-set genuinely atomic across cores.
//
// We use a FIXED spinlock instance (PICO_SPINLOCK_ID_OS1) rather than claiming
// one in a global constructor. A constructor that ran before the SDK was ready
// (or that panicked out of spin_lock_claim_unused) would hang the board before
// USB ever enumerates - exactly the kind of silent early-boot failure we must
// avoid. RP2040 hardware spinlocks are just SIO registers, reset at boot by the
// SDK runtime init, so a fixed instance is safe to use directly without the
// hw_claim bookkeeping. OS1 is reserved for OS/user use and we are its only
// user here.
//
// ponytail: one fixed spinlock guards every __atomic_test_and_set call. Fine
// for the ADC lock / LA setup (negligible contention); if a future hot path
// needs lock-free byte TAS, upgrade to GCC-libatomic-style pointer-hashed
// spinlock striping.

#if defined(OG_JUMPERLESS)

#include <stdbool.h>
#include <stdint.h>
#include "hardware/sync.h"
#include "pico/sync.h"

extern "C" bool __atomic_test_and_set( volatile void *ptr, int memorder ) {
    (void)memorder;
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    spin_lock_t *lock = spin_lock_instance( PICO_SPINLOCK_ID_OS1 );
    uint32_t save = spin_lock_blocking( lock );
    bool prev = ( *p != 0 );
    *p = 1;
    spin_unlock( lock, save );
    return prev;
}

#endif // OG_JUMPERLESS
