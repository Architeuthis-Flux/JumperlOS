/*
 * Minimal machine.Pin implementation for Jumperless embed port (RP23xx)
 * Provides basic Pin IN/OUT/OPEN_DRAIN functionality with pull control.
 *
 * Behaviour mirrors the rp2 port where practical, but omits
 * board/cpu pin name tables for now. Pins are addressed by integer id.
 * Pin.irq() is supported (adapted from ports/rp2/machine_pin.c).
 */

#include <stdio.h>
#include <string.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "shared/runtime/mpirq.h"



// No need to include extmod/modmachine.h here

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/iobank0.h"

// Forward declaration for pin ownership function
extern void jl_gpio_claim_pin(int pin);

// Jumperless node type extern for pin mapping
extern const mp_obj_type_t node_type;

// Fallback Pin protocol definitions if extmod/virtpin.h is not packaged
#ifndef MP_PIN_READ
#define MP_PIN_READ   (1)
#define MP_PIN_WRITE  (2)
typedef struct _mp_pin_p_t {
    mp_uint_t (*ioctl)(mp_obj_t obj, mp_uint_t request, uintptr_t arg, int *errcode);
} mp_pin_p_t;
#endif

// Pull flags (match rp2 values used by machine.Pin)
#define JL_GPIO_PULL_UP    (1)
#define JL_GPIO_PULL_DOWN  (2)

// All four GPIO IRQ event bits (level low/high, edge fall/rise)
#define JL_GPIO_IRQ_ALL    (0xf)

// Default number of GPIOs on RP2 family bank0 (GP0..GP29)
#ifndef JL_NUM_GPIOS
#define JL_NUM_GPIOS (30)
#endif

typedef struct _machine_pin_obj_t {
    mp_obj_base_t base;
    uint8_t id;
} machine_pin_obj_t;



// Forward declare the type used in static initialiser below
extern const mp_obj_type_t machine_pin_type;

// Track which pins are configured as OPEN_DRAIN to emulate behaviour
static uint64_t jl_open_drain_mask;

// Forward decls
static mp_obj_t mp_pin_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args);

// Statically-defined pin objects for GPIO 0..JL_NUM_GPIOS-1
static const machine_pin_obj_t machine_pin_obj_table[JL_NUM_GPIOS] = {
#define PIN_OBJ_ENTRY(n) { .base = { &machine_pin_type }, .id = (uint8_t)(n) }
    PIN_OBJ_ENTRY(0),  PIN_OBJ_ENTRY(1),  PIN_OBJ_ENTRY(2),  PIN_OBJ_ENTRY(3),
    PIN_OBJ_ENTRY(4),  PIN_OBJ_ENTRY(5),  PIN_OBJ_ENTRY(6),  PIN_OBJ_ENTRY(7),
    PIN_OBJ_ENTRY(8),  PIN_OBJ_ENTRY(9),  PIN_OBJ_ENTRY(10), PIN_OBJ_ENTRY(11),
    PIN_OBJ_ENTRY(12), PIN_OBJ_ENTRY(13), PIN_OBJ_ENTRY(14), PIN_OBJ_ENTRY(15),
    PIN_OBJ_ENTRY(16), PIN_OBJ_ENTRY(17), PIN_OBJ_ENTRY(18), PIN_OBJ_ENTRY(19),
    PIN_OBJ_ENTRY(20), PIN_OBJ_ENTRY(21), PIN_OBJ_ENTRY(22), PIN_OBJ_ENTRY(23),
    PIN_OBJ_ENTRY(24), PIN_OBJ_ENTRY(25), PIN_OBJ_ENTRY(26), PIN_OBJ_ENTRY(27),
    PIN_OBJ_ENTRY(28), PIN_OBJ_ENTRY(29)
#undef PIN_OBJ_ENTRY
};

// ---------------------------------------------------------------------------
// GPIO IRQ support (adapted from ports/rp2/machine_pin.c)
// ---------------------------------------------------------------------------

typedef struct _machine_pin_irq_obj_t {
    mp_irq_obj_t base;
    uint32_t flags;
    uint32_t trigger;
} machine_pin_irq_obj_t;

static const mp_irq_methods_t machine_pin_irq_methods;

