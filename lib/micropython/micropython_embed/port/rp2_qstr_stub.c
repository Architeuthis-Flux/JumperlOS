/*
 * Stub file for QSTR scanning on the host compiler.
 * This file provides the MP_REGISTER_MODULE, MP_REGISTER_ROOT_POINTER,
 * and MP_QSTR references from the rp2 module files so the MicroPython
 * build system can generate the necessary headers without needing
 * Pico SDK hardware headers.
 *
 * The actual implementation is in modrp2_jl.c, rp2_pio_jl.c, rp2_dma_jl.c
 */

#include "py/obj.h"

// From modrp2_jl.c: register the _rp2 module
extern const mp_obj_module_t mp_module_rp2;
MP_REGISTER_MODULE(MP_QSTR__rp2, mp_module_rp2);

// From rp2_pio_jl.c: root pointers for PIO IRQ objects
// NUM_PIOS is 2 on RP2040, 3 on RP2350 — use 3 as max
MP_REGISTER_ROOT_POINTER(void *rp2_pio_irq_obj[3]);
MP_REGISTER_ROOT_POINTER(void *rp2_state_machine_irq_obj[3 * 4]);

// From rp2_dma_jl.c: root pointer for DMA IRQ objects
// NUM_DMA_CHANNELS is typically 12-16; use 16 as safe max
MP_REGISTER_ROOT_POINTER(void *rp2_dma_irq_obj[16]);

// From machine_pin_jl.c: root pointers for Pin.irq() objects (GPIO 0..29)
MP_REGISTER_ROOT_POINTER(void *machine_pin_irq_obj[30]);

// Force all PIO/DMA/rp2 QSTRs into the QSTR scan.
// These are used by the actual source files but can't be scanned directly
// because those files include Pico SDK hardware headers.
static void rp2_qstr_refs(void) {
    // Module names
    (void)MP_QSTR__rp2; (void)MP_QSTR_rp2;

    // modrp2_jl.c: module globals
    (void)MP_QSTR_PIO; (void)MP_QSTR_StateMachine; (void)MP_QSTR_DMA;
    (void)MP_QSTR_bootsel_button;

    // rp2_pio_jl.c: PIO type methods and constants
    (void)MP_QSTR_add_program; (void)MP_QSTR_remove_program;
    (void)MP_QSTR_state_machine; (void)MP_QSTR_gpio_base;
    (void)MP_QSTR_irq;
    (void)MP_QSTR_IN_LOW; (void)MP_QSTR_IN_HIGH;
    (void)MP_QSTR_OUT_LOW; (void)MP_QSTR_OUT_HIGH;
    (void)MP_QSTR_SHIFT_LEFT; (void)MP_QSTR_SHIFT_RIGHT;
    (void)MP_QSTR_JOIN_NONE; (void)MP_QSTR_JOIN_TX; (void)MP_QSTR_JOIN_RX;
    (void)MP_QSTR_IRQ_SM0; (void)MP_QSTR_IRQ_SM1;
    (void)MP_QSTR_IRQ_SM2; (void)MP_QSTR_IRQ_SM3;

    // rp2_pio_jl.c: PIO.irq() args
    (void)MP_QSTR_handler; (void)MP_QSTR_trigger; (void)MP_QSTR_hard;

    // machine_pin_jl.c: Pin.irq() trigger constants
    (void)MP_QSTR_IRQ_RISING; (void)MP_QSTR_IRQ_FALLING;

    // rp2_pio_jl.c: StateMachine type methods
    (void)MP_QSTR_active; (void)MP_QSTR_restart;
    (void)MP_QSTR_exec; (void)MP_QSTR_get; (void)MP_QSTR_put;
    (void)MP_QSTR_rx_fifo; (void)MP_QSTR_tx_fifo;

    // rp2_pio_jl.c: StateMachine.init() args
    (void)MP_QSTR_prog; (void)MP_QSTR_freq;
    (void)MP_QSTR_in_base; (void)MP_QSTR_out_base;
    (void)MP_QSTR_set_base; (void)MP_QSTR_jmp_pin; (void)MP_QSTR_sideset_base;
    (void)MP_QSTR_in_shiftdir; (void)MP_QSTR_out_shiftdir;
    (void)MP_QSTR_push_thresh; (void)MP_QSTR_pull_thresh;

    // rp2_pio_jl.c: StateMachine.exec() imports
    (void)MP_QSTR_asm_pio_encode;

    // rp2_dma_jl.c: DMA type methods and attributes
    (void)MP_QSTR_config; (void)MP_QSTR_pack_ctrl; (void)MP_QSTR_unpack_ctrl;
    (void)MP_QSTR_close; (void)MP_QSTR_channel; (void)MP_QSTR_registers;
    (void)MP_QSTR_ctrl; (void)MP_QSTR_read; (void)MP_QSTR_write; (void)MP_QSTR_count;
    (void)MP_QSTR_transfer_count;

    // rp2_dma_jl.c: DMA control fields
    (void)MP_QSTR_enable; (void)MP_QSTR_size;
    (void)MP_QSTR_high_pri; (void)MP_QSTR_inc_read; (void)MP_QSTR_inc_write;
    (void)MP_QSTR_inc_read_rev; (void)MP_QSTR_inc_write_rev;
    (void)MP_QSTR_ring_size; (void)MP_QSTR_ring_sel;
    (void)MP_QSTR_chain_to; (void)MP_QSTR_treq_sel;
    (void)MP_QSTR_irq_quiet; (void)MP_QSTR_bswap; (void)MP_QSTR_sniff_en;
    (void)MP_QSTR_busy; (void)MP_QSTR_write_err; (void)MP_QSTR_read_err;
    (void)MP_QSTR_ahb_err; (void)MP_QSTR_default;
}
