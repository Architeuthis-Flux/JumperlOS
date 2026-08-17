// SPDX-License-Identifier: MIT
#include "CoreMailbox.h"
#include "hardware/sync.h"
#include "pico/platform.h"

namespace core1req {

struct SlotState {
    volatile uint32_t bits;
    volatile uint32_t reqGen;
    volatile uint32_t doneGen;
};

static SlotState s_slot[ SLOT_COUNT ] = { };

// Fixed SIO spinlock (see the header for why fixed, not claimed).
static inline spin_lock_t* lock( void ) {
    return spin_lock_instance( PICO_SPINLOCK_ID_OS2 );
}

// The core-1 side (take / complete) runs from RAM: sendPaths() and its
// callers are RAM-resident since T1.8, and these sit right next to them.

uint32_t post( Slot s, uint32_t bits ) {
    SlotState& st = s_slot[ s ];
    uint32_t save = spin_lock_blocking( lock( ) );
    st.bits |= bits;
    uint32_t gen = ++st.reqGen;
    spin_unlock( lock( ), save );
    return gen;
}

bool pending( Slot s ) {
    __dmb( );
    return s_slot[ s ].bits != 0;
}

uint32_t __not_in_flash_func( take )( Slot s, uint32_t* gen ) {
    SlotState& st = s_slot[ s ];
    uint32_t save = spin_lock_blocking( lock( ) );
    uint32_t bits = st.bits;
    if ( bits != 0 ) {
        st.bits = 0;
        if ( gen ) *gen = st.reqGen;
    }
    spin_unlock( lock( ), save );
    return bits;
}

void __not_in_flash_func( complete )( Slot s, uint32_t gen ) {
    SlotState& st = s_slot[ s ];
    uint32_t save = spin_lock_blocking( lock( ) );
    if ( (int32_t)( gen - st.doneGen ) > 0 ) {
        st.doneGen = gen;
    }
    spin_unlock( lock( ), save );
}

bool isDone( Slot s, uint32_t gen ) {
    __dmb( );
    return (int32_t)( s_slot[ s ].doneGen - gen ) >= 0;
}

bool idle( Slot s ) {
    __dmb( );
    const SlotState& st = s_slot[ s ];
    // Nothing pending, and everything requested has completed.
    return st.bits == 0 && (int32_t)( st.doneGen - st.reqGen ) >= 0;
}

bool allIdle( void ) {
    return idle( REQ_SEND ) && idle( REQ_BYPASS );
}

void snapshot( Slot s, uint32_t* bits, uint32_t* reqGen, uint32_t* doneGen ) {
    __dmb( );
    if ( bits ) *bits = s_slot[ s ].bits;
    if ( reqGen ) *reqGen = s_slot[ s ].reqGen;
    if ( doneGen ) *doneGen = s_slot[ s ].doneGen;
}

} // namespace core1req