// Whether our shared IO_IRQ_BANK0 handler is currently installed
static bool jl_pin_irq_handler_installed;

static void jl_gpio_irq(void) {
    // Only service the registers covering GPIO 0..JL_NUM_GPIOS-1
    for (int i = 0; i < (JL_NUM_GPIOS + 7) / 8; ++i) {
        uint32_t intr = iobank0_hw->intr[i];
        if (intr) {
            for (int j = 0; j < 8; ++j) {
                if (intr & 0xf) {
                    uint32_t gpio = 8 * i + j;
                    if (gpio >= JL_NUM_GPIOS) {
                        break;
                    }
                    gpio_acknowledge_irq(gpio, intr & 0xf);
                    machine_pin_irq_obj_t *irq = MP_STATE_PORT(machine_pin_irq_obj[gpio]);
                    if (irq != NULL && (intr & irq->trigger)) {
                        irq->flags = intr & irq->trigger;
                        mp_irq_handler(&irq->base);
                    }
                }
                intr >>= 4;
            }
        }
    }
}

// Called from mp_embed_init() (after mp_init) so each VM lifecycle starts with
// clean IRQ state; safe to call even if the previous deinit was skipped.
void machine_pin_irq_init(void) {
    memset(MP_STATE_PORT(machine_pin_irq_obj), 0, sizeof(MP_STATE_PORT(machine_pin_irq_obj)));
    if (!jl_pin_irq_handler_installed) {
        irq_add_shared_handler(IO_IRQ_BANK0, jl_gpio_irq, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(IO_IRQ_BANK0, true);
        jl_pin_irq_handler_installed = true;
    }
}

// Called from mp_embed_deinit() (before mp_deinit); disables all pin IRQs so
// no callback can fire into a torn-down VM. The shared handler stays installed
// (removing/re-adding shared handlers repeatedly can exhaust SDK handler slots).
void machine_pin_irq_deinit(void) {
    if (!jl_pin_irq_handler_installed) {
        return;
    }
    for (int i = 0; i < JL_NUM_GPIOS; ++i) {
        gpio_set_irq_enabled(i, JL_GPIO_IRQ_ALL, false);
        MP_STATE_PORT(machine_pin_irq_obj[i]) = NULL;
    }
}

static void machine_pin_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "Pin(%u)", (unsigned)self->id);
}

enum {
    ARG_mode, ARG_pull, ARG_value, ARG_alt
};

static const mp_arg_t jl_allowed_args[] = {
    { MP_QSTR_mode,  MP_ARG_OBJ,                  { .u_rom_obj = MP_ROM_NONE } },
    { MP_QSTR_pull,  MP_ARG_OBJ,                  { .u_rom_obj = MP_ROM_NONE } },
    { MP_QSTR_value, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    { MP_QSTR_alt,   MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = GPIO_FUNC_SIO } },
};

