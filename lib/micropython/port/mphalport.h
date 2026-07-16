#ifndef MICROPY_INCLUDED_MPHALPORT_H
#define MICROPY_INCLUDED_MPHALPORT_H

#include <stdint.h>
#include <stddef.h>

// Basic MicroPython types
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;

// Forward declarations for C functions - these will be implemented in the main Jumperless code
void jl_gpio_set_dir(int pin, int direction);
void jl_gpio_set(int pin, int value);
void jl_gpio_set_pull(int pin, int pull);
int jl_gpio_get(int pin);
int jl_gpio_get_pull(int pin);




// Timing/stdio HAL (implemented in mphalport.c)
mp_uint_t mp_hal_set_interrupt_char(int c);
void mp_hal_stdout_tx_str(const char *str);
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);
int mp_hal_stdin_rx_chr(void);
void mp_hal_delay_ms(mp_uint_t ms);
void mp_hal_delay_us(mp_uint_t us);
mp_uint_t mp_hal_ticks_ms(void);
mp_uint_t mp_hal_ticks_us(void);
mp_uint_t mp_hal_ticks_cpu(void);
uint32_t mp_hal_get_cpu_freq(void);
uint64_t mp_hal_time_ns(void);

// Fast delay for soft I2C/SPI - just use regular delay
#define mp_hal_delay_us_fast(us) mp_hal_delay_us(us)

// C-level pin HAL. Only use Pico SDK under Arduino/PICO builds.
//#ifdef JL_USE_PICO_HAL
#include "py/obj.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#define MP_HAL_PIN_FMT "%u"
#define mp_hal_pin_obj_t uint32_t

static inline unsigned int mp_hal_pin_name(mp_hal_pin_obj_t pin) { return pin; }

// Get pin number from either a Pin object or an integer
// For Pin objects, call the value() method to get the pin number
static inline mp_hal_pin_obj_t mp_hal_get_pin_obj(mp_obj_t pin_in) {
    if (mp_obj_is_int(pin_in)) {
        // Direct integer pin number
        return (mp_hal_pin_obj_t)mp_obj_get_int(pin_in);
    } else {
        // Pin object - get the pin number via the type's name method
        // Machine pin objects store ID in a standard location after the base
        // mp_obj_base_t is pointer-sized, then comes the id field
        typedef struct {
            mp_obj_base_t base;
            uint8_t id;
        } pin_obj_t;
        const pin_obj_t *pin = (const pin_obj_t *)MP_OBJ_TO_PTR(pin_in);
        return (mp_hal_pin_obj_t)pin->id;
    }
}
// Debug flag for HAL pin operations
extern bool debugGpioPinOwnership;  // Reuse the existing debug flag

// Debug print helper that works in embedded context
// Declared extern "C" in JumperlessMicroPythonAPI.cpp
extern void jl_debug_printf(const char* format, ...);

static inline void mp_hal_pin_input(mp_hal_pin_obj_t pin) { 
    gpio_init(pin);  // Ensure pin is initialized for GPIO
    gpio_set_function(pin, GPIO_FUNC_SIO);  // Set to GPIO function
    gpio_set_dir(pin, GPIO_IN); 
}