static mp_obj_t machine_pin_obj_init_helper(const machine_pin_obj_t *self, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mp_arg_val_t args[MP_ARRAY_SIZE(jl_allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(jl_allowed_args), jl_allowed_args, args);

    // Determine initial value for OUT/OPEN_DRAIN modes
    int initial_value = -1;
    if (args[ARG_value].u_obj != mp_const_none) {
        initial_value = mp_obj_is_true(args[ARG_value].u_obj);
    }

    // Configure mode if provided
    if (args[ARG_mode].u_obj != mp_const_none) {
        mp_int_t mode = mp_obj_get_int(args[ARG_mode].u_obj);
        if (mode == 0 /* MACHINE_PIN_MODE_IN */) {
            gpio_set_dir(self->id, GPIO_IN);
            gpio_set_function(self->id, GPIO_FUNC_SIO);
            jl_open_drain_mask &= ~(1ULL << self->id);
        } else if (mode == 1 /* MACHINE_PIN_MODE_OUT */) {
            // Set initial value before configuring as output
            if (initial_value != -1) {
                gpio_put(self->id, initial_value);
            }
            gpio_set_dir(self->id, GPIO_OUT);
            gpio_set_function(self->id, GPIO_FUNC_SIO); 
            jl_open_drain_mask &= ~(1ULL << self->id);
        } else if (mode == 2 /* MACHINE_PIN_MODE_OPEN_DRAIN */) {
            // Open-drain: high=INPUT, low=OUTPUT driving 0
            int od_high = initial_value == -1 ? 1 : initial_value;
            if (od_high) {
                gpio_set_dir(self->id, GPIO_IN); // float
            } else {
                gpio_put(self->id, 0);
                gpio_set_dir(self->id, GPIO_OUT); // drive low
            }
            gpio_set_function(self->id, GPIO_FUNC_SIO);
            jl_open_drain_mask |= (1ULL << self->id);
        } else if (mode == 3 /* MACHINE_PIN_MODE_ALT */) {
            uint32_t af = (uint32_t)args[ARG_alt].u_int;
            gpio_set_function(self->id, af);
            jl_open_drain_mask &= ~(1ULL << self->id);
        } else {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid pin mode"));
        }
    }

    // Configure pull (unconditionally because None means no-pull)
    uint32_t pull_bits = 0;
    if (args[ARG_pull].u_obj != mp_const_none) {
        pull_bits = (uint32_t)mp_obj_get_int(args[ARG_pull].u_obj);
    }
    bool up = pull_bits & JL_GPIO_PULL_UP;
    bool down = pull_bits & JL_GPIO_PULL_DOWN;
    
    gpio_set_pulls(self->id, up, down);

    // CRITICAL: Claim pin for MicroPython to prevent background GPIO reading interference
    // This is essential for timing-critical operations like NeoPixel control
    // Claim the pin whenever init() is called to ensure bitstream operations work
    jl_gpio_claim_pin(self->id);

    return mp_const_none;
}

// constructor(id, ...)
static mp_obj_t mp_pin_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    int wanted_pin = -1;

    // Accept integers or Jumperless node objects
    if (mp_obj_is_int(all_args[0])) {
        wanted_pin = mp_obj_get_int(all_args[0]);
    } else if (mp_obj_get_type(all_args[0]) == &node_type) {
        // Support passing j.GPIO_1 etc directly
        // Call the unary op MP_UNARY_OP_INT_MAYBE on the node object
        // This is implemented in modjumperless.c:node_unary_op
        mp_obj_t val = mp_unary_op(MP_UNARY_OP_INT_MAYBE, all_args[0]);
        if (val != MP_OBJ_NULL) {
            wanted_pin = mp_obj_get_int(val);
        }
    }

    if (wanted_pin == -1) {
        mp_raise_ValueError(MP_ERROR_TEXT("Pin id must be integer or node"));
    }

    // Jumperless mapping logic:
    // Some "pins" passed in might be Jumperless node numbers (131..138, etc.)
    // We need to map these to the physical 0..29 RP2350 pins.
    
    if (wanted_pin >= 131 && wanted_pin <= 138) {
        // RP_GPIO_1..8 (131..138) map to Physical 20..27
        wanted_pin = (wanted_pin - 131) + 20;
    } else if (wanted_pin == 116) {
        // RP_UART_TX (116) maps to Physical 0
        wanted_pin = 0;
    } else if (wanted_pin == 117) {
        // RP_UART_RX (117) maps to Physical 1
        wanted_pin = 1;
    }

    bool valid = (wanted_pin >= 0 && wanted_pin < JL_NUM_GPIOS);
    if (!valid) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid pin"));
    }

    const machine_pin_obj_t *self = &machine_pin_obj_table[wanted_pin];

    if (n_args > 1 || n_kw > 0) {
        // pin mode given, so configure this GPIO
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, n_kw, all_args + n_args);
        machine_pin_obj_init_helper(self, n_args - 1, all_args + 1, &kw_args);
    } else {
        // Even if no mode is specified, we need to initialize the pin for GPIO use
        // This is critical for operations like NeoPixels that use bitstream
        printf("[Pin Init] Initializing GPIO %d for SIO function\n", self->id);
        gpio_init(self->id);  // Initialize pin for GPIO (SIO function)
        gpio_set_function(self->id, GPIO_FUNC_SIO);  // Explicitly set to SIO
        printf("[Pin Init] GPIO %d function = %d (should be %d for SIO)\n", 
               self->id, gpio_get_function(self->id), GPIO_FUNC_SIO);
        
        // Claim the pin to prevent interference from readGPIO()
        jl_gpio_claim_pin(self->id);
    }

    return MP_OBJ_FROM_PTR(self);
}

// fast method for getting/setting pin value
static mp_obj_t machine_pin_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    (void)n_kw;
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (n_args == 0) {
        return MP_OBJ_NEW_SMALL_INT(gpio_get(self->id));
    } else {
        bool value = mp_obj_is_true(args[0]);
        if (jl_open_drain_mask & (1ULL << self->id)) {
            // Open-drain: drive low = output, drive high = input (float)
            if (!value) {
                gpio_put(self->id, 0);
                gpio_set_dir(self->id, GPIO_OUT);
            } else {
                gpio_set_dir(self->id, GPIO_IN);
            }
        } else {
            gpio_put(self->id, value);
        }
        return mp_const_none;
    }
}

// pin.init(mode=..., pull=..., *, value=None, alt=GPIO_FUNC_SIO)
static mp_obj_t machine_pin_obj_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return machine_pin_obj_init_helper(args[0], n_args - 1, args + 1, kw_args);
}
MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_init_obj, 1, machine_pin_obj_init);

// pin.value([value])
static mp_obj_t machine_pin_value(size_t n_args, const mp_obj_t *args) {
    return machine_pin_call(args[0], n_args - 1, 0, args + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_value_obj, 1, 2, machine_pin_value);

// pin.low()
static mp_obj_t machine_pin_low(mp_obj_t self_in) {
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (jl_open_drain_mask & (1ULL << self->id)) {
        gpio_put(self->id, 0);
        gpio_set_dir(self->id, GPIO_OUT);
    } else {
        gpio_put(self->id, 0);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_low_obj, machine_pin_low);

// pin.high()
static mp_obj_t machine_pin_high(mp_obj_t self_in) {
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (jl_open_drain_mask & (1ULL << self->id)) {
        gpio_set_dir(self->id, GPIO_IN);
    } else {
        gpio_put(self->id, 1);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_high_obj, machine_pin_high);

// pin.toggle()
static mp_obj_t machine_pin_toggle(mp_obj_t self_in) {
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    int v = gpio_get(self->id);
    mp_obj_t args[2] = { self_in, MP_OBJ_NEW_SMALL_INT(!v) };
    return machine_pin_value(2, args);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_toggle_obj, machine_pin_toggle);

// Get (or lazily allocate) the IRQ object for a pin
static machine_pin_irq_obj_t *machine_pin_get_irq(uint8_t pin) {
    machine_pin_irq_obj_t *irq = MP_STATE_PORT(machine_pin_irq_obj[pin]);
    if (irq == NULL) {
        irq = m_new_obj(machine_pin_irq_obj_t);
        irq->base.base.type = &mp_irq_type;
        irq->base.methods = (mp_irq_methods_t *)&machine_pin_irq_methods;
        irq->base.parent = MP_OBJ_FROM_PTR(&machine_pin_obj_table[pin]);
        irq->base.handler = mp_const_none;
        irq->base.ishard = false;
        irq->flags = 0;
        irq->trigger = 0;
        MP_STATE_PORT(machine_pin_irq_obj[pin]) = irq;
    }
    return irq;
}

// pin.irq(handler=None, trigger=IRQ_FALLING|IRQ_RISING, hard=False)
static mp_obj_t machine_pin_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_handler, ARG_trigger, ARG_hard };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_handler, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_trigger, MP_ARG_INT, {.u_int = GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE} },
        { MP_QSTR_hard, MP_ARG_BOOL, {.u_bool = false} },
    };
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    machine_pin_irq_obj_t *irq = machine_pin_get_irq(self->id);

    if (n_args > 1 || kw_args->used != 0) {
        mp_obj_t handler = args[ARG_handler].u_obj;
        mp_uint_t trigger = (mp_uint_t)args[ARG_trigger].u_int;

        // Disable this pin's IRQs while data is updated
        gpio_set_irq_enabled(self->id, JL_GPIO_IRQ_ALL, false);

        irq->base.handler = handler;
        irq->base.ishard = args[ARG_hard].u_bool;
        irq->flags = 0;
        irq->trigger = (uint32_t)trigger;

        if (handler != mp_const_none && trigger != 0) {
            gpio_set_irq_enabled(self->id, trigger, true);
        }
    }
    return MP_OBJ_FROM_PTR(irq);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_irq_obj_fun, 1, machine_pin_irq);