static inline void mp_hal_pin_output(mp_hal_pin_obj_t pin) { 
    if (debugGpioPinOwnership) {
        jl_debug_printf("[HAL] Setting pin %u as OUTPUT (func=%u, dir=OUT)\n", pin, gpio_get_function(pin));
    }
    gpio_init(pin);  // Ensure pin is initialized for GPIO
    gpio_set_function(pin, GPIO_FUNC_SIO);  // Set to GPIO function
    gpio_set_dir(pin, GPIO_OUT);
    if (debugGpioPinOwnership) {
        jl_debug_printf("[HAL] Pin %u initialized: func=%u (SIO=%d), dir=%d\n", 
               pin, gpio_get_function(pin), GPIO_FUNC_SIO, gpio_get_dir(pin));
    }
}
static inline void mp_hal_pin_open_drain_with_value(mp_hal_pin_obj_t pin, int v) { if (v) { gpio_set_dir(pin, 1); gpio_put(pin, 0); } else { gpio_put(pin, 0); gpio_set_dir(pin, 0); } }
static inline void mp_hal_pin_open_drain(mp_hal_pin_obj_t pin) { mp_hal_pin_open_drain_with_value(pin, 1); }
static inline void mp_hal_pin_config(mp_hal_pin_obj_t pin, uint32_t mode, uint32_t pull, uint32_t alt) { (void)alt; gpio_set_dir(pin, mode); gpio_set_pulls(pin, pull == 1, pull == 2); }
static inline int mp_hal_pin_read(mp_hal_pin_obj_t pin) { return (int)gpio_get(pin); }
static inline void mp_hal_pin_write(mp_hal_pin_obj_t pin, int v) { gpio_put(pin, v); }
static inline void mp_hal_pin_od_low(mp_hal_pin_obj_t pin) { gpio_set_dir(pin, 0); }
static inline void mp_hal_pin_od_high(mp_hal_pin_obj_t pin) { gpio_set_dir(pin, 1); }

// Additional pin control functions required by bitstream
static inline void mp_hal_pin_high(mp_hal_pin_obj_t pin) { 
    gpio_put(pin, 1); 
}
static inline void mp_hal_pin_low(mp_hal_pin_obj_t pin) { 
    gpio_put(pin, 0); 
}
// #else
// // Minimal stub pin HAL for embed/host build so py/mphal.h won't include extmod/virtpin.h
// #include "py/obj.h"
// #define MP_HAL_PIN_FMT "%u"
// #define mp_hal_pin_obj_t uintptr_t
// static inline unsigned int mp_hal_pin_name(mp_hal_pin_obj_t pin) { return (unsigned int)pin; }
// static inline mp_hal_pin_obj_t mp_hal_get_pin_obj(mp_obj_t pin_in) { return (mp_hal_pin_obj_t)(uintptr_t)pin_in; }
// static inline void mp_hal_pin_input(mp_hal_pin_obj_t pin) { (void)pin; }
// static inline void mp_hal_pin_output(mp_hal_pin_obj_t pin) { (void)pin; }
// static inline void mp_hal_pin_open_drain_with_value(mp_hal_pin_obj_t pin, int v) { (void)pin; (void)v; }
// static inline void mp_hal_pin_open_drain(mp_hal_pin_obj_t pin) { (void)pin; }
// static inline void mp_hal_pin_config(mp_hal_pin_obj_t pin, uint32_t mode, uint32_t pull, uint32_t alt) { (void)pin; (void)mode; (void)pull; (void)alt; }
// static inline int mp_hal_pin_read(mp_hal_pin_obj_t pin) { (void)pin; return 0; }
// static inline void mp_hal_pin_write(mp_hal_pin_obj_t pin, int v) { (void)pin; (void)v; }
// static inline void mp_hal_pin_od_low(mp_hal_pin_obj_t pin) { (void)pin; }
// static inline void mp_hal_pin_od_high(mp_hal_pin_obj_t pin) { (void)pin; }
// #endif

// IRQ control for timing-critical operations (e.g., bitstream)
// Use Pico SDK's save_and_disable_interrupts/restore_interrupts
#define mp_hal_quiet_timing_enter() save_and_disable_interrupts()
#define mp_hal_quiet_timing_exit(irq_state) restore_interrupts(irq_state)

// Atomic sections (used by py/scheduler.c and mpirq). The py/mphal.h default
// is a NO-OP, which is unsafe once callbacks are scheduled from hardware ISRs
// (Pin.irq, Timer). Match stock rp2: mask interrupts on this core.
// ponytail: single-core masking only — MicroPython runs on one core here; if
// the other core ever schedules MP callbacks this needs the rp2 spinlock version.
#define MICROPY_BEGIN_ATOMIC_SECTION()     save_and_disable_interrupts()
#define MICROPY_END_ATOMIC_SECTION(state)  restore_interrupts(state)

#endif // MICROPY_INCLUDED_MPHALPORT_H