// mpirq protocol: irq_obj.trigger(new_trigger)
static mp_uint_t machine_pin_irq_trigger(mp_obj_t self_in, mp_uint_t new_trigger) {
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    machine_pin_irq_obj_t *irq = MP_STATE_PORT(machine_pin_irq_obj[self->id]);
    gpio_set_irq_enabled(self->id, JL_GPIO_IRQ_ALL, false);
    irq->flags = 0;
    irq->trigger = (uint32_t)new_trigger;
    gpio_set_irq_enabled(self->id, new_trigger, true);
    return 0;
}

// mpirq protocol: irq_obj.flags() / triggers query
static mp_uint_t machine_pin_irq_info(mp_obj_t self_in, mp_uint_t info_type) {
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    machine_pin_irq_obj_t *irq = MP_STATE_PORT(machine_pin_irq_obj[self->id]);
    if (info_type == MP_IRQ_INFO_FLAGS) {
        return irq->flags;
    } else if (info_type == MP_IRQ_INFO_TRIGGERS) {
        return irq->trigger;
    }
    return 0;
}

static const mp_irq_methods_t machine_pin_irq_methods = {
    .trigger = machine_pin_irq_trigger,
    .info = machine_pin_irq_info,
};

// Pin protocol support
static mp_uint_t pin_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    (void)errcode;
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    switch (request) {
        case MP_PIN_READ:
            return gpio_get(self->id);
        case MP_PIN_WRITE:
            gpio_put(self->id, arg);
            return 0;
    }
    return (mp_uint_t)-1;
}

static const mp_pin_p_t pin_pin_p = {
    .ioctl = pin_ioctl,
};

// Locals dict
static const mp_rom_map_elem_t machine_pin_locals_dict_table[] = {
    // instance methods
    { MP_ROM_QSTR(MP_QSTR_init),   MP_ROM_PTR(&machine_pin_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_value),  MP_ROM_PTR(&machine_pin_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_low),    MP_ROM_PTR(&machine_pin_low_obj) },
    { MP_ROM_QSTR(MP_QSTR_high),   MP_ROM_PTR(&machine_pin_high_obj) },
    { MP_ROM_QSTR(MP_QSTR_off),    MP_ROM_PTR(&machine_pin_low_obj) },
    { MP_ROM_QSTR(MP_QSTR_on),     MP_ROM_PTR(&machine_pin_high_obj) },
    { MP_ROM_QSTR(MP_QSTR_toggle), MP_ROM_PTR(&machine_pin_toggle_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq),    MP_ROM_PTR(&machine_pin_irq_obj_fun) },

    // class constants (match rp2 values)
    { MP_ROM_QSTR(MP_QSTR_IN),         MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_OUT),        MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_OPEN_DRAIN), MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_ALT),        MP_ROM_INT(3) },
    { MP_ROM_QSTR(MP_QSTR_PULL_UP),    MP_ROM_INT(JL_GPIO_PULL_UP) },
    { MP_ROM_QSTR(MP_QSTR_PULL_DOWN),  MP_ROM_INT(JL_GPIO_PULL_DOWN) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_RISING),  MP_ROM_INT(GPIO_IRQ_EDGE_RISE) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_FALLING), MP_ROM_INT(GPIO_IRQ_EDGE_FALL) },
};

static MP_DEFINE_CONST_DICT(machine_pin_locals_dict, machine_pin_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_type,
    MP_QSTR_Pin,
    MP_TYPE_FLAG_NONE,
    make_new, mp_pin_make_new,
    print, machine_pin_print,
    call, machine_pin_call,
    protocol, &pin_pin_p,
    locals_dict, &machine_pin_locals_dict
    );

// NOTE: this registration is duplicated in rp2_qstr_stub.c so the host-side
// QSTR/root-pointer scan picks it up (this file can't be scanned on the host
// because it includes Pico SDK headers). Keep the two in sync.
MP_REGISTER_ROOT_POINTER(void *machine_pin_irq_obj[30]);


