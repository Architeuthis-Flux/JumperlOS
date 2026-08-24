/*
 * This file is part of the Jumperless project
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Kevin Santo Cappuccio
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 */

#include "py/builtin.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/stream.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Terminal color functions
extern void jl_change_terminal_color( int color, bool flush );
extern void jl_cycle_term_color( bool reset, float step, bool flush );

// Note: GPIO functions now always return formatted strings like HIGH/LOW, INPUT/OUTPUT, etc.
// Voltage/current functions still return floats for backward compatibility

// Forward declarations for C functions - these will be implemented in the main Jumperless code
void jl_dac_set( int channel, float voltage, int save );
float jl_dac_get( int channel );
float jl_adc_get( int channel );

// USB Audio (UAC2 microphone) - two ADC channels streamed to the host as
// 2ch/16kHz/16-bit. enable/disable re-enumerate the device; the host itself
// starts and stops capture by opening the input device.
int  jl_usb_audio_enable( void );
int  jl_usb_audio_disable( void );
int  jl_usb_audio_is_enabled( void );
int  jl_usb_audio_is_streaming( void );
int  jl_usb_audio_set_channels( int left, int right );
void jl_usb_audio_save( void );
int  jl_usb_audio_set_rate( int hz );
int  jl_usb_audio_set_full_scale( float volts );
void jl_usb_audio_set_dc_block( int on );
void jl_usb_audio_status( int *enabled, int *streaming, int *host_open, int *left, int *right,
                          float *full_scale, int *dc_block, int *sample_rate, int *pending_rate,
                          int *frames_sent, int *fifo_overflow, int *adc_overrun,
                          int *late_irq, int *resyncs, int *probe_pauses, int *claim_fail,
                          int *init_fail );
float jl_ina_get_current( int sensor );
float jl_ina_get_voltage( int sensor );
float jl_ina_get_bus_voltage( int sensor );
float jl_ina_get_power( int sensor );

// Net voltage scan queries (return 1 on success, 0 when no fresh data)
int jl_scan_node_voltage( int node, float* voltage );
int jl_scan_net_current( int net, float* current_mA, float* voltage,
                         int* fromNode, int* toNode );
int jl_scan_path_current( int pathIndex, float* current_mA );

// Wavegen C wrappers (C linkage)
void jl_wavegen_set_output( int channel );
void jl_wavegen_set_freq( float hz );
void jl_wavegen_set_wave( int wave );
void jl_wavegen_set_amplitude( float vpp );
void jl_wavegen_set_offset( float v );
void jl_wavegen_set_sweep( float start_hz, float end_hz, float seconds );
void jl_wavegen_start( int start );
void jl_wavegen_stop( void );
int jl_wavegen_get_output( void );
float jl_wavegen_get_freq( void );
int jl_wavegen_get_wave( void );
float jl_wavegen_get_amplitude( void );
float jl_wavegen_get_offset( void );
int jl_wavegen_is_running( void );
void jl_wavegen_get_sweep( float* start_hz, float* end_hz, float* seconds );
void jl_gpio_set( int pin, int value );
int jl_gpio_get( int pin );
void jl_gpio_set_dir( int pin, int direction );
int jl_gpio_get_dir( int pin );
void jl_gpio_set_pull( int pin, int pull );
int jl_gpio_get_pull( int pin );
void jl_gpio_set_floating_read( int pin, int floating );
int jl_gpio_get_floating_read( int pin );
void jl_gpio_claim_pin( int pin );
void jl_gpio_release_pin( int pin );
void jl_gpio_release_all_pins( void );
int jl_nodes_connect( int node1, int node2, int save, int duplicates );
int jl_nodes_disconnect( int node1, int node2 );
int jl_nodes_fast_connect( int node1, int node2, int duplicates );
int jl_nodes_fast_disconnect( int node1, int node2 );
int jl_nodes_is_connected( int node1, int node2 );
int jl_nodes_save( int slot );
int jl_nodes_print_bridges( void );
int jl_nodes_print_paths( void );
int jl_nodes_print_crossbars( void );
int jl_nodes_print_nets( void );
int jl_nodes_print_chip_status( void );
void jl_init_micropython_local_copy( void );
void jl_send_raw( int chip, int x, int y, int setOrClear );
void jl_send_raw_str( const char* chip_str, int x, int y, int setOrClear );
int jl_switch_slot( int slot );
void jl_restore_micropython_entry_state( void );
int jl_has_unsaved_changes( void );
const char* jl_get_state( void );
int jl_set_state( const char* jsonState, int clearFirst, int fromWokwi );

// Net Information API Functions
const char* jl_get_net_name( int netNum );
void jl_set_net_name( int netNum, const char* name );
uint32_t jl_get_net_color( int netNum );
const char* jl_get_net_color_name( int netNum );
int jl_set_net_color( int netNum, const char* colorStr );
int jl_set_net_color_rgb( int netNum, int r, int g, int b );
int jl_set_net_color_hsv( int netNum, float h, float s, float v );
int jl_get_num_nets( void );
int jl_get_num_bridges( void );
const char* jl_get_net_nodes( int netNum );
int jl_get_bridge( int bridgeIdx, int* node1, int* node2, int* duplicates );

// Fake GPIO path query functions
int jl_get_num_paths( int include_duplicates );
const char* jl_get_path_info( int pathIdx );
const char* jl_get_all_path_info( void );
const char* jl_get_path_between( int node1, int node2 );

// Fast toggle functions
int jl_fake_gpio_disconnect( int node1, int node2 );
int jl_fake_gpio_reconnect( int node1, int node2 );

// Fake GPIO pin functions
int jl_fake_gpio_config_input( int node, float threshold_high, float threshold_low );
int jl_fake_gpio_config_output( int node, float v_high, float v_low, float threshold_high, float threshold_low );
int jl_fake_gpio_config_output_nodes( int node, int high_node, int low_node, float threshold_high, float threshold_low );
int jl_fake_gpio_config( int node, float v_high, float v_low, float threshold_high, float threshold_low, int mode );
void jl_fake_gpio_set_mode( int node, int mode );
void jl_fake_gpio_write( int node, int value );
int jl_fake_gpio_read( int node );
void jl_fake_gpio_reroute_chip_k( int chip_k_x, int chip_k_y, int target_node );

// Filesystem functions - bridge to existing FatFS
int jl_fs_exists( const char* path );
char* jl_fs_listdir( const char* path ); // '\n'-separated entries; NULL if dir missing
char* jl_fs_read_file( const char* path );
int jl_fs_write_file( const char* path, const char* content, int len );
char* jl_fs_get_current_dir( void );

// Extended file operations
void* jl_fs_open_file( const char* path, const char* mode );
int jl_fs_open_errno( void );        // errno explaining the last NULL return (ENOENT/EMFILE)
int jl_fs_close_file( void* file_handle ); // 1 = closed/already closed, 0 = mutex timeout (still open+tracked)
void jl_close_all_jfs_files( void ); // Close all open JFS files (for cleanup)
int jl_fs_read_bytes( void* file_handle, char* buffer, int size );
int jl_fs_write_bytes( void* file_handle, const char* data, int size );
int jl_fs_seek( void* file_handle, int position, int mode );
int jl_fs_position( void* file_handle );
int jl_fs_size( void* file_handle );
int jl_fs_available( void* file_handle );
void jl_fs_flush( void* file_handle );

// Directory operations
int jl_fs_mkdir( const char* path );
int jl_fs_rmdir( const char* path );
int jl_fs_remove( const char* path );
int jl_fs_rename( const char* pathFrom, const char* pathTo );
int jl_fs_stat_size( const char* path );
int jl_fs_stat_isdir( const char* path );

// Filesystem info
int jl_fs_total_bytes( void );
int jl_fs_used_bytes( void );
int jl_nodes_clear( void );

// OLED
int jl_oled_print( const char* text, int size );
int jl_oled_clear( int show );
int jl_oled_show( void );
int jl_oled_connect( void );
int jl_oled_disconnect( void );
int jl_oled_set_text_size( int size );
int jl_oled_get_text_size( void );
int jl_oled_copy_print( int enable );
const char* jl_oled_get_fonts( int* count );
int jl_oled_set_font( const char* fontName );
const char* jl_oled_get_current_font( void );
int jl_oled_load_bitmap( const char* filepath );
int jl_oled_display_bitmap( int x, int y, int width, int height, const uint8_t* data, size_t data_len );
int jl_oled_show_bitmap_file( const char* filepath, int x, int y );
const uint8_t* jl_oled_get_framebuffer( int* width, int* height, int* buffer_size );
int jl_oled_set_framebuffer( const uint8_t* data, size_t len );
void jl_oled_get_framebuffer_size( int* width, int* height, int* buffer_bytes );
int jl_oled_set_pixel( int x, int y, int color );
int jl_oled_get_pixel( int x, int y );

// OLED GUI (retained screens)
int  jl_oled_screen_new( void );
void jl_oled_screen_free( int screen );
void jl_oled_screen_clear( int screen );
int  jl_oled_screen_show( int screen, int persist );
void jl_oled_screen_hide( void );
void jl_oled_screen_reset( void );
int  jl_oled_add_text( int screen, const char* text, int x, int y, const char* font, int size, int halign, int valign, int z );
int  jl_oled_add_shape( int screen, int kind, int x, int y, int w, int h, int filled, int z );
int  jl_oled_elem_set_str( int elem, const char* prop, const char* value );
int  jl_oled_elem_set_int( int elem, const char* prop, int value );
int  jl_oled_set_var( const char* name, const char* value );
int  jl_oled_set_var_num( const char* name, float value );
int  jl_oled_screen_save( int screen, const char* name );
int  jl_oled_screen_load( const char* name );


void jl_arduino_reset( void );
void jl_probe_tap( int node );
int jl_probe_read_blocking( void );
int jl_probe_read_nonblocking( void );
int jl_probe_button_blocking( int consume );
int jl_probe_button_nonblocking( int consume );
void jl_clickwheel_up( int clicks );
void jl_clickwheel_down( int clicks );
void jl_clickwheel_press( void );
void jl_run_app( char* appName );
void jl_help( void );
void jl_help_section( const char* section );
void jl_pause_core2( bool pause );
int jl_pwm_setup( int gpio_pin, float frequency, float duty_cycle );

// Service management functions
int jl_force_service( const char* service_name );
int jl_force_service_by_index( int index );
int jl_get_service_index( const char* service_name );

// Probe switch functions
int jl_get_switch_position( void );
void jl_set_switch_position( int position );
int jl_check_switch_position( void );
int jl_probe_autoconnect( int enable );

// Clickwheel (rotary encoder) functions
// Clickwheel (rotary encoder) functions
long jl_clickwheel_get_position( void );
void jl_clickwheel_reset_position( void );
int jl_clickwheel_get_direction( int consume );
int jl_clickwheel_get_button( void );
bool jl_clickwheel_is_initialized( void );
int jl_pwm_set_duty_cycle( int gpio_pin, float duty_cycle );
int jl_pwm_set_frequency( int gpio_pin, float frequency );
int jl_pwm_stop( int gpio_pin );

// Overlay functions
int jl_overlay_set(const char* name, int startRow, int startCol, int width, int height, const uint32_t* colors);
int jl_overlay_clear(const char* name);
void jl_overlay_clear_all(void);
void jl_overlay_set_pixel(int row, int col, uint32_t color);
int jl_overlay_count(void);
int jl_overlay_shift(const char* name, int dRow, int dCol);
int jl_overlay_place(const char* name, int row, int col);
char* jl_overlay_serialize(void);

//=============================================================================
// Custom Boolean-like Types for Jumperless
//=============================================================================

// Forward declarations for custom types
const mp_obj_type_t gpio_state_type;
const mp_obj_type_t gpio_direction_type;
const mp_obj_type_t gpio_pull_type;
const mp_obj_type_t connection_state_type;
const mp_obj_type_t probe_button_type;
const mp_obj_type_t node_type;
const mp_obj_type_t probe_pad_type;

// Forward declarations for functions
static void gpio_state_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind );
static mp_obj_t gpio_state_unary_op( mp_unary_op_t op, mp_obj_t self_in );
static mp_obj_t gpio_state_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in );
static mp_obj_t gpio_state_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args );
static void gpio_direction_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind );
static mp_obj_t gpio_direction_unary_op( mp_unary_op_t op, mp_obj_t self_in );
static mp_obj_t gpio_direction_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in );
static mp_obj_t gpio_direction_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args );
static void gpio_pull_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind );
static mp_obj_t gpio_pull_unary_op( mp_unary_op_t op, mp_obj_t self_in );
static mp_obj_t gpio_pull_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in );
static mp_obj_t gpio_pull_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args );
static void connection_state_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind );
static mp_obj_t connection_state_unary_op( mp_unary_op_t op, mp_obj_t self_in );
static mp_obj_t connection_state_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in );
static mp_obj_t connection_state_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args );

// Helper function declarations
static int get_direction_value( mp_obj_t obj );
static int get_pull_value( mp_obj_t obj );
static int get_gpio_state_value( mp_obj_t obj );
static void node_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind );
static mp_obj_t node_unary_op( mp_unary_op_t op, mp_obj_t self_in );
static mp_obj_t node_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args );
static void probe_button_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind );
static mp_obj_t probe_button_unary_op( mp_unary_op_t op, mp_obj_t self_in );
static mp_obj_t probe_button_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in );
static mp_obj_t probe_button_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args );
static void probe_pad_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind );
static mp_obj_t probe_pad_unary_op( mp_unary_op_t op, mp_obj_t self_in );
static mp_obj_t probe_pad_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args );

//=============================================================================
// Node Name Mapping - Comprehensive table of all node names and aliases
//=============================================================================

typedef struct {
    const char* name;
    int value;
} NodeMapping;

// All possible node name mappings including aliases
static const NodeMapping node_mappings[] = {
    // Special functions with all aliases
    { "GND", 100 },
    { "GROUND", 100 },
    { "TOP_RAIL", 101 },
    { "TOPRAIL", 101 },
    { "T_R", 101 },
    { "TOP_R", 101 },
    { "BOTTOM_RAIL", 102 },
    { "BOT_RAIL", 102 },
    { "BOTTOMRAIL", 102 },
    { "BOTRAIL", 102 },
    { "B_R", 102 },
    { "BOT_R", 102 },
    { "SUPPLY_3V3", 103 },
    { "3V3", 103 },
    { "3.3V", 103 },
    { "TOP_RAIL_GND", 104 },
    { "TOP_GND", 104 },
    { "SUPPLY_5V", 105 },
    { "5V", 105 },
    { "+5V", 105 },

    // DACs
    { "DAC0", 106 },
    { "DAC_0", 106 },
    { "DAC0_5V", 106 },
    { "DAC1", 107 },
    { "DAC_1", 107 },
    { "DAC1_8V", 107 },

    // Current sense
    { "ISENSE_PLUS", 108 },
    { "ISENSE_POS", 108 },
    { "ISENSE_P", 108 },
    { "INA_P", 108 },
    { "I_P", 108 },
    { "CURRENT_SENSE_PLUS", 108 },
    { "ISENSE_POSITIVE", 108 },
    { "I_POS", 108 },
    { "ISENSE_MINUS", 109 },
    { "ISENSE_NEG", 109 },
    { "ISENSE_N", 109 },
    { "INA_N", 109 },
    { "I_N", 109 },
    { "CURRENT_SENSE_MINUS", 109 },
    { "ISENSE_NEGATIVE", 109 },
    { "I_NEG", 109 },

    // ADCs
    { "ADC0", 110 },
    { "ADC_0", 110 },
    { "ADC0_8V", 110 },
    { "ADC1", 111 },
    { "ADC_1", 111 },
    { "ADC1_8V", 111 },
    { "ADC2", 112 },
    { "ADC_2", 112 },
    { "ADC2_8V", 112 },
    { "ADC3", 113 },
    { "ADC_3", 113 },
    { "ADC3_8V", 113 },
    { "ADC4", 114 },
    { "ADC_4", 114 },
    { "ADC4_5V", 114 },
    { "ADC7", 115 },
    { "ADC_7", 115 },
    { "ADC7_PROBE", 115 },
    { "PROBE", 115 },

    // UART
    { "RP_UART_TX", 116 },
    { "UART_TX", 116 },
    { "TX", 116 },
    { "RP_GPIO_16", 116 },
    { "RP_UART_RX", 117 },
    { "UART_RX", 117 },
    { "RX", 117 },
    { "RP_GPIO_17", 117 },

    // Other RP GPIOs
    { "RP_GPIO_18", 118 },
    { "GP_18", 118 },
    { "RP_GPIO_19", 119 },
    { "GP_19", 119 },

    // Power supplies
    { "SUPPLY_8V_P", 120 },
    { "8V_P", 120 },
    { "8V_POS", 120 },
    { "SUPPLY_8V_N", 121 },
    { "8V_N", 121 },
    { "8V_NEG", 121 },

    // Ground rails
    { "BOTTOM_RAIL_GND", 126 },
    { "BOT_GND", 126 },
    { "BOTTOM_GND", 126 },
    { "EMPTY_NET", 127 },
    { "EMPTY", 127 },

    // User GPIO pins (with all common aliases)
    { "RP_GPIO_1", 131 },
    { "GPIO_1", 131 },
    { "GPIO1", 131 },
    { "GP_1", 131 },
    { "GP1", 131 },
    { "RP_GPIO_2", 132 },
    { "GPIO_2", 132 },
    { "GPIO2", 132 },
    { "GP_2", 132 },
    { "GP2", 132 },
    { "RP_GPIO_3", 133 },
    { "GPIO_3", 133 },
    { "GPIO3", 133 },
    { "GP_3", 133 },
    { "GP3", 133 },
    { "RP_GPIO_4", 134 },
    { "GPIO_4", 134 },
    { "GPIO4", 134 },
    { "GP_4", 134 },
    { "GP4", 134 },
    { "RP_GPIO_5", 135 },
    { "GPIO_5", 135 },
    { "GPIO5", 135 },
    { "GP_5", 135 },
    { "GP5", 135 },
    { "RP_GPIO_6", 136 },
    { "GPIO_6", 136 },
    { "GPIO6", 136 },
    { "GP_6", 136 },
    { "GP6", 136 },
    { "RP_GPIO_7", 137 },
    { "GPIO_7", 137 },
    { "GPIO7", 137 },
    { "GP_7", 137 },
    { "GP7", 137 },
    { "RP_GPIO_8", 138 },
    { "GPIO_8", 138 },
    { "GPIO8", 138 },
    { "GP_8", 138 },
    { "GP8", 138 },

    // Buffer
    { "ROUTABLE_BUFFER_IN", 139 },
    { "BUFFER_IN", 139 },
    { "BUF_IN", 139 },
    { "BUFF_IN", 139 },
    { "BUFFIN", 139 },
    { "ROUTABLE_BUFFER_OUT", 140 },
    { "BUFFER_OUT", 140 },
    { "BUF_OUT", 140 },
    { "BUFF_OUT", 140 },
    { "BUFFOUT", 140 },

    // Arduino Nano pins
    { "NANO_VIN", 69 },
    { "VIN", 69 },
    { "NANO_D0", 70 },
    { "D0", 70 },
    { "NANO_D1", 71 },
    { "D1", 71 },
    { "NANO_D2", 72 },
    { "D2", 72 },
    { "NANO_D3", 73 },
    { "D3", 73 },
    { "NANO_D4", 74 },
    { "D4", 74 },
    { "NANO_D5", 75 },
    { "D5", 75 },
    { "NANO_D6", 76 },
    { "D6", 76 },
    { "NANO_D7", 77 },
    { "D7", 77 },
    { "NANO_D8", 78 },
    { "D8", 78 },
    { "NANO_D9", 79 },
    { "D9", 79 },
    { "NANO_D10", 80 },
    { "D10", 80 },
    { "NANO_D11", 81 },
    { "D11", 81 },
    { "NANO_D12", 82 },
    { "D12", 82 },
    { "NANO_D13", 83 },
    { "D13", 83 },
    { "NANO_RESET", 84 },
    { "RESET", 84 },
    { "NANO_AREF", 85 },
    { "AREF", 85 },
    { "NANO_A0", 86 },
    { "A0", 86 },
    { "NANO_A1", 87 },
    { "A1", 87 },
    { "NANO_A2", 88 },
    { "A2", 88 },
    { "NANO_A3", 89 },
    { "A3", 89 },
    { "NANO_A4", 90 },
    { "A4", 90 },
    { "NANO_A5", 91 },
    { "A5", 91 },
    { "NANO_A6", 92 },
    { "A6", 92 },
    { "NANO_A7", 93 },
    { "A7", 93 },
    { "NANO_RESET_0", 94 },
    { "RST0", 94 },
    { "NANO_RESET_1", 95 },
    { "RST1", 95 },
    { "NANO_GND_1", 96 },
    { "N_GND1", 96 },
    { "NANO_GND_0", 97 },
    { "N_GND0", 97 },
    { "NANO_3V3", 98 },
    { "NANO_5V", 99 },
};

static const size_t node_mappings_count = sizeof( node_mappings ) / sizeof( NodeMapping );

//=============================================================================
// Probe Pad Mapping - Define all possible probe pad types
//=============================================================================

typedef struct {
    const char* name;
    int value;
} PadMapping;

// Define all possible probe pad types including special pads
static const PadMapping pad_mappings[] = {
    // Regular breadboard pads (1-60)
    { "PAD_1", 1 },
    { "PAD_2", 2 },
    { "PAD_3", 3 },
    { "PAD_4", 4 },
    { "PAD_5", 5 },
    { "PAD_6", 6 },
    { "PAD_7", 7 },
    { "PAD_8", 8 },
    { "PAD_9", 9 },
    { "PAD_10", 10 },
    { "PAD_11", 11 },
    { "PAD_12", 12 },
    { "PAD_13", 13 },
    { "PAD_14", 14 },
    { "PAD_15", 15 },
    { "PAD_16", 16 },
    { "PAD_17", 17 },
    { "PAD_18", 18 },
    { "PAD_19", 19 },
    { "PAD_20", 20 },
    { "PAD_21", 21 },
    { "PAD_22", 22 },
    { "PAD_23", 23 },
    { "PAD_24", 24 },
    { "PAD_25", 25 },
    { "PAD_26", 26 },
    { "PAD_27", 27 },
    { "PAD_28", 28 },
    { "PAD_29", 29 },
    { "PAD_30", 30 },
    { "PAD_31", 31 },
    { "PAD_32", 32 },
    { "PAD_33", 33 },
    { "PAD_34", 34 },
    { "PAD_35", 35 },
    { "PAD_36", 36 },
    { "PAD_37", 37 },
    { "PAD_38", 38 },
    { "PAD_39", 39 },
    { "PAD_40", 40 },
    { "PAD_41", 41 },
    { "PAD_42", 42 },
    { "PAD_43", 43 },
    { "PAD_44", 44 },
    { "PAD_45", 45 },
    { "PAD_46", 46 },
    { "PAD_47", 47 },
    { "PAD_48", 48 },
    { "PAD_49", 49 },
    { "PAD_50", 50 },
    { "PAD_51", 51 },
    { "PAD_52", 52 },
    { "PAD_53", 53 },
    { "PAD_54", 54 },
    { "PAD_55", 55 },
    { "PAD_56", 56 },
    { "PAD_57", 57 },
    { "PAD_58", 58 },
    { "PAD_59", 59 },
    { "PAD_60", 60 },

    // Special pads
    { "NO_PAD", -1 },
    { "NONE", -1 },
    { "LOGO_PAD_TOP", 142 },
    { "LOGO_PAD_BOTTOM", 143 },
    { "GPIO_PAD", 144 },
    { "DAC_PAD", 145 },
    { "ADC_PAD", 146 },
    { "BUILDING_PAD_TOP", 147 },
    { "BUILDING_PAD_BOTTOM", 148 },

    // Nano header pads (digital pins)
    { "NANO_D0", 70 },
    { "D0_PAD", 70 },
    { "NANO_D1", 71 },
    { "D1_PAD", 71 },
    { "NANO_D2", 72 },
    { "D2_PAD", 72 },
    { "NANO_D3", 73 },
    { "D3_PAD", 73 },
    { "NANO_D4", 74 },
    { "D4_PAD", 74 },
    { "NANO_D5", 75 },
    { "D5_PAD", 75 },
    { "NANO_D6", 76 },
    { "D6_PAD", 76 },
    { "NANO_D7", 77 },
    { "D7_PAD", 77 },
    { "NANO_D8", 78 },
    { "D8_PAD", 78 },
    { "NANO_D9", 79 },
    { "D9_PAD", 79 },
    { "NANO_D10", 80 },
    { "D10_PAD", 80 },
    { "NANO_D11", 81 },
    { "D11_PAD", 81 },
    { "NANO_D12", 82 },
    { "D12_PAD", 82 },
    { "NANO_D13", 83 },
    { "D13_PAD", 83 },
    { "NANO_RESET", 84 },
    { "RESET_PAD", 84 },
    { "NANO_AREF", 85 },
    { "AREF_PAD", 85 },

    // Nano header pads (analog pins)
    { "NANO_A0", 86 },
    { "A0_PAD", 86 },
    { "NANO_A1", 87 },
    { "A1_PAD", 87 },
    { "NANO_A2", 88 },
    { "A2_PAD", 88 },
    { "NANO_A3", 89 },
    { "A3_PAD", 89 },
    { "NANO_A4", 90 },
    { "A4_PAD", 90 },
    { "NANO_A5", 91 },
    { "A5_PAD", 91 },
    { "NANO_A6", 92 },
    { "A6_PAD", 92 },
    { "NANO_A7", 93 },
    { "A7_PAD", 93 },

    // Nano power/control pads (generally not routable but detectable)
    { "NANO_VIN", 69 },
    { "VIN_PAD", 69 },
    { "NANO_RESET_0", 94 },
    { "RESET_0_PAD", 94 },
    { "NANO_RESET_1", 95 },
    { "RESET_1_PAD", 95 },
    { "NANO_GND_1", 96 },
    { "GND_1_PAD", 96 },
    { "NANO_GND_0", 97 },
    { "GND_0_PAD", 97 },
    { "NANO_3V3", 98 },
    { "3V3_PAD", 98 },
    { "NANO_5V", 99 },
    { "5V_PAD", 99 },

    // Rail pads
    { "TOP_RAIL", 101 },
    { "TOP_RAIL_PAD", 101 },
    { "BOTTOM_RAIL", 102 },
    { "BOTTOM_RAIL_PAD", 102 },
    { "BOT_RAIL_PAD", 102 },
    { "TOP_RAIL_GND", 104 },
    { "TOP_GND_PAD", 104 },
    { "BOTTOM_RAIL_GND", 126 },
    { "BOT_RAIL_GND", 126 },
    { "BOTTOM_GND_PAD", 126 },
    { "BOT_GND_PAD", 126 },
};

static const size_t pad_mappings_count = sizeof( pad_mappings ) / sizeof( PadMapping );

// Forward declaration of function to get node name from value
const char* jl_get_node_name( int node_value );

// Function to get pad name from value
const char* jl_get_pad_name( int pad_value ) {
    // Check for special pads first
    for ( size_t i = 0; i < pad_mappings_count; i++ ) {
        if ( pad_mappings[ i ].value == pad_value ) {
            return pad_mappings[ i ].name;
        }
    }

    // For numbered pads (1-60), return the number as string
    static char pad_str[ 8 ];
    if ( pad_value >= 1 && pad_value <= 60 ) {
        snprintf( pad_str, sizeof( pad_str ), "%d", pad_value );
        return pad_str;
    }

    return "UNKNOWN_PAD";
}

// Implementation of function to get node name from value
const char* jl_get_node_name( int node_value ) {

    // For numbered nodes (1-60), just return the number as string
    static char num_str[ 8 ];
    if ( node_value >= 1 && node_value <= 60 ) {
        snprintf( num_str, sizeof( num_str ), "%d", node_value );
        return num_str;
    }

    // Search in mappings for a name
    for ( size_t i = 0; i < node_mappings_count; i++ ) {
        if ( node_mappings[ i ].value == node_value ) {
            return node_mappings[ i ].name;
        }
    }

    return ""; // Unknown node
}

// Function to find node value from string name (case-insensitive)
static int find_node_value( const char* name ) {
    // Convert to uppercase for comparison
    char upper_name[ 32 ];
    size_t name_len = strlen( name );
    if ( name_len >= sizeof( upper_name ) ) {
        return -1; // Name too long
    }

    for ( size_t i = 0; i < name_len; i++ ) {
        upper_name[ i ] = ( name[ i ] >= 'a' && name[ i ] <= 'z' ) ? ( name[ i ] - 'a' + 'A' ) : name[ i ];
    }
    upper_name[ name_len ] = '\0';

    // Check direct integer first
    char* endptr;
    long int_val = strtol( upper_name, &endptr, 10 );
    if ( *endptr == '\0' && int_val >= 1 && int_val <= 200 ) {
        return (int)int_val;
    }

    // Search in mappings
    for ( size_t i = 0; i < node_mappings_count; i++ ) {
        if ( strcmp( upper_name, node_mappings[ i ].name ) == 0 ) {
            return node_mappings[ i ].value;
        }
    }

    return -1; // Not found
}

// GPIO State Type (HIGH/LOW/FLOATING) that behaves like bool in conditionals
typedef enum {
    GPIO_STATE_LOW = 0,
    GPIO_STATE_HIGH = 1,
    GPIO_STATE_FLOATING = 2
} gpio_state_value_t;

typedef struct _gpio_state_obj_t {
    mp_obj_base_t base;
    gpio_state_value_t value;
} gpio_state_obj_t;

static void gpio_state_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind ) {
    gpio_state_obj_t* self = MP_OBJ_TO_PTR( self_in );
    switch ( self->value ) {
    case GPIO_STATE_HIGH:
        mp_printf( print, "HIGH" );
        break;
    case GPIO_STATE_LOW:
        mp_printf( print, "LOW" );
        break;
    case GPIO_STATE_FLOATING:
        mp_printf( print, "FLOATING" );
        break;
    default:
        mp_printf( print, "UNKNOWN" );
        break;
    }
}

static mp_obj_t gpio_state_unary_op( mp_unary_op_t op, mp_obj_t self_in ) {
    gpio_state_obj_t* self = MP_OBJ_TO_PTR( self_in );
    switch ( op ) {
    case MP_UNARY_OP_BOOL:
        // HIGH = True, LOW = False, FLOATING = False
        return mp_obj_new_bool( self->value == GPIO_STATE_HIGH );
    case MP_UNARY_OP_INT_MAYBE:
        // Support int(state) conversion (HIGH=1, LOW=0, FLOATING=2)
        return mp_obj_new_int( self->value );
    case MP_UNARY_OP_FLOAT_MAYBE:
        // Support float(state) conversion (HIGH=1.0, LOW=0.0, FLOATING=2.0)
        return mp_obj_new_float( (float)self->value );
    default:
        return MP_OBJ_NULL;
    }
}

// Binary operations to allow gpio_state to work with equality comparisons
static mp_obj_t gpio_state_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in ) {
    if ( mp_obj_get_type( lhs_in ) == &gpio_state_type ) {
        gpio_state_obj_t* lhs = MP_OBJ_TO_PTR( lhs_in );
        if ( op == MP_BINARY_OP_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &gpio_state_type ) {
                gpio_state_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value == rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value == mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_NOT_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &gpio_state_type ) {
                gpio_state_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value != rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value != mp_obj_get_int( rhs_in ) );
            }
        }
    }
    return MP_OBJ_NULL;
}

MP_DEFINE_CONST_OBJ_TYPE(
    gpio_state_type,
    MP_QSTR_GPIOState,
    MP_TYPE_FLAG_NONE,
    make_new, gpio_state_make_new,
    print, gpio_state_print,
    unary_op, gpio_state_unary_op,
    binary_op, gpio_state_binary_op );

static mp_obj_t gpio_state_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args ) {
    mp_arg_check_num( n_args, n_kw, 1, 1, false );
    gpio_state_obj_t* o = m_new_obj( gpio_state_obj_t );
    o->base.type = &gpio_state_type;

    if ( mp_obj_is_int( args[ 0 ] ) ) {
        // Handle integer values: 0=LOW, 1=HIGH, 2=FLOATING
        int val = mp_obj_get_int( args[ 0 ] );
        if ( val == 0 )
            o->value = GPIO_STATE_LOW;
        else if ( val == 1 )
            o->value = GPIO_STATE_HIGH;
        else if ( val == 2 )
            o->value = GPIO_STATE_FLOATING;
        else
            o->value = mp_obj_is_true( args[ 0 ] ) ? GPIO_STATE_HIGH : GPIO_STATE_LOW;
    } else if ( mp_obj_is_str( args[ 0 ] ) ) {
        // Handle string values: "HIGH", "LOW", "FLOATING"
        const char* str = mp_obj_str_get_str( args[ 0 ] );
        if ( strcmp( str, "HIGH" ) == 0 || strcmp( str, "high" ) == 0 || strcmp( str, "1" ) == 0 ) {
            o->value = GPIO_STATE_HIGH;
        } else if ( strcmp( str, "LOW" ) == 0 || strcmp( str, "low" ) == 0 || strcmp( str, "0" ) == 0 ) {
            o->value = GPIO_STATE_LOW;
        } else if ( strcmp( str, "FLOATING" ) == 0 || strcmp( str, "floating" ) == 0 ||
                    strcmp( str, "FLOAT" ) == 0 || strcmp( str, "float" ) == 0 || strcmp( str, "2" ) == 0 ) {
            o->value = GPIO_STATE_FLOATING;
        } else {
            mp_raise_ValueError( "GPIO state must be 'HIGH', 'LOW', or 'FLOATING'" );
        }
    } else {
        // Handle boolean values (default behavior)
        o->value = mp_obj_is_true( args[ 0 ] ) ? GPIO_STATE_HIGH : GPIO_STATE_LOW;
    }

    return MP_OBJ_FROM_PTR( o );
}

// These make_new functions will be implemented after the struct definitions later in the file

static mp_obj_t gpio_state_new( gpio_state_value_t value ) {
    gpio_state_obj_t* o = m_new_obj( gpio_state_obj_t );
    o->base.type = &gpio_state_type;
    o->value = value;
    return MP_OBJ_FROM_PTR( o );
}

// GPIO Direction Type (INPUT/OUTPUT)
typedef struct _gpio_direction_obj_t {
    mp_obj_base_t base;
    bool value; // true = OUTPUT, false = INPUT
} gpio_direction_obj_t;

static void gpio_direction_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind ) {
    gpio_direction_obj_t* self = MP_OBJ_TO_PTR( self_in );
    mp_printf( print, "%s", self->value ? "OUTPUT" : "INPUT" );
}

static mp_obj_t gpio_direction_unary_op( mp_unary_op_t op, mp_obj_t self_in ) {
    gpio_direction_obj_t* self = MP_OBJ_TO_PTR( self_in );
    switch ( op ) {
    case MP_UNARY_OP_BOOL:
        return mp_obj_new_bool( self->value );
    case MP_UNARY_OP_INT_MAYBE:
        // Support int(direction) conversion using firmware convention (OUTPUT=0, INPUT=1)
        return mp_obj_new_int( self->value ? 0 : 1 );
    case MP_UNARY_OP_FLOAT_MAYBE:
        // Support float(direction) conversion (OUTPUT=0.0, INPUT=1.0)
        return mp_obj_new_float( self->value ? 0.0 : 1.0 );
    default:
        return MP_OBJ_NULL;
    }
}

// Binary operations to allow gpio_direction to work with equality comparisons
static mp_obj_t gpio_direction_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in ) {
    if ( mp_obj_get_type( lhs_in ) == &gpio_direction_type ) {
        gpio_direction_obj_t* lhs = MP_OBJ_TO_PTR( lhs_in );
        if ( op == MP_BINARY_OP_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &gpio_direction_type ) {
                gpio_direction_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value == rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                int rhs_val = mp_obj_get_int( rhs_in );
                return mp_obj_new_bool( ( lhs->value ? 0 : 1 ) == rhs_val );
            } else if ( mp_obj_is_bool( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value == mp_obj_is_true( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_NOT_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &gpio_direction_type ) {
                gpio_direction_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value != rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                int rhs_val = mp_obj_get_int( rhs_in );
                return mp_obj_new_bool( ( lhs->value ? 0 : 1 ) != rhs_val );
            } else if ( mp_obj_is_bool( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value != mp_obj_is_true( rhs_in ) );
            }
        }
    }
    return MP_OBJ_NULL;
}

MP_DEFINE_CONST_OBJ_TYPE(
    gpio_direction_type,
    MP_QSTR_GPIODirection,
    MP_TYPE_FLAG_NONE,
    make_new, gpio_direction_make_new,
    print, gpio_direction_print,
    unary_op, gpio_direction_unary_op,
    binary_op, gpio_direction_binary_op );

static mp_obj_t gpio_direction_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args ) {
    mp_arg_check_num( n_args, n_kw, 1, 1, false );
    gpio_direction_obj_t* o = m_new_obj( gpio_direction_obj_t );
    o->base.type = &gpio_direction_type;
    o->value = mp_obj_is_true( args[ 0 ] );
    return MP_OBJ_FROM_PTR( o );
}

static mp_obj_t gpio_direction_new( bool value ) {
    gpio_direction_obj_t* o = m_new_obj( gpio_direction_obj_t );
    o->base.type = &gpio_direction_type;
    o->value = value;
    return MP_OBJ_FROM_PTR( o );
}

// GPIO Pull Type (PULLUP/PULLDOWN/NO_PULL)
typedef struct _gpio_pull_obj_t {
    mp_obj_base_t base;
    int value; // 1 = PULLUP, -1 = PULLDOWN, 0 = NO_PULL, 2 = BUS_KEEPER
} gpio_pull_obj_t;

static void gpio_pull_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind ) {
    gpio_pull_obj_t* self = MP_OBJ_TO_PTR( self_in );
    if ( self->value == 1 ) {
        mp_printf( print, "PULLUP" );
    } else if ( self->value == -1 ) {
        mp_printf( print, "PULLDOWN" );
    } else if ( self->value == 2 ) {
        mp_printf( print, "BUS_KEEPER" );
    } else {
        mp_printf( print, "NO_PULL" );
    }
}

static mp_obj_t gpio_pull_unary_op( mp_unary_op_t op, mp_obj_t self_in ) {
    gpio_pull_obj_t* self = MP_OBJ_TO_PTR( self_in );
    switch ( op ) {
    case MP_UNARY_OP_BOOL:
        // Only PULLUP is "truthy"
        return mp_obj_new_bool( self->value == 1 );
    case MP_UNARY_OP_INT_MAYBE:
        // Support int(pull) conversion (PULLUP=1, PULLDOWN=-1, NO_PULL=0, BUS_KEEPER=2)
        return mp_obj_new_int( self->value );
    case MP_UNARY_OP_FLOAT_MAYBE:
        // Support float(pull) conversion (PULLUP=1.0, PULLDOWN=-1.0, NO_PULL=0.0, BUS_KEEPER=2.0)
        return mp_obj_new_float( (mp_float_t)self->value );
    default:
        return MP_OBJ_NULL;
    }
}

// Binary operations to allow gpio_pull to work with equality comparisons
static mp_obj_t gpio_pull_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in ) {
    if ( mp_obj_get_type( lhs_in ) == &gpio_pull_type ) {
        gpio_pull_obj_t* lhs = MP_OBJ_TO_PTR( lhs_in );
        if ( op == MP_BINARY_OP_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &gpio_pull_type ) {
                gpio_pull_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value == rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value == mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_NOT_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &gpio_pull_type ) {
                gpio_pull_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value != rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value != mp_obj_get_int( rhs_in ) );
            }
        }
    }
    return MP_OBJ_NULL;
}

MP_DEFINE_CONST_OBJ_TYPE(
    gpio_pull_type,
    MP_QSTR_GPIOPull,
    MP_TYPE_FLAG_NONE,
    make_new, gpio_pull_make_new,
    print, gpio_pull_print,
    unary_op, gpio_pull_unary_op,
    binary_op, gpio_pull_binary_op );

static mp_obj_t gpio_pull_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args ) {
    mp_arg_check_num( n_args, n_kw, 1, 1, false );
    gpio_pull_obj_t* o = m_new_obj( gpio_pull_obj_t );
    o->base.type = &gpio_pull_type;
    o->value = mp_obj_get_int( args[ 0 ] );
    return MP_OBJ_FROM_PTR( o );
}

static mp_obj_t gpio_pull_new( int value ) {
    gpio_pull_obj_t* o = m_new_obj( gpio_pull_obj_t );
    o->base.type = &gpio_pull_type;
    o->value = value;
    return MP_OBJ_FROM_PTR( o );
}

// Connection State Type (CONNECTED/DISCONNECTED)
typedef struct _connection_state_obj_t {
    mp_obj_base_t base;
    bool value;
} connection_state_obj_t;

static void connection_state_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind ) {
    connection_state_obj_t* self = MP_OBJ_TO_PTR( self_in );
    mp_printf( print, "%s", self->value ? "CONNECTED" : "DISCONNECTED" );
}

static mp_obj_t connection_state_unary_op( mp_unary_op_t op, mp_obj_t self_in ) {
    connection_state_obj_t* self = MP_OBJ_TO_PTR( self_in );
    switch ( op ) {
    case MP_UNARY_OP_BOOL:
        return mp_obj_new_bool( self->value );
    case MP_UNARY_OP_INT_MAYBE:
        // Support int(connection) conversion (CONNECTED=1, DISCONNECTED=0)
        return mp_obj_new_int( self->value ? 1 : 0 );
    case MP_UNARY_OP_FLOAT_MAYBE:
        // Support float(connection) conversion (CONNECTED=1.0, DISCONNECTED=0.0)
        return mp_obj_new_float( self->value ? 1.0 : 0.0 );
    default:
        return MP_OBJ_NULL;
    }
}

// Binary operations to allow connection_state to work with equality comparisons
static mp_obj_t connection_state_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in ) {
    if ( mp_obj_get_type( lhs_in ) == &connection_state_type ) {
        connection_state_obj_t* lhs = MP_OBJ_TO_PTR( lhs_in );
        if ( op == MP_BINARY_OP_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &connection_state_type ) {
                connection_state_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value == rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                int rhs_val = mp_obj_get_int( rhs_in );
                return mp_obj_new_bool( ( lhs->value ? 1 : 0 ) == rhs_val );
            } else if ( mp_obj_is_bool( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value == mp_obj_is_true( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_NOT_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &connection_state_type ) {
                connection_state_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value != rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                int rhs_val = mp_obj_get_int( rhs_in );
                return mp_obj_new_bool( ( lhs->value ? 1 : 0 ) != rhs_val );
            } else if ( mp_obj_is_bool( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value != mp_obj_is_true( rhs_in ) );
            }
        }
    }
    return MP_OBJ_NULL;
}

MP_DEFINE_CONST_OBJ_TYPE(
    connection_state_type,
    MP_QSTR_ConnectionState,
    MP_TYPE_FLAG_NONE,
    make_new, connection_state_make_new,
    print, connection_state_print,
    unary_op, connection_state_unary_op,
    binary_op, connection_state_binary_op );

static mp_obj_t connection_state_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args ) {
    mp_arg_check_num( n_args, n_kw, 1, 1, false );
    connection_state_obj_t* o = m_new_obj( connection_state_obj_t );
    o->base.type = &connection_state_type;
    o->value = mp_obj_is_true( args[ 0 ] );
    return MP_OBJ_FROM_PTR( o );
}

static mp_obj_t connection_state_new( bool value ) {
    connection_state_obj_t* o = m_new_obj( connection_state_obj_t );
    o->base.type = &connection_state_type;
    o->value = value;
    return MP_OBJ_FROM_PTR( o );
}

//=============================================================================
// Probe Button Type - Represents probe button states (NONE/CONNECT/REMOVE)
//=============================================================================

typedef struct _probe_button_obj_t {
    mp_obj_base_t base;
    int value; // 0 = NONE, 1 = CONNECT, 2 = REMOVE
} probe_button_obj_t;

static void probe_button_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind ) {
    probe_button_obj_t* self = MP_OBJ_TO_PTR( self_in );
    if ( self->value == 1 ) {
        mp_printf( print, "CONNECT" );
    } else if ( self->value == 2 ) {
        mp_printf( print, "REMOVE" );
    } else {
        mp_printf( print, "NONE" );
    }
}

static mp_obj_t probe_button_unary_op( mp_unary_op_t op, mp_obj_t self_in ) {
    probe_button_obj_t* self = MP_OBJ_TO_PTR( self_in );
    switch ( op ) {
    case MP_UNARY_OP_BOOL:
        // Only CONNECT and REMOVE are "truthy"
        return mp_obj_new_bool( self->value != 0 );
    case MP_UNARY_OP_INT_MAYBE:
        // Support int(button) conversion (CONNECT=1, REMOVE=2, NONE=0)
        return mp_obj_new_int( self->value );
    case MP_UNARY_OP_FLOAT_MAYBE:
        // Support float(button) conversion (CONNECT=1.0, REMOVE=2.0, NONE=0.0)
        return mp_obj_new_float( (mp_float_t)self->value );
    default:
        return MP_OBJ_NULL;
    }
}

// Binary operations to allow probe_button to work with equality comparisons
static mp_obj_t probe_button_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in ) {
    if ( mp_obj_get_type( lhs_in ) == &probe_button_type ) {
        probe_button_obj_t* lhs = MP_OBJ_TO_PTR( lhs_in );
        if ( op == MP_BINARY_OP_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &probe_button_type ) {
                probe_button_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value == rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value == mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_NOT_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &probe_button_type ) {
                probe_button_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value != rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value != mp_obj_get_int( rhs_in ) );
            }
        }
    }
    return MP_OBJ_NULL;
}

MP_DEFINE_CONST_OBJ_TYPE(
    probe_button_type,
    MP_QSTR_ProbeButton,
    MP_TYPE_FLAG_NONE,
    make_new, probe_button_make_new,
    print, probe_button_print,
    unary_op, probe_button_unary_op,
    binary_op, probe_button_binary_op );

static mp_obj_t probe_button_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args ) {
    mp_arg_check_num( n_args, n_kw, 1, 1, false );
    probe_button_obj_t* o = m_new_obj( probe_button_obj_t );
    o->base.type = &probe_button_type;
    o->value = mp_obj_get_int( args[ 0 ] );
    return MP_OBJ_FROM_PTR( o );
}

static mp_obj_t probe_button_new( int value ) {
    probe_button_obj_t* o = m_new_obj( probe_button_obj_t );
    o->base.type = &probe_button_type;
    o->value = value;
    return MP_OBJ_FROM_PTR( o );
}

//=============================================================================
// Node Type - Handles string names and aliases for node numbers
//=============================================================================

typedef struct _node_obj_t {
    mp_obj_base_t base;
    int value; // the actual node number
} node_obj_t;

static void node_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind ) {
    node_obj_t* self = MP_OBJ_TO_PTR( self_in );

    // Get the human-readable name for this node
    const char* name = jl_get_node_name( self->value );
    if ( name && strlen( name ) > 0 ) {
        mp_printf( print, "%s", name );
    } else {
        mp_printf( print, "%d", self->value );
    }
}

static mp_obj_t node_unary_op( mp_unary_op_t op, mp_obj_t self_in ) {
    node_obj_t* self = MP_OBJ_TO_PTR( self_in );
    switch ( op ) {
    case MP_UNARY_OP_BOOL:
        return mp_obj_new_bool( self->value != 0 );
    case MP_UNARY_OP_INT_MAYBE:
        // Support int(node) conversion - returns the node number
        return mp_obj_new_int( self->value );
    case MP_UNARY_OP_FLOAT_MAYBE:
        // Support float(node) conversion - returns the node number as float
        return mp_obj_new_float( (mp_float_t)self->value );
    default:
        return MP_OBJ_NULL;
    }
}

// Binary operations to allow nodes to work with integers
static mp_obj_t node_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in ) {
    if ( mp_obj_get_type( lhs_in ) == &node_type ) {
        node_obj_t* lhs = MP_OBJ_TO_PTR( lhs_in );
        
        // Comparison operators
        if ( op == MP_BINARY_OP_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &node_type ) {
                node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value == rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value == mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_NOT_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &node_type ) {
                node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value != rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value != mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_LESS ) {
            if ( mp_obj_get_type( rhs_in ) == &node_type ) {
                node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value < rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value < mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_LESS_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &node_type ) {
                node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value <= rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value <= mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_MORE ) {
            if ( mp_obj_get_type( rhs_in ) == &node_type ) {
                node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value > rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value > mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_MORE_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &node_type ) {
                node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value >= rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value >= mp_obj_get_int( rhs_in ) );
            }
        }
        // Arithmetic operators - return int (not Node)
        else if ( op == MP_BINARY_OP_ADD ) {
            int rhs_val = 0;
            if ( mp_obj_get_type( rhs_in ) == &node_type ) {
                node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                rhs_val = rhs->value;
            } else if ( mp_obj_is_int( rhs_in ) ) {
                rhs_val = mp_obj_get_int( rhs_in );
            } else {
                return MP_OBJ_NULL;
            }
            return mp_obj_new_int( lhs->value + rhs_val );
        } else if ( op == MP_BINARY_OP_SUBTRACT ) {
            int rhs_val = 0;
            if ( mp_obj_get_type( rhs_in ) == &node_type ) {
                node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                rhs_val = rhs->value;
            } else if ( mp_obj_is_int( rhs_in ) ) {
                rhs_val = mp_obj_get_int( rhs_in );
            } else {
                return MP_OBJ_NULL;
            }
            return mp_obj_new_int( lhs->value - rhs_val );
        } else if ( op == MP_BINARY_OP_MULTIPLY ) {
            int rhs_val = 0;
            if ( mp_obj_get_type( rhs_in ) == &node_type ) {
                node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                rhs_val = rhs->value;
            } else if ( mp_obj_is_int( rhs_in ) ) {
                rhs_val = mp_obj_get_int( rhs_in );
            } else {
                return MP_OBJ_NULL;
            }
            return mp_obj_new_int( lhs->value * rhs_val );
        } else if ( op == MP_BINARY_OP_FLOOR_DIVIDE ) {
            int rhs_val = 0;
            if ( mp_obj_get_type( rhs_in ) == &node_type ) {
                node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                rhs_val = rhs->value;
            } else if ( mp_obj_is_int( rhs_in ) ) {
                rhs_val = mp_obj_get_int( rhs_in );
            } else {
                return MP_OBJ_NULL;
            }
            if ( rhs_val == 0 ) {
                mp_raise_msg( &mp_type_ZeroDivisionError, MP_ERROR_TEXT( "division by zero" ) );
            }
            return mp_obj_new_int( lhs->value / rhs_val );
        } else if ( op == MP_BINARY_OP_MODULO ) {
            int rhs_val = 0;
            if ( mp_obj_get_type( rhs_in ) == &node_type ) {
                node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                rhs_val = rhs->value;
            } else if ( mp_obj_is_int( rhs_in ) ) {
                rhs_val = mp_obj_get_int( rhs_in );
            } else {
                return MP_OBJ_NULL;
            }
            if ( rhs_val == 0 ) {
                mp_raise_msg( &mp_type_ZeroDivisionError, MP_ERROR_TEXT( "modulo by zero" ) );
            }
            return mp_obj_new_int( lhs->value % rhs_val );
        }
    }
    
    // Handle reverse operations: int op Node
    if ( mp_obj_get_type( rhs_in ) == &node_type && mp_obj_is_int( lhs_in ) ) {
        node_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
        int lhs_val = mp_obj_get_int( lhs_in );
        
        // Comparison operators (reversed)
        if ( op == MP_BINARY_OP_EQUAL ) {
            return mp_obj_new_bool( lhs_val == rhs->value );
        } else if ( op == MP_BINARY_OP_NOT_EQUAL ) {
            return mp_obj_new_bool( lhs_val != rhs->value );
        } else if ( op == MP_BINARY_OP_LESS ) {
            return mp_obj_new_bool( lhs_val < rhs->value );
        } else if ( op == MP_BINARY_OP_LESS_EQUAL ) {
            return mp_obj_new_bool( lhs_val <= rhs->value );
        } else if ( op == MP_BINARY_OP_MORE ) {
            return mp_obj_new_bool( lhs_val > rhs->value );
        } else if ( op == MP_BINARY_OP_MORE_EQUAL ) {
            return mp_obj_new_bool( lhs_val >= rhs->value );
        }
        // Arithmetic operators (reversed)
        else if ( op == MP_BINARY_OP_ADD ) {
            return mp_obj_new_int( lhs_val + rhs->value );
        } else if ( op == MP_BINARY_OP_SUBTRACT ) {
            return mp_obj_new_int( lhs_val - rhs->value );
        } else if ( op == MP_BINARY_OP_MULTIPLY ) {
            return mp_obj_new_int( lhs_val * rhs->value );
        } else if ( op == MP_BINARY_OP_FLOOR_DIVIDE ) {
            if ( rhs->value == 0 ) {
                mp_raise_msg( &mp_type_ZeroDivisionError, MP_ERROR_TEXT( "division by zero" ) );
            }
            return mp_obj_new_int( lhs_val / rhs->value );
        } else if ( op == MP_BINARY_OP_MODULO ) {
            if ( rhs->value == 0 ) {
                mp_raise_msg( &mp_type_ZeroDivisionError, MP_ERROR_TEXT( "modulo by zero" ) );
            }
            return mp_obj_new_int( lhs_val % rhs->value );
        }
    }
    
    return MP_OBJ_NULL;
}

MP_DEFINE_CONST_OBJ_TYPE(
    node_type,
    MP_QSTR_node,
    MP_TYPE_FLAG_NONE,
    make_new, node_make_new,
    print, node_print,
    unary_op, node_unary_op,
    binary_op, node_binary_op );

static mp_obj_t node_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args ) {
    mp_arg_check_num( n_args, n_kw, 1, 1, false );

    node_obj_t* o = m_new_obj( node_obj_t );
    o->base.type = &node_type;

    if ( mp_obj_is_str( args[ 0 ] ) ) {
        // Handle string input
        const char* name = mp_obj_str_get_str( args[ 0 ] );
        int value = find_node_value( name );
        if ( value == -1 ) {
            mp_raise_ValueError( MP_ERROR_TEXT( "Unknown node name" ) );
        }
        o->value = value;
    } else if ( mp_obj_is_int( args[ 0 ] ) ) {
        // Handle integer input
        o->value = mp_obj_get_int( args[ 0 ] );
    } else if ( mp_obj_get_type( args[ 0 ] ) == &node_type ) {
        // Handle copying another node
        node_obj_t* other = MP_OBJ_TO_PTR( args[ 0 ] );
        o->value = other->value;
    } else {
        mp_raise_TypeError( MP_ERROR_TEXT( "Node must be created from string, int, or another node" ) );
    }

    return MP_OBJ_FROM_PTR( o );
}

static mp_obj_t node_new( int value ) {
    node_obj_t* o = m_new_obj( node_obj_t );
    o->base.type = &node_type;
    o->value = value;
    return MP_OBJ_FROM_PTR( o );
}

//=============================================================================
// Probe Pad Type - Represents probe pad readings and states
//=============================================================================

typedef struct _probe_pad_obj_t {
    mp_obj_base_t base;
    int value; // the pad number or -1 for no pad
} probe_pad_obj_t;

static void probe_pad_print( const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind ) {
    probe_pad_obj_t* self = MP_OBJ_TO_PTR( self_in );

    // Get the human-readable name for this pad
    const char* name = jl_get_pad_name( self->value );
    mp_printf( print, "%s", name );
}

static mp_obj_t probe_pad_unary_op( mp_unary_op_t op, mp_obj_t self_in ) {
    probe_pad_obj_t* self = MP_OBJ_TO_PTR( self_in );
    switch ( op ) {
    case MP_UNARY_OP_BOOL:
        // Only valid pads (not -1) are "truthy"
        return mp_obj_new_bool( self->value != -1 );
    case MP_UNARY_OP_INT_MAYBE:
        // Support int(pad) conversion
        return mp_obj_new_int( self->value );
    case MP_UNARY_OP_FLOAT_MAYBE:
        // Support float(pad) conversion
        return mp_obj_new_float( (mp_float_t)self->value );
    default:
        return MP_OBJ_NULL;
    }
}

// Binary operations to allow pads to work with integers and strings
static mp_obj_t probe_pad_binary_op( mp_binary_op_t op, mp_obj_t lhs_in, mp_obj_t rhs_in ) {
    // Handle string concatenation for all cases involving ProbePad
    if ( op == MP_BINARY_OP_ADD ) {
        if ( mp_obj_is_str( lhs_in ) && mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
            // "string" + ProbePad
            const char* lhs_str = mp_obj_str_get_str( lhs_in );
            probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
            const char* rhs_str = jl_get_pad_name( rhs->value );

            // Concatenate strings
            size_t lhs_len = strlen( lhs_str );
            size_t rhs_len = strlen( rhs_str );
            char* result = m_new( char, lhs_len + rhs_len + 1 );
            strcpy( result, lhs_str );
            strcat( result, rhs_str );

            mp_obj_t result_obj = mp_obj_new_str( result, lhs_len + rhs_len );
            m_del( char, result, lhs_len + rhs_len + 1 );
            return result_obj;

        } else if ( mp_obj_get_type( lhs_in ) == &probe_pad_type && mp_obj_is_str( rhs_in ) ) {
            // ProbePad + "string"
            probe_pad_obj_t* lhs = MP_OBJ_TO_PTR( lhs_in );
            const char* lhs_str = jl_get_pad_name( lhs->value );
            const char* rhs_str = mp_obj_str_get_str( rhs_in );

            // Concatenate strings
            size_t lhs_len = strlen( lhs_str );
            size_t rhs_len = strlen( rhs_str );
            char* result = m_new( char, lhs_len + rhs_len + 1 );
            strcpy( result, lhs_str );
            strcat( result, rhs_str );

            mp_obj_t result_obj = mp_obj_new_str( result, lhs_len + rhs_len );
            m_del( char, result, lhs_len + rhs_len + 1 );
            return result_obj;
        } else if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
            // Handle reverse operation: when string + ProbePad fails, MicroPython tries ProbePad + string
            // Convert any left operand to string and concatenate with ProbePad
            probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
            const char* rhs_str = jl_get_pad_name( rhs->value );

            // Convert left operand to string
            if ( mp_obj_is_str( lhs_in ) ) {
                const char* lhs_str = mp_obj_str_get_str( lhs_in );
                size_t lhs_len = strlen( lhs_str );
                size_t rhs_len = strlen( rhs_str );
                char* result = m_new( char, lhs_len + rhs_len + 1 );
                strcpy( result, lhs_str );
                strcat( result, rhs_str );

                mp_obj_t result_obj = mp_obj_new_str( result, lhs_len + rhs_len );
                m_del( char, result, lhs_len + rhs_len + 1 );
                return result_obj;
            }
        }
    }

    if ( mp_obj_get_type( lhs_in ) == &probe_pad_type ) {
        probe_pad_obj_t* lhs = MP_OBJ_TO_PTR( lhs_in );
        
        // Comparison operators
        if ( op == MP_BINARY_OP_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
                probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value == rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value == mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_NOT_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
                probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value != rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value != mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_LESS ) {
            if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
                probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value < rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value < mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_LESS_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
                probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value <= rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value <= mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_MORE ) {
            if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
                probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value > rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value > mp_obj_get_int( rhs_in ) );
            }
        } else if ( op == MP_BINARY_OP_MORE_EQUAL ) {
            if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
                probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                return mp_obj_new_bool( lhs->value >= rhs->value );
            } else if ( mp_obj_is_int( rhs_in ) ) {
                return mp_obj_new_bool( lhs->value >= mp_obj_get_int( rhs_in ) );
            }
        }
        // Arithmetic operators - return int (not ProbePad)
        else if ( op == MP_BINARY_OP_ADD ) {
            int rhs_val = 0;
            if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
                probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                rhs_val = rhs->value;
            } else if ( mp_obj_is_int( rhs_in ) ) {
                rhs_val = mp_obj_get_int( rhs_in );
            } else {
                return MP_OBJ_NULL;
            }
            return mp_obj_new_int( lhs->value + rhs_val );
        } else if ( op == MP_BINARY_OP_SUBTRACT ) {
            int rhs_val = 0;
            if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
                probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                rhs_val = rhs->value;
            } else if ( mp_obj_is_int( rhs_in ) ) {
                rhs_val = mp_obj_get_int( rhs_in );
            } else {
                return MP_OBJ_NULL;
            }
            return mp_obj_new_int( lhs->value - rhs_val );
        } else if ( op == MP_BINARY_OP_MULTIPLY ) {
            int rhs_val = 0;
            if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
                probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                rhs_val = rhs->value;
            } else if ( mp_obj_is_int( rhs_in ) ) {
                rhs_val = mp_obj_get_int( rhs_in );
            } else {
                return MP_OBJ_NULL;
            }
            return mp_obj_new_int( lhs->value * rhs_val );
        } else if ( op == MP_BINARY_OP_FLOOR_DIVIDE ) {
            int rhs_val = 0;
            if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
                probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                rhs_val = rhs->value;
            } else if ( mp_obj_is_int( rhs_in ) ) {
                rhs_val = mp_obj_get_int( rhs_in );
            } else {
                return MP_OBJ_NULL;
            }
            if ( rhs_val == 0 ) {
                mp_raise_msg( &mp_type_ZeroDivisionError, MP_ERROR_TEXT( "division by zero" ) );
            }
            return mp_obj_new_int( lhs->value / rhs_val );
        } else if ( op == MP_BINARY_OP_MODULO ) {
            int rhs_val = 0;
            if ( mp_obj_get_type( rhs_in ) == &probe_pad_type ) {
                probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
                rhs_val = rhs->value;
            } else if ( mp_obj_is_int( rhs_in ) ) {
                rhs_val = mp_obj_get_int( rhs_in );
            } else {
                return MP_OBJ_NULL;
            }
            if ( rhs_val == 0 ) {
                mp_raise_msg( &mp_type_ZeroDivisionError, MP_ERROR_TEXT( "modulo by zero" ) );
            }
            return mp_obj_new_int( lhs->value % rhs_val );
        }
    }
    
    // Handle reverse operations: int op ProbePad
    if ( mp_obj_get_type( rhs_in ) == &probe_pad_type && mp_obj_is_int( lhs_in ) ) {
        probe_pad_obj_t* rhs = MP_OBJ_TO_PTR( rhs_in );
        int lhs_val = mp_obj_get_int( lhs_in );
        
        // Comparison operators (reversed)
        if ( op == MP_BINARY_OP_EQUAL ) {
            return mp_obj_new_bool( lhs_val == rhs->value );
        } else if ( op == MP_BINARY_OP_NOT_EQUAL ) {
            return mp_obj_new_bool( lhs_val != rhs->value );
        } else if ( op == MP_BINARY_OP_LESS ) {
            return mp_obj_new_bool( lhs_val < rhs->value );
        } else if ( op == MP_BINARY_OP_LESS_EQUAL ) {
            return mp_obj_new_bool( lhs_val <= rhs->value );
        } else if ( op == MP_BINARY_OP_MORE ) {
            return mp_obj_new_bool( lhs_val > rhs->value );
        } else if ( op == MP_BINARY_OP_MORE_EQUAL ) {
            return mp_obj_new_bool( lhs_val >= rhs->value );
        }
        // Arithmetic operators (reversed)
        else if ( op == MP_BINARY_OP_ADD ) {
            return mp_obj_new_int( lhs_val + rhs->value );
        } else if ( op == MP_BINARY_OP_SUBTRACT ) {
            return mp_obj_new_int( lhs_val - rhs->value );
        } else if ( op == MP_BINARY_OP_MULTIPLY ) {
            return mp_obj_new_int( lhs_val * rhs->value );
        } else if ( op == MP_BINARY_OP_FLOOR_DIVIDE ) {
            if ( rhs->value == 0 ) {
                mp_raise_msg( &mp_type_ZeroDivisionError, MP_ERROR_TEXT( "division by zero" ) );
            }
            return mp_obj_new_int( lhs_val / rhs->value );
        } else if ( op == MP_BINARY_OP_MODULO ) {
            if ( rhs->value == 0 ) {
                mp_raise_msg( &mp_type_ZeroDivisionError, MP_ERROR_TEXT( "modulo by zero" ) );
            }
            return mp_obj_new_int( lhs_val % rhs->value );
        }
    }
    
    return MP_OBJ_NULL;
}

// __str__ method for ProbePad
static mp_obj_t probe_pad_str( mp_obj_t self_in ) {
    probe_pad_obj_t* self = MP_OBJ_TO_PTR( self_in );
    const char* name = jl_get_pad_name( self->value );
    return mp_obj_new_str( name, strlen( name ) );
}
static MP_DEFINE_CONST_FUN_OBJ_1( probe_pad_str_obj, probe_pad_str );

// ProbePad methods table
static const mp_rom_map_elem_t probe_pad_locals_dict_table[] = {
    { MP_ROM_QSTR( MP_QSTR___str__ ), MP_ROM_PTR( &probe_pad_str_obj ) },
};
static MP_DEFINE_CONST_DICT( probe_pad_locals_dict, probe_pad_locals_dict_table );

MP_DEFINE_CONST_OBJ_TYPE(
    probe_pad_type,
    MP_QSTR_ProbePad,
    MP_TYPE_FLAG_NONE,
    make_new, probe_pad_make_new,
    print, probe_pad_print,
    unary_op, probe_pad_unary_op,
    binary_op, probe_pad_binary_op,
    locals_dict, &probe_pad_locals_dict );

static mp_obj_t probe_pad_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args ) {
    mp_arg_check_num( n_args, n_kw, 1, 1, false );

    probe_pad_obj_t* o = m_new_obj( probe_pad_obj_t );
    o->base.type = &probe_pad_type;
    o->value = mp_obj_get_int( args[ 0 ] );

    return MP_OBJ_FROM_PTR( o );
}

static mp_obj_t probe_pad_new( int value ) {
    probe_pad_obj_t* o = m_new_obj( probe_pad_obj_t );
    o->base.type = &probe_pad_type;
    o->value = value;
    return MP_OBJ_FROM_PTR( o );
}

// Helper function to extract integer from node or int argument
static int get_node_value( mp_obj_t obj ) {
    if ( mp_obj_get_type( obj ) == &node_type ) {
        node_obj_t* node = MP_OBJ_TO_PTR( obj );
        return node->value;
    } else if ( mp_obj_is_int( obj ) ) {
        return mp_obj_get_int( obj );
    } else if ( mp_obj_is_str( obj ) ) {
        // Allow direct string to int conversion in functions
        const char* name = mp_obj_str_get_str( obj );
        int value = find_node_value( name );
        if ( value == -1 ) {
            mp_raise_ValueError( MP_ERROR_TEXT( "Unknown node name" ) );
        }
        return value;
    }
    mp_raise_TypeError( MP_ERROR_TEXT( "Expected node, int, or string" ) );
}

// Helper function to map node values to DAC channels
static int map_node_to_dac_channel( int node_value ) {
    switch ( node_value ) {
    case 106: // DAC0
        return 0;
    case 107: // DAC1
        return 1;
    case 101: // TOP_RAIL
        return 2;
    case 102: // BOTTOM_RAIL
        return 3;
    default:
        // If it's already a channel number (0-3), return it
        if ( node_value >= 0 && node_value <= 3 ) {
            return node_value;
        }
        return -1; // Invalid
    }
}

// Helper function to get DAC channel from node or int argument
static int get_dac_channel( mp_obj_t obj ) {
    int node_value = get_node_value( obj );
    int channel = map_node_to_dac_channel( node_value );
    if ( channel == -1 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "Invalid DAC channel or node. Use 0-3, DAC0, DAC1, TOP_RAIL, or BOTTOM_RAIL" ) );
    }
    return channel;
}

//=============================================================================
// Function Implementations
//=============================================================================

// DAC Functions
static mp_obj_t jl_dac_set_func( size_t n_args, const mp_obj_t* args ) {
    int channel = get_dac_channel( args[ 0 ] );
    float voltage = mp_obj_get_float( args[ 1 ] );
    int save = ( n_args > 2 ) ? mp_obj_is_true( args[ 2 ] ) ? 1 : 0 : 1; // Default save=True

    printf( "jl_dac_set: channel: %d, voltage: %f, save: %d\n", channel, voltage, save );
    jl_dac_set( channel, voltage, save );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_dac_set_obj, 2, 3, jl_dac_set_func );

static mp_obj_t jl_dac_get_func( mp_obj_t channel_obj ) {
    int channel = get_dac_channel( channel_obj );

    float voltage = jl_dac_get( channel );

    // Return voltage as float for backward compatibility
    return mp_obj_new_float( voltage );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_dac_get_obj, jl_dac_get_func );

// Wavegen MicroPython bindings
// Helper: parse node/int/str to DAC channel 0..3 for wavegen_set_output
static int get_wavegen_channel( mp_obj_t obj ) {
    int node_value = get_node_value( obj );
    int ch = map_node_to_dac_channel( node_value );
    if ( ch < 0 || ch > 3 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "Invalid output. Use DAC0, DAC1, TOP_RAIL, BOTTOM_RAIL" ) );
    }
    return ch;
}

// Helper: parse waveform from int or string
static int get_wavegen_wave( mp_obj_t obj ) {
    if ( mp_obj_is_int( obj ) ) {
        int w = mp_obj_get_int( obj );
        if ( w < 0 || w > 4 ) {
            mp_raise_ValueError( MP_ERROR_TEXT( "Waveform must be 0-4" ) );
        }
        return w;
    } else if ( mp_obj_is_str( obj ) ) {
        const char* s = mp_obj_str_get_str( obj );
        // accept common aliases case-insensitively
        char up[ 24 ];
        size_t n = strlen( s );
        if ( n > 23 )
            n = 23;
        for ( size_t i = 0; i < n; i++ )
            up[ i ] = (char)toupper( (unsigned char)s[ i ] );
        up[ n ] = '\0';
        if ( strcmp( up, "SINE" ) == 0 )
            return 0;
        if ( strcmp( up, "TRIANGLE" ) == 0 || strcmp( up, "TRI" ) == 0 )
            return 1;
        if ( strcmp( up, "RAMP" ) == 0 || strcmp( up, "SAW" ) == 0 || strcmp( up, "SAWTOOTH" ) == 0 )
            return 2;
        if ( strcmp( up, "SQUARE" ) == 0 || strcmp( up, "SQ" ) == 0 )
            return 3;
        if ( strcmp( up, "ARBITRARY" ) == 0 || strcmp( up, "ARB" ) == 0 )
            return 4;
        mp_raise_ValueError( MP_ERROR_TEXT( "Unknown waveform" ) );
    }
    mp_raise_TypeError( MP_ERROR_TEXT( "Expected int or string for waveform" ) );
}

static mp_obj_t jl_wavegen_set_output_func( mp_obj_t out_obj ) {
    int ch = get_wavegen_channel( out_obj );
    jl_wavegen_set_output( ch );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_wavegen_set_output_obj, jl_wavegen_set_output_func );

static mp_obj_t jl_wavegen_set_freq_func( mp_obj_t hz_obj ) {
    float hz = mp_obj_get_float( hz_obj );
    jl_wavegen_set_freq( hz );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_wavegen_set_freq_obj, jl_wavegen_set_freq_func );

static mp_obj_t jl_wavegen_set_wave_func( mp_obj_t w_obj ) {
    int w = get_wavegen_wave( w_obj );
    if ( w == 4 ) {
        // ARBITRARY not implemented yet
        mp_raise_NotImplementedError( MP_ERROR_TEXT( "ARBITRARY waveform not implemented yet" ) );
    }
    jl_wavegen_set_wave( w );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_wavegen_set_wave_obj, jl_wavegen_set_wave_func );

static mp_obj_t jl_wavegen_set_sweep_func( size_t n_args, const mp_obj_t* args ) {
    if ( n_args != 3 ) {
        mp_raise_TypeError( MP_ERROR_TEXT( "wavegen_set_sweep(start_hz, end_hz, seconds)" ) );
    }
    float start_hz = mp_obj_get_float( args[ 0 ] );
    float end_hz = mp_obj_get_float( args[ 1 ] );
    float seconds = mp_obj_get_float( args[ 2 ] );
    jl_wavegen_set_sweep( start_hz, end_hz, seconds );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_wavegen_set_sweep_obj, 3, 3, jl_wavegen_set_sweep_func );

static mp_obj_t jl_wavegen_set_amplitude_func( mp_obj_t vpp_obj ) {
    float vpp = mp_obj_get_float( vpp_obj );
    jl_wavegen_set_amplitude( vpp );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_wavegen_set_amplitude_obj, jl_wavegen_set_amplitude_func );

static mp_obj_t jl_wavegen_set_offset_func( mp_obj_t v_obj ) {
    float v = mp_obj_get_float( v_obj );
    jl_wavegen_set_offset( v );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_wavegen_set_offset_obj, jl_wavegen_set_offset_func );

static mp_obj_t jl_wavegen_start_func( size_t n_args, const mp_obj_t* args ) {
    // wavegen_start([run=True])
    int run = 1;
    if ( n_args >= 1 ) {
        run = mp_obj_is_true( args[ 0 ] ) ? 1 : 0;
    }
    jl_wavegen_start( run );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_wavegen_start_obj, 0, 1, jl_wavegen_start_func );

static mp_obj_t jl_wavegen_stop_func( void ) {
    jl_wavegen_stop( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_wavegen_stop_obj, jl_wavegen_stop_func );

// Getters
static mp_obj_t jl_wavegen_get_output_func( void ) { return mp_obj_new_int( jl_wavegen_get_output( ) ); }
static MP_DEFINE_CONST_FUN_OBJ_0( jl_wavegen_get_output_obj, jl_wavegen_get_output_func );
static mp_obj_t jl_wavegen_get_freq_func( void ) { return mp_obj_new_float( jl_wavegen_get_freq( ) ); }
static MP_DEFINE_CONST_FUN_OBJ_0( jl_wavegen_get_freq_obj, jl_wavegen_get_freq_func );
static mp_obj_t jl_wavegen_get_wave_func( void ) { return mp_obj_new_int( jl_wavegen_get_wave( ) ); }
static MP_DEFINE_CONST_FUN_OBJ_0( jl_wavegen_get_wave_obj, jl_wavegen_get_wave_func );
static mp_obj_t jl_wavegen_get_amplitude_func( void ) { return mp_obj_new_float( jl_wavegen_get_amplitude( ) ); }
static MP_DEFINE_CONST_FUN_OBJ_0( jl_wavegen_get_amplitude_obj, jl_wavegen_get_amplitude_func );
static mp_obj_t jl_wavegen_get_offset_func( void ) { return mp_obj_new_float( jl_wavegen_get_offset( ) ); }
static MP_DEFINE_CONST_FUN_OBJ_0( jl_wavegen_get_offset_obj, jl_wavegen_get_offset_func );
static mp_obj_t jl_wavegen_is_running_func( void ) { return mp_obj_new_bool( jl_wavegen_is_running( ) ); }
static MP_DEFINE_CONST_FUN_OBJ_0( jl_wavegen_is_running_obj, jl_wavegen_is_running_func );
// Remove old AWG stubs; replaced by wavegen_* API

// ADC Functions
static mp_obj_t jl_adc_get_func( mp_obj_t channel_obj ) {
    int channel = mp_obj_get_int( channel_obj );

    // 0-3 = breadboard ADCs, 4 = the 0-5V channel, 5 = probe pad sense,
    // 7 = probe tip (buffer output). readAdcVoltage() handles all of 0-7;
    // the old 0-3 limit predated the extra channels.
    if ( channel < 0 || channel > 7 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "ADC channel must be 0-7" ) );
    }

    float voltage = jl_adc_get( channel );

    // Return voltage as float for backward compatibility
    return mp_obj_new_float( voltage );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_adc_get_obj, jl_adc_get_func );

// USB Audio Functions
//
// Two layers, on purpose. usb_audio_enable()/disable() control whether the
// device advertises a microphone at all - they rewrite the USB config
// descriptor and re-enumerate, so THIS SERIAL PORT DROPS and comes back with
// the same name a couple of seconds later. Capture itself is started by the
// host opening the input device, not from here: MicroPython could never feed
// 48 kHz through the interpreter, so a C-side DMA pump does it.
static mp_obj_t jl_usb_audio_enable_func( void ) {
    return mp_obj_new_bool( jl_usb_audio_enable( ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_usb_audio_enable_obj, jl_usb_audio_enable_func );

static mp_obj_t jl_usb_audio_disable_func( void ) {
    return mp_obj_new_bool( jl_usb_audio_disable( ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_usb_audio_disable_obj, jl_usb_audio_disable_func );

static mp_obj_t jl_usb_audio_is_enabled_func( void ) {
    return mp_obj_new_bool( jl_usb_audio_is_enabled( ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_usb_audio_is_enabled_obj, jl_usb_audio_is_enabled_func );

static mp_obj_t jl_usb_audio_active_func( void ) {
    return mp_obj_new_bool( jl_usb_audio_is_streaming( ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_usb_audio_active_obj, jl_usb_audio_active_func );

// usb_audio_setup(left=0, right=1, full_scale=8.0, dc_block=True)
// Routes the two ADC channels to L/R and makes the device visible.
static mp_obj_t jl_usb_audio_setup_func( size_t n_args, const mp_obj_t *pos_args,
                                         mp_map_t *kw_args ) {
    static const mp_arg_t allowed_args[] = {
        // Positional-or-keyword, so both usb_audio_setup(0, 1) and
        // usb_audio_setup(left=0, right=1, full_scale=8.0) read naturally.
        { MP_QSTR_left,       MP_ARG_INT,  { .u_int = 0 } },
        { MP_QSTR_right,      MP_ARG_INT,  { .u_int = 1 } },
        { MP_QSTR_full_scale, MP_ARG_OBJ,  { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_dc_block,   MP_ARG_BOOL, { .u_bool = true } },
    };
    mp_arg_val_t args[ MP_ARRAY_SIZE( allowed_args ) ];
    mp_arg_parse_all( n_args, pos_args, kw_args,
                      MP_ARRAY_SIZE( allowed_args ), allowed_args, args );

    if ( !jl_usb_audio_set_channels( args[ 0 ].u_int, args[ 1 ].u_int ) ) {
        mp_raise_ValueError( MP_ERROR_TEXT(
            "left/right must be distinct ADC channels 0-7" ) );
    }
    if ( args[ 2 ].u_obj != MP_OBJ_NULL ) {
        if ( !jl_usb_audio_set_full_scale( mp_obj_get_float( args[ 2 ].u_obj ) ) ) {
            mp_raise_ValueError( MP_ERROR_TEXT( "full_scale must be 0.05-20.0 volts" ) );
        }
    }
    jl_usb_audio_set_dc_block( args[ 3 ].u_bool ? 1 : 0 );

    return mp_obj_new_bool( jl_usb_audio_enable( ) );
}
static MP_DEFINE_CONST_FUN_OBJ_KW( jl_usb_audio_setup_obj, 0, jl_usb_audio_setup_func );

static mp_obj_t jl_usb_audio_save_func( void ) {
    jl_usb_audio_save( );
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_usb_audio_save_obj, jl_usb_audio_save_func );

static mp_obj_t jl_usb_audio_set_rate_func( mp_obj_t hz_obj ) {
    if ( !jl_usb_audio_set_rate( mp_obj_get_int( hz_obj ) ) ) {
        mp_raise_ValueError( MP_ERROR_TEXT(
            "rate must be 8000-48000 Hz in 1 kHz steps" ) );
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_usb_audio_set_rate_obj, jl_usb_audio_set_rate_func );

static mp_obj_t jl_usb_audio_set_range_func( mp_obj_t volts_obj ) {
    if ( !jl_usb_audio_set_full_scale( mp_obj_get_float( volts_obj ) ) ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "full scale must be 0.05-20.0 volts" ) );
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_usb_audio_set_range_obj, jl_usb_audio_set_range_func );

static mp_obj_t jl_usb_audio_status_func( void ) {
    int enabled = 0, streaming = 0, host_open = 0, left = 0, right = 0, dc_block = 0;
    int sample_rate = 0, pending_rate = 0, frames_sent = 0, fifo_overflow = 0, adc_overrun = 0;
    int late_irq = 0, resyncs = 0, probe_pauses = 0, claim_fail = 0, init_fail = 0;
    float full_scale = 0.0f;
    jl_usb_audio_status( &enabled, &streaming, &host_open, &left, &right, &full_scale,
                         &dc_block, &sample_rate, &pending_rate, &frames_sent,
                         &fifo_overflow, &adc_overrun,
                         &late_irq, &resyncs, &probe_pauses, &claim_fail, &init_fail );

    // Health counters: a clean recording has frames_sent climbing at
    // sample_rate per second and everything else flat (late_irq/resyncs tick
    // once per flash write, probe_pauses once per probe use).
    mp_obj_t d = mp_obj_new_dict( 17 );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_enabled ),      mp_obj_new_bool( enabled ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_streaming ),    mp_obj_new_bool( streaming ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_host_open ),    mp_obj_new_bool( host_open ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_left ),         mp_obj_new_int( left ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_right ),        mp_obj_new_int( right ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_full_scale ),   mp_obj_new_float( full_scale ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_dc_block ),     mp_obj_new_bool( dc_block ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_sample_rate ),  mp_obj_new_int( sample_rate ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_pending_rate ), mp_obj_new_int( pending_rate ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_frames_sent ),  mp_obj_new_int( frames_sent ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_fifo_overflow ), mp_obj_new_int( fifo_overflow ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_adc_overrun ),  mp_obj_new_int( adc_overrun ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_late_irq ),     mp_obj_new_int( late_irq ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_resyncs ),      mp_obj_new_int( resyncs ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_probe_pauses ), mp_obj_new_int( probe_pauses ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_claim_fail ),   mp_obj_new_int( claim_fail ) );
    mp_obj_dict_store( d, MP_ROM_QSTR( MP_QSTR_init_fail ),    mp_obj_new_int( init_fail ) );
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_usb_audio_status_obj, jl_usb_audio_status_func );

// INA Functions
static mp_obj_t jl_ina_get_current_func( mp_obj_t sensor_obj ) {
    int sensor = mp_obj_get_int( sensor_obj );

    if ( sensor < 0 || sensor > 1 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "INA sensor must be 0 or 1" ) );
    }

    float current = jl_ina_get_current( sensor );

    // Return current as float for backward compatibility
    return mp_obj_new_float( current );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_ina_get_current_obj, jl_ina_get_current_func );

static mp_obj_t jl_ina_get_voltage_func( mp_obj_t sensor_obj ) {
    int sensor = mp_obj_get_int( sensor_obj );

    if ( sensor < 0 || sensor > 1 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "INA sensor must be 0 or 1" ) );
    }

    float voltage = jl_ina_get_voltage( sensor );

    // Return voltage as float for backward compatibility
    return mp_obj_new_float( voltage );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_ina_get_voltage_obj, jl_ina_get_voltage_func );

static mp_obj_t jl_ina_get_bus_voltage_func( mp_obj_t sensor_obj ) {
    int sensor = mp_obj_get_int( sensor_obj );

    if ( sensor < 0 || sensor > 1 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "INA sensor must be 0 or 1" ) );
    }

    float voltage = jl_ina_get_bus_voltage( sensor );

    // Return voltage as float for backward compatibility
    return mp_obj_new_float( voltage );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_ina_get_bus_voltage_obj, jl_ina_get_bus_voltage_func );

static mp_obj_t jl_ina_get_power_func( mp_obj_t sensor_obj ) {
    int sensor = mp_obj_get_int( sensor_obj );

    if ( sensor < 0 || sensor > 1 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "INA sensor must be 0 or 1" ) );
    }

    float power = jl_ina_get_power( sensor );

    // Return power as float for backward compatibility
    return mp_obj_new_float( power );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_ina_get_power_obj, jl_ina_get_power_func );

// GPIO Functions
// Helper: map an incoming pin object (int or node) to a physical GPIO that
// jl_gpio_* backends understand. Supports:
// - Breadboard/routable GPIO indices: 1-10 (as-is)
// - RP GPIOs 20-27 (as-is)
// - Node constants GPIO_1..GPIO_8 (131..138) → RP GPIO 20..27
// - UART nodes UART_TX (116) → 0, UART_RX (117) → 1
static int map_pin_obj_to_physical_gpio( mp_obj_t pin_obj ) {
    int pin = -1;

    // Accept pre-wrapped node objects
    if ( mp_obj_get_type( pin_obj ) == &node_type ) {
        int node = get_node_value( pin_obj );
        if ( node >= 131 && node <= 138 ) {
            // GPIO_1..GPIO_8 → GP20..GP27
            pin = 20 + ( node - 131 );
        } else if ( node == 116 ) {
            // UART_TX → GP0
            pin = 0;
        } else if ( node == 117 ) {
            // UART_RX → GP1
            pin = 1;
        } else {
            // Not a supported GPIO-like node
            pin = -1;
        }
    } else {
        // Try numeric conversion (accepts small-int and int objects)
        pin = mp_obj_get_int( pin_obj );
    }

    // Validate the mapped pin for our backends
    if ( !( ( pin >= 1 && pin <= 10 ) || ( pin >= 20 && pin <= 27 ) || pin == 0 || pin == 1 ) ) {
        return -1;
    }
    return pin;
}

static mp_obj_t jl_gpio_set_func( mp_obj_t pin_obj, mp_obj_t value_obj ) {
    int pin = map_pin_obj_to_physical_gpio( pin_obj );
    int value = get_gpio_state_value( value_obj );

    if ( pin < 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-10, GPIO_1-GPIO_8, GPIO_20-GPIO_27, or UART_TX/UART_RX" ) );
    }

    jl_gpio_set( pin, value );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_gpio_set_obj, jl_gpio_set_func );

static mp_obj_t jl_gpio_set_dir_func( mp_obj_t pin_obj, mp_obj_t direction_obj ) {
    int pin = map_pin_obj_to_physical_gpio( pin_obj );
    int direction = get_direction_value( direction_obj );

    if ( pin < 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-10, GPIO_1-GPIO_8, GPIO_20-GPIO_27, or UART_TX/UART_RX" ) );
    }

    jl_gpio_set_dir( pin, direction );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_gpio_set_dir_obj, jl_gpio_set_dir_func );

static mp_obj_t jl_gpio_get_func( mp_obj_t pin_obj ) {
    int pin = map_pin_obj_to_physical_gpio( pin_obj );

    if ( pin < 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-10, GPIO_1-GPIO_8, GPIO_20-GPIO_27, or UART_TX/UART_RX" ) );
    }

    int value = jl_gpio_get( pin );

    // Return custom GPIO state object that displays as HIGH/LOW/FLOATING
    // jl_gpio_get returns: 0=LOW, 1=HIGH, 2=FLOATING
    gpio_state_value_t state;
    switch ( value ) {
    case 0:
        state = GPIO_STATE_LOW;
        break;
    case 1:
        state = GPIO_STATE_HIGH;
        break;
    case 2:
        state = GPIO_STATE_FLOATING;
        break;
    default:
        state = GPIO_STATE_LOW;
        break; // Default to LOW for safety
    }

    return gpio_state_new( state );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_gpio_get_obj, jl_gpio_get_func );

static mp_obj_t jl_gpio_get_dir_func( mp_obj_t pin_obj ) {
    int pin = map_pin_obj_to_physical_gpio( pin_obj );
    if ( pin < 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-10, GPIO_1-GPIO_8, GPIO_20-GPIO_27, or UART_TX/UART_RX" ) );
    }
    int direction = jl_gpio_get_dir( pin );

    // Return custom GPIO direction object that displays as INPUT/OUTPUT but behaves as boolean
    // Convert numeric firmware convention (0 = OUTPUT, 1 = INPUT) into boolean (true=OUTPUT)
    return gpio_direction_new( direction == 0 );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_gpio_get_dir_obj, jl_gpio_get_dir_func );

static mp_obj_t jl_gpio_set_pull_func( mp_obj_t pin_obj, mp_obj_t pull_obj ) {
    int pin = map_pin_obj_to_physical_gpio( pin_obj );
    int pull = get_pull_value( pull_obj );

    if ( pin < 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-10, GPIO_1-GPIO_8, GPIO_20-GPIO_27, or UART_TX/UART_RX" ) );
    }
    jl_gpio_set_pull( pin, pull );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_gpio_set_pull_obj, jl_gpio_set_pull_func );

static mp_obj_t jl_gpio_get_pull_func( mp_obj_t pin_obj ) {
    int pin = map_pin_obj_to_physical_gpio( pin_obj );
    if ( pin < 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-10, GPIO_1-GPIO_8, GPIO_20-GPIO_27, or UART_TX/UART_RX" ) );
    }
    int pull = jl_gpio_get_pull( pin );

    // Return custom GPIO pull object that displays as PULLUP/PULLDOWN/NO_PULL/BUS_KEEPER
    return gpio_pull_new( pull );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_gpio_get_pull_obj, jl_gpio_get_pull_func );

// GPIO read-floating control: control whether reads treat floating pins specially
static mp_obj_t jl_gpio_set_floating_read_func( mp_obj_t pin_obj, mp_obj_t enabled_obj ) {
    int pin = map_pin_obj_to_physical_gpio( pin_obj );
    if ( pin < 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-10, GPIO_1-GPIO_8, GPIO_20-GPIO_27, or UART_TX/UART_RX" ) );
    }
    int enabled = mp_obj_is_true( enabled_obj ) ? 1 : 0;
    jl_gpio_set_floating_read( pin, enabled );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_gpio_set_floating_read_obj, jl_gpio_set_floating_read_func );

static mp_obj_t jl_gpio_get_floating_read_func( mp_obj_t pin_obj ) {
    int pin = map_pin_obj_to_physical_gpio( pin_obj );
    if ( pin < 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-10, GPIO_1-GPIO_8, GPIO_20-GPIO_27, or UART_TX/UART_RX" ) );
    }
    int val = jl_gpio_get_floating_read( pin );
    return mp_obj_new_bool( val ? true : false );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_gpio_get_floating_read_obj, jl_gpio_get_floating_read_func );

// GPIO pin ownership functions for timing-critical operations (e.g., NeoPixels)
static mp_obj_t jl_gpio_claim_pin_func( mp_obj_t pin_obj ) {
    int pin = map_pin_obj_to_physical_gpio( pin_obj );
    if ( pin < 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-10, GPIO_1-GPIO_8, GPIO_20-GPIO_27, or UART_TX/UART_RX" ) );
    }
    jl_gpio_claim_pin( pin );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_gpio_claim_pin_obj, jl_gpio_claim_pin_func );

static mp_obj_t jl_gpio_release_pin_func( mp_obj_t pin_obj ) {
    int pin = map_pin_obj_to_physical_gpio( pin_obj );
    if ( pin < 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-10, GPIO_1-GPIO_8, GPIO_20-GPIO_27, or UART_TX/UART_RX" ) );
    }
    jl_gpio_release_pin( pin );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_gpio_release_pin_obj, jl_gpio_release_pin_func );

static mp_obj_t jl_gpio_release_all_pins_func( void ) {
    jl_gpio_release_all_pins( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_gpio_release_all_pins_obj, jl_gpio_release_all_pins_func );

// PWM Functions
static mp_obj_t jl_pwm_func( size_t n_args, const mp_obj_t* args ) {
    int gpio_pin = mp_obj_get_int( args[ 0 ] );
    float frequency = 1.0;  // Default frequency
    float duty_cycle = 0.5; // Default duty cycle

    if ( n_args > 1 ) {
        frequency = mp_obj_get_float( args[ 1 ] );
    }
    if ( n_args > 2 ) {
        duty_cycle = mp_obj_get_float( args[ 2 ] );
    }

    // Convert GPIO node constants (131-138) to pin numbers (1-8)
    if ( gpio_pin >= 131 && gpio_pin <= 138 ) {
        gpio_pin = gpio_pin - 131 + 1;
    }

    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-8 or GPIO_1-GPIO_8" ) );
    }

    if ( frequency < 0.0009 || frequency > 62500000.0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "PWM frequency must be 0.001Hz to 62.5MHz" ) );
    }

    if ( duty_cycle < 0.0 || duty_cycle > 1.0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "PWM duty cycle must be 0.0 to 1.0" ) );
    }

    int result = jl_pwm_setup( gpio_pin, frequency, duty_cycle );

    if ( result != 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "PWM setup failed" ) );
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_pwm_obj, 1, 3, jl_pwm_func );

static mp_obj_t jl_pwm_set_duty_cycle_func( mp_obj_t pin_obj, mp_obj_t duty_cycle_obj ) {
    int gpio_pin = mp_obj_get_int( pin_obj );
    float duty_cycle = mp_obj_get_float( duty_cycle_obj );

    // Convert GPIO node constants (131-138) to pin numbers (1-8)
    if ( gpio_pin >= 131 && gpio_pin <= 138 ) {
        gpio_pin = gpio_pin - 131 + 1;
    }

    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-8 or GPIO_1-GPIO_8" ) );
    }

    if ( duty_cycle < 0.0 || duty_cycle > 1.0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "PWM duty cycle must be 0.0 to 1.0" ) );
    }

    int result = jl_pwm_set_duty_cycle( gpio_pin, duty_cycle );

    if ( result != 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "PWM duty cycle set failed" ) );
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_pwm_set_duty_cycle_obj, jl_pwm_set_duty_cycle_func );

static mp_obj_t jl_pwm_set_frequency_func( mp_obj_t pin_obj, mp_obj_t frequency_obj ) {
    int gpio_pin = mp_obj_get_int( pin_obj );
    float frequency = mp_obj_get_float( frequency_obj );

    // Convert GPIO node constants (131-138) to pin numbers (1-8)
    if ( gpio_pin >= 131 && gpio_pin <= 138 ) {
        gpio_pin = gpio_pin - 131 + 1;
    }

    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-8 or GPIO_1-GPIO_8" ) );
    }

    if ( frequency < 0.0009 || frequency > 62500000.0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "PWM frequency must be 0.001Hz to 62.5MHz" ) );
    }

    int result = jl_pwm_set_frequency( gpio_pin, frequency );

    if ( result != 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "PWM frequency set failed" ) );
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_pwm_set_frequency_obj, jl_pwm_set_frequency_func );

static mp_obj_t jl_pwm_stop_func( mp_obj_t pin_obj ) {
    int gpio_pin = mp_obj_get_int( pin_obj );

    // Convert GPIO node constants (131-138) to pin numbers (1-8)
    if ( gpio_pin >= 131 && gpio_pin <= 138 ) {
        gpio_pin = gpio_pin - 131 + 1;
    }

    if ( gpio_pin < 1 || gpio_pin > 8 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "GPIO pin must be 1-8 or GPIO_1-GPIO_8" ) );
    }

    int result = jl_pwm_stop( gpio_pin );

    if ( result != 0 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "PWM stop failed" ) );
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_pwm_stop_obj, jl_pwm_stop_func );

// Node Functions
static mp_obj_t jl_nodes_connect_func( size_t n_args, const mp_obj_t* args ) {
    int node1 = get_node_value( args[ 0 ] );
    int node2 = get_node_value( args[ 1 ] );
    int duplicates = ( n_args > 2 ) ? mp_obj_get_int( args[ 2 ] ) : -1; // Default -1 = use global config

    jl_nodes_connect( node1, node2, 0, duplicates );  // save=0 (always use RAM state)
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_nodes_connect_obj, 2, 3, jl_nodes_connect_func );

static mp_obj_t jl_nodes_disconnect_func( mp_obj_t node1_obj, mp_obj_t node2_obj ) {
    int node1 = get_node_value( node1_obj );
    int node2 = get_node_value( node2_obj );

    jl_nodes_disconnect( node1, node2 );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_nodes_disconnect_obj, jl_nodes_disconnect_func );

static mp_obj_t jl_nodes_fast_connect_func( size_t n_args, const mp_obj_t* args ) {
    int node1 = get_node_value( args[ 0 ] );
    int node2 = get_node_value( args[ 1 ] );
    int duplicates = ( n_args > 2 ) ? mp_obj_get_int( args[ 2 ] ) : -1; // Default -1 = allow duplicates

    jl_nodes_fast_connect( node1, node2, duplicates );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_nodes_fast_connect_obj, 2, 3, jl_nodes_fast_connect_func );

static mp_obj_t jl_nodes_fast_disconnect_func( mp_obj_t node1_obj, mp_obj_t node2_obj ) {
    int node1 = get_node_value( node1_obj );
    int node2 = get_node_value( node2_obj );

    jl_nodes_fast_disconnect( node1, node2 );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_nodes_fast_disconnect_obj, jl_nodes_fast_disconnect_func );

static mp_obj_t jl_nodes_clear_func( void ) {
    jl_nodes_clear( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_nodes_clear_obj, jl_nodes_clear_func );

static mp_obj_t jl_nodes_is_connected_func( mp_obj_t node1_obj, mp_obj_t node2_obj ) {
    int node1 = get_node_value( node1_obj );
    int node2 = get_node_value( node2_obj );
    int connected = jl_nodes_is_connected( node1, node2 );

    // Return custom connection state object that displays as CONNECTED/DISCONNECTED but behaves as boolean
    return connection_state_new( connected );
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_nodes_is_connected_obj, jl_nodes_is_connected_func );

static mp_obj_t jl_nodes_save_func( size_t n_args, const mp_obj_t* args ) {
    int slot = ( n_args > 0 ) ? mp_obj_get_int( args[ 0 ] ) : -1; // Default to current slot if not specified

    int result = jl_nodes_save( slot );
    return mp_obj_new_int( result );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_nodes_save_obj, 0, 1, jl_nodes_save_func );

// ============================================================================
// Undo / Redo / History
// ============================================================================
extern bool undoUndo( void );
extern bool undoRedo( void );
extern bool undoCanUndo( void );
extern bool undoCanRedo( void );
extern int  undoPosition( void );
extern int  undoTotalTxns( void );
extern const char* undoLabelAt( int relativeOffset );
extern bool undoScrubTo( int targetPosition );
extern bool undoForceSnapshot( const char* reason );
extern int  undoSnapshotCount( void );

static mp_obj_t jl_undo_func( void ) { return mp_obj_new_bool( undoUndo( ) ); }
static MP_DEFINE_CONST_FUN_OBJ_0( jl_undo_obj, jl_undo_func );

static mp_obj_t jl_redo_func( void ) { return mp_obj_new_bool( undoRedo( ) ); }
static MP_DEFINE_CONST_FUN_OBJ_0( jl_redo_obj, jl_redo_func );

static mp_obj_t jl_history_position_func( void ) { return mp_obj_new_int( undoPosition( ) ); }
static MP_DEFINE_CONST_FUN_OBJ_0( jl_history_position_obj, jl_history_position_func );

static mp_obj_t jl_history_size_func( void ) { return mp_obj_new_int( undoTotalTxns( ) ); }
static MP_DEFINE_CONST_FUN_OBJ_0( jl_history_size_obj, jl_history_size_func );

static mp_obj_t jl_history_label_func( size_t n_args, const mp_obj_t* args ) {
    int offs = ( n_args > 0 ) ? mp_obj_get_int( args[ 0 ] ) : 0;
    const char* s = undoLabelAt( offs );
    return mp_obj_new_str( s ? s : "", s ? strlen( s ) : 0 );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_history_label_obj, 0, 1, jl_history_label_func );

static mp_obj_t jl_history_jump_func( mp_obj_t target ) {
    int t = mp_obj_get_int( target );
    return mp_obj_new_bool( undoScrubTo( t ) );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_history_jump_obj, jl_history_jump_func );

static mp_obj_t jl_history_snapshot_func( void ) {
    return mp_obj_new_bool( undoForceSnapshot( "python" ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_history_snapshot_obj, jl_history_snapshot_func );

static mp_obj_t jl_history_snapshot_count_func( void ) {
    return mp_obj_new_int( undoSnapshotCount( ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_history_snapshot_count_obj, jl_history_snapshot_count_func );

// Raw Hardware Functions
static mp_obj_t jl_send_raw_func( size_t n_args, const mp_obj_t* args ) {
    int x = mp_obj_get_int( args[ 1 ] );
    int y = mp_obj_get_int( args[ 2 ] );
    int setOrClear = ( n_args > 3 ) ? mp_obj_get_int( args[ 3 ] ) : 1; // Default to set (1)

    // Handle chip parameter (can be int, string, or char)
    if ( mp_obj_is_int( args[ 0 ] ) ) {
        // Integer chip number (0-11)
        int chip = mp_obj_get_int( args[ 0 ] );
        jl_send_raw( chip, x, y, setOrClear );
    } else if ( mp_obj_is_str( args[ 0 ] ) ) {
        // String chip identifier ("A"-"L" or "0"-"11")
        const char* chip_str = mp_obj_str_get_str( args[ 0 ] );
        jl_send_raw_str( chip_str, x, y, setOrClear );
    } else {
        mp_raise_ValueError( MP_ERROR_TEXT( "Chip must be integer (0-11) or string ('A'-'L')" ) );
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_send_raw_obj, 3, 4, jl_send_raw_func );

static mp_obj_t jl_switch_slot_func( mp_obj_t slot_obj ) {
    int slot = mp_obj_get_int( slot_obj );
    int result = jl_switch_slot( slot );

    if ( result == -1 ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "Invalid slot number" ) );
    }

    return mp_obj_new_int( result );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_switch_slot_obj, jl_switch_slot_func );

static mp_obj_t jl_nodes_discard_func( void ) {
    jl_restore_micropython_entry_state( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_nodes_discard_obj, jl_nodes_discard_func );

static mp_obj_t jl_nodes_has_changes_func( void ) {
    int has_changes = jl_has_unsaved_changes( );
    return mp_obj_new_bool( has_changes );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_nodes_has_changes_obj, jl_nodes_has_changes_func );

// =============================================================================
// Net Information API - Get/Set net names, colors, and info
// =============================================================================

// get_net_name(net_num) - Returns the name of a net
static mp_obj_t jl_get_net_name_func( mp_obj_t net_num_obj ) {
    int net_num = mp_obj_get_int( net_num_obj );
    const char* name = jl_get_net_name( net_num );

    if ( name == NULL ) {
        return mp_const_none;
    }
    return mp_obj_new_str( name, strlen( name ) );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_get_net_name_obj, jl_get_net_name_func );

// set_net_name(net_num, name) - Sets a custom name for a net
static mp_obj_t jl_set_net_name_func( mp_obj_t net_num_obj, mp_obj_t name_obj ) {
    int net_num = mp_obj_get_int( net_num_obj );

    if ( name_obj == mp_const_none ) {
        jl_set_net_name( net_num, NULL ); // Clear custom name
    } else {
        const char* name = mp_obj_str_get_str( name_obj );
        jl_set_net_name( net_num, name );
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_set_net_name_obj, jl_set_net_name_func );

// get_net_color(net_num) - Returns the color as hex integer (0xRRGGBB)
static mp_obj_t jl_get_net_color_func( mp_obj_t net_num_obj ) {
    int net_num = mp_obj_get_int( net_num_obj );
    uint32_t color = jl_get_net_color( net_num );
    return mp_obj_new_int( color );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_get_net_color_obj, jl_get_net_color_func );

// get_net_color_name(net_num) - Returns the color name as a string
static mp_obj_t jl_get_net_color_name_func( mp_obj_t net_num_obj ) {
    int net_num = mp_obj_get_int( net_num_obj );
    const char* name = jl_get_net_color_name( net_num );
    return mp_obj_new_str( name, strlen( name ) );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_get_net_color_name_obj, jl_get_net_color_name_func );

// set_net_color(net_num, color) - Sets color by name ("red") or hex string ("#FF0000")
// Can also accept RGB tuple or integer
static mp_obj_t jl_set_net_color_func( size_t n_args, const mp_obj_t* args ) {
    int net_num = mp_obj_get_int( args[ 0 ] );

    if ( n_args == 2 ) {
        // Single argument: string color name or hex, or integer
        if ( mp_obj_is_str( args[ 1 ] ) ) {
            const char* color_str = mp_obj_str_get_str( args[ 1 ] );
            int result = jl_set_net_color( net_num, color_str );
            return mp_obj_new_bool( result );
        } else if ( mp_obj_is_int( args[ 1 ] ) ) {
            // Integer RGB value
            uint32_t color = mp_obj_get_int( args[ 1 ] );
            int r = ( color >> 16 ) & 0xFF;
            int g = ( color >> 8 ) & 0xFF;
            int b = color & 0xFF;
            int result = jl_set_net_color_rgb( net_num, r, g, b );
            return mp_obj_new_bool( result );
        } else if ( mp_obj_is_type( args[ 1 ], &mp_type_tuple ) || mp_obj_is_type( args[ 1 ], &mp_type_list ) ) {
            // RGB tuple/list
            mp_obj_t* items;
            size_t len;
            mp_obj_get_array( args[ 1 ], &len, &items );
            if ( len >= 3 ) {
                int r = mp_obj_get_int( items[ 0 ] );
                int g = mp_obj_get_int( items[ 1 ] );
                int b = mp_obj_get_int( items[ 2 ] );
                int result = jl_set_net_color_rgb( net_num, r, g, b );
                return mp_obj_new_bool( result );
            }
        }
        mp_raise_ValueError( MP_ERROR_TEXT( "Color must be string name, hex integer, or (r,g,b) tuple" ) );
    } else if ( n_args == 4 ) {
        // Three RGB arguments: set_net_color(net, r, g, b)
        int r = mp_obj_get_int( args[ 1 ] );
        int g = mp_obj_get_int( args[ 2 ] );
        int b = mp_obj_get_int( args[ 3 ] );
        int result = jl_set_net_color_rgb( net_num, r, g, b );
        return mp_obj_new_bool( result );
    }

    mp_raise_ValueError( MP_ERROR_TEXT( "Invalid arguments for set_net_color" ) );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_set_net_color_obj, 2, 4, jl_set_net_color_func );

// set_net_color_hsv(net, h, [s], [v]) - Sets color by HSV values
// Auto-detects 0.0-1.0 vs 0-255 range based on h value
// s defaults to max (255) if not provided or < 0
// v defaults to 32 (reasonable brightness) if not provided or < 0
static mp_obj_t jl_set_net_color_hsv_func( size_t n_args, const mp_obj_t* args ) {
    int net_num = mp_obj_get_int( args[ 0 ] );
    float h = mp_obj_get_float( args[ 1 ] );

    // Default S to -1 (which means use max in C++ function)
    float s = ( n_args > 2 ) ? mp_obj_get_float( args[ 2 ] ) : -1.0f;

    // Default V to -1 (which means use max in C++ function)
    float v = ( n_args > 3 ) ? mp_obj_get_float( args[ 3 ] ) : -1.0f;

    int result = jl_set_net_color_hsv( net_num, h, s, v );
    return mp_obj_new_bool( result );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_set_net_color_hsv_obj, 2, 4, jl_set_net_color_hsv_func );

// get_num_nets() - Returns the number of active nets
static mp_obj_t jl_get_num_nets_func( void ) {
    return mp_obj_new_int( jl_get_num_nets( ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_get_num_nets_obj, jl_get_num_nets_func );

// get_num_bridges() - Returns the number of bridges
static mp_obj_t jl_get_num_bridges_func( void ) {
    return mp_obj_new_int( jl_get_num_bridges( ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_get_num_bridges_obj, jl_get_num_bridges_func );

// get_net_nodes(net_num) - Returns nodes in a net as a comma-separated string
static mp_obj_t jl_get_net_nodes_func( mp_obj_t net_num_obj ) {
    int net_num = mp_obj_get_int( net_num_obj );
    const char* nodes = jl_get_net_nodes( net_num );
    return mp_obj_new_str( nodes, strlen( nodes ) );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_get_net_nodes_obj, jl_get_net_nodes_func );

// get_bridge(index) - Returns bridge info as tuple (node1, node2, duplicates)
static mp_obj_t jl_get_bridge_func( mp_obj_t idx_obj ) {
    int idx = mp_obj_get_int( idx_obj );
    int node1, node2, duplicates;

    if ( !jl_get_bridge( idx, &node1, &node2, &duplicates ) ) {
        return mp_const_none;
    }

    mp_obj_t items[ 3 ] = {
        mp_obj_new_int( node1 ),
        mp_obj_new_int( node2 ),
        mp_obj_new_int( duplicates ) };
    return mp_obj_new_tuple( 3, items );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_get_bridge_obj, jl_get_bridge_func );

// get_net_info(net_num) - Returns dict with all net info
static mp_obj_t jl_get_net_info_func( mp_obj_t net_num_obj ) {
    int net_num = mp_obj_get_int( net_num_obj );

    const char* name = jl_get_net_name( net_num );
    if ( name == NULL ) {
        return mp_const_none;
    }

    mp_obj_t dict = mp_obj_new_dict( 5 );

    // Add name
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_name ),
                       mp_obj_new_str( name, strlen( name ) ) );

    // Add number
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_number ),
                       mp_obj_new_int( net_num ) );

    // Add color (hex)
    uint32_t color = jl_get_net_color( net_num );
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_color ),
                       mp_obj_new_int( color ) );

    // Add color_name
    const char* color_name = jl_get_net_color_name( net_num );
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_color_name ),
                       mp_obj_new_str( color_name, strlen( color_name ) ) );

    // Add nodes
    const char* nodes = jl_get_net_nodes( net_num );
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_nodes ),
                       mp_obj_new_str( nodes, strlen( nodes ) ) );

    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_get_net_info_obj, jl_get_net_info_func );

// get_all_nets() - Returns list of dicts for all active nets
static mp_obj_t mp_jl_get_all_nets( void ) {
    int num_nets = jl_get_num_nets( );
    mp_obj_t list = mp_obj_new_list( 0, NULL );
    for ( int i = 0; i < num_nets; i++ ) {
        mp_obj_list_append( list, jl_get_net_info_func( mp_obj_new_int( i ) ) );
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_get_all_nets_obj, mp_jl_get_all_nets );

// get_path_info(path_idx) - Returns path info as dict
static mp_obj_t jl_get_path_info_func( mp_obj_t idx_obj ) {
    int idx = mp_obj_get_int( idx_obj );
    const char* path_str = jl_get_path_info( idx );
    
    if ( strlen( path_str ) == 0 ) {
        return mp_const_none;
    }
    
    // Parse CSV: node1,node2,net,chip[4],x[6],y[6],duplicate
    int vals[ 20 ];
    int count = 0;
    const char* p = path_str;
    while ( *p && count < 20 ) {
        vals[ count++ ] = atoi( p );
        while ( *p && *p != ',' ) p++;
        if ( *p == ',' ) p++;
    }
    
    if ( count != 20 ) {
        return mp_const_none; // Parse error
    }
    
    // Create dict with parsed values
    mp_obj_t dict = mp_obj_new_dict( 7 );
    
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_node1 ), mp_obj_new_int( vals[ 0 ] ) );
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_node2 ), mp_obj_new_int( vals[ 1 ] ) );
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_net ), mp_obj_new_int( vals[ 2 ] ) );
    
    // Create chip array [4]
    mp_obj_t chips[ 4 ];
    for ( int i = 0; i < 4; i++ ) {
        chips[ i ] = mp_obj_new_int( vals[ 3 + i ] );
    }
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_chips ), mp_obj_new_list( 4, chips ) );
    
    // Create x array [6]
    mp_obj_t x_arr[ 6 ];
    for ( int i = 0; i < 6; i++ ) {
        x_arr[ i ] = mp_obj_new_int( vals[ 7 + i ] );
    }
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_x ), mp_obj_new_list( 6, x_arr ) );
    
    // Create y array [6]
    mp_obj_t y_arr[ 6 ];
    for ( int i = 0; i < 6; i++ ) {
        y_arr[ i ] = mp_obj_new_int( vals[ 13 + i ] );
    }
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_y ), mp_obj_new_list( 6, y_arr ) );
    
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_duplicate ), mp_obj_new_int( vals[ 19 ] ) );
    
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_get_path_info_obj, jl_get_path_info_func );

// get_num_paths(include_duplicates=True) - Returns the number of paths
// By default includes duplicates; pass False to count only non-duplicate ("primary") paths.
static mp_obj_t jl_get_num_paths_func( size_t n_args, const mp_obj_t* args ) {
    int include_duplicates = ( n_args > 0 ) ? ( mp_obj_is_true( args[ 0 ] ) ? 1 : 0 ) : 1; // Default True
    return mp_obj_new_int( jl_get_num_paths( include_duplicates ) );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_get_num_paths_obj, 0, 1, jl_get_num_paths_func );

// get_all_paths() - Returns list of all path dicts
static mp_obj_t jl_get_all_paths_func( void ) {
    const char* all_paths_str = jl_get_all_path_info( );
    
    // First line is count
    int num_paths = atoi( all_paths_str );
    if ( num_paths <= 0 ) {
        return mp_obj_new_list( 0, NULL );
    }
    
    // Create list to hold all path dicts
    mp_obj_t list = mp_obj_new_list( 0, NULL );
    
    // Parse each line
    const char* p = all_paths_str;
    // Skip first line (count)
    while ( *p && *p != '\n' ) p++;
    if ( *p == '\n' ) p++;
    
    for ( int i = 0; i < num_paths && *p; i++ ) {
        // Parse this line's CSV
        int vals[ 20 ];
        int count = 0;
        while ( *p && *p != '\n' && count < 20 ) {
            vals[ count++ ] = atoi( p );
            while ( *p && *p != ',' && *p != '\n' ) p++;
            if ( *p == ',' ) p++;
        }
        if ( *p == '\n' ) p++;
        
        if ( count == 20 ) {
            // Create dict for this path
            mp_obj_t dict = mp_obj_new_dict( 7 );
            
            mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_node1 ), mp_obj_new_int( vals[ 0 ] ) );
            mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_node2 ), mp_obj_new_int( vals[ 1 ] ) );
            mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_net ), mp_obj_new_int( vals[ 2 ] ) );
            
            // Create chip array
            mp_obj_t chips[ 4 ];
            for ( int j = 0; j < 4; j++ ) {
                chips[ j ] = mp_obj_new_int( vals[ 3 + j ] );
            }
            mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_chips ), mp_obj_new_list( 4, chips ) );
            
            // Create x array
            mp_obj_t x_arr[ 6 ];
            for ( int j = 0; j < 6; j++ ) {
                x_arr[ j ] = mp_obj_new_int( vals[ 7 + j ] );
            }
            mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_x ), mp_obj_new_list( 6, x_arr ) );
            
            // Create y array
            mp_obj_t y_arr[ 6 ];
            for ( int j = 0; j < 6; j++ ) {
                y_arr[ j ] = mp_obj_new_int( vals[ 13 + j ] );
            }
            mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_y ), mp_obj_new_list( 6, y_arr ) );
            
            mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_duplicate ), mp_obj_new_int( vals[ 19 ] ) );
            
            mp_obj_list_append( list, dict );
        }
    }
    
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_get_all_paths_obj, jl_get_all_paths_func );

// get_path_between(node1, node2) - Returns path dict or None if not found
static mp_obj_t jl_get_path_between_func( mp_obj_t node1_obj, mp_obj_t node2_obj ) {
    int node1 = mp_obj_get_int( node1_obj );
    int node2 = mp_obj_get_int( node2_obj );
    const char* path_str = jl_get_path_between( node1, node2 );
    
    if ( strlen( path_str ) == 0 ) {
        return mp_const_none;
    }
    
    // Parse CSV: same format as get_path_info
    int vals[ 20 ];
    int count = 0;
    const char* p = path_str;
    while ( *p && count < 20 ) {
        vals[ count++ ] = atoi( p );
        while ( *p && *p != ',' ) p++;
        if ( *p == ',' ) p++;
    }
    
    if ( count != 20 ) {
        return mp_const_none;
    }
    
    // Create dict
    mp_obj_t dict = mp_obj_new_dict( 7 );
    
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_node1 ), mp_obj_new_int( vals[ 0 ] ) );
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_node2 ), mp_obj_new_int( vals[ 1 ] ) );
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_net ), mp_obj_new_int( vals[ 2 ] ) );
    
    // Create chip array
    mp_obj_t chips[ 4 ];
    for ( int i = 0; i < 4; i++ ) {
        chips[ i ] = mp_obj_new_int( vals[ 3 + i ] );
    }
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_chips ), mp_obj_new_list( 4, chips ) );
    
    // Create x array
    mp_obj_t x_arr[ 6 ];
    for ( int i = 0; i < 6; i++ ) {
        x_arr[ i ] = mp_obj_new_int( vals[ 7 + i ] );
    }
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_x ), mp_obj_new_list( 6, x_arr ) );
    
    // Create y array
    mp_obj_t y_arr[ 6 ];
    for ( int i = 0; i < 6; i++ ) {
        y_arr[ i ] = mp_obj_new_int( vals[ 13 + i ] );
    }
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_y ), mp_obj_new_list( 6, y_arr ) );
    
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_duplicate ), mp_obj_new_int( vals[ 19 ] ) );
    
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_get_path_between_obj, jl_get_path_between_func );

// Net Current Scan API - values measured by the background net voltage scan
// (enabled by [display] net_currents / the 'i' serial command). All return
// None when the scan has no fresh data.

// get_node_voltage(node) - scanned voltage of a routed node in volts
static mp_obj_t jl_get_node_voltage_func( mp_obj_t node_obj ) {
    int node = get_node_value( node_obj );
    float voltage = 0.0f;
    if ( !jl_scan_node_voltage( node, &voltage ) ) {
        return mp_const_none;
    }
    return mp_obj_new_float( voltage );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_get_node_voltage_obj, jl_get_node_voltage_func );

// get_net_current(net_num) - dict {current_mA, voltage, from_node, to_node}
// for the net's dominant path; conventional current flows from -> to
static mp_obj_t jl_get_net_current_func( mp_obj_t net_num_obj ) {
    int net = mp_obj_get_int( net_num_obj );
    float current_mA = 0.0f;
    float voltage = 0.0f;
    int from_node = -1;
    int to_node = -1;
    if ( !jl_scan_net_current( net, &current_mA, &voltage, &from_node, &to_node ) ) {
        return mp_const_none;
    }
    mp_obj_t dict = mp_obj_new_dict( 4 );
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_current_mA ), mp_obj_new_float( current_mA ) );
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_voltage ), mp_obj_new_float( voltage ) );
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_from_node ), mp_obj_new_int( from_node ) );
    mp_obj_dict_store( dict, MP_OBJ_NEW_QSTR( MP_QSTR_to_node ), mp_obj_new_int( to_node ) );
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_get_net_current_obj, jl_get_net_current_func );

// get_path_current(path_idx) - signed mA through one routing path; positive
// means conventional current flows node1 -> node2 (same fields and index
// space as get_path_info)
static mp_obj_t jl_get_path_current_func( mp_obj_t idx_obj ) {
    int idx = mp_obj_get_int( idx_obj );
    float current_mA = 0.0f;
    if ( !jl_scan_path_current( idx, &current_mA ) ) {
        return mp_const_none;
    }
    return mp_obj_new_float( current_mA );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_get_path_current_obj, jl_get_path_current_func );

// Fast GPIO Toggle Functions
// fake_gpio_disconnect(node1, node2) - Context manager for temporary disconnection
typedef struct _mp_obj_fake_gpio_disconnect_t {
    mp_obj_base_t base;
    int node1;
    int node2;
} mp_obj_fake_gpio_disconnect_t;

static mp_obj_t fake_gpio_disconnect_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args ) {
    mp_arg_check_num( n_args, n_kw, 2, 2, false );

    mp_obj_fake_gpio_disconnect_t* self = m_new_obj( mp_obj_fake_gpio_disconnect_t );
    self->base.type = type;
    self->node1 = mp_obj_get_int( args[ 0 ] );
    self->node2 = mp_obj_get_int( args[ 1 ] );

    return MP_OBJ_FROM_PTR( self );
}

static mp_obj_t fake_gpio_disconnect_enter( mp_obj_t self_in ) {
    mp_obj_fake_gpio_disconnect_t* self = MP_OBJ_TO_PTR( self_in );

    if ( !jl_fake_gpio_disconnect( self->node1, self->node2 ) ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "Failed to disconnect path" ) );
    }

    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_1( fake_gpio_disconnect_enter_obj, fake_gpio_disconnect_enter );

static mp_obj_t fake_gpio_disconnect_exit( size_t n_args, const mp_obj_t* args ) {
    mp_obj_fake_gpio_disconnect_t* self = MP_OBJ_TO_PTR( args[ 0 ] );

    if ( !jl_fake_gpio_reconnect( self->node1, self->node2 ) ) {
        mp_raise_ValueError( MP_ERROR_TEXT( "Failed to reconnect path" ) );
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( fake_gpio_disconnect_exit_obj, 4, 4, fake_gpio_disconnect_exit );

static const mp_rom_map_elem_t fake_gpio_disconnect_locals_dict_table[] = {
    { MP_ROM_QSTR( MP_QSTR___enter__ ), MP_ROM_PTR( &fake_gpio_disconnect_enter_obj ) },
    { MP_ROM_QSTR( MP_QSTR___exit__ ), MP_ROM_PTR( &fake_gpio_disconnect_exit_obj ) },
};
static MP_DEFINE_CONST_DICT( fake_gpio_disconnect_locals_dict, fake_gpio_disconnect_locals_dict_table );

MP_DEFINE_CONST_OBJ_TYPE(
    fake_gpio_disconnect_type,
    MP_QSTR_FakeGpioDisconnect,
    MP_TYPE_FLAG_NONE,
    make_new, fake_gpio_disconnect_make_new,
    locals_dict, &fake_gpio_disconnect_locals_dict
    );

// FakeGpioPin class - machine.Pin compatible fake GPIO
// 
// Usage Examples:
//   # Simple 5V/0V digital output (most common case)
//   pin = j.FakeGpioPin(20)  # Defaults to OUTPUT mode, 5V HIGH, 0V LOW
//   pin.on()   # Set HIGH (5V)
//   pin.off()  # Set LOW (0V)
//
//   # Explicit mode specification
//   pin = j.FakeGpioPin(20, j.OUTPUT)
//
//   # Custom voltage levels (e.g., ±8V using DACs)
//   pin = j.FakeGpioPin(20, j.OUTPUT, v_high=8.0, v_low=-8.0)
//
//   # Input mode (for reading voltages - future feature)
//   pin = j.FakeGpioPin(20, j.INPUT, threshold_high=2.0, threshold_low=0.8)
//
// Note: Configuration automatically:
//   - Clears any existing connections to the pin
//   - Intelligently allocates voltage sources (TOP_RAIL, GND, or DACs)
//   - Errors if no DAC is available for custom voltages (prevents damage)
//   - Uses fast chip K switching for high-speed toggling
//
typedef struct _mp_obj_fake_gpio_pin_t {
    mp_obj_base_t base;
    int node;
} mp_obj_fake_gpio_pin_t;

// Mode constants
#define FAKE_GPIO_MODE_INPUT  0
#define FAKE_GPIO_MODE_OUTPUT 1

// Voltage source node constants (MicroPython exposed values)
// These are passed directly to the C++ config function which handles conversion
#define MP_NODE_GND          100
#define MP_NODE_TOP_RAIL     101
#define MP_NODE_BOTTOM_RAIL  102
#define MP_NODE_DAC0         106
#define MP_NODE_DAC1         107

// Helper to check if a value is a valid voltage source node constant
static bool is_voltage_source_node(int value) {
    return (value == MP_NODE_GND || 
            value == MP_NODE_TOP_RAIL || 
            value == MP_NODE_BOTTOM_RAIL || 
            value == MP_NODE_DAC0 || 
            value == MP_NODE_DAC1);
}

static mp_obj_t fake_gpio_pin_make_new( const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args ) {
    mp_arg_check_num( n_args, n_kw, 1, 6, false );

    mp_obj_fake_gpio_pin_t* self = m_new_obj( mp_obj_fake_gpio_pin_t );
    self->base.type = type;
    self->node = mp_obj_get_int( args[ 0 ] );
    
    // Mode defaults to OUTPUT (most common case)
    int mode = ( n_args > 1 ) ? mp_obj_get_int( args[ 1 ] ) : FAKE_GPIO_MODE_OUTPUT;

    // Threshold defaults
    float threshold_high = ( n_args > 4 ) ? mp_obj_get_float( args[ 4 ] ) : 2.0;
    float threshold_low = ( n_args > 5 ) ? mp_obj_get_float( args[ 5 ] ) : 0.8;

    if ( mode == FAKE_GPIO_MODE_INPUT ) {
        // INPUT mode: args[2] = threshold_high, args[3] = threshold_low
        if ( n_args > 2 ) threshold_high = mp_obj_get_float( args[ 2 ] );
        if ( n_args > 3 ) threshold_low = mp_obj_get_float( args[ 3 ] );
        
        if ( jl_fake_gpio_config_input( self->node, threshold_high, threshold_low ) < 0 ) {
            mp_raise_ValueError( MP_ERROR_TEXT( "Failed to configure fake GPIO INPUT pin" ) );
        }
    } else {
        // OUTPUT mode: Detect if using node-based or voltage-based configuration
        // Node-based: args[2] and args[3] are MicroPython node constants (100-102, 106-107)
        // Voltage-based: args[2] and args[3] are voltage values
        
        bool use_node_based = false;
        int high_node = MP_NODE_TOP_RAIL;  // Default to TOP_RAIL
        int low_node = MP_NODE_GND;         // Default to GND
        float v_high = 5.0;                 // Fallback voltage defaults
        float v_low = 0.0;
        
        if ( n_args > 2 ) {
            // Check if arg[2] is a voltage source node constant
            int arg2_int = mp_obj_get_int( args[ 2 ] );
            
            if ( is_voltage_source_node( arg2_int ) ) {
                // Valid node constant - use node-based mode
                use_node_based = true;
                high_node = arg2_int;  // Pass through as-is, C++ will convert
            } else {
                // Not a node constant - treat as voltage value
                v_high = mp_obj_get_float( args[ 2 ] );
            }
        }
        
        if ( n_args > 3 ) {
            if ( use_node_based ) {
                int arg3_int = mp_obj_get_int( args[ 3 ] );
                if ( is_voltage_source_node( arg3_int ) ) {
                    low_node = arg3_int;  // Pass through as-is, C++ will convert
                } else {
                    mp_raise_ValueError( MP_ERROR_TEXT( "Invalid low_node - must be TOP_RAIL/BOTTOM_RAIL/DAC0/DAC1/GND" ) );
                }
            } else {
                v_low = mp_obj_get_float( args[ 3 ] );
            }
        }
        
        // Configure using appropriate method
        // Node values are passed directly - C++ config function handles conversion
        int result;
        if ( use_node_based ) {
            result = jl_fake_gpio_config_output_nodes( self->node, high_node, low_node, threshold_high, threshold_low );
        } else {
            result = jl_fake_gpio_config_output( self->node, v_high, v_low, threshold_high, threshold_low );
        }
        
        if ( result < 0 ) {
            mp_raise_ValueError( MP_ERROR_TEXT( "Failed to configure fake GPIO OUTPUT pin" ) );
        }
    }

    // Set mode (already stored by config, but this is for consistency)
    jl_fake_gpio_set_mode( self->node, mode );

    return MP_OBJ_FROM_PTR( self );
}

// value([val]) - Get or set pin value
static mp_obj_t fake_gpio_pin_value( size_t n_args, const mp_obj_t* args ) {
    mp_obj_fake_gpio_pin_t* self = MP_OBJ_TO_PTR( args[ 0 ] );

    if ( n_args == 1 ) {
        // Read value
        int val = jl_fake_gpio_read( self->node );
        return mp_obj_new_int( val );
    } else {
        // Write value
        int val = mp_obj_get_int( args[ 1 ] );
        jl_fake_gpio_write( self->node, val );
        return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( fake_gpio_pin_value_obj, 1, 2, fake_gpio_pin_value );

// on() - Set pin HIGH
static mp_obj_t fake_gpio_pin_on( mp_obj_t self_in ) {
    mp_obj_fake_gpio_pin_t* self = MP_OBJ_TO_PTR( self_in );
    jl_fake_gpio_write( self->node, 1 );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( fake_gpio_pin_on_obj, fake_gpio_pin_on );

// off() - Set pin LOW
static mp_obj_t fake_gpio_pin_off( mp_obj_t self_in ) {
    mp_obj_fake_gpio_pin_t* self = MP_OBJ_TO_PTR( self_in );
    jl_fake_gpio_write( self->node, 0 );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( fake_gpio_pin_off_obj, fake_gpio_pin_off );

// toggle() - Toggle pin state
static mp_obj_t fake_gpio_pin_toggle( mp_obj_t self_in ) {
    mp_obj_fake_gpio_pin_t* self = MP_OBJ_TO_PTR( self_in );
    int current = jl_fake_gpio_read( self->node );
    jl_fake_gpio_write( self->node, !current );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( fake_gpio_pin_toggle_obj, fake_gpio_pin_toggle );

static const mp_rom_map_elem_t fake_gpio_pin_locals_dict_table[] = {
    { MP_ROM_QSTR( MP_QSTR_value ), MP_ROM_PTR( &fake_gpio_pin_value_obj ) },
    { MP_ROM_QSTR( MP_QSTR_on ), MP_ROM_PTR( &fake_gpio_pin_on_obj ) },
    { MP_ROM_QSTR( MP_QSTR_off ), MP_ROM_PTR( &fake_gpio_pin_off_obj ) },
    { MP_ROM_QSTR( MP_QSTR_toggle ), MP_ROM_PTR( &fake_gpio_pin_toggle_obj ) },
};
static MP_DEFINE_CONST_DICT( fake_gpio_pin_locals_dict, fake_gpio_pin_locals_dict_table );

MP_DEFINE_CONST_OBJ_TYPE(
    fake_gpio_pin_type,
    MP_QSTR_FakeGpioPin,
    MP_TYPE_FLAG_NONE,
    make_new, fake_gpio_pin_make_new,
    locals_dict, &fake_gpio_pin_locals_dict
    );

// OLED Functions
// Supports print()-style usage:
//   oled_print(*values, sep=' ', end='', size=-1)
// Backwards compatibility:
//   oled_print(text, size)
static mp_obj_t jl_oled_print_func( size_t n_args, const mp_obj_t* pos_args, mp_map_t* kw_args ) {
    enum {
        ARG_sep,
        ARG_end,
        ARG_size
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sep, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_end, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_size, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = -1 } },
    };

    mp_arg_val_t args[ MP_ARRAY_SIZE( allowed_args ) ];
    // Parse keyword-only args from kwargs map only; positional args are print values.
    mp_arg_parse_all( 0, NULL, kw_args, MP_ARRAY_SIZE( allowed_args ), allowed_args, args );

    const char* sep = " ";
    if ( args[ ARG_sep ].u_obj != mp_const_none ) {
        if ( !mp_obj_is_str( args[ ARG_sep ].u_obj ) ) {
            mp_raise_TypeError( MP_ERROR_TEXT( "sep must be str or None" ) );
        }
        sep = mp_obj_str_get_str( args[ ARG_sep ].u_obj );
    }

    // Keep historical oled_print behavior (no implicit newline) unless user requests one.
    const char* end = "";
    if ( args[ ARG_end ].u_obj != mp_const_none ) {
        if ( !mp_obj_is_str( args[ ARG_end ].u_obj ) ) {
            mp_raise_TypeError( MP_ERROR_TEXT( "end must be str or None" ) );
        }
        end = mp_obj_str_get_str( args[ ARG_end ].u_obj );
    }

    int size = args[ ARG_size ].u_int;
    size_t value_count = n_args;

    // Backwards compatibility with oled_print(text, size)
    if ( kw_args->used == 0 && n_args == 2 && mp_obj_is_int( pos_args[ 1 ] ) ) {
        size = mp_obj_get_int( pos_args[ 1 ] );
        value_count = 1;
    }

    vstr_t output;
    vstr_init( &output, 64 );

    for ( size_t i = 0; i < value_count; i++ ) {
        mp_print_t print;
        vstr_t item;
        vstr_init_print( &item, 16, &print );
        mp_obj_print_helper( &print, pos_args[ i ], PRINT_STR );
        vstr_add_strn( &output, item.buf, item.len );
        vstr_clear( &item );

        if ( i + 1 < value_count ) {
            vstr_add_str( &output, sep );
        }
    }

    vstr_add_str( &output, end );
    jl_oled_print( output.buf, size );
    vstr_clear( &output );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW( jl_oled_print_obj, 0, jl_oled_print_func );

static mp_obj_t jl_oled_clear_func( size_t n_args, const mp_obj_t* args ) {
    // Default show=True if not provided
    int show = 1;
    if ( n_args >= 1 ) {
        show = mp_obj_is_true( args[ 0 ] ) ? 1 : 0;
    }
    jl_oled_clear( show );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_oled_clear_obj, 0, 1, jl_oled_clear_func );

static mp_obj_t jl_oled_show_func( void ) {
    jl_oled_show( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_oled_show_obj, jl_oled_show_func );

static mp_obj_t jl_oled_connect_func( void ) {
    int result = jl_oled_connect( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_oled_connect_obj, jl_oled_connect_func );

static mp_obj_t jl_oled_disconnect_func( void ) {
    jl_oled_disconnect( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_oled_disconnect_obj, jl_oled_disconnect_func );

// OLED Text Size Control
static mp_obj_t jl_oled_set_text_size_func( mp_obj_t size_obj ) {
    int size = mp_obj_get_int( size_obj );
    int result = jl_oled_set_text_size( size );
    return mp_obj_new_bool( result );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_oled_set_text_size_obj, jl_oled_set_text_size_func );

static mp_obj_t jl_oled_get_text_size_func( void ) {
    return mp_obj_new_int( jl_oled_get_text_size( ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_oled_get_text_size_obj, jl_oled_get_text_size_func );

// OLED Print Redirection
static mp_obj_t jl_oled_copy_print_func( mp_obj_t enable_obj ) {
    int enable = mp_obj_is_true( enable_obj ) ? 1 : 0;
    jl_oled_copy_print( enable );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_oled_copy_print_obj, jl_oled_copy_print_func );

// OLED Font System
static mp_obj_t jl_oled_get_fonts_func( void ) {
    int count = 0;
    const char* fonts = jl_oled_get_fonts( &count );
    
    // Parse comma-separated string into list
    mp_obj_t font_list = mp_obj_new_list( 0, NULL );
    
    const char* start = fonts;
    const char* end = fonts;
    
    while ( *end != '\0' ) {
        if ( *end == ',' ) {
            // Extract font name
            size_t len = end - start;
            mp_obj_t font_name = mp_obj_new_str( start, len );
            mp_obj_list_append( font_list, font_name );
            start = end + 1;
        }
        end++;
    }
    
    // Add last font name
    if ( start < end ) {
        size_t len = end - start;
        mp_obj_t font_name = mp_obj_new_str( start, len );
        mp_obj_list_append( font_list, font_name );
    }
    
    return font_list;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_oled_get_fonts_obj, jl_oled_get_fonts_func );

static mp_obj_t jl_oled_set_font_func( mp_obj_t font_obj ) {
    const char* font_name = mp_obj_str_get_str( font_obj );
    int result = jl_oled_set_font( font_name );
    return mp_obj_new_bool( result > 0 );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_oled_set_font_obj, jl_oled_set_font_func );

static mp_obj_t jl_oled_get_current_font_func( void ) {
    const char* font_name = jl_oled_get_current_font( );
    return mp_obj_new_str( font_name, strlen( font_name ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_oled_get_current_font_obj, jl_oled_get_current_font_func );

// OLED Bitmap Functions
static mp_obj_t jl_oled_load_bitmap_func( mp_obj_t filepath_obj ) {
    const char* filepath = mp_obj_str_get_str( filepath_obj );
    int result = jl_oled_load_bitmap( filepath );
    return mp_obj_new_bool( result );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_oled_load_bitmap_obj, jl_oled_load_bitmap_func );

static mp_obj_t jl_oled_display_bitmap_func( size_t n_args, const mp_obj_t* args ) {
    int x = mp_obj_get_int( args[ 0 ] );
    int y = mp_obj_get_int( args[ 1 ] );
    int width = mp_obj_get_int( args[ 2 ] );
    int height = mp_obj_get_int( args[ 3 ] );
    
    const uint8_t* data = NULL;
    size_t data_len = 0;
    
    if ( n_args >= 5 && args[ 4 ] != mp_const_none ) {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise( args[ 4 ], &bufinfo, MP_BUFFER_READ );
        data = (const uint8_t*)bufinfo.buf;
        data_len = bufinfo.len;
    }
    
    int result = jl_oled_display_bitmap( x, y, width, height, data, data_len );
    return mp_obj_new_bool( result );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_oled_display_bitmap_obj, 4, 5, jl_oled_display_bitmap_func );

static mp_obj_t jl_oled_show_bitmap_file_func( size_t n_args, const mp_obj_t* args ) {
    const char* filepath = mp_obj_str_get_str( args[ 0 ] );
    int x = mp_obj_get_int( args[ 1 ] );
    int y = mp_obj_get_int( args[ 2 ] );
    
    int result = jl_oled_show_bitmap_file( filepath, x, y );
    return mp_obj_new_bool( result );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_oled_show_bitmap_file_obj, 3, 3, jl_oled_show_bitmap_file_func );

// OLED Framebuffer Access
static mp_obj_t jl_oled_get_framebuffer_func( void ) {
    int width, height, buffer_size;
    const uint8_t* buffer = jl_oled_get_framebuffer( &width, &height, &buffer_size );
    
    if ( buffer == NULL ) {
        return mp_const_none;
    }
    
    return mp_obj_new_bytes( buffer, buffer_size );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_oled_get_framebuffer_obj, jl_oled_get_framebuffer_func );

static mp_obj_t jl_oled_set_framebuffer_func( mp_obj_t data_obj ) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise( data_obj, &bufinfo, MP_BUFFER_READ );
    
    int result = jl_oled_set_framebuffer( (const uint8_t*)bufinfo.buf, bufinfo.len );
    return mp_obj_new_bool( result );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_oled_set_framebuffer_obj, jl_oled_set_framebuffer_func );

static mp_obj_t jl_oled_get_framebuffer_size_func( void ) {
    int width, height, buffer_bytes;
    jl_oled_get_framebuffer_size( &width, &height, &buffer_bytes );
    
    mp_obj_t tuple[ 3 ];
    tuple[ 0 ] = mp_obj_new_int( width );
    tuple[ 1 ] = mp_obj_new_int( height );
    tuple[ 2 ] = mp_obj_new_int( buffer_bytes );
    
    return mp_obj_new_tuple( 3, tuple );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_oled_get_framebuffer_size_obj, jl_oled_get_framebuffer_size_func );

static mp_obj_t jl_oled_set_pixel_func( size_t n_args, const mp_obj_t* args ) {
    int x = mp_obj_get_int( args[ 0 ] );
    int y = mp_obj_get_int( args[ 1 ] );
    int color = mp_obj_get_int( args[ 2 ] );
    
    int result = jl_oled_set_pixel( x, y, color );
    return mp_obj_new_bool( result );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_oled_set_pixel_obj, 3, 3, jl_oled_set_pixel_func );

static mp_obj_t jl_oled_get_pixel_func( size_t n_args, const mp_obj_t* args ) {
    int x = mp_obj_get_int( args[ 0 ] );
    int y = mp_obj_get_int( args[ 1 ] );
    
    int result = jl_oled_get_pixel( x, y );
    return mp_obj_new_int( result );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_oled_get_pixel_obj, 2, 2, jl_oled_get_pixel_func );

// ---------------------------------------------------------------------------
// OLED GUI (retained screens)
// Flat handle-based API; the oledgui.py wrapper builds Screen/Text/Shape on top.
//   s = oled_screen()                          -> screen handle (int)
//   e = oled_add_text(s, "{adc:0} V", x=0, y=0, font="Pragmatism", size=12)
//   e = oled_add_shape(s, kind, x=, y=, w=, h=, filled=, z=)
//   oled_set(e, "text", "...")  /  oled_set(e, "x", 10)
//   oled_set_var("name", value)                -> push a live value
//   oled_screen_show(s) / oled_screen_hide()
//   oled_screen_save(s, "name") / s = oled_screen_load("name")
// ---------------------------------------------------------------------------

static mp_obj_t jl_oled_screen_new_func( void ) {
    return mp_obj_new_int( jl_oled_screen_new( ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_oled_screen_new_obj, jl_oled_screen_new_func );

static mp_obj_t jl_oled_screen_free_func( mp_obj_t screen_obj ) {
    jl_oled_screen_free( mp_obj_get_int( screen_obj ) );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_oled_screen_free_obj, jl_oled_screen_free_func );

static mp_obj_t jl_oled_screen_clear_func( mp_obj_t screen_obj ) {
    jl_oled_screen_clear( mp_obj_get_int( screen_obj ) );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_oled_screen_clear_obj, jl_oled_screen_clear_func );

// oled_screen_show(screen, persist=False) -> bool
// persist registers the screen as the idle display (takes the logo's place and
// survives the script). Defaults to a one-shot foreground show.
static mp_obj_t jl_oled_screen_show_func( size_t n_args, const mp_obj_t* args ) {
    int persist = ( n_args > 1 ) ? mp_obj_is_true( args[1] ) : 0;
    return mp_obj_new_bool( jl_oled_screen_show( mp_obj_get_int( args[0] ), persist ) );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_oled_screen_show_obj, 1, 2, jl_oled_screen_show_func );

static mp_obj_t jl_oled_screen_hide_func( void ) {
    jl_oled_screen_hide( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_oled_screen_hide_obj, jl_oled_screen_hide_func );

static mp_obj_t jl_oled_screen_reset_func( void ) {
    jl_oled_screen_reset( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_oled_screen_reset_obj, jl_oled_screen_reset_func );

static mp_obj_t jl_oled_add_text_func( size_t n_args, const mp_obj_t* pos_args, mp_map_t* kw_args ) {
    enum { ARG_screen, ARG_text, ARG_x, ARG_y, ARG_font, ARG_size, ARG_halign, ARG_valign, ARG_z };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_screen, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_text,   MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_x,      MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_y,      MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_font,   MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_size,   MP_ARG_INT, { .u_int = 8 } },
        { MP_QSTR_halign, MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_valign, MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_z,      MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t args[ MP_ARRAY_SIZE( allowed_args ) ];
    mp_arg_parse_all( n_args, pos_args, kw_args, MP_ARRAY_SIZE( allowed_args ), allowed_args, args );

    int screen = args[ ARG_screen ].u_int;
    const char* text = mp_obj_str_get_str( args[ ARG_text ].u_obj );
    const char* font = "Pragmatism";
    if ( args[ ARG_font ].u_obj != mp_const_none ) {
        font = mp_obj_str_get_str( args[ ARG_font ].u_obj );
    }
    int elem = jl_oled_add_text( screen, text, args[ ARG_x ].u_int, args[ ARG_y ].u_int,
                                 font, args[ ARG_size ].u_int, args[ ARG_halign ].u_int,
                                 args[ ARG_valign ].u_int, args[ ARG_z ].u_int );
    return mp_obj_new_int( elem );
}
static MP_DEFINE_CONST_FUN_OBJ_KW( jl_oled_add_text_obj, 2, jl_oled_add_text_func );

static mp_obj_t jl_oled_add_shape_func( size_t n_args, const mp_obj_t* pos_args, mp_map_t* kw_args ) {
    enum { ARG_screen, ARG_kind, ARG_x, ARG_y, ARG_w, ARG_h, ARG_filled, ARG_z };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_screen, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_kind,   MP_ARG_INT, { .u_int = 1 } },   // default RECT outline
        { MP_QSTR_x,      MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_y,      MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_w,      MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_h,      MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_filled, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_z,      MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t args[ MP_ARRAY_SIZE( allowed_args ) ];
    mp_arg_parse_all( n_args, pos_args, kw_args, MP_ARRAY_SIZE( allowed_args ), allowed_args, args );

    int elem = jl_oled_add_shape( args[ ARG_screen ].u_int, args[ ARG_kind ].u_int,
                                  args[ ARG_x ].u_int, args[ ARG_y ].u_int,
                                  args[ ARG_w ].u_int, args[ ARG_h ].u_int,
                                  args[ ARG_filled ].u_int, args[ ARG_z ].u_int );
    return mp_obj_new_int( elem );
}
static MP_DEFINE_CONST_FUN_OBJ_KW( jl_oled_add_shape_obj, 1, jl_oled_add_shape_func );

// oled_set(elem, prop, value) - value may be a string or an integer.
static mp_obj_t jl_oled_set_func( mp_obj_t elem_obj, mp_obj_t prop_obj, mp_obj_t value_obj ) {
    int elem = mp_obj_get_int( elem_obj );
    const char* prop = mp_obj_str_get_str( prop_obj );
    int ok;
    if ( mp_obj_is_str( value_obj ) ) {
        ok = jl_oled_elem_set_str( elem, prop, mp_obj_str_get_str( value_obj ) );
    } else {
        ok = jl_oled_elem_set_int( elem, prop, mp_obj_get_int( value_obj ) );
    }
    return mp_obj_new_bool( ok );
}
static MP_DEFINE_CONST_FUN_OBJ_3( jl_oled_set_obj, jl_oled_set_func );

// oled_set_var(name, value) - push a live value; str stored verbatim, numbers formatted.
static mp_obj_t jl_oled_set_var_func( mp_obj_t name_obj, mp_obj_t value_obj ) {
    const char* name = mp_obj_str_get_str( name_obj );
    if ( mp_obj_is_str( value_obj ) ) {
        jl_oled_set_var( name, mp_obj_str_get_str( value_obj ) );
    } else {
        jl_oled_set_var_num( name, (float)mp_obj_get_float( value_obj ) );
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_oled_set_var_obj, jl_oled_set_var_func );

static mp_obj_t jl_oled_screen_save_func( mp_obj_t screen_obj, mp_obj_t name_obj ) {
    return mp_obj_new_bool( jl_oled_screen_save( mp_obj_get_int( screen_obj ),
                                                 mp_obj_str_get_str( name_obj ) ) );
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_oled_screen_save_obj, jl_oled_screen_save_func );

static mp_obj_t jl_oled_screen_load_func( mp_obj_t name_obj ) {
    return mp_obj_new_int( jl_oled_screen_load( mp_obj_str_get_str( name_obj ) ) );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_oled_screen_load_obj, jl_oled_screen_load_func );

// Arduino Functions
static mp_obj_t jl_arduino_reset_func( void ) {
    jl_arduino_reset( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_arduino_reset_obj, jl_arduino_reset_func );

// Core2 Functions
static mp_obj_t jl_pause_core2_func( mp_obj_t pause_obj ) {
    bool pause;

    // Handle different input types: bool, int, or string
    if ( mp_obj_is_bool( pause_obj ) ) {
        pause = mp_obj_is_true( pause_obj );
    } else if ( mp_obj_is_int( pause_obj ) ) {
        pause = mp_obj_get_int( pause_obj ) != 0;
    } else if ( mp_obj_is_str( pause_obj ) ) {
        const char* str = mp_obj_str_get_str( pause_obj );
        // Accept "true", "1", "on", "yes" as true, everything else as false
        pause = ( strcmp( str, "true" ) == 0 || strcmp( str, "1" ) == 0 ||
                  strcmp( str, "on" ) == 0 || strcmp( str, "yes" ) == 0 );
    } else {
        mp_raise_TypeError( "pause_core2() argument must be bool, int, or string" );
    }

    jl_pause_core2( pause );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_pause_core2_obj, jl_pause_core2_func );

// Terminal Color Functions
static mp_obj_t jl_change_terminal_color_func( size_t n_args, const mp_obj_t* args ) {
    int color = -1;
    bool flush = true;

    if ( n_args >= 1 ) {
        color = mp_obj_get_int( args[ 0 ] );
    }
    if ( n_args >= 2 ) {
        flush = mp_obj_is_true( args[ 1 ] );
    }

    jl_change_terminal_color( color, flush );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_change_terminal_color_obj, 0, 2, jl_change_terminal_color_func );

static mp_obj_t jl_cycle_term_color_func( size_t n_args, const mp_obj_t* args ) {
    bool reset = false;
    float step = 100.0;
    bool flush = true;

    if ( n_args >= 1 ) {
        reset = mp_obj_is_true( args[ 0 ] );
    }
    if ( n_args >= 2 ) {
        step = mp_obj_get_float( args[ 1 ] );
    }
    if ( n_args >= 3 ) {
        flush = mp_obj_is_true( args[ 2 ] );
    }

    jl_cycle_term_color( reset, step, flush );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_cycle_term_color_obj, 0, 3, jl_cycle_term_color_func );

// Status Functions

static mp_obj_t jl_nodes_print_bridges_func( void ) {
    jl_nodes_print_bridges( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_nodes_print_bridges_obj, jl_nodes_print_bridges_func );

static mp_obj_t jl_nodes_print_paths_func( void ) {
    jl_nodes_print_paths( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_nodes_print_paths_obj, jl_nodes_print_paths_func );

static mp_obj_t jl_nodes_print_crossbars_func( void ) {
    jl_nodes_print_crossbars( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_nodes_print_crossbars_obj, jl_nodes_print_crossbars_func );

static mp_obj_t jl_nodes_print_nets_func( void ) {
    jl_nodes_print_nets( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_nodes_print_nets_obj, jl_nodes_print_nets_func );

static mp_obj_t jl_nodes_print_chip_status_func( void ) {
    jl_nodes_print_chip_status( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_nodes_print_chip_status_obj, jl_nodes_print_chip_status_func );

static mp_obj_t jl_run_app_func( mp_obj_t appName_obj ) {
    const char* appName = mp_obj_str_get_str( appName_obj );
    jl_run_app( (char*)appName ); // Cast to remove const qualifier
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_run_app_obj, jl_run_app_func );

// Format output function removed - GPIO functions now always return formatted strings

// Probe Functions
static mp_obj_t jl_probe_tap_func( mp_obj_t node_obj ) {
    int node = get_node_value( node_obj );
    jl_probe_tap( node );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_probe_tap_obj, jl_probe_tap_func );

static mp_obj_t jl_probe_read_blocking_func( void ) {
    int pad = jl_probe_read_blocking( );

    // Check for interrupt signal (-999)
    if ( pad == -999 ) {
        mp_raise_msg( &mp_type_KeyboardInterrupt, "Ctrl+Q" );
    }

    return probe_pad_new( pad );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_probe_read_blocking_obj, jl_probe_read_blocking_func );

static mp_obj_t jl_probe_read_nonblocking_func( void ) {
    int pad = jl_probe_read_nonblocking( );
    return probe_pad_new( pad );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_probe_read_nonblocking_obj, jl_probe_read_nonblocking_func );

static mp_obj_t jl_probe_button_blocking_func( size_t n_args, const mp_obj_t* args ) {
    int consume = ( n_args > 0 ) ? mp_obj_is_true( args[ 0 ] ) : 0; // Default to not consuming (hold works)
    int button_state = jl_probe_button_blocking( consume );

    // Check for interrupt signal (-999)
    if ( button_state == -999 ) {
        mp_raise_msg( &mp_type_KeyboardInterrupt, "Ctrl+Q" );
    }

    return probe_button_new( button_state );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_probe_button_blocking_obj, 0, 1, jl_probe_button_blocking_func );

static mp_obj_t jl_probe_button_nonblocking_func( size_t n_args, const mp_obj_t* args ) {
    int consume = ( n_args > 0 ) ? mp_obj_is_true( args[ 0 ] ) : 0; // Default to not consuming (hold works)
    int button_state = jl_probe_button_nonblocking( consume );
    return probe_button_new( button_state );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_probe_button_nonblocking_obj, 0, 1, jl_probe_button_nonblocking_func );

// Probe aliases
static mp_obj_t jl_probe_read_func( void ) {
    return jl_probe_read_blocking_func( );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_probe_read_obj, jl_probe_read_func );

static mp_obj_t jl_read_probe_func( void ) {
    return jl_probe_read_blocking_func( );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_read_probe_obj, jl_read_probe_func );

// Parameterized probe_read function with blocking parameter
static mp_obj_t jl_probe_read_param_func( size_t n_args, const mp_obj_t* args ) {
    bool blocking = true; // Default to blocking
    if ( n_args > 0 ) {
        blocking = mp_obj_is_true( args[ 0 ] );
    }

    if ( blocking ) {
        return jl_probe_read_blocking_func( );
    } else {
        return jl_probe_read_nonblocking_func( );
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_probe_read_param_obj, 0, 1, jl_probe_read_param_func );

// Parameterized read_probe function with blocking parameter
static mp_obj_t jl_read_probe_param_func( size_t n_args, const mp_obj_t* args ) {
    bool blocking = true; // Default to blocking
    if ( n_args > 0 ) {
        blocking = mp_obj_is_true( args[ 0 ] );
    }

    if ( blocking ) {
        return jl_probe_read_blocking_func( );
    } else {
        return jl_probe_read_nonblocking_func( );
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_read_probe_param_obj, 0, 1, jl_read_probe_param_func );

// Additional probe_read_blocking aliases
static mp_obj_t jl_probe_wait_func( void ) {
    return jl_probe_read_blocking_func( );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_probe_wait_obj, jl_probe_wait_func );

static mp_obj_t jl_wait_probe_func( void ) {
    return jl_probe_read_blocking_func( );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_wait_probe_obj, jl_wait_probe_func );

static mp_obj_t jl_probe_touch_func( void ) {
    return jl_probe_read_blocking_func( );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_probe_touch_obj, jl_probe_touch_func );

static mp_obj_t jl_wait_touch_func( void ) {
    return jl_probe_read_blocking_func( );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_wait_touch_obj, jl_wait_touch_func );

// Probe button aliases (blocking by default for simplicity, non-consuming by default)
static mp_obj_t jl_get_button_func( void ) {
    mp_obj_t consume_arg = mp_obj_new_int( 0 ); // Default: don't consume
    return jl_probe_button_blocking_func( 1, &consume_arg );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_get_button_obj, jl_get_button_func );

static mp_obj_t jl_button_read_func( void ) {
    mp_obj_t consume_arg = mp_obj_new_int( 0 ); // Default: don't consume
    return jl_probe_button_blocking_func( 1, &consume_arg );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_button_read_obj, jl_button_read_func );

static mp_obj_t jl_read_button_func( void ) {
    mp_obj_t consume_arg = mp_obj_new_int( 0 ); // Default: don't consume
    return jl_probe_button_blocking_func( 1, &consume_arg );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_read_button_obj, jl_read_button_func );

static mp_obj_t jl_probe_button_func( void ) {
    mp_obj_t consume_arg = mp_obj_new_int( 0 ); // Default: don't consume
    return jl_probe_button_blocking_func( 1, &consume_arg );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_probe_button_obj, jl_probe_button_func );

// Non-blocking button aliases (non-consuming by default)
static mp_obj_t jl_check_button_func( void ) {
    mp_obj_t consume_arg = mp_obj_new_int( 0 ); // Default: don't consume
    return jl_probe_button_nonblocking_func( 1, &consume_arg );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_check_button_obj, jl_check_button_func );

static mp_obj_t jl_button_check_func( void ) {
    mp_obj_t consume_arg = mp_obj_new_int( 0 ); // Default: don't consume
    return jl_probe_button_nonblocking_func( 1, &consume_arg );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_button_check_obj, jl_button_check_func );

// Parameterized button functions with blocking parameter
static mp_obj_t jl_probe_button_param_func( size_t n_args, const mp_obj_t* args ) {
    bool blocking = true; // Default to blocking
    int consume = 0;      // Default to not consuming (hold works)

    if ( n_args > 0 ) {
        blocking = mp_obj_is_true( args[ 0 ] );
    }
    if ( n_args > 1 ) {
        consume = mp_obj_is_true( args[ 1 ] ) ? 1 : 0;
    }

    mp_obj_t consume_arg = mp_obj_new_int( consume );
    if ( blocking ) {
        return jl_probe_button_blocking_func( 1, &consume_arg );
    } else {
        return jl_probe_button_nonblocking_func( 1, &consume_arg );
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_probe_button_param_obj, 0, 2, jl_probe_button_param_func );

static mp_obj_t jl_get_button_param_func( size_t n_args, const mp_obj_t* args ) {
    bool blocking = true; // Default to blocking
    int consume = 0;      // Default to not consuming (hold works)

    if ( n_args > 0 ) {
        blocking = mp_obj_is_true( args[ 0 ] );
    }
    if ( n_args > 1 ) {
        consume = mp_obj_is_true( args[ 1 ] ) ? 1 : 0;
    }

    mp_obj_t consume_arg = mp_obj_new_int( consume );
    if ( blocking ) {
        return jl_probe_button_blocking_func( 1, &consume_arg );
    } else {
        return jl_probe_button_nonblocking_func( 1, &consume_arg );
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_get_button_param_obj, 0, 2, jl_get_button_param_func );

static mp_obj_t jl_button_read_param_func( size_t n_args, const mp_obj_t* args ) {
    bool blocking = true; // Default to blocking
    int consume = 0;      // Default to not consuming (hold works)

    if ( n_args > 0 ) {
        blocking = mp_obj_is_true( args[ 0 ] );
    }
    if ( n_args > 1 ) {
        consume = mp_obj_is_true( args[ 1 ] ) ? 1 : 0;
    }

    mp_obj_t consume_arg = mp_obj_new_int( consume );
    if ( blocking ) {
        return jl_probe_button_blocking_func( 1, &consume_arg );
    } else {
        return jl_probe_button_nonblocking_func( 1, &consume_arg );
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_button_read_param_obj, 0, 2, jl_button_read_param_func );

static mp_obj_t jl_read_button_param_func( size_t n_args, const mp_obj_t* args ) {
    bool blocking = true; // Default to blocking
    int consume = 0;      // Default to not consuming (hold works)

    if ( n_args > 0 ) {
        blocking = mp_obj_is_true( args[ 0 ] );
    }
    if ( n_args > 1 ) {
        consume = mp_obj_is_true( args[ 1 ] ) ? 1 : 0;
    }

    mp_obj_t consume_arg = mp_obj_new_int( consume );
    if ( blocking ) {
        return jl_probe_button_blocking_func( 1, &consume_arg );
    } else {
        return jl_probe_button_nonblocking_func( 1, &consume_arg );
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_read_button_param_obj, 0, 2, jl_read_button_param_func );

// Clickwheel Functions
static mp_obj_t jl_clickwheel_up_func( size_t n_args, const mp_obj_t* args ) {
    int clicks = ( n_args > 0 ) ? mp_obj_get_int( args[ 0 ] ) : 1; // Default clicks=1
    jl_clickwheel_up( clicks );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_clickwheel_up_obj, 0, 1, jl_clickwheel_up_func );

static mp_obj_t jl_clickwheel_down_func( size_t n_args, const mp_obj_t* args ) {
    int clicks = ( n_args > 0 ) ? mp_obj_get_int( args[ 0 ] ) : 1; // Default clicks=1
    jl_clickwheel_down( clicks );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_clickwheel_down_obj, 0, 1, jl_clickwheel_down_func );

static mp_obj_t jl_clickwheel_press_func( void ) {
    jl_clickwheel_press( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_clickwheel_press_obj, jl_clickwheel_press_func );

// Service Management Functions
static mp_obj_t jl_force_service_func( mp_obj_t service_name_obj ) {
    const char* service_name = mp_obj_str_get_str( service_name_obj );
    int result = jl_force_service( service_name );
    return mp_obj_new_bool( result );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_force_service_obj, jl_force_service_func );

static mp_obj_t jl_force_service_by_index_func( mp_obj_t index_obj ) {
    int index = mp_obj_get_int( index_obj );
    int result = jl_force_service_by_index( index );
    return mp_obj_new_bool( result );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_force_service_by_index_obj, jl_force_service_by_index_func );

static mp_obj_t jl_get_service_index_func( mp_obj_t service_name_obj ) {
    const char* service_name = mp_obj_str_get_str( service_name_obj );
    int index = jl_get_service_index( service_name );
    return mp_obj_new_int( index );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_get_service_index_obj, jl_get_service_index_func );

// Probe Switch Functions
static mp_obj_t jl_get_switch_position_func( void ) {
    int position = jl_get_switch_position( );
    return mp_obj_new_int( position );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_get_switch_position_obj, jl_get_switch_position_func );

static mp_obj_t jl_set_switch_position_func( mp_obj_t position_obj ) {
    int position = mp_obj_get_int( position_obj );
    jl_set_switch_position( position );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_set_switch_position_obj, jl_set_switch_position_func );

static mp_obj_t jl_check_switch_position_func( void ) {
    int position = jl_check_switch_position( );
    return mp_obj_new_int( position );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_check_switch_position_obj, jl_check_switch_position_func );

static mp_obj_t jl_probe_autoconnect_func( size_t n_args, const mp_obj_t* args ) {
    if ( n_args == 0 ) {
        return mp_obj_new_bool( jl_probe_autoconnect( -1 ) > 0 );
    }
    int enable = mp_obj_is_true( args[0] );
    int result = jl_probe_autoconnect( enable );
    return mp_obj_new_bool( result > 0 );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_probe_autoconnect_obj, 0, 1, jl_probe_autoconnect_func );

// Clickwheel (Rotary Encoder) Functions
static mp_obj_t jl_clickwheel_get_position_func( void ) {
    long position = jl_clickwheel_get_position( );
    return mp_obj_new_int( position );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_clickwheel_get_position_obj, jl_clickwheel_get_position_func );

static mp_obj_t jl_clickwheel_reset_position_func( void ) {
    jl_clickwheel_reset_position( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_clickwheel_reset_position_obj, jl_clickwheel_reset_position_func );

static mp_obj_t jl_clickwheel_get_direction_func( size_t n_args, const mp_obj_t* args ) {
    int consume = 1;  // Default: consume the direction event (one-shot)
    
    if ( n_args > 0 ) {
        consume = mp_obj_is_true( args[ 0 ] ) ? 1 : 0;
    }
    
    int direction = jl_clickwheel_get_direction( consume );
    return mp_obj_new_int( direction );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_clickwheel_get_direction_obj, 0, 1, jl_clickwheel_get_direction_func );

static mp_obj_t jl_clickwheel_get_button_func( void ) {
    int button = jl_clickwheel_get_button( );
    return mp_obj_new_int( button );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_clickwheel_get_button_obj, jl_clickwheel_get_button_func );

static mp_obj_t jl_clickwheel_is_initialized_func( void ) {
    bool initialized = jl_clickwheel_is_initialized( );
    return mp_obj_new_bool( initialized );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_clickwheel_is_initialized_obj, jl_clickwheel_is_initialized_func );

// Note: Formatted output is enabled by default
// Functions return formatted strings like "HIGH", "3.300V", "123.4mA", etc.

// Node creation function
static mp_obj_t jl_node_func( mp_obj_t name_obj ) {
    if ( mp_obj_is_str( name_obj ) ) {
        const char* name = mp_obj_str_get_str( name_obj );
        int value = find_node_value( name );
        if ( value == -1 ) {
            mp_raise_ValueError( MP_ERROR_TEXT( "Unknown node name" ) );
        }
        return node_new( value );
    } else if ( mp_obj_is_int( name_obj ) ) {
        return node_new( mp_obj_get_int( name_obj ) );
    } else if ( mp_obj_get_type( name_obj ) == &node_type ) {
        // Return copy of existing node
        return name_obj;
    }
    mp_raise_TypeError( MP_ERROR_TEXT( "Node must be created from string, int, or another node" ) );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_node_obj, jl_node_func );

// Pre-defined node constants
static const node_obj_t node_top_rail_obj = { .base = { &node_type }, .value = 101 };
static const node_obj_t node_bottom_rail_obj = { .base = { &node_type }, .value = 102 };
static const node_obj_t node_gnd_obj = { .base = { &node_type }, .value = 100 };
static const node_obj_t node_dac0_obj = { .base = { &node_type }, .value = 106 };
static const node_obj_t node_dac1_obj = { .base = { &node_type }, .value = 107 };

// Pre-defined probe button constants
static const probe_button_obj_t probe_button_none_obj = { .base = { &probe_button_type }, .value = 0 };
static const probe_button_obj_t probe_button_connect_obj = { .base = { &probe_button_type }, .value = 1 };
static const probe_button_obj_t probe_button_remove_obj = { .base = { &probe_button_type }, .value = 2 };

// Pre-defined probe pad constants
static const probe_pad_obj_t probe_no_pad_obj = { .base = { &probe_pad_type }, .value = -1 };
static const probe_pad_obj_t probe_logo_pad_top_obj = { .base = { &probe_pad_type }, .value = 142 };
static const probe_pad_obj_t probe_logo_pad_bottom_obj = { .base = { &probe_pad_type }, .value = 143 };
static const probe_pad_obj_t probe_gpio_pad_obj = { .base = { &probe_pad_type }, .value = 144 };
static const probe_pad_obj_t probe_dac_pad_obj = { .base = { &probe_pad_type }, .value = 145 };
static const probe_pad_obj_t probe_adc_pad_obj = { .base = { &probe_pad_type }, .value = 146 };
static const probe_pad_obj_t probe_building_pad_top_obj = { .base = { &probe_pad_type }, .value = 147 };
static const probe_pad_obj_t probe_building_pad_bottom_obj = { .base = { &probe_pad_type }, .value = 148 };

// Nano power/control pad constants
static const probe_pad_obj_t probe_nano_vin_obj = { .base = { &probe_pad_type }, .value = 69 };
static const probe_pad_obj_t probe_nano_reset_0_obj = { .base = { &probe_pad_type }, .value = 94 };
static const probe_pad_obj_t probe_nano_reset_1_obj = { .base = { &probe_pad_type }, .value = 95 };
static const probe_pad_obj_t probe_nano_gnd_1_obj = { .base = { &probe_pad_type }, .value = 96 };
static const probe_pad_obj_t probe_nano_gnd_0_obj = { .base = { &probe_pad_type }, .value = 97 };
static const probe_pad_obj_t probe_nano_3v3_obj = { .base = { &probe_pad_type }, .value = 98 };
static const probe_pad_obj_t probe_nano_5v_obj = { .base = { &probe_pad_type }, .value = 99 };

// Nano digital pin pad constants
static const probe_pad_obj_t probe_d0_pad_obj = { .base = { &probe_pad_type }, .value = 70 };
static const probe_pad_obj_t probe_d1_pad_obj = { .base = { &probe_pad_type }, .value = 71 };
static const probe_pad_obj_t probe_d2_pad_obj = { .base = { &probe_pad_type }, .value = 72 };
static const probe_pad_obj_t probe_d3_pad_obj = { .base = { &probe_pad_type }, .value = 73 };
static const probe_pad_obj_t probe_d4_pad_obj = { .base = { &probe_pad_type }, .value = 74 };
static const probe_pad_obj_t probe_d5_pad_obj = { .base = { &probe_pad_type }, .value = 75 };
static const probe_pad_obj_t probe_d6_pad_obj = { .base = { &probe_pad_type }, .value = 76 };
static const probe_pad_obj_t probe_d7_pad_obj = { .base = { &probe_pad_type }, .value = 77 };
static const probe_pad_obj_t probe_d8_pad_obj = { .base = { &probe_pad_type }, .value = 78 };
static const probe_pad_obj_t probe_d9_pad_obj = { .base = { &probe_pad_type }, .value = 79 };
static const probe_pad_obj_t probe_d10_pad_obj = { .base = { &probe_pad_type }, .value = 80 };
static const probe_pad_obj_t probe_d11_pad_obj = { .base = { &probe_pad_type }, .value = 81 };
static const probe_pad_obj_t probe_d12_pad_obj = { .base = { &probe_pad_type }, .value = 82 };
static const probe_pad_obj_t probe_d13_pad_obj = { .base = { &probe_pad_type }, .value = 83 };
static const probe_pad_obj_t probe_reset_pad_obj = { .base = { &probe_pad_type }, .value = 84 };
static const probe_pad_obj_t probe_aref_pad_obj = { .base = { &probe_pad_type }, .value = 85 };

// Nano analog pin pad constants
static const probe_pad_obj_t probe_a0_pad_obj = { .base = { &probe_pad_type }, .value = 86 };
static const probe_pad_obj_t probe_a1_pad_obj = { .base = { &probe_pad_type }, .value = 87 };
static const probe_pad_obj_t probe_a2_pad_obj = { .base = { &probe_pad_type }, .value = 88 };
static const probe_pad_obj_t probe_a3_pad_obj = { .base = { &probe_pad_type }, .value = 89 };
static const probe_pad_obj_t probe_a4_pad_obj = { .base = { &probe_pad_type }, .value = 90 };
static const probe_pad_obj_t probe_a5_pad_obj = { .base = { &probe_pad_type }, .value = 91 };
static const probe_pad_obj_t probe_a6_pad_obj = { .base = { &probe_pad_type }, .value = 92 };
static const probe_pad_obj_t probe_a7_pad_obj = { .base = { &probe_pad_type }, .value = 93 };

// Rail pad constants
static const probe_pad_obj_t probe_top_rail_pad_obj = { .base = { &probe_pad_type }, .value = 101 };
static const probe_pad_obj_t probe_bottom_rail_pad_obj = { .base = { &probe_pad_type }, .value = 102 };
static const probe_pad_obj_t probe_top_rail_gnd_obj = { .base = { &probe_pad_type }, .value = 104 };
static const probe_pad_obj_t probe_bottom_rail_gnd_obj = { .base = { &probe_pad_type }, .value = 126 };

// Arduino Nano pin constants
static const node_obj_t node_d0_obj = { .base = { &node_type }, .value = 70 };
static const node_obj_t node_d1_obj = { .base = { &node_type }, .value = 71 };
static const node_obj_t node_d2_obj = { .base = { &node_type }, .value = 72 };
static const node_obj_t node_d3_obj = { .base = { &node_type }, .value = 73 };
static const node_obj_t node_d4_obj = { .base = { &node_type }, .value = 74 };
static const node_obj_t node_d5_obj = { .base = { &node_type }, .value = 75 };
static const node_obj_t node_d6_obj = { .base = { &node_type }, .value = 76 };
static const node_obj_t node_d7_obj = { .base = { &node_type }, .value = 77 };
static const node_obj_t node_d8_obj = { .base = { &node_type }, .value = 78 };
static const node_obj_t node_d9_obj = { .base = { &node_type }, .value = 79 };
static const node_obj_t node_d10_obj = { .base = { &node_type }, .value = 80 };
static const node_obj_t node_d11_obj = { .base = { &node_type }, .value = 81 };
static const node_obj_t node_d12_obj = { .base = { &node_type }, .value = 82 };
static const node_obj_t node_d13_obj = { .base = { &node_type }, .value = 83 };
static const node_obj_t node_a0_obj = { .base = { &node_type }, .value = 86 };
static const node_obj_t node_a1_obj = { .base = { &node_type }, .value = 87 };
static const node_obj_t node_a2_obj = { .base = { &node_type }, .value = 88 };
static const node_obj_t node_a3_obj = { .base = { &node_type }, .value = 89 };
static const node_obj_t node_a4_obj = { .base = { &node_type }, .value = 90 };
static const node_obj_t node_a5_obj = { .base = { &node_type }, .value = 91 };
static const node_obj_t node_a6_obj = { .base = { &node_type }, .value = 92 };
static const node_obj_t node_a7_obj = { .base = { &node_type }, .value = 93 };

// GPIO pin constants
static const node_obj_t node_gpio1_obj = { .base = { &node_type }, .value = 131 };
static const node_obj_t node_gpio2_obj = { .base = { &node_type }, .value = 132 };
static const node_obj_t node_gpio3_obj = { .base = { &node_type }, .value = 133 };
static const node_obj_t node_gpio4_obj = { .base = { &node_type }, .value = 134 };
static const node_obj_t node_gpio5_obj = { .base = { &node_type }, .value = 135 };
static const node_obj_t node_gpio6_obj = { .base = { &node_type }, .value = 136 };
static const node_obj_t node_gpio7_obj = { .base = { &node_type }, .value = 137 };
static const node_obj_t node_gpio8_obj = { .base = { &node_type }, .value = 138 };
// UART pins
static const node_obj_t node_uart_tx_obj = { .base = { &node_type }, .value = 116 };
static const node_obj_t node_uart_rx_obj = { .base = { &node_type }, .value = 117 };

// ADC pins
static const node_obj_t node_adc0_obj = { .base = { &node_type }, .value = 110 };
static const node_obj_t node_adc1_obj = { .base = { &node_type }, .value = 111 };
static const node_obj_t node_adc2_obj = { .base = { &node_type }, .value = 112 };
static const node_obj_t node_adc3_obj = { .base = { &node_type }, .value = 113 };
static const node_obj_t node_adc4_obj = { .base = { &node_type }, .value = 114 };
static const node_obj_t node_adc7_obj = { .base = { &node_type }, .value = 115 };

// Current sense pins
static const node_obj_t node_isense_plus_obj = { .base = { &node_type }, .value = 108 };
static const node_obj_t node_isense_minus_obj = { .base = { &node_type }, .value = 109 };

// Buffer pins
static const node_obj_t node_buffer_in_obj = { .base = { &node_type }, .value = 139 };
static const node_obj_t node_buffer_out_obj = { .base = { &node_type }, .value = 140 };

// GPIO State constants
static const gpio_state_obj_t gpio_state_high_obj = { .base = { &gpio_state_type }, .value = GPIO_STATE_HIGH };
static const gpio_state_obj_t gpio_state_low_obj = { .base = { &gpio_state_type }, .value = GPIO_STATE_LOW };
static const gpio_state_obj_t gpio_state_floating_obj = { .base = { &gpio_state_type }, .value = GPIO_STATE_FLOATING };

// GPIO Direction constants (OUTPUT=0, INPUT=1)
// Numeric convention used by firmware: 0 = OUTPUT, 1 = INPUT
static const gpio_direction_obj_t gpio_direction_input_obj = { .base = { &gpio_direction_type }, .value = false };
static const gpio_direction_obj_t gpio_direction_output_obj = { .base = { &gpio_direction_type }, .value = true };

// Helper function to get direction value from various input types
// Returns numeric firmware convention: 0 = OUTPUT, 1 = INPUT
static int get_direction_value( mp_obj_t obj ) {
    if ( mp_obj_is_int( obj ) ) {
        // Integer: 0=OUTPUT, 1=INPUT
        int v = mp_obj_get_int( obj );
        return v ? 1 : 0;
    } else if ( mp_obj_is_bool( obj ) ) {
        // Boolean: True=OUTPUT, False=INPUT  -> numeric: True -> 0, False -> 1
        //  mp_raise_ValueError( MP_ERROR_TEXT( "Bool'" ) );
        return mp_obj_is_true( obj ) ? 0 : 1;
    } else if ( mp_obj_is_str( obj ) ) {
        // String: "OUTPUT"/"INPUT" (case insensitive)
        const char* str = mp_obj_str_get_str( obj );
        if ( strcmp( str, "OUTPUT" ) == 0 || strcmp( str, "output" ) == 0 || strcmp( str, "OUT" ) == 0 || strcmp( str, "out" ) == 0 ) {
            //  mp_raise_ValueError( MP_ERROR_TEXT( "String Output" ) );
            return 0;
        } else if ( strcmp( str, "INPUT" ) == 0 || strcmp( str, "input" ) == 0 || strcmp( str, "IN" ) == 0 || strcmp( str, "in" ) == 0 ) {
                // mp_raise_ValueError( MP_ERROR_TEXT( "String Input" ) );
            return 1;
        } else {
            mp_raise_ValueError( MP_ERROR_TEXT( "Direction string must be 'INPUT' or 'OUTPUT'" ) );
        }
    } else if ( mp_obj_get_type( obj ) == &gpio_direction_type ) {
        // GPIO Direction object (true == OUTPUT)
        gpio_direction_obj_t* dir = MP_OBJ_TO_PTR( obj );
        if ( dir->value == true) {
            //  mp_raise_ValueError( MP_ERROR_TEXT( "Object True" ) );
        } else {
        //  mp_raise_ValueError( MP_ERROR_TEXT( "Object False" ) );
        }
        return dir->value ? 0 : 1; // Convert to numeric convention: true (OUTPUT) -> 0, false (INPUT) -> 1
    } else {
        // Try to convert to int as fallback
        int v = mp_obj_get_int( obj );
        //  mp_raise_ValueError( MP_ERROR_TEXT( "Direction string must be 'INPUT' or 'OUTPUT'" ) );
        return v ? 1 : 0;
    }
}

// Helper function to get pull value from various input types
static int get_pull_value( mp_obj_t obj ) {
    if ( mp_obj_is_int( obj ) ) {
        // Integer: 0=NO_PULL, 1=PULLUP, -1=PULLDOWN, 2=BUS_KEEPER
        return mp_obj_get_int( obj );
    } else if ( mp_obj_is_bool( obj ) ) {
        // Boolean: False=NO_PULL, True=PULLUP
        return mp_obj_is_true( obj ) ? 1 : 0;
    } else if ( mp_obj_is_str( obj ) ) {
        // String: "NO_PULL"/"PULLUP"/"PULLDOWN"/"BUS_KEEPER" (case insensitive)
        const char* str = mp_obj_str_get_str( obj );
        if ( strcmp( str, "PULLUP" ) == 0 || strcmp( str, "pullup" ) == 0 || strcmp( str, "UP" ) == 0 || strcmp( str, "up" ) == 0 ) {
            return 1;
        } else if ( strcmp( str, "PULLDOWN" ) == 0 || strcmp( str, "pulldown" ) == 0 || strcmp( str, "DOWN" ) == 0 || strcmp( str, "down" ) == 0 ) {
            return -1;  // Changed from 2 to -1 to match C++ implementation
        } else if ( strcmp( str, "NO_PULL" ) == 0 || strcmp( str, "no_pull" ) == 0 || strcmp( str, "NOPULL" ) == 0 || strcmp( str, "nopull" ) == 0 ||
                    strcmp( str, "NONE" ) == 0 || strcmp( str, "none" ) == 0 || strcmp( str, "OFF" ) == 0 || strcmp( str, "off" ) == 0 ) {
            return 0;
        } else if ( strcmp( str, "BUS_KEEPER" ) == 0 || strcmp( str, "bus_keeper" ) == 0 || strcmp( str, "BUSKEEPER" ) == 0 || strcmp( str, "buskeeper" ) == 0 ) {
            return 2;
        } else {
            mp_raise_ValueError( MP_ERROR_TEXT( "Pull string must be 'NO_PULL', 'PULLUP', 'PULLDOWN', or 'BUS_KEEPER'" ) );
        }
    } else if ( mp_obj_get_type( obj ) == &gpio_pull_type ) {
        // Handle gpio_pull_type objects directly
        gpio_pull_obj_t* pull_obj = MP_OBJ_TO_PTR( obj );
        return pull_obj->value;
    } else {
        // Try to convert to int as fallback
        return mp_obj_get_int( obj );
    }
}

// Helper function to get GPIO state value from various input types
static int get_gpio_state_value( mp_obj_t obj ) {
    if ( mp_obj_is_int( obj ) ) {
        // Integer: 0=LOW, 1=HIGH, 2=FLOATING
        return mp_obj_get_int( obj );
    } else if ( mp_obj_is_bool( obj ) ) {
        // Boolean: False=LOW, True=HIGH
        return mp_obj_is_true( obj ) ? 1 : 0;
    } else if ( mp_obj_is_str( obj ) ) {
        // String: "HIGH"/"LOW"/"FLOATING" (case insensitive)
        const char* str = mp_obj_str_get_str( obj );
        if ( strcmp( str, "HIGH" ) == 0 || strcmp( str, "high" ) == 0 || strcmp( str, "1" ) == 0 ) {
            return 1;
        } else if ( strcmp( str, "LOW" ) == 0 || strcmp( str, "low" ) == 0 || strcmp( str, "0" ) == 0 ) {
            return 0;
        } else if ( strcmp( str, "FLOATING" ) == 0 || strcmp( str, "floating" ) == 0 || strcmp( str, "Z" ) == 0 || strcmp( str, "z" ) == 0 ) {
            return 2;
        } else {
            mp_raise_ValueError( MP_ERROR_TEXT( "GPIO state string must be 'HIGH', 'LOW', or 'FLOATING'" ) );
        }
    } else if ( mp_obj_get_type( obj ) == &gpio_state_type ) {
        // GPIO State object
        gpio_state_obj_t* state = MP_OBJ_TO_PTR( obj );
        return state->value;
    } else {
        // Try to convert to int as fallback
        return mp_obj_get_int( obj ) ? 1 : 0;
    }
}

// Nodes Help Function
static mp_obj_t jl_help_nodes_func( void ) {
    mp_printf( &mp_plat_print, "Jumperless Node Reference\n" );
    mp_printf( &mp_plat_print, "========================\n\n" );

    mp_printf( &mp_plat_print, "NODE TYPES:\n" );
    mp_printf( &mp_plat_print, "  Numbered:     1-60 (breadboard)\n" );
    mp_printf( &mp_plat_print, "  Arduino:      D0-D13, A0-A7 (nano header)\n" );
    mp_printf( &mp_plat_print, "  GPIO:         GPIO_1-GPIO_8 (routable GPIO)\n" );
    mp_printf( &mp_plat_print, "  Power:        TOP_RAIL, BOTTOM_RAIL, GND\n" );
    mp_printf( &mp_plat_print, "  DAC:          DAC0, DAC1 (analog outputs)\n" );
    mp_printf( &mp_plat_print, "  ADC:          ADC0-ADC4, PROBE (analog inputs)\n" );
    mp_printf( &mp_plat_print, "  Current:      ISENSE_PLUS, ISENSE_MINUS\n" );
    mp_printf( &mp_plat_print, "  UART:         UART_TX, UART_RX\n" );
    mp_printf( &mp_plat_print, "  Buffer:       BUFFER_IN, BUFFER_OUT\n\n" );

    mp_printf( &mp_plat_print, "THREE WAYS TO USE NODES:\n\n" );

    mp_printf( &mp_plat_print, "1. NUMBERS (direct breadboard holes):\n" );
    mp_printf( &mp_plat_print, "   connect(1, 30)                     # Connect holes 1 and 30\n" );
    mp_printf( &mp_plat_print, "   connect(15, 42)                    # Any number 1-60\n\n" );

    mp_printf( &mp_plat_print, "2. STRINGS (case-insensitive names):\n" );
    mp_printf( &mp_plat_print, "   connect(\"D13\", \"TOP_RAIL\")         # Arduino pin to power rail\n" );
    mp_printf( &mp_plat_print, "   connect(\"gpio_1\", \"adc0\")          # GPIO to ADC (case-insensitive)\n" );
    mp_printf( &mp_plat_print, "   connect(\"15\", \"dac1\")              # Mix numbers and names\n\n" );

    mp_printf( &mp_plat_print, "3. CONSTANTS (pre-defined objects):\n" );
    mp_printf( &mp_plat_print, "   connect(TOP_RAIL, D13)            # Using imported constants\n" );
    mp_printf( &mp_plat_print, "   connect(GPIO_1, A0)               # No quotes needed\n" );
    mp_printf( &mp_plat_print, "   connect(DAC0, 25)                 # Mix constants and numbers\n\n" );

    mp_printf( &mp_plat_print, "MIXED USAGE:\n" );
    mp_printf( &mp_plat_print, "   my_pin = node(\"D13\")              # Create node object from string\n" );
    mp_printf( &mp_plat_print, "   connect(my_pin, TOP_RAIL)         # Use node object with constant\n" );
    mp_printf( &mp_plat_print, "   oled_print(my_pin)                # Display shows 'D13'\n\n" );

    mp_printf( &mp_plat_print, "COMMON ALIASES (many names work for same node):\n" );
    mp_printf( &mp_plat_print, "   \"TOP_RAIL\" = \"T_R\"\n" );
    mp_printf( &mp_plat_print, "   \"GPIO_1\" = \"GPIO1\" = \"GP1\"\n" );
    mp_printf( &mp_plat_print, "   \"DAC0\" = \"DAC_0\"\n" );
    mp_printf( &mp_plat_print, "   \"UART_TX\" = \"TX\"\n\n" );

    mp_printf( &mp_plat_print, "NOTES:\n" );
    mp_printf( &mp_plat_print, "  - String names are case-insensitive: \"d13\" = \"D13\" = \"nAnO_d13\"\n" );
    mp_printf( &mp_plat_print, "  - Constants are case-sensitive: use D13, not d13\n" );
    mp_printf( &mp_plat_print, "  - All three methods work in any function\n" );
    // mp_printf(&mp_plat_print, "  - Use 'from jumperless_nodes import *' for global constants\n\n");

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_help_nodes_obj, jl_help_nodes_func );

// Help Function
static mp_obj_t jl_help_func( size_t n_args, const mp_obj_t* args ) {
    if ( n_args == 0 ) {
        // Show all sections
        mp_printf( &mp_plat_print, "Jumperless Native MicroPython Module\n" );
        // mp_printf(&mp_plat_print, "Hardware Control Functions with Formatted Output:\n");
        // mp_printf(&mp_plat_print, "(GPIO functions return formatted strings like HIGH/LOW, INPUT/OUTPUT, PULLUP/NONE, CONNECTED/DISCONNECTED)\n\n");
        jl_cycle_term_color( true, 5.0, 1 );
        mp_printf( &mp_plat_print, "Available help sections:\n\n" );

        mp_printf( &mp_plat_print, "  help() or help(\"all\")     - Show all functions\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"DAC\")              - DAC functions\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"ADC\")              - ADC functions\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"GPIO\")             - GPIO functions\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"PWM\")              - PWM functions\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"WAVEGEN\")          - Waveform generator\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"INA\")              - INA current/power monitor\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"NODES\")            - Node connections\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"NETS\")             - Net info (names, colors)\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"SLOTS\")            - Slot management\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"OLED\")             - OLED display\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"PROBE\")            - Probe and button functions\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"CLICKWHEEL\")       - Clickwheel (rotary encoder) functions\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"STATUS\")           - Status and debug functions\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"FILESYSTEM\")       - Filesystem functions\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"MISC\")             - Miscellaneous functions\n" );
        jl_cycle_term_color( false, 100.0, 1 );
        mp_printf( &mp_plat_print, "  help(\"EXAMPLES\")         - Usage examples\n\n" );

        // Show all sections with colors
        jl_help_section( "DAC" );
        jl_help_section( "ADC" );
        jl_help_section( "GPIO" );
        jl_help_section( "PWM" );
        jl_help_section( "WAVEGEN" );
        jl_help_section( "INA" );
        jl_help_section( "NODES" );
        jl_help_section( "NETS" );
        jl_help_section( "SLOTS" );
        jl_help_section( "OLED" );
        jl_help_section( "PROBE" );
        jl_help_section( "CLICKWHEEL" );
        jl_help_section( "STATUS" );
        jl_help_section( "FILESYSTEM" );
        jl_help_section( "MISC" );
        jl_help_section( "EXAMPLES" );
    } else {
        // Show specific section
        const char* section = mp_obj_str_get_str( args[ 0 ] );
        jl_help_section( section );
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_help_obj, 0, 1, jl_help_func );

// Help section function implementation
void jl_help_section( const char* section ) {
    // Color cycling for different sections (disabled for now)
    static int color_index = 0;
    int colors[] = { 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45 }; // ANSI colors
    int color = colors[ color_index % 15 ];
    color_index++;
    jl_cycle_term_color( true, 5.0, 1 );

    // Convert section to uppercase for comparison
    char section_upper[ 32 ];
    strcpy( section_upper, section );
    for ( int i = 0; section_upper[ i ]; i++ ) {
        section_upper[ i ] = toupper( section_upper[ i ] );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "DAC" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "DAC (Digital-to-Analog Converter):\n\n" );
        mp_printf( &mp_plat_print, "   dac_set(channel, voltage)         - Set DAC output voltage\n" );
        mp_printf( &mp_plat_print, "   dac_get(channel)                  - Get DAC output voltage\n" );
        mp_printf( &mp_plat_print, "   set_dac(channel, voltage)         - Alias for dac_set\n" );
        mp_printf( &mp_plat_print, "   get_dac(channel)                  - Alias for dac_get\n\n" );
        mp_printf( &mp_plat_print, "          channel: 0-3, DAC0, DAC1, TOP_RAIL, BOTTOM_RAIL\n" );
        mp_printf( &mp_plat_print, "          channel 0/DAC0: DAC 0\n" );
        mp_printf( &mp_plat_print, "          channel 1/DAC1: DAC 1\n" );
        mp_printf( &mp_plat_print, "          channel 2/TOP_RAIL: top rail\n" );
        mp_printf( &mp_plat_print, "          channel 3/BOTTOM_RAIL: bottom rail\n" );
        mp_printf( &mp_plat_print, "          voltage: -8.0 to 8.0V\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );

    if ( strcmp( section_upper, "ADC" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "ADC (Analog-to-Digital Converter):\n\n" );
        mp_printf( &mp_plat_print, "   adc_get(channel)                  - Read ADC input voltage\n" );
        mp_printf( &mp_plat_print, "   get_adc(channel)                  - Alias for adc_get\n\n" );
        mp_printf( &mp_plat_print, "                                              channel: 0-4\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "GPIO" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {

        mp_printf( &mp_plat_print, "GPIO:\n\n" );
        mp_printf( &mp_plat_print, "   gpio_set(pin, value)             - Set GPIO pin state\n" );
        mp_printf( &mp_plat_print, "   gpio_get(pin)                    - Read GPIO pin state\n" );
        mp_printf( &mp_plat_print, "   gpio_set_dir(pin, direction)     - Set GPIO pin direction\n" );
        mp_printf( &mp_plat_print, "   gpio_get_dir(pin)                - Get GPIO pin direction\n" );
        mp_printf( &mp_plat_print, "   gpio_set_pull(pin, pull)         - Set GPIO pull-up/down\n" );
        mp_printf( &mp_plat_print, "   gpio_get_pull(pin)               - Get GPIO pull-up/down\n\n" );
        mp_printf( &mp_plat_print, "  Aliases: set_gpio, get_gpio, set_gpio_dir, get_gpio_dir, etc.\n\n" );
        mp_printf( &mp_plat_print, "            pin 1-8: GPIO 1-8\n" );
        mp_printf( &mp_plat_print, "            pin   9: UART Tx\n" );
        mp_printf( &mp_plat_print, "            pin  10: UART Rx\n" );
        mp_printf( &mp_plat_print, "              value: True/False   for HIGH/LOW\n" );
        mp_printf( &mp_plat_print, "          direction: True/False   for OUTPUT/INPUT\n" );
        mp_printf( &mp_plat_print, "               pull: -1/0/1/2     for PULLDOWN/NO_PULL/PULLUP/BUS_KEEPER\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "PWM" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "PWM (Pulse Width Modulation):\n\n" );
        mp_printf( &mp_plat_print, "   pwm(pin, [frequency], [duty])    - Setup PWM on GPIO pin\n" );
        mp_printf( &mp_plat_print, "   pwm_set_duty_cycle(pin, duty)    - Set PWM duty cycle\n" );
        mp_printf( &mp_plat_print, "   pwm_set_frequency(pin, freq)     - Set PWM frequency\n" );
        mp_printf( &mp_plat_print, "   pwm_stop(pin)                    - Stop PWM on pin\n\n" );
        mp_printf( &mp_plat_print, "  Aliases: set_pwm, set_pwm_duty_cycle, set_pwm_frequency, stop_pwm\n\n" );
        mp_printf( &mp_plat_print, "             pin: 1-8       GPIO pins only\n" );
        mp_printf( &mp_plat_print, "       frequency: 0.001Hz-62.5MHz default 1000Hz\n" );
        mp_printf( &mp_plat_print, "      duty_cycle: 0.0-1.0   default 0.5 (50%%)\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "WAVEGEN" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "WaveGen (Waveform Generator):\n\n" );
        mp_printf( &mp_plat_print, "   wavegen_set_output(channel)      - Set output: DAC0, DAC1, TOP_RAIL, BOTTOM_RAIL\n" );
        mp_printf( &mp_plat_print, "   wavegen_set_freq(hz)             - Set frequency (0.0001-10000 Hz)\n" );
        mp_printf( &mp_plat_print, "   wavegen_set_wave(shape)          - Set waveform shape\n" );
        mp_printf( &mp_plat_print, "   wavegen_set_amplitude(vpp)       - Set amplitude (0-16 Vpp)\n" );
        mp_printf( &mp_plat_print, "   wavegen_set_offset(v)            - Set DC offset (-8 to +8 V)\n" );
        mp_printf( &mp_plat_print, "   wavegen_start()                  - Start waveform generation\n" );
        mp_printf( &mp_plat_print, "   wavegen_stop()                   - Stop waveform generation\n\n" );
        mp_printf( &mp_plat_print, "  Getters: wavegen_get_output(), wavegen_get_freq(), wavegen_get_wave(),\n" );
        mp_printf( &mp_plat_print, "           wavegen_get_amplitude(), wavegen_get_offset(), wavegen_is_running()\n\n" );
        mp_printf( &mp_plat_print, "  Waveform constants: SINE, TRIANGLE, SAWTOOTH (RAMP), SQUARE\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "INA" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "INA (Current/Power Monitor):\n\n" );
        mp_printf( &mp_plat_print, "   ina_get_current(sensor)          - Read current in amps\n" );
        mp_printf( &mp_plat_print, "   ina_get_voltage(sensor)          - Read shunt voltage\n" );
        mp_printf( &mp_plat_print, "   ina_get_bus_voltage(sensor)      - Read bus voltage\n" );
        mp_printf( &mp_plat_print, "   ina_get_power(sensor)            - Read power in watts\n\n" );
        mp_printf( &mp_plat_print, "  Aliases: get_current, get_voltage, get_bus_voltage, get_power\n\n" );
        mp_printf( &mp_plat_print, "             sensor: 0 or 1\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "NODES" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "Node Connections:\n\n" );
        mp_printf( &mp_plat_print, "   connect(node1, node2)            - Connect two nodes\n" );
        mp_printf( &mp_plat_print, "   disconnect(node1, node2)         - Disconnect nodes\n" );
        mp_printf( &mp_plat_print, "   is_connected(node1, node2)       - Check if nodes are connected\n" );
        mp_printf( &mp_plat_print, "   nodes_clear()                    - Clear all connections\n\n" );
        mp_printf( &mp_plat_print, "         set node2 to -1 to disconnect everything connected to node1\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "NETS" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "Net Information:\n\n" );
        mp_printf( &mp_plat_print, "   get_net_name(netNum)             - Get net name\n" );
        mp_printf( &mp_plat_print, "   set_net_name(netNum, name)       - Set custom net name\n" );
        mp_printf( &mp_plat_print, "   get_net_color(netNum)            - Get net color as 0xRRGGBB\n" );
        mp_printf( &mp_plat_print, "   get_net_color_name(netNum)       - Get net color name\n" );
        mp_printf( &mp_plat_print, "   set_net_color(netNum, color)     - Set net color by name or hex\n" );
        mp_printf( &mp_plat_print, "   set_net_color_hsv(netNum, h, [s], [v]) - Set by HSV (auto-detects range)\n" );
        mp_printf( &mp_plat_print, "   get_num_nets()                   - Get number of active nets\n" );
        mp_printf( &mp_plat_print, "   get_num_bridges()                - Get number of bridges\n" );
        mp_printf( &mp_plat_print, "   get_net_nodes(netNum)            - Get comma-separated node list\n" );
        mp_printf( &mp_plat_print, "   get_bridge(bridgeIdx)            - Get bridge info tuple\n" );
        mp_printf( &mp_plat_print, "   get_net_info(netNum)             - Get full net info as dict\n" );
        mp_printf( &mp_plat_print, "   get_num_paths(include_duplicates=True) - Get number of paths (optionally exclude duplicates)\n" );
        mp_printf( &mp_plat_print, "   get_node_voltage(node)           - Scanned node voltage (None if no data)\n" );
        mp_printf( &mp_plat_print, "   get_net_current(netNum)          - Net current dict (None if no data)\n" );
        mp_printf( &mp_plat_print, "   get_path_current(pathIdx)        - Signed path current in mA (None if no data)\n\n" );
        mp_printf( &mp_plat_print, "  Colors: red, orange, yellow, green, cyan, blue, purple, pink, etc.\n" );
        mp_printf( &mp_plat_print, "  HSV: h=0.0-1.0 or 0-255 (auto), s=0-1/0-255 (default max), v=0-1/0-255 (default 32)\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "SLOTS" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "Slot Management:\n\n" );
        mp_printf( &mp_plat_print, "   nodes_save([slot])               - Save connections to slot\n" );
        mp_printf( &mp_plat_print, "   nodes_discard()                  - Discard unsaved changes\n" );
        mp_printf( &mp_plat_print, "   nodes_has_changes()              - Check for unsaved changes\n" );
        mp_printf( &mp_plat_print, "   switch_slot(slot)                - Switch to different slot (0-7)\n" );
        mp_printf( &mp_plat_print, "   CURRENT_SLOT                     - Get current slot number\n\n" );
        mp_printf( &mp_plat_print, "  Context (controls persistence):\n" );
        mp_printf( &mp_plat_print, "   context_toggle()                 - Toggle global/python mode\n" );
        mp_printf( &mp_plat_print, "   context_get()                    - Get current mode name\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "OLED" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "OLED Display:\n\n" );
        mp_printf( &mp_plat_print, "   oled_print(\"text\")               - Display text\n" );
        mp_printf( &mp_plat_print, "   oled_clear()                     - Clear display\n" );
        mp_printf( &mp_plat_print, "   oled_connect()                   - Connect OLED\n" );
        mp_printf( &mp_plat_print, "   oled_disconnect()                - Disconnect OLED\n\n" );
        mp_printf( &mp_plat_print, "OLED Layout (retained screens, live-updating):\n\n" );
        mp_printf( &mp_plat_print, "   from oledgui import Screen, Text, Rect, set_var\n" );
        mp_printf( &mp_plat_print, "   s = Screen()\n" );
        mp_printf( &mp_plat_print, "   s.add(Text(\"A0 {adc:0}V\", x=0, y=0, font=\"Pragmatism\", size=10))\n" );
        mp_printf( &mp_plat_print, "   s.show()                         - make it the active display\n" );
        mp_printf( &mp_plat_print, "   set_var(\"name\", value)           - push a live {name} value\n" );
        mp_printf( &mp_plat_print, "   s.save(\"layout\")                 - save to /screens/layout.json\n\n" );
        mp_printf( &mp_plat_print, "   Tokens: {adc:N} {gpio:N} {dac:N} {uptime} {freemem} {undo}\n" );
        mp_printf( &mp_plat_print, "   Flat API: oled_screen/oled_add_text/oled_add_shape/oled_set\n\n" );
    }

    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "PROBE" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "Probe Functions:\n\n" );
        mp_printf( &mp_plat_print, "   probe_read([blocking=True])                - Read probe (default: blocking)\n" );
        mp_printf( &mp_plat_print, "   read_probe([blocking=True])                - Read probe (default: blocking)\n" );
        mp_printf( &mp_plat_print, "   probe_read_blocking()                      - Wait for probe touch (explicit)\n" );
        mp_printf( &mp_plat_print, "   probe_read_nonblocking()                   - Check probe immediately (explicit)\n" );
        mp_printf( &mp_plat_print, "   get_button([blocking], [consume])          - Get button (blocking=True, consume=False)\n" );
        mp_printf( &mp_plat_print, "   probe_button([blocking], [consume])        - Get button (blocking=True, consume=False)\n" );
        mp_printf( &mp_plat_print, "   check_button([consume])                    - Check button non-blocking (consume=False)\n" );
        mp_printf( &mp_plat_print, "   probe_button_blocking([consume])           - Wait for button (consume=False)\n" );
        mp_printf( &mp_plat_print, "   probe_button_nonblocking([consume])        - Check button immediate (consume=False)\n\n" );
        mp_printf( &mp_plat_print, "  consume=False (default): Holding button returns same state (continuous control)\n" );
        mp_printf( &mp_plat_print, "  consume=True: Each press detected once (one-shot detection)\n\n" );
        mp_printf( &mp_plat_print, "  Probe Switch Functions:\n" );
        mp_printf( &mp_plat_print, "   get_switch_position()                      - Get current switch position\n" );
        mp_printf( &mp_plat_print, "   set_switch_position(pos)                   - Set switch position manually\n" );
        mp_printf( &mp_plat_print, "   check_switch_position()                    - Check switch via current sensing\n\n" );
        mp_printf( &mp_plat_print, "       Touch returns: ProbePad object (1-60, D13_PAD, TOP_RAIL_PAD, LOGO_PAD_TOP, etc.)\n" );
        mp_printf( &mp_plat_print, "       Button returns: CONNECT, REMOVE, or NONE (front=connect, rear=remove)\n" );
        mp_printf( &mp_plat_print, "       Switch returns: SWITCH_MEASURE (0), SWITCH_SELECT (1), SWITCH_UNKNOWN (-1)\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "CLICKWHEEL" ) == 0 || strcmp( section_upper, "ENCODER" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "Clickwheel (Rotary Encoder):\n\n" );
        mp_printf( &mp_plat_print, "   clickwheel_get_position()                  - Get raw position counter\n" );
        mp_printf( &mp_plat_print, "   clickwheel_reset_position()                - Reset position to 0\n" );
        mp_printf( &mp_plat_print, "   clickwheel_get_direction([consume=True])   - Get direction event\n" );
        mp_printf( &mp_plat_print, "   clickwheel_get_button()                    - Get button state\n" );
        mp_printf( &mp_plat_print, "   clickwheel_is_initialized()                - Check if clickwheel is ready\n\n" );
        mp_printf( &mp_plat_print, "  consume=True (default): Direction cleared after reading (one-shot detection)\n" );
        mp_printf( &mp_plat_print, "  consume=False: Direction persists until consumed (can read multiple times)\n\n" );
        mp_printf( &mp_plat_print, "  Direction returns: CLICKWHEEL_NONE (0), CLICKWHEEL_UP (1), CLICKWHEEL_DOWN (2)\n" );
        mp_printf( &mp_plat_print, "  Button returns: CLICKWHEEL_IDLE (0), CLICKWHEEL_PRESSED (1), CLICKWHEEL_HELD (2),\n" );
        mp_printf( &mp_plat_print, "                  CLICKWHEEL_RELEASED (3)\n" );
        mp_printf( &mp_plat_print, "  CLICKWHEEL_DOUBLECLICKED (4) is reserved and NEVER returned - the encoder\n" );
        mp_printf( &mp_plat_print, "  has no double-click gesture. Two fast presses are two ordinary clicks.\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "STATUS" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "Status:\n\n" );
        mp_printf( &mp_plat_print, "   print_bridges()                  - Print all bridges\n" );
        mp_printf( &mp_plat_print, "   print_paths()                    - Print path between nodes\n" );
        mp_printf( &mp_plat_print, "   print_crossbars()                - Print crossbar array\n" );
        mp_printf( &mp_plat_print, "   print_nets()                     - Print nets\n" );
        mp_printf( &mp_plat_print, "   print_chip_status()              - Print chip status\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "FILESYSTEM" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "Filesystem:\n\n" );
        mp_printf( &mp_plat_print, "  jfs.open(path, mode)              - Open file\n" );
        mp_printf( &mp_plat_print, "  jfs.read(file, size)              - Read from file\n" );
        mp_printf( &mp_plat_print, "  jfs.write(file, data)             - Write to file\n" );
        mp_printf( &mp_plat_print, "  jfs.close(file)                   - Close file\n" );
        mp_printf( &mp_plat_print, "  jfs.exists(path)                  - Check if file exists\n" );
        mp_printf( &mp_plat_print, "  jfs.listdir(path)                 - List directory\n" );
        mp_printf( &mp_plat_print, "  jfs.mkdir(path)                   - Create directory\n" );
        mp_printf( &mp_plat_print, "  jfs.remove(path)                  - Remove file\n" );
        mp_printf( &mp_plat_print, "  jfs.rename(from, to)              - Rename file\n" );
        mp_printf( &mp_plat_print, "  jfs.info()                        - Get filesystem info\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "MISC" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "Misc:\n\n" );
        mp_printf( &mp_plat_print, "   arduino_reset()                  - Reset Arduino\n" );
        mp_printf( &mp_plat_print, "   run_app(appName)                 - Run built-in app\n" );
        mp_printf( &mp_plat_print, "   pause_core2(pause)               - Pause/unpause Core2 (True/False)\n" );
        mp_printf( &mp_plat_print, "   send_raw(chip, x, y, set)        - Send raw data to crossbar chip\n" );
        mp_printf( &mp_plat_print, "   force_service(name)              - Force run a specific service (e.g., \"ProbeButton\")\n" );
        mp_printf( &mp_plat_print, "   force_service_by_index(idx)      - Force run service by index (faster)\n" );
        mp_printf( &mp_plat_print, "   get_service_index(name)          - Get service index by name (cache for fast calls)\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "EXAMPLES" ) == 0 || strcmp( section_upper, "ALL" ) == 0 ) {
        mp_printf( &mp_plat_print, "Examples (all functions available globally):\n\n" );
        mp_printf( &mp_plat_print, "  dac_set(DAC0, 5.0)                         # Set DAC0 using node constant\n" );
        mp_printf( &mp_plat_print, "  voltage = get_adc(1)                       # Read ADC1 using alias\n" );
        mp_printf( &mp_plat_print, "  connect(TOP_RAIL, D13)                     # Connect using constants\n" );
        mp_printf( &mp_plat_print, "  connect(4, 20)                             # Connect using numbers\n" );
        mp_printf( &mp_plat_print, "  top_rail = node(\"TOP_RAIL\")                # Create node object\n" );
        mp_printf( &mp_plat_print, "  oled_print(\"Hello!\")                       # Display text on OLED\n" );
        mp_printf( &mp_plat_print, "  current = get_current(0)                   # Read current using alias\n" );
        mp_printf( &mp_plat_print, "  set_gpio(1, True)                          # Set GPIO pin high\n" );
        mp_printf( &mp_plat_print, "  pwm(1, 1000, 0.5)                          # 1kHz PWM, 50%% duty\n" );
        mp_printf( &mp_plat_print, "  wavegen_set_wave(SINE); wavegen_start()    # Start sine wave\n" );
        mp_printf( &mp_plat_print, "  set_net_color(0, \"red\")                    # Color net 0 red\n" );
        mp_printf( &mp_plat_print, "  set_net_color_hsv(1, 0.5)                  # Cyan net 1 (HSV hue)\n" );
        mp_printf( &mp_plat_print, "  nodes_save()                               # Save current connections\n" );
        mp_printf( &mp_plat_print, "  pad = probe_read()                         # Wait for probe touch\n" );
        mp_printf( &mp_plat_print, "  button = get_button()                      # Wait for button press\n\n" );
    }
    jl_cycle_term_color( false, 100.0, 1 );
    if ( strcmp( section_upper, "ALL" ) != 0 &&
         strcmp( section_upper, "DAC" ) != 0 && strcmp( section_upper, "ADC" ) != 0 &&
         strcmp( section_upper, "GPIO" ) != 0 && strcmp( section_upper, "PWM" ) != 0 &&
         strcmp( section_upper, "WAVEGEN" ) != 0 && strcmp( section_upper, "INA" ) != 0 &&
         strcmp( section_upper, "NODES" ) != 0 && strcmp( section_upper, "NETS" ) != 0 &&
         strcmp( section_upper, "SLOTS" ) != 0 && strcmp( section_upper, "OLED" ) != 0 &&
         strcmp( section_upper, "PROBE" ) != 0 && strcmp( section_upper, "CLICKWHEEL" ) != 0 &&
         strcmp( section_upper, "ENCODER" ) != 0 && strcmp( section_upper, "STATUS" ) != 0 &&
         strcmp( section_upper, "FILESYSTEM" ) != 0 &&
         strcmp( section_upper, "MISC" ) != 0 && strcmp( section_upper, "EXAMPLES" ) != 0 ) {
        mp_printf( &mp_plat_print, "Unknown help section: %s\n", section );
        mp_printf( &mp_plat_print, "Use help() to see available sections.\n\n" );
    }
}

// Filesystem Functions
static mp_obj_t jl_fs_exists_func( mp_obj_t path_obj ) {
    const char* path = mp_obj_str_get_str( path_obj );
    int exists = jl_fs_exists( path );
    return mp_obj_new_bool( exists );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_fs_exists_obj, jl_fs_exists_func );

static mp_obj_t jl_fs_listdir_func( mp_obj_t path_obj ) {
    const char* path = mp_obj_str_get_str( path_obj );
    char* result = jl_fs_listdir( path );
    if ( result == NULL ) {
        mp_raise_OSError( MP_ENOENT ); // directory doesn't exist
    }

    // Entries are '\n'-separated ('\n' is illegal in FAT filenames, so it can
    // never collide with a name the way the old ',' separator did). Build the
    // strings straight out of the producer's static buffer - no copy, no
    // strtok, and nothing leaks if mp_obj_new_str raises UnicodeError.
    mp_obj_t list_obj = mp_obj_new_list( 0, NULL );
    for ( const char* p = result; *p; ) {
        const char* nl = strchr( p, '\n' );
        size_t n = nl ? (size_t)( nl - p ) : strlen( p );
        if ( n > 0 ) {
            mp_obj_list_append( list_obj, mp_obj_new_str( p, n ) );
        }
        p += n + ( nl ? 1 : 0 );
    }
    return list_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_fs_listdir_obj, jl_fs_listdir_func );

static mp_obj_t jl_fs_read_file_func( mp_obj_t path_obj ) {
    const char* path = mp_obj_str_get_str( path_obj );
    char* content = jl_fs_read_file( path );
    if ( content == NULL ) {
        return mp_const_none;
    }
    // ponytail: this convenience API truncates at the C side's static buffer
    // (4KB) and at the first NUL byte (strlen below - the char* contract has no
    // length channel). Binary-safe reads: jfs.open(path).read().
    mp_obj_t content_obj = mp_obj_new_str( content, strlen( content ) );
    return content_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_fs_read_file_obj, jl_fs_read_file_func );

static mp_obj_t jl_fs_write_file_func( mp_obj_t path_obj, mp_obj_t content_obj ) {
    const char* path = mp_obj_str_get_str( path_obj );
    // Pass the true byte length so content with embedded NULs is written
    // intact (the old strlen-based path silently truncated at the first NUL).
    size_t len;
    const char* content = mp_obj_str_get_data( content_obj, &len );
    int result = jl_fs_write_file( path, content, (int)len );
    return mp_obj_new_bool( result == 1 );
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_fs_write_file_obj, jl_fs_write_file_func );

static mp_obj_t jl_fs_get_current_dir_func( void ) {
    char* current_dir = jl_fs_get_current_dir( );
    if ( current_dir == NULL ) {
        return mp_obj_new_str( "/", 1 );
    }
    mp_obj_t dir_obj = mp_obj_new_str( current_dir, strlen( current_dir ) );
    return dir_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_fs_get_current_dir_obj, jl_fs_get_current_dir_func );

//==============================================================================
// JFS (Jumperless FileSystem) Module - Comprehensive File I/O
//==============================================================================

// File object type
typedef struct _mp_obj_jfs_file_t {
    mp_obj_base_t base;
    void* file_handle;
    bool is_open;
    bool is_binary;
    char filename[256];  // Store filename to avoid static buffer issues - MUST be initialized!
} mp_obj_jfs_file_t;

// File type declaration
const mp_obj_type_t mp_type_jfs_file;

// File object methods
static mp_obj_t jfs_file_read( size_t n_args, const mp_obj_t* args ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( args[ 0 ] );
    
    if ( !self->is_open || !self->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }

    // Determine how many bytes to read
    mp_int_t requested = -1; // no size (or a negative one) means read-all
    if ( n_args > 1 ) {
        requested = mp_obj_get_int( args[ 1 ] );
    }
    size_t size;
    if ( requested < 0 ) {
        // True read-all: sized by the bytes remaining from the current
        // position (no more silent 8KB cap). A file too big for the GC heap
        // raises MemoryError, same as upstream VFS reads.
        int avail = jl_fs_available( self->file_handle );
        size = avail > 0 ? (size_t)avail : 0;
    } else {
        size = (size_t)requested;
    }
    if ( size == 0 ) {
        // read(0) or nothing left to read: empty result, not an error
        return self->is_binary ? mp_obj_new_bytes( (const byte*)"", 0 ) : mp_obj_new_str( "", 0 );
    }

    // Use MicroPython's memory allocator for better GC integration
    vstr_t vstr;
    vstr_init_len( &vstr, size );

    int bytes_read = jl_fs_read_bytes( self->file_handle, vstr.buf, size );
    if ( bytes_read < 0 ) {
        vstr_clear( &vstr );
        mp_raise_OSError( 5 ); // EIO
    }

    // Adjust length to actual bytes read
    vstr.len = bytes_read;

    mp_obj_t result;
    if ( self->is_binary ) {
        // Return bytes for binary mode; mp_obj_new_bytes copies the buffer
        result = mp_obj_new_bytes( (const byte*)vstr.buf, vstr.len );
        vstr_clear( &vstr ); // Safe to free since bytes object owns its own copy
    } else {
        // Convert to string (takes ownership of vstr buffer)
        result = mp_obj_new_str_from_vstr( &vstr );
    }

    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jfs_file_read_obj, 1, 2, jfs_file_read );

static mp_obj_t jfs_file_write( mp_obj_t self_in, mp_obj_t data_obj ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( self_in );
    if ( !self->is_open || !self->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }

    const char* data = NULL;
    size_t len = 0;
    mp_buffer_info_t bufinfo;

    if ( self->is_binary ) {
        // Binary mode: accept any buffer-protocol object (bytes/bytearray/memoryview)
        mp_get_buffer_raise( data_obj, &bufinfo, MP_BUFFER_READ );
        data = (const char*)bufinfo.buf;
        len = bufinfo.len;
    } else {
        // Text mode: allow str/bytes and coerce to a string buffer
        if ( mp_obj_is_str_or_bytes( data_obj ) ) {
            mp_get_buffer_raise( data_obj, &bufinfo, MP_BUFFER_READ );
            data = (const char*)bufinfo.buf;
            len = bufinfo.len;
        } else {
            data = mp_obj_str_get_data( data_obj, &len );
        }
    }

    if ( len == 0 ) {
        return mp_obj_new_int( 0 ); // write('') is a no-op, not an EIO (C side rejects size<=0)
    }

    int bytes_written = jl_fs_write_bytes( self->file_handle, data, len );
    if ( bytes_written < 0 ) {
        mp_raise_OSError( 5 ); // EIO
    }

    // NOTE: No auto-flush - too slow on embedded FatFS (~2 sec per flush!)
    // Data is flushed automatically on seek() or close(), or call flush() manually

    return mp_obj_new_int( bytes_written );
}
static MP_DEFINE_CONST_FUN_OBJ_2( jfs_file_write_obj, jfs_file_write );

// print() method - works like Python's print() with automatic newline
// Usage: file.print(arg1, arg2, ...) - prints args separated by spaces, adds newline
// NOTE: No auto-flush - call flush() manually or data is flushed on seek()/close()
// OPTIMIZATION: Build the complete string first, then write once to minimize Core2 pauses
static mp_obj_t jfs_file_print( size_t n_args, const mp_obj_t* args ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( args[ 0 ] );
    if ( !self->is_open || !self->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }
    if ( self->is_binary ) {
        mp_raise_ValueError( "print not supported on binary file" );
    }

    // Build complete output string first to minimize flash write operations
    // Each write pauses Core2, so batching is critical for stability
    vstr_t output;
    vstr_init( &output, 64 ); // Start with reasonable size

    // Print each argument, separated by spaces
    for ( size_t i = 1; i < n_args; i++ ) {
        // Convert argument to string
        mp_print_t print;
        vstr_t vstr;
        vstr_init_print( &vstr, 16, &print );
        mp_obj_print_helper( &print, args[ i ], PRINT_STR );

        // Append to output buffer
        vstr_add_strn( &output, vstr.buf, vstr.len );
        vstr_clear( &vstr );

        // Add space separator between arguments (not after last one)
        if ( i < n_args - 1 ) {
            vstr_add_char( &output, ' ' );
        }
    }

    // Add newline at end
    vstr_add_char( &output, '\n' );

    // Single write for the entire output - only one Core2 pause!
    int written = jl_fs_write_bytes( self->file_handle, output.buf, output.len );
    vstr_clear( &output );

    if ( written < 0 ) {
        mp_raise_OSError( 5 ); // EIO
    }

    return mp_obj_new_int( written );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR( jfs_file_print_obj, 1, jfs_file_print );

static mp_obj_t jfs_file_seek( size_t n_args, const mp_obj_t* args ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( args[ 0 ] );
    if ( !self->is_open || !self->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }

    // CRITICAL: Flush before seeking to ensure all written data is on disk
    // This prevents read-after-write issues where buffered writes aren't visible
    jl_fs_flush( self->file_handle );

    int position = mp_obj_get_int( args[ 1 ] );
    int whence = 0; // Default to SEEK_SET
    if ( n_args > 2 ) {
        whence = mp_obj_get_int( args[ 2 ] );
    }

    int result = jl_fs_seek( self->file_handle, position, whence );
    return mp_obj_new_bool( result );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jfs_file_seek_obj, 2, 3, jfs_file_seek );

static mp_obj_t jfs_file_tell( mp_obj_t self_in ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( self_in );
    if ( !self->is_open || !self->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }

    int position = jl_fs_position( self->file_handle );
    return mp_obj_new_int( position );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_file_tell_obj, jfs_file_tell );

static mp_obj_t jfs_file_size( mp_obj_t self_in ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( self_in );
    if ( !self->is_open || !self->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }

    int size = jl_fs_size( self->file_handle );
    return mp_obj_new_int( size );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_file_size_obj, jfs_file_size );

static mp_obj_t jfs_file_available( mp_obj_t self_in ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( self_in );
    if ( !self->is_open || !self->file_handle ) {
        return mp_obj_new_int( 0 );
    }

    int available = jl_fs_available( self->file_handle );
    return mp_obj_new_int( available );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_file_available_obj, jfs_file_available );

static mp_obj_t jfs_file_name( mp_obj_t self_in ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( self_in );
    if ( !self->is_open || !self->file_handle ) {
        return mp_const_none;
    }

    // Use stored filename instead of calling jl_fs_name() to avoid static buffer issues
    return mp_obj_new_str( self->filename, strlen( self->filename ) );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_file_name_obj, jfs_file_name );

static mp_obj_t jfs_file_close( mp_obj_t self_in ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( self_in );
    
    if ( self->is_open && self->file_handle ) {
        if ( !jl_fs_close_file( self->file_handle ) ) {
            // fs_mutex held by the other core past the timeout: the file is
            // still open and tracked. Report it (caller may retry close())
            // instead of marking the object closed and leaking the slot.
            mp_raise_OSError( MP_EBUSY );
        }
        self->file_handle = NULL;
        self->is_open = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_file_close_obj, jfs_file_close );

// Context manager methods for 'with' statement support
static mp_obj_t jfs_file_enter( mp_obj_t self_in ) {
    // Just return self for context manager
    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_file_enter_obj, jfs_file_enter );

static mp_obj_t jfs_file_exit( size_t n_args, const mp_obj_t* args ) {
    // args[0] is self, args[1-3] are exception info (exc_type, exc_val, exc_tb)
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( args[ 0 ] );

    // Always close the file when exiting context
    if ( self->is_open && self->file_handle ) {
        if ( !jl_fs_close_file( self->file_handle ) ) {
            // See jfs_file_close: don't silently mark a still-open file closed
            mp_raise_OSError( MP_EBUSY );
        }
        self->file_handle = NULL;
        self->is_open = false;
    }

    // Return False to not suppress any exceptions
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jfs_file_exit_obj, 4, 4, jfs_file_exit );

// Flush buffered data to disk - CRITICAL for read-after-write operations
static mp_obj_t jfs_file_flush( mp_obj_t self_in ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( self_in );
    if ( !self->is_open || !self->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }
    jl_fs_flush( self->file_handle );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_file_flush_obj, jfs_file_flush );

// Forward declaration for finalizer
static mp_obj_t jfs_file_del( mp_obj_t self_in );
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_file_del_obj, jfs_file_del );

// File object locals dict
static const mp_rom_map_elem_t jfs_file_locals_dict_table[] = {
    { MP_ROM_QSTR( MP_QSTR_read ), MP_ROM_PTR( &jfs_file_read_obj ) },
    { MP_ROM_QSTR( MP_QSTR_write ), MP_ROM_PTR( &jfs_file_write_obj ) },
    { MP_ROM_QSTR( MP_QSTR_print ), MP_ROM_PTR( &jfs_file_print_obj ) }, // Like print() but to file, auto-flush
    { MP_ROM_QSTR( MP_QSTR_seek ), MP_ROM_PTR( &jfs_file_seek_obj ) },
    { MP_ROM_QSTR( MP_QSTR_tell ), MP_ROM_PTR( &jfs_file_tell_obj ) },
    { MP_ROM_QSTR( MP_QSTR_position ), MP_ROM_PTR( &jfs_file_tell_obj ) }, // Alias
    { MP_ROM_QSTR( MP_QSTR_size ), MP_ROM_PTR( &jfs_file_size_obj ) },
    { MP_ROM_QSTR( MP_QSTR_available ), MP_ROM_PTR( &jfs_file_available_obj ) },
    { MP_ROM_QSTR( MP_QSTR_name ), MP_ROM_PTR( &jfs_file_name_obj ) },
    { MP_ROM_QSTR( MP_QSTR_close ), MP_ROM_PTR( &jfs_file_close_obj ) },
    { MP_ROM_QSTR( MP_QSTR_flush ), MP_ROM_PTR( &jfs_file_flush_obj ) },

    // Context manager methods for 'with' statement support
    { MP_ROM_QSTR( MP_QSTR___enter__ ), MP_ROM_PTR( &jfs_file_enter_obj ) },
    { MP_ROM_QSTR( MP_QSTR___exit__ ), MP_ROM_PTR( &jfs_file_exit_obj ) },

    // Destructor for cleanup when garbage collected
    { MP_ROM_QSTR( MP_QSTR___del__ ), MP_ROM_PTR( &jfs_file_del_obj ) },
};
static MP_DEFINE_CONST_DICT( jfs_file_locals_dict, jfs_file_locals_dict_table );

// Finalizer implementation for JFS file objects - called by GC to clean up C++ File handle
// This is CRITICAL to prevent memory leaks when file objects are garbage collected
// without being explicitly closed (e.g., when scripts crash or variables go out of scope)
// NOTE: This runs during gc_sweep_run_finalisers() while the scheduler is locked.
// WARNING: Do NOT call jl_fs_flush() separately here - jl_fs_close_file() already flushes,
// and calling both would cause a mutex DEADLOCK since pico SDK mutexes are not recursive!
static mp_obj_t jfs_file_del( mp_obj_t self_in ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( self_in );
    if ( self->is_open && self->file_handle ) {
        // jl_fs_close_file() includes flush before close - do NOT call jl_fs_flush separately!
        // A finaliser can't raise: on mutex-timeout failure the handle stays
        // tracked and jl_close_all_jfs_files() reclaims it at script exit.
        (void)jl_fs_close_file( self->file_handle );
        self->file_handle = NULL;
        self->is_open = false;
    }
    return mp_const_none;
}

// ============================================================================
// STREAM PROTOCOL IMPLEMENTATION
// Required for VFS reader to import .py files from filesystem
// ============================================================================

// Stream protocol read function - called by mp_stream_rw() during imports
// Returns number of bytes read, or MP_STREAM_ERROR on failure
static mp_uint_t jfs_file_stream_read( mp_obj_t self_in, void* buf, mp_uint_t size, int* errcode ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( self_in );
    
    // Validate file is open
    if ( !self->is_open || !self->file_handle ) {
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
    
    // Read bytes from JFS
    int bytes_read = jl_fs_read_bytes( self->file_handle, (char*)buf, size );
    
    if ( bytes_read < 0 ) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    
    return (mp_uint_t)bytes_read;
}

// Stream protocol write function - called by file write operations
// Returns number of bytes written, or MP_STREAM_ERROR on failure
static mp_uint_t jfs_file_stream_write( mp_obj_t self_in, const void* buf, mp_uint_t size, int* errcode ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( self_in );
    
    // Validate file is open
    if ( !self->is_open || !self->file_handle ) {
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
    
    // Write bytes to JFS
    int bytes_written = jl_fs_write_bytes( self->file_handle, (const char*)buf, size );
    
    if ( bytes_written < 0 ) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    
    return (mp_uint_t)bytes_written;
}

// Stream protocol ioctl function - handles close, seek, flush, etc.
// Called by VFS reader for various stream operations
static mp_uint_t jfs_file_stream_ioctl( mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int* errcode ) {
    mp_obj_jfs_file_t* self = MP_OBJ_TO_PTR( self_in );
    
    switch ( request ) {
        case MP_STREAM_CLOSE:
            // Close the file handle
            if ( self->is_open && self->file_handle ) {
                if ( !jl_fs_close_file( self->file_handle ) ) {
                    // Mutex timeout: still open+tracked - report, don't leak
                    *errcode = MP_EBUSY;
                    return MP_STREAM_ERROR;
                }
                self->file_handle = NULL;
                self->is_open = false;
            }
            return 0;
            
        case MP_STREAM_SEEK: {
            // Seek to a position in the file
            if ( !self->is_open || !self->file_handle ) {
                *errcode = MP_EINVAL;
                return MP_STREAM_ERROR;
            }
            
            struct mp_stream_seek_t* seek_s = (struct mp_stream_seek_t*)arg;
            int result = jl_fs_seek( self->file_handle, seek_s->offset, seek_s->whence );
            
            if ( !result ) {
                *errcode = MP_EIO;
                return MP_STREAM_ERROR;
            }
            
            // ioctl contract (see py/stream.h and objstringio.c): write the
            // resulting position back into seek_s->offset and return 0.
            // Returning the position directly would be misread as an error
            // code by C-level stream consumers.
            int new_pos = jl_fs_position( self->file_handle );
            if ( new_pos < 0 ) {
                *errcode = MP_EIO;
                return MP_STREAM_ERROR;
            }
            seek_s->offset = new_pos;
            return 0;
        }
            
        case MP_STREAM_FLUSH:
            // Flush buffered writes to disk
            if ( self->is_open && self->file_handle ) {
                jl_fs_flush( self->file_handle );
            }
            return 0;
            
        case MP_STREAM_GET_BUFFER_SIZE:
            // Return preferred buffer size for reading
            // VFS reader uses this to allocate read buffer
            // Return 64 bytes as a reasonable chunk size for our filesystem
            return 64;
            
        default:
            *errcode = MP_EINVAL;
            return MP_STREAM_ERROR;
    }
}

// Stream protocol structure - MicroPython uses this to read/write files
static const mp_stream_p_t jfs_file_stream_p = {
    .read = jfs_file_stream_read,
    .write = jfs_file_stream_write,
    .ioctl = jfs_file_stream_ioctl,
    .is_text = 0,  // Binary mode by default (text mode handled by wrapper)
};

// File type definition with finalizer AND stream protocol
// The MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS flag enables the __del__ method to be called during GC
// The protocol slot enables VFS reader to import .py files
MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_jfs_file,
    MP_QSTR_JFSFile,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    protocol, &jfs_file_stream_p,
    locals_dict, &jfs_file_locals_dict );

// JFS module functions
static mp_obj_t jfs_open( size_t n_args, const mp_obj_t* args ) {
    const char* path = mp_obj_str_get_str( args[ 0 ] );
    const char* mode = "r"; // Default mode
    if ( n_args > 1 ) {
        mode = mp_obj_str_get_str( args[ 1 ] );
    }
    bool is_binary = strchr( mode, 'b' ) != NULL;

    void* file_handle = jl_fs_open_file( path, mode );
    if ( !file_handle ) {
        // ENOENT when the open itself failed, EMFILE when the 8-slot handle
        // table is full - tells the user to close files, not chase paths
        mp_raise_OSError( jl_fs_open_errno( ) );
    }

    // CRITICAL: Use mp_obj_malloc_with_finaliser() to register for GC finalizer callback
    // This ensures __del__ is called when the object is garbage collected,
    // which closes the underlying C++ File handle and prevents memory leaks
    // Note: mp_obj_malloc_with_finaliser sets the type for us
    mp_obj_jfs_file_t* file_obj = mp_obj_malloc_with_finaliser( mp_obj_jfs_file_t, &mp_type_jfs_file );
    
    // CRITICAL: Initialize ALL fields before ANY operations
    // Store filename FIRST to avoid crashes if anything tries to access it
    strncpy( file_obj->filename, path, sizeof( file_obj->filename ) - 1 );
    file_obj->filename[ sizeof( file_obj->filename ) - 1 ] = '\0';
    
    file_obj->file_handle = file_handle;
    file_obj->is_open = true;
    file_obj->is_binary = is_binary;

    return MP_OBJ_FROM_PTR( file_obj );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jfs_open_obj, 1, 2, jfs_open );

static mp_obj_t jfs_exists( mp_obj_t path_obj ) {
    const char* path = mp_obj_str_get_str( path_obj );
    int exists = jl_fs_exists( path );
    return mp_obj_new_bool( exists );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_exists_obj, jfs_exists );

// Same behavior as jl.fs_listdir: raises ENOENT on a missing dir, parses the
// '\n'-separated producer buffer (see jl_fs_listdir_func).
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_listdir_obj, jl_fs_listdir_func );

static mp_obj_t jfs_mkdir( mp_obj_t path_obj ) {
    const char* path = mp_obj_str_get_str( path_obj );
    int result = jl_fs_mkdir( path );
    if ( result < 0 ) {
        // jl_fs_mkdir returns negative errno on failure
        mp_raise_OSError( -result );
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_mkdir_obj, jfs_mkdir );

static mp_obj_t jfs_rmdir( mp_obj_t path_obj ) {
    const char* path = mp_obj_str_get_str( path_obj );
    int result = jl_fs_rmdir( path );
    if ( !result ) {
        mp_raise_OSError( 2 ); // ENOENT - failed to remove directory
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_rmdir_obj, jfs_rmdir );

static mp_obj_t jfs_remove( mp_obj_t path_obj ) {
    const char* path = mp_obj_str_get_str( path_obj );
    int result = jl_fs_remove( path );
    if ( !result ) {
        mp_raise_OSError( 2 ); // ENOENT - failed to remove file
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_remove_obj, jfs_remove );

static mp_obj_t jfs_rename( mp_obj_t from_obj, mp_obj_t to_obj ) {
    const char* from_path = mp_obj_str_get_str( from_obj );
    const char* to_path = mp_obj_str_get_str( to_obj );
    int result = jl_fs_rename( from_path, to_path );
    if ( !result ) {
        mp_raise_OSError( 2 ); // ENOENT - failed to rename file
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jfs_rename_obj, jfs_rename );

static mp_obj_t jfs_stat( mp_obj_t path_obj ) {
    const char* path = mp_obj_str_get_str( path_obj );

    // Simple stat implementation - just check if file exists and get basic info
    if ( !jl_fs_exists( path ) ) {
        mp_raise_OSError( 2 ); // ENOENT
    }

    // Path-based size/type queries: doesn't burn one of the 8 handle slots
    // (the old open-for-size did, and reported directories as regular files)
    int isdir = jl_fs_stat_isdir( path );
    int size = isdir ? 0 : jl_fs_stat_size( path );
    if ( size < 0 ) {
        size = 0;
    }

    // Return a simple tuple with (mode, ino, dev, nlink, uid, gid, size, atime, mtime, ctime)
    mp_obj_t tuple[ 10 ];
    tuple[ 0 ] = mp_obj_new_int( isdir ? 0x4000 : 0x8000 ); // S_IFDIR : S_IFREG
    tuple[ 1 ] = mp_obj_new_int( 0 );      // inode
    tuple[ 2 ] = mp_obj_new_int( 0 );      // device
    tuple[ 3 ] = mp_obj_new_int( 1 );      // nlink
    tuple[ 4 ] = mp_obj_new_int( 0 );      // uid
    tuple[ 5 ] = mp_obj_new_int( 0 );      // gid
    tuple[ 6 ] = mp_obj_new_int( size );   // size
    tuple[ 7 ] = mp_obj_new_int( 0 );      // atime
    tuple[ 8 ] = mp_obj_new_int( 0 );      // mtime
    tuple[ 9 ] = mp_obj_new_int( 0 );      // ctime

    return mp_obj_new_tuple( 10, tuple );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_stat_obj, jfs_stat );

static mp_obj_t jfs_info( void ) {
    int total = jl_fs_total_bytes( );
    int used = jl_fs_used_bytes( );
    int free = total - used;

    mp_obj_t tuple[ 3 ];
    tuple[ 0 ] = mp_obj_new_int( total );
    tuple[ 1 ] = mp_obj_new_int( used );
    tuple[ 2 ] = mp_obj_new_int( free );

    return mp_obj_new_tuple( 3, tuple );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jfs_info_obj, jfs_info );

// File handle operations as module functions
static mp_obj_t jfs_read( size_t n_args, const mp_obj_t* args ) {
    mp_obj_jfs_file_t* file = MP_OBJ_TO_PTR( args[ 0 ] );
    if ( !file->is_open || !file->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }

    mp_int_t requested = 1024; // Default read size
    if ( n_args > 1 ) {
        requested = mp_obj_get_int( args[ 1 ] );
    }
    size_t size;
    if ( requested < 0 ) {
        // Negative size: read everything from the current position
        int avail = jl_fs_available( file->file_handle );
        size = avail > 0 ? (size_t)avail : 0;
    } else {
        size = (size_t)requested;
    }
    if ( size == 0 ) {
        return mp_obj_new_str( "", 0 );
    }

    // GC-heap buffer (like jfs_file_read): if mp_obj_new_str_from_vstr raises
    // UnicodeError on non-UTF-8 data, the buffer is collected - the old libc
    // malloc here leaked on that path.
    vstr_t vstr;
    vstr_init_len( &vstr, size );

    int bytes_read = jl_fs_read_bytes( file->file_handle, vstr.buf, size );
    if ( bytes_read < 0 ) {
        vstr_clear( &vstr );
        mp_raise_OSError( 5 ); // EIO
    }

    vstr.len = bytes_read;
    return mp_obj_new_str_from_vstr( &vstr );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jfs_read_obj, 1, 2, jfs_read );

static mp_obj_t jfs_write( mp_obj_t file_obj, mp_obj_t data_obj ) {
    mp_obj_jfs_file_t* file = MP_OBJ_TO_PTR( file_obj );
    if ( !file->is_open || !file->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }

    // True byte length: data with embedded NULs is written intact (the old
    // strlen-based path silently truncated at the first NUL)
    size_t len;
    const char* data = mp_obj_str_get_data( data_obj, &len );

    if ( len == 0 ) {
        return mp_const_none; // write('') is a no-op, not an EIO (C side rejects size<=0)
    }

    int bytes_written = jl_fs_write_bytes( file->file_handle, data, len );
    if ( bytes_written < 0 ) {
        mp_raise_OSError( 5 ); // EIO
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jfs_write_obj, jfs_write );

static mp_obj_t jfs_close( mp_obj_t file_obj ) {
    mp_obj_jfs_file_t* file = MP_OBJ_TO_PTR( file_obj );
    if ( file->is_open && file->file_handle ) {
        if ( !jl_fs_close_file( file->file_handle ) ) {
            // See jfs_file_close: don't silently mark a still-open file closed
            mp_raise_OSError( MP_EBUSY );
        }
        file->file_handle = NULL;
        file->is_open = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_close_obj, jfs_close );

static mp_obj_t jfs_seek( size_t n_args, const mp_obj_t* args ) {
    mp_obj_jfs_file_t* file = MP_OBJ_TO_PTR( args[ 0 ] );
    if ( !file->is_open || !file->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }

    int position = mp_obj_get_int( args[ 1 ] );
    int whence = 0; // Default to SEEK_SET
    if ( n_args > 2 ) {
        whence = mp_obj_get_int( args[ 2 ] );
    }

    int result = jl_fs_seek( file->file_handle, position, whence );
    return mp_obj_new_bool( result );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jfs_seek_obj, 2, 3, jfs_seek );

static mp_obj_t jfs_tell( mp_obj_t file_obj ) {
    mp_obj_jfs_file_t* file = MP_OBJ_TO_PTR( file_obj );
    if ( !file->is_open || !file->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }

    int position = jl_fs_position( file->file_handle );
    return mp_obj_new_int( position );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_tell_obj, jfs_tell );

static mp_obj_t jfs_size( mp_obj_t file_obj ) {
    mp_obj_jfs_file_t* file = MP_OBJ_TO_PTR( file_obj );
    if ( !file->is_open || !file->file_handle ) {
        mp_raise_ValueError( "I/O operation on closed file" );
    }

    int size = jl_fs_size( file->file_handle );
    return mp_obj_new_int( size );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_size_obj, jfs_size );

static mp_obj_t jfs_available( mp_obj_t file_obj ) {
    mp_obj_jfs_file_t* file = MP_OBJ_TO_PTR( file_obj );
    if ( !file->is_open || !file->file_handle ) {
        return mp_obj_new_int( 0 );
    }

    int available = jl_fs_available( file->file_handle );
    return mp_obj_new_int( available );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jfs_available_obj, jfs_available );

// JFS module globals
static const mp_rom_map_elem_t jfs_module_globals_table[] = {
    { MP_ROM_QSTR( MP_QSTR___name__ ), MP_ROM_QSTR( MP_QSTR_jfs ) },

    // File operations
    { MP_ROM_QSTR( MP_QSTR_open ), MP_ROM_PTR( &jfs_open_obj ) },

    // File handle operations (module-level functions)
    { MP_ROM_QSTR( MP_QSTR_read ), MP_ROM_PTR( &jfs_read_obj ) },
    { MP_ROM_QSTR( MP_QSTR_write ), MP_ROM_PTR( &jfs_write_obj ) },
    { MP_ROM_QSTR( MP_QSTR_close ), MP_ROM_PTR( &jfs_close_obj ) },
    { MP_ROM_QSTR( MP_QSTR_seek ), MP_ROM_PTR( &jfs_seek_obj ) },
    { MP_ROM_QSTR( MP_QSTR_tell ), MP_ROM_PTR( &jfs_tell_obj ) },
    { MP_ROM_QSTR( MP_QSTR_size ), MP_ROM_PTR( &jfs_size_obj ) },
    { MP_ROM_QSTR( MP_QSTR_available ), MP_ROM_PTR( &jfs_available_obj ) },

    // Directory operations
    { MP_ROM_QSTR( MP_QSTR_exists ), MP_ROM_PTR( &jfs_exists_obj ) },
    { MP_ROM_QSTR( MP_QSTR_listdir ), MP_ROM_PTR( &jfs_listdir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_mkdir ), MP_ROM_PTR( &jfs_mkdir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_rmdir ), MP_ROM_PTR( &jfs_rmdir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_remove ), MP_ROM_PTR( &jfs_remove_obj ) },
    { MP_ROM_QSTR( MP_QSTR_rename ), MP_ROM_PTR( &jfs_rename_obj ) },
    { MP_ROM_QSTR( MP_QSTR_stat ), MP_ROM_PTR( &jfs_stat_obj ) },

    // Filesystem info
    { MP_ROM_QSTR( MP_QSTR_info ), MP_ROM_PTR( &jfs_info_obj ) },

    // Constants
    { MP_ROM_QSTR( MP_QSTR_SEEK_SET ), MP_ROM_INT( 0 ) },
    { MP_ROM_QSTR( MP_QSTR_SEEK_CUR ), MP_ROM_INT( 1 ) },
    { MP_ROM_QSTR( MP_QSTR_SEEK_END ), MP_ROM_INT( 2 ) },
};
static MP_DEFINE_CONST_DICT( jfs_module_globals, jfs_module_globals_table );

const mp_obj_module_t jfs_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&jfs_module_globals,
};

// Function to get current slot for MicroPython
extern int netSlot; // Reference to global netSlot variable

static mp_obj_t jl_get_current_slot( void ) {
    return mp_obj_new_int( netSlot );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_get_current_slot_obj, jl_get_current_slot );

// Connection context toggle functions
extern void jl_toggle_connection_context( void );
extern const char* jl_get_connection_context_name( void );

static mp_obj_t jl_context_toggle( void ) {
    jl_toggle_connection_context( );
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_context_toggle_obj, jl_context_toggle );

static mp_obj_t jl_context_get( void ) {
    const char* context = jl_get_connection_context_name( );
    return mp_obj_new_str( context, strlen( context ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_context_get_obj, jl_context_get );

//==============================================================================
// Jumperless VFS driver (bridges jl_fs_* to MicroPython's native VFS layer)
//==============================================================================
#if MICROPY_VFS || 1

// extmod/vfs.h isn't part of the embedded subset we compile, so pull in the
// minimal pieces we need here to avoid another copy of the header.
#ifndef MICROPY_INCLUDED_EXTMOD_VFS_H
typedef struct _mp_vfs_proto_t {
    mp_import_stat_t ( *import_stat )( void* self, const char* path );
} mp_vfs_proto_t;
#endif

// Prototypes from extmod/vfs.c (linked from micropython_repo)
extern mp_obj_t mp_vfs_mount( size_t n_args, const mp_obj_t* pos_args, mp_map_t* kw_args );
extern mp_obj_t mp_vfs_chdir( mp_obj_t path_in );

typedef struct _mp_obj_vfs_jl_t {
    mp_obj_base_t base;
} mp_obj_vfs_jl_t;

static char jl_vfs_cwd[ 256 ] = "/";

static mp_import_stat_t jl_vfs_import_stat( void* self, const char* path ) {
    (void)self;
    
    // CRITICAL: Null check - import system may pass NULL paths
    if ( !path || path[0] == '\0' ) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    
    // Check directory first (more common for package lookups)
    if ( jl_fs_stat_isdir( path ) ) {
        return MP_IMPORT_STAT_DIR;
    }
    
    // Then check if it's a regular file
    if ( jl_fs_exists( path ) ) {
        return MP_IMPORT_STAT_FILE;
    }
    
    return MP_IMPORT_STAT_NO_EXIST;
}

// mount(readonly=False, mkfs=False) - no-op for jl filesystem
static mp_obj_t jl_vfs_mount_method( size_t n_args, const mp_obj_t* args ) {
    (void)n_args;
    (void)args;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_vfs_mount_obj, 1, 3, jl_vfs_mount_method );

static mp_obj_t jl_vfs_umount( mp_obj_t self_in ) {
    (void)self_in;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_vfs_umount_obj, jl_vfs_umount );

// open(path, mode)
static mp_obj_t jl_vfs_open_method( mp_obj_t self_in, mp_obj_t path_obj, mp_obj_t mode_obj ) {
    (void)self_in;
    const char* path = mp_obj_str_get_str( path_obj );
    const char* mode = mp_obj_str_get_str( mode_obj );
    
    // CRITICAL: Validate inputs before any operations
    if ( !path || path[0] == '\0' ) {
        mp_raise_OSError( MP_EINVAL );
    }
    if ( !mode || mode[0] == '\0' ) {
        mode = "r";  // Default to read mode
    }
    
    bool is_binary = strchr( mode, 'b' ) != NULL;

    void* file_handle = jl_fs_open_file( path, mode );
    if ( !file_handle ) {
        // ENOENT for open failures, EMFILE when the handle table is full
        mp_raise_OSError( jl_fs_open_errno( ) );
    }

    // Allocate file object with finalizer for automatic cleanup
    mp_obj_jfs_file_t* file_obj = mp_obj_malloc_with_finaliser( mp_obj_jfs_file_t, &mp_type_jfs_file );
    
    // CRITICAL: Initialize ALL fields in order - filename FIRST
    // This ensures the object is always in a valid state
    strncpy( file_obj->filename, path, sizeof( file_obj->filename ) - 1 );
    file_obj->filename[ sizeof( file_obj->filename ) - 1 ] = '\0';
    
    file_obj->file_handle = file_handle;
    file_obj->is_open = true;
    file_obj->is_binary = is_binary;
    
    return MP_OBJ_FROM_PTR( file_obj );
}
static MP_DEFINE_CONST_FUN_OBJ_3( jl_vfs_open_obj, jl_vfs_open_method );

// chdir(path)
static mp_obj_t jl_vfs_chdir_method( mp_obj_t self_in, mp_obj_t path_obj ) {
    (void)self_in;
    const char* path = mp_obj_str_get_str( path_obj );

    if ( path[ 0 ] == '/' ) {
        strncpy( jl_vfs_cwd, path, sizeof( jl_vfs_cwd ) - 1 );
        jl_vfs_cwd[ sizeof( jl_vfs_cwd ) - 1 ] = '\0';
    } else {
        size_t cwd_len = strlen( jl_vfs_cwd );
        if ( cwd_len > 1 && jl_vfs_cwd[ cwd_len - 1 ] != '/' ) {
            strncat( jl_vfs_cwd, "/", sizeof( jl_vfs_cwd ) - cwd_len - 1 );
        }
        strncat( jl_vfs_cwd, path, sizeof( jl_vfs_cwd ) - strlen( jl_vfs_cwd ) - 1 );
    }

    size_t len = strlen( jl_vfs_cwd );
    if ( len > 1 && jl_vfs_cwd[ len - 1 ] == '/' ) {
        jl_vfs_cwd[ len - 1 ] = '\0';
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_vfs_chdir_obj, jl_vfs_chdir_method );

// getcwd()
static mp_obj_t jl_vfs_getcwd_method( mp_obj_t self_in ) {
    (void)self_in;
    return mp_obj_new_str( jl_vfs_cwd, strlen( jl_vfs_cwd ) );
}
static MP_DEFINE_CONST_FUN_OBJ_1( jl_vfs_getcwd_obj, jl_vfs_getcwd_method );

// ilistdir(path='')
static mp_obj_t jl_vfs_ilistdir_method( mp_obj_t self_in, mp_obj_t path_obj ) {
    (void)self_in;
    const char* path = mp_obj_str_get_str( path_obj );
    if ( path[ 0 ] == '\0' ) {
        path = jl_vfs_cwd;
    }

    char* result = jl_fs_listdir( path );
    if ( result == NULL ) {
        mp_raise_OSError( MP_ENOENT ); // directory doesn't exist
    }

    // Entries are '\n'-separated (see jl_fs_listdir_func); parse straight out
    // of the producer's static buffer - no copy, no strtok, and nothing leaks
    // if an allocation below raises. The per-entry stat calls don't touch the
    // producer's buffer, so parsing in place is safe.
    mp_obj_t list_obj = mp_obj_new_list( 0, NULL );
    for ( const char* p = result; *p; ) {
        const char* nl = strchr( p, '\n' );
        size_t entry_len = nl ? (size_t)( nl - p ) : strlen( p );
        // Strip any trailing slash returned by jl_fs_listdir to match standard os.listdir
        size_t name_len = entry_len;
        if ( name_len > 0 && p[ name_len - 1 ] == '/' ) {
            name_len -= 1;
        }
        if ( name_len > 0 ) {
            char fullpath[ 256 ];
            if ( path[ 0 ] == '/' && path[ 1 ] == '\0' ) {
                snprintf( fullpath, sizeof( fullpath ), "/%.*s", (int)name_len, p );
            } else {
                snprintf( fullpath, sizeof( fullpath ), "%s/%.*s", path, (int)name_len, p );
            }

            int isdir = jl_fs_stat_isdir( fullpath );
            int size = jl_fs_stat_size( fullpath );

            mp_obj_t items[ 4 ] = {
                mp_obj_new_str( p, name_len ),
                MP_OBJ_NEW_SMALL_INT( isdir ? 0x4000 : 0x8000 ),
                MP_OBJ_NEW_SMALL_INT( 0 ),
                MP_OBJ_NEW_SMALL_INT( size < 0 ? 0 : size ) };
            mp_obj_list_append( list_obj, mp_obj_new_tuple( 4, items ) );
        }
        p += entry_len + ( nl ? 1 : 0 );
    }

    return mp_getiter( list_obj, NULL );
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_vfs_ilistdir_obj, jl_vfs_ilistdir_method );

// stat(path)
static mp_obj_t jl_vfs_stat_method( mp_obj_t self_in, mp_obj_t path_obj ) {
    (void)self_in;
    const char* path = mp_obj_str_get_str( path_obj );

    int size = jl_fs_stat_size( path );
    int isdir = jl_fs_stat_isdir( path );

    if ( size < 0 && !isdir ) {
        mp_raise_OSError( ENOENT );
    }

    int mode = isdir ? ( 0x4000 | 0x1FF ) : ( 0x8000 | 0x1FF );

    mp_obj_t items[ 10 ] = {
        MP_OBJ_NEW_SMALL_INT( mode ),
        MP_OBJ_NEW_SMALL_INT( 0 ),
        MP_OBJ_NEW_SMALL_INT( 0 ),
        MP_OBJ_NEW_SMALL_INT( 1 ),
        MP_OBJ_NEW_SMALL_INT( 0 ),
        MP_OBJ_NEW_SMALL_INT( 0 ),
        MP_OBJ_NEW_SMALL_INT( size < 0 ? 0 : size ),
        MP_OBJ_NEW_SMALL_INT( 0 ),
        MP_OBJ_NEW_SMALL_INT( 0 ),
        MP_OBJ_NEW_SMALL_INT( 0 ),
    };

    return mp_obj_new_tuple( 10, items );
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_vfs_stat_obj, jl_vfs_stat_method );

// mkdir(path)
static mp_obj_t jl_vfs_mkdir_method( mp_obj_t self_in, mp_obj_t path_obj ) {
    (void)self_in;
    const char* path = mp_obj_str_get_str( path_obj );
    int result = jl_fs_mkdir( path );
    if ( result < 0 ) {
        // jl_fs_mkdir returns negative errno on failure
        mp_raise_OSError( -result );
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_vfs_mkdir_obj, jl_vfs_mkdir_method );

// rmdir(path)
static mp_obj_t jl_vfs_rmdir_method( mp_obj_t self_in, mp_obj_t path_obj ) {
    (void)self_in;
    const char* path = mp_obj_str_get_str( path_obj );
    if ( !jl_fs_rmdir( path ) ) {
        mp_raise_OSError( EIO );
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_vfs_rmdir_obj, jl_vfs_rmdir_method );

// remove(path)
static mp_obj_t jl_vfs_remove_method( mp_obj_t self_in, mp_obj_t path_obj ) {
    (void)self_in;
    const char* path = mp_obj_str_get_str( path_obj );
    if ( !jl_fs_remove( path ) ) {
        mp_raise_OSError( EIO );
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_vfs_remove_obj, jl_vfs_remove_method );

// rename(old, new)
static mp_obj_t jl_vfs_rename_method( mp_obj_t self_in, mp_obj_t old_obj, mp_obj_t new_obj ) {
    (void)self_in;
    const char* old_path = mp_obj_str_get_str( old_obj );
    const char* new_path = mp_obj_str_get_str( new_obj );
    if ( !jl_fs_rename( old_path, new_path ) ) {
        mp_raise_OSError( EIO );
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3( jl_vfs_rename_obj, jl_vfs_rename_method );

// statvfs(path) - minimal info (block size, total, free)
static mp_obj_t jl_vfs_statvfs_method( mp_obj_t self_in, mp_obj_t path_obj ) {
    (void)self_in;
    (void)path_obj;
    int total = jl_fs_total_bytes( );
    int used = jl_fs_used_bytes( );
    int free = total - used;

    // Report a 1-byte block so f_blocks/f_bfree count raw bytes directly. This
    // keeps the standard MicroPython usage recipe correct:
    //   size = f_frsize * f_blocks   (s[1] * s[2])
    //   avail = f_bsize  * f_bfree   (s[0] * s[3])
    // Returning 0 for f_bsize/f_frsize (as before) made both products 0, so
    // tools like JumperIDE reported the filesystem as empty/0 bytes.
    mp_obj_t items[ 10 ] = {
        MP_OBJ_NEW_SMALL_INT( 1 ),     // f_bsize
        MP_OBJ_NEW_SMALL_INT( 1 ),     // f_frsize
        MP_OBJ_NEW_SMALL_INT( total ), // f_blocks
        MP_OBJ_NEW_SMALL_INT( free ),  // f_bfree
        MP_OBJ_NEW_SMALL_INT( free ),  // f_bavail
        MP_OBJ_NEW_SMALL_INT( 0 ),     // f_files
        MP_OBJ_NEW_SMALL_INT( 0 ),     // f_ffree
        MP_OBJ_NEW_SMALL_INT( 0 ),     // f_favail
        MP_OBJ_NEW_SMALL_INT( 0 ),     // f_flag
        MP_OBJ_NEW_SMALL_INT( 0 ),     // f_namemax
    };
    return mp_obj_new_tuple( 10, items );
}
static MP_DEFINE_CONST_FUN_OBJ_2( jl_vfs_statvfs_obj, jl_vfs_statvfs_method );

static const mp_rom_map_elem_t jl_vfs_locals_dict_table[] = {
    { MP_ROM_QSTR( MP_QSTR_mount ), MP_ROM_PTR( &jl_vfs_mount_obj ) },
    { MP_ROM_QSTR( MP_QSTR_umount ), MP_ROM_PTR( &jl_vfs_umount_obj ) },
    { MP_ROM_QSTR( MP_QSTR_open ), MP_ROM_PTR( &jl_vfs_open_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ilistdir ), MP_ROM_PTR( &jl_vfs_ilistdir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_listdir ), MP_ROM_PTR( &jl_vfs_ilistdir_obj ) }, // simple alias
    { MP_ROM_QSTR( MP_QSTR_stat ), MP_ROM_PTR( &jl_vfs_stat_obj ) },
    { MP_ROM_QSTR( MP_QSTR_statvfs ), MP_ROM_PTR( &jl_vfs_statvfs_obj ) },
    { MP_ROM_QSTR( MP_QSTR_mkdir ), MP_ROM_PTR( &jl_vfs_mkdir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_rmdir ), MP_ROM_PTR( &jl_vfs_rmdir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_remove ), MP_ROM_PTR( &jl_vfs_remove_obj ) },
    { MP_ROM_QSTR( MP_QSTR_unlink ), MP_ROM_PTR( &jl_vfs_remove_obj ) },
    { MP_ROM_QSTR( MP_QSTR_rename ), MP_ROM_PTR( &jl_vfs_rename_obj ) },
    { MP_ROM_QSTR( MP_QSTR_chdir ), MP_ROM_PTR( &jl_vfs_chdir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_getcwd ), MP_ROM_PTR( &jl_vfs_getcwd_obj ) },
};
static MP_DEFINE_CONST_DICT( jl_vfs_locals_dict, jl_vfs_locals_dict_table );

static const mp_vfs_proto_t jl_vfs_proto = {
    .import_stat = jl_vfs_import_stat,
};

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_vfs_jl,
    MP_QSTR_jfs,
    MP_TYPE_FLAG_NONE,
    protocol, &jl_vfs_proto,
    locals_dict, &jl_vfs_locals_dict );

static mp_obj_vfs_jl_t jl_vfs_obj = {
    .base = { &mp_type_vfs_jl },
};

void jl_vfs_mount_root( void ) {
    // Mount at "/" and set it as current working directory
    mp_obj_t args[ 2 ] = {
        MP_OBJ_FROM_PTR( &jl_vfs_obj ),
        MP_OBJ_NEW_QSTR( MP_QSTR__slash_ ),
    };
    mp_map_t kw_args;
    mp_map_init( &kw_args, 0 );
    mp_vfs_mount( 2, args, &kw_args );
    mp_vfs_chdir( args[ 1 ] );
}

#endif // MICROPY_VFS

// Register the modules with MicroPython
MP_REGISTER_MODULE( MP_QSTR_jumperless, jumperless_user_cmodule );
MP_REGISTER_MODULE( MP_QSTR_jfs, jfs_user_cmodule );

//=============================================================================
// Module Definition
//=============================================================================

// Module globals table

// get_state() -> String
static mp_obj_t mp_jl_get_state( void ) {
    return mp_obj_new_str( jl_get_state( ), strlen( jl_get_state( ) ) );
}
static MP_DEFINE_CONST_FUN_OBJ_0( jl_get_state_obj, mp_jl_get_state );

// set_state(json, clear_first=True, from_wokwi=False) -> int
// If `from_wokwi` is true the first argument may be a board-filepath or
// raw Wokwi diagram JSON.  The contents are converted into Jumperless state
// before applying.
static mp_obj_t mp_jl_set_state( size_t n_args, const mp_obj_t* args ) {
    const char* json = mp_obj_str_get_str( args[ 0 ] );
    int clear_first = 1;
    int from_wokwi = 0;
    if ( n_args > 1 ) {
        clear_first = mp_obj_is_true( args[ 1 ] ) ? 1 : 0;
    }
    if ( n_args > 2 ) {
        from_wokwi = mp_obj_is_true( args[ 2 ] ) ? 1 : 0;
    }
    
    int result = jl_set_state( json, clear_first, from_wokwi );
    return mp_obj_new_int( result );
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN( jl_set_state_obj, 1, 3, mp_jl_set_state );
// Overlay functions

// overlay_set(name, x, y, width, height, colors_list)
static mp_obj_t jl_overlay_set_func(size_t n_args, const mp_obj_t* args) {
    const char* name = mp_obj_str_get_str(args[0]);
    // Swap row/col and w/h to match user expectations (x, y, w, h)
    int col = mp_obj_get_int(args[1]);
    int row = mp_obj_get_int(args[2]);
    int height = mp_obj_get_int(args[3]);
    int width = mp_obj_get_int(args[4]);
    
    mp_obj_t colors_obj = args[5];
    size_t len;
    mp_obj_t* items;
    mp_obj_get_array(colors_obj, &len, &items);

    // Allocate exact buffer needed
    size_t required_pixels = (size_t)(width * height);
    uint32_t* colors = (uint32_t*)malloc(required_pixels * sizeof(uint32_t));
    if (!colors) {
        mp_raise_OSError(12); // ENOMEM
    }

    size_t current_pixel = 0;

    for (size_t i = 0; i < len; i++) {
        mp_obj_t item = items[i];
        
        // Check for nested list/tuple (2D array row)
        if (mp_obj_is_type(item, &mp_type_list) || mp_obj_is_type(item, &mp_type_tuple)) {
             size_t row_len;
             mp_obj_t* row_items;
             mp_obj_get_array(item, &row_len, &row_items);
             
             for (size_t j = 0; j < row_len; j++) {
                 if (current_pixel < required_pixels) {
                     colors[current_pixel++] = (uint32_t)mp_obj_get_int(row_items[j]);
                 }
             }
        } else {
             // Assume flat list item (int)
             if (current_pixel < required_pixels) {
                 colors[current_pixel++] = (uint32_t)mp_obj_get_int(item);
             }
        }
    }
    
    if (current_pixel < required_pixels) {
        free(colors);
        mp_raise_ValueError("Color array too small");
    }
    
    int result = jl_overlay_set(name, row, col, width, height, colors);
    free(colors);
    
    return mp_obj_new_int(result);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(jl_overlay_set_obj, 6, 6, jl_overlay_set_func);

// overlay_clear(name)
static mp_obj_t jl_overlay_clear_func(mp_obj_t name_obj) {
    const char* name = mp_obj_str_get_str(name_obj);
    int result = jl_overlay_clear(name);
    return mp_obj_new_int(result);
}
static MP_DEFINE_CONST_FUN_OBJ_1(jl_overlay_clear_obj, jl_overlay_clear_func);

// overlay_clear_all()
static mp_obj_t jl_overlay_clear_all_func(void) {
    jl_overlay_clear_all();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(jl_overlay_clear_all_obj, jl_overlay_clear_all_func);

// overlay_set_pixel(x, y, color)
static mp_obj_t jl_overlay_set_pixel_func(mp_obj_t x_obj, mp_obj_t y_obj, mp_obj_t color_obj) {
    int col = mp_obj_get_int(x_obj);
    int row = mp_obj_get_int(y_obj);
    uint32_t color = (uint32_t)mp_obj_get_int(color_obj);
    jl_overlay_set_pixel(row, col, color);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(jl_overlay_set_pixel_obj, jl_overlay_set_pixel_func);

// overlay_count()
static mp_obj_t jl_overlay_count_func(void) {
    return mp_obj_new_int(jl_overlay_count());
}
static MP_DEFINE_CONST_FUN_OBJ_0(jl_overlay_count_obj, jl_overlay_count_func);

// overlay_shift(name, dx, dy)
static mp_obj_t jl_overlay_shift_func(mp_obj_t name_obj, mp_obj_t dx_obj, mp_obj_t dy_obj) {
    const char* name = mp_obj_str_get_str(name_obj);
    int dCol = mp_obj_get_int(dx_obj);
    int dRow = mp_obj_get_int(dy_obj);
    int result = jl_overlay_shift(name, dRow, dCol);
    return mp_obj_new_int(result);
}
static MP_DEFINE_CONST_FUN_OBJ_3(jl_overlay_shift_obj, jl_overlay_shift_func);

// overlay_place(name, x, y)
static mp_obj_t jl_overlay_place_func(mp_obj_t name_obj, mp_obj_t x_obj, mp_obj_t y_obj) {
    const char* name = mp_obj_str_get_str(name_obj);
    int col = mp_obj_get_int(x_obj);
    int row = mp_obj_get_int(y_obj);
    int result = jl_overlay_place(name, row, col);
    return mp_obj_new_int(result);
}
static MP_DEFINE_CONST_FUN_OBJ_3(jl_overlay_place_obj, jl_overlay_place_func);

// overlay_serialize()
static mp_obj_t jl_overlay_serialize_func(void) {
    char* yaml = jl_overlay_serialize();
    return mp_obj_new_str(yaml, strlen(yaml));
}
static MP_DEFINE_CONST_FUN_OBJ_0(jl_overlay_serialize_obj, jl_overlay_serialize_func);

static const mp_rom_map_elem_t jumperless_module_globals_table[] = {
    { MP_ROM_QSTR( MP_QSTR___name__ ), MP_ROM_QSTR( MP_QSTR_jumperless ) },

    // Global variables
    { MP_ROM_QSTR( MP_QSTR_CURRENT_SLOT ), MP_ROM_PTR( &jl_get_current_slot_obj ) },

    // Connection context control
    { MP_ROM_QSTR( MP_QSTR_context_toggle ), MP_ROM_PTR( &jl_context_toggle_obj ) },
    { MP_ROM_QSTR( MP_QSTR_context_get ), MP_ROM_PTR( &jl_context_get_obj ) },

    // Node creation function
    { MP_ROM_QSTR( MP_QSTR_node ), MP_ROM_PTR( &jl_node_obj ) },

    // GPIO State constants
    { MP_ROM_QSTR( MP_QSTR_HIGH ), MP_ROM_PTR( &gpio_state_high_obj ) },
    { MP_ROM_QSTR( MP_QSTR_LOW ), MP_ROM_PTR( &gpio_state_low_obj ) },
    { MP_ROM_QSTR( MP_QSTR_FLOATING ), MP_ROM_PTR( &gpio_state_floating_obj ) },

    // GPIO Direction constants
    { MP_ROM_QSTR( MP_QSTR_INPUT ), MP_ROM_PTR( &gpio_direction_input_obj ) },
    { MP_ROM_QSTR( MP_QSTR_OUTPUT ), MP_ROM_PTR( &gpio_direction_output_obj ) },

    // Common node constants
    { MP_ROM_QSTR( MP_QSTR_TOP_RAIL ), MP_ROM_PTR( &node_top_rail_obj ) },
    { MP_ROM_QSTR( MP_QSTR_T_RAIL ), MP_ROM_PTR( &node_top_rail_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BOTTOM_RAIL ), MP_ROM_PTR( &node_bottom_rail_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BOT_RAIL ), MP_ROM_PTR( &node_bottom_rail_obj ) },
    { MP_ROM_QSTR( MP_QSTR_B_RAIL ), MP_ROM_PTR( &node_bottom_rail_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GND ), MP_ROM_PTR( &node_gnd_obj ) },
    { MP_ROM_QSTR( MP_QSTR_DAC0 ), MP_ROM_PTR( &node_dac0_obj ) },
    { MP_ROM_QSTR( MP_QSTR_DAC_0 ), MP_ROM_PTR( &node_dac0_obj ) },
    { MP_ROM_QSTR( MP_QSTR_DAC1 ), MP_ROM_PTR( &node_dac1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_DAC_1 ), MP_ROM_PTR( &node_dac1_obj ) },

    // Current sense pins
    { MP_ROM_QSTR( MP_QSTR_ISENSE_PLUS ), MP_ROM_PTR( &node_isense_plus_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ISENSE_P ), MP_ROM_PTR( &node_isense_plus_obj ) },
    { MP_ROM_QSTR( MP_QSTR_I_P ), MP_ROM_PTR( &node_isense_plus_obj ) },
    { MP_ROM_QSTR( MP_QSTR_CURRENT_SENSE_P ), MP_ROM_PTR( &node_isense_plus_obj ) },
    { MP_ROM_QSTR( MP_QSTR_CURRENT_SENSE_PLUS ), MP_ROM_PTR( &node_isense_plus_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ISENSE_MINUS ), MP_ROM_PTR( &node_isense_minus_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ISENSE_N ), MP_ROM_PTR( &node_isense_minus_obj ) },
    { MP_ROM_QSTR( MP_QSTR_I_N ), MP_ROM_PTR( &node_isense_minus_obj ) },
    { MP_ROM_QSTR( MP_QSTR_CURRENT_SENSE_N ), MP_ROM_PTR( &node_isense_minus_obj ) },
    { MP_ROM_QSTR( MP_QSTR_CURRENT_SENSE_MINUS ), MP_ROM_PTR( &node_isense_minus_obj ) },

    // Buffer pins
    { MP_ROM_QSTR( MP_QSTR_BUFFER_IN ), MP_ROM_PTR( &node_buffer_in_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BUF_IN ), MP_ROM_PTR( &node_buffer_in_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BUFFER_OUT ), MP_ROM_PTR( &node_buffer_out_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BUF_OUT ), MP_ROM_PTR( &node_buffer_out_obj ) },

    // ADC pins
    { MP_ROM_QSTR( MP_QSTR_ADC0 ), MP_ROM_PTR( &node_adc0_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ADC1 ), MP_ROM_PTR( &node_adc1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ADC2 ), MP_ROM_PTR( &node_adc2_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ADC3 ), MP_ROM_PTR( &node_adc3_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ADC4 ), MP_ROM_PTR( &node_adc4_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ADC7 ), MP_ROM_PTR( &node_adc7_obj ) },

    // UART pins
    { MP_ROM_QSTR( MP_QSTR_UART_TX ), MP_ROM_PTR( &node_uart_tx_obj ) },
    { MP_ROM_QSTR( MP_QSTR_TX ), MP_ROM_PTR( &node_uart_tx_obj ) },
    { MP_ROM_QSTR( MP_QSTR_UART_RX ), MP_ROM_PTR( &node_uart_rx_obj ) },
    { MP_ROM_QSTR( MP_QSTR_RX ), MP_ROM_PTR( &node_uart_rx_obj ) },

    // Arduino Nano pins
    { MP_ROM_QSTR( MP_QSTR_D0 ), MP_ROM_PTR( &node_d0_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D1 ), MP_ROM_PTR( &node_d1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D2 ), MP_ROM_PTR( &node_d2_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D3 ), MP_ROM_PTR( &node_d3_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D4 ), MP_ROM_PTR( &node_d4_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D5 ), MP_ROM_PTR( &node_d5_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D6 ), MP_ROM_PTR( &node_d6_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D7 ), MP_ROM_PTR( &node_d7_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D8 ), MP_ROM_PTR( &node_d8_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D9 ), MP_ROM_PTR( &node_d9_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D10 ), MP_ROM_PTR( &node_d10_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D11 ), MP_ROM_PTR( &node_d11_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D12 ), MP_ROM_PTR( &node_d12_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D13 ), MP_ROM_PTR( &node_d13_obj ) },

    // NANO prefixed digital pins
    { MP_ROM_QSTR( MP_QSTR_NANO_D0 ), MP_ROM_PTR( &node_d0_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D1 ), MP_ROM_PTR( &node_d1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D2 ), MP_ROM_PTR( &node_d2_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D3 ), MP_ROM_PTR( &node_d3_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D4 ), MP_ROM_PTR( &node_d4_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D5 ), MP_ROM_PTR( &node_d5_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D6 ), MP_ROM_PTR( &node_d6_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D7 ), MP_ROM_PTR( &node_d7_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D8 ), MP_ROM_PTR( &node_d8_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D9 ), MP_ROM_PTR( &node_d9_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D10 ), MP_ROM_PTR( &node_d10_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D11 ), MP_ROM_PTR( &node_d11_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D12 ), MP_ROM_PTR( &node_d12_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_D13 ), MP_ROM_PTR( &node_d13_obj ) },

    // Arduino analog pins
    { MP_ROM_QSTR( MP_QSTR_A0 ), MP_ROM_PTR( &node_a0_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A1 ), MP_ROM_PTR( &node_a1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A2 ), MP_ROM_PTR( &node_a2_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A3 ), MP_ROM_PTR( &node_a3_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A4 ), MP_ROM_PTR( &node_a4_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A5 ), MP_ROM_PTR( &node_a5_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A6 ), MP_ROM_PTR( &node_a6_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A7 ), MP_ROM_PTR( &node_a7_obj ) },

    // NANO prefixed analog pins
    { MP_ROM_QSTR( MP_QSTR_NANO_A0 ), MP_ROM_PTR( &node_a0_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_A1 ), MP_ROM_PTR( &node_a1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_A2 ), MP_ROM_PTR( &node_a2_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_A3 ), MP_ROM_PTR( &node_a3_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_A4 ), MP_ROM_PTR( &node_a4_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_A5 ), MP_ROM_PTR( &node_a5_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_A6 ), MP_ROM_PTR( &node_a6_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_A7 ), MP_ROM_PTR( &node_a7_obj ) },

    // GPIO pins with multiple aliases
    { MP_ROM_QSTR( MP_QSTR_GPIO_1 ), MP_ROM_PTR( &node_gpio1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_2 ), MP_ROM_PTR( &node_gpio2_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_3 ), MP_ROM_PTR( &node_gpio3_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_4 ), MP_ROM_PTR( &node_gpio4_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_5 ), MP_ROM_PTR( &node_gpio5_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_6 ), MP_ROM_PTR( &node_gpio6_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_7 ), MP_ROM_PTR( &node_gpio7_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_8 ), MP_ROM_PTR( &node_gpio8_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GP1 ), MP_ROM_PTR( &node_gpio1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GP2 ), MP_ROM_PTR( &node_gpio2_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GP3 ), MP_ROM_PTR( &node_gpio3_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GP4 ), MP_ROM_PTR( &node_gpio4_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GP5 ), MP_ROM_PTR( &node_gpio5_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GP6 ), MP_ROM_PTR( &node_gpio6_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GP7 ), MP_ROM_PTR( &node_gpio7_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GP8 ), MP_ROM_PTR( &node_gpio8_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_20 ), MP_ROM_PTR( &node_gpio1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_21 ), MP_ROM_PTR( &node_gpio2_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_22 ), MP_ROM_PTR( &node_gpio3_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_23 ), MP_ROM_PTR( &node_gpio4_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_24 ), MP_ROM_PTR( &node_gpio5_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_25 ), MP_ROM_PTR( &node_gpio6_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_26 ), MP_ROM_PTR( &node_gpio7_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_27 ), MP_ROM_PTR( &node_gpio8_obj ) },

    // Probe button constants
    { MP_ROM_QSTR( MP_QSTR_BUTTON_NONE ), MP_ROM_PTR( &probe_button_none_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BUTTON_CONNECT ), MP_ROM_PTR( &probe_button_connect_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BUTTON_REMOVE ), MP_ROM_PTR( &probe_button_remove_obj ) },
    { MP_ROM_QSTR( MP_QSTR_CONNECT_BUTTON ), MP_ROM_PTR( &probe_button_connect_obj ) },
    { MP_ROM_QSTR( MP_QSTR_REMOVE_BUTTON ), MP_ROM_PTR( &probe_button_remove_obj ) },

    // Probe switch position constants
    { MP_ROM_QSTR( MP_QSTR_SWITCH_MEASURE ), MP_ROM_INT( 0 ) },
    { MP_ROM_QSTR( MP_QSTR_SWITCH_SELECT ), MP_ROM_INT( 1 ) },
    { MP_ROM_QSTR( MP_QSTR_SWITCH_UNKNOWN ), MP_ROM_INT( -1 ) },

    // Clickwheel direction constants
    { MP_ROM_QSTR( MP_QSTR_CLICKWHEEL_NONE ), MP_ROM_INT( 0 ) },
    { MP_ROM_QSTR( MP_QSTR_CLICKWHEEL_UP ), MP_ROM_INT( 1 ) },
    { MP_ROM_QSTR( MP_QSTR_CLICKWHEEL_DOWN ), MP_ROM_INT( 2 ) },

    // Clickwheel button state constants
    { MP_ROM_QSTR( MP_QSTR_CLICKWHEEL_IDLE ), MP_ROM_INT( 0 ) },
    { MP_ROM_QSTR( MP_QSTR_CLICKWHEEL_PRESSED ), MP_ROM_INT( 1 ) },
    { MP_ROM_QSTR( MP_QSTR_CLICKWHEEL_HELD ), MP_ROM_INT( 2 ) },
    { MP_ROM_QSTR( MP_QSTR_CLICKWHEEL_RELEASED ), MP_ROM_INT( 3 ) },
    // RESERVED, never returned by clickwheel_get_button(): the encoder has no
    // double-click gesture (rule of 2026-08-22). Kept defined so scripts that
    // import it still load, and so LONG_HELD/MEDIUM_HELD keep values 5/6.
    { MP_ROM_QSTR( MP_QSTR_CLICKWHEEL_DOUBLECLICKED ), MP_ROM_INT( 4 ) },

    // Probe pad constants
    { MP_ROM_QSTR( MP_QSTR_NO_PAD ), MP_ROM_PTR( &probe_no_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_LOGO_PAD_TOP ), MP_ROM_PTR( &probe_logo_pad_top_obj ) },
    { MP_ROM_QSTR( MP_QSTR_LOGO_PAD_BOTTOM ), MP_ROM_PTR( &probe_logo_pad_bottom_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GPIO_PAD ), MP_ROM_PTR( &probe_gpio_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_DAC_PAD ), MP_ROM_PTR( &probe_dac_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ADC_PAD ), MP_ROM_PTR( &probe_adc_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BUILDING_PAD_TOP ), MP_ROM_PTR( &probe_building_pad_top_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BUILDING_PAD_BOTTOM ), MP_ROM_PTR( &probe_building_pad_bottom_obj ) },

    // Nano power/control pad constants
    { MP_ROM_QSTR( MP_QSTR_NANO_VIN ), MP_ROM_PTR( &probe_nano_vin_obj ) },
    { MP_ROM_QSTR( MP_QSTR_VIN_PAD ), MP_ROM_PTR( &probe_nano_vin_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_RESET_0 ), MP_ROM_PTR( &probe_nano_reset_0_obj ) },
    { MP_ROM_QSTR( MP_QSTR_RESET_0_PAD ), MP_ROM_PTR( &probe_nano_reset_0_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_RESET_1 ), MP_ROM_PTR( &probe_nano_reset_1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_RESET_1_PAD ), MP_ROM_PTR( &probe_nano_reset_1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_GND_1 ), MP_ROM_PTR( &probe_nano_gnd_1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GND_1_PAD ), MP_ROM_PTR( &probe_nano_gnd_1_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_GND_0 ), MP_ROM_PTR( &probe_nano_gnd_0_obj ) },
    { MP_ROM_QSTR( MP_QSTR_GND_0_PAD ), MP_ROM_PTR( &probe_nano_gnd_0_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_3V3 ), MP_ROM_PTR( &probe_nano_3v3_obj ) },
    { MP_ROM_QSTR( MP_QSTR_3V3_PAD ), MP_ROM_PTR( &probe_nano_3v3_obj ) },
    { MP_ROM_QSTR( MP_QSTR_NANO_5V ), MP_ROM_PTR( &probe_nano_5v_obj ) },
    { MP_ROM_QSTR( MP_QSTR_5V_PAD ), MP_ROM_PTR( &probe_nano_5v_obj ) },

    // Nano digital pin pad constants
    { MP_ROM_QSTR( MP_QSTR_D0_PAD ), MP_ROM_PTR( &probe_d0_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D1_PAD ), MP_ROM_PTR( &probe_d1_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D2_PAD ), MP_ROM_PTR( &probe_d2_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D3_PAD ), MP_ROM_PTR( &probe_d3_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D4_PAD ), MP_ROM_PTR( &probe_d4_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D5_PAD ), MP_ROM_PTR( &probe_d5_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D6_PAD ), MP_ROM_PTR( &probe_d6_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D7_PAD ), MP_ROM_PTR( &probe_d7_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D8_PAD ), MP_ROM_PTR( &probe_d8_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D9_PAD ), MP_ROM_PTR( &probe_d9_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D10_PAD ), MP_ROM_PTR( &probe_d10_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D11_PAD ), MP_ROM_PTR( &probe_d11_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D12_PAD ), MP_ROM_PTR( &probe_d12_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_D13_PAD ), MP_ROM_PTR( &probe_d13_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_RESET_PAD ), MP_ROM_PTR( &probe_reset_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_AREF_PAD ), MP_ROM_PTR( &probe_aref_pad_obj ) },

    // Nano analog pin pad constants
    { MP_ROM_QSTR( MP_QSTR_A0_PAD ), MP_ROM_PTR( &probe_a0_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A1_PAD ), MP_ROM_PTR( &probe_a1_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A2_PAD ), MP_ROM_PTR( &probe_a2_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A3_PAD ), MP_ROM_PTR( &probe_a3_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A4_PAD ), MP_ROM_PTR( &probe_a4_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A5_PAD ), MP_ROM_PTR( &probe_a5_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A6_PAD ), MP_ROM_PTR( &probe_a6_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_A7_PAD ), MP_ROM_PTR( &probe_a7_pad_obj ) },

    // Rail pad constants
    { MP_ROM_QSTR( MP_QSTR_TOP_RAIL_PAD ), MP_ROM_PTR( &probe_top_rail_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BOTTOM_RAIL_PAD ), MP_ROM_PTR( &probe_bottom_rail_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BOT_RAIL_PAD ), MP_ROM_PTR( &probe_bottom_rail_pad_obj ) },
    { MP_ROM_QSTR( MP_QSTR_TOP_RAIL_GND ), MP_ROM_PTR( &probe_top_rail_gnd_obj ) },
    { MP_ROM_QSTR( MP_QSTR_TOP_GND_PAD ), MP_ROM_PTR( &probe_top_rail_gnd_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BOTTOM_RAIL_GND ), MP_ROM_PTR( &probe_bottom_rail_gnd_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BOT_RAIL_GND ), MP_ROM_PTR( &probe_bottom_rail_gnd_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BOTTOM_GND_PAD ), MP_ROM_PTR( &probe_bottom_rail_gnd_obj ) },
    { MP_ROM_QSTR( MP_QSTR_BOT_GND_PAD ), MP_ROM_PTR( &probe_bottom_rail_gnd_obj ) },

    // DAC functions
    { MP_ROM_QSTR( MP_QSTR_dac_set ), MP_ROM_PTR( &jl_dac_set_obj ) },
    { MP_ROM_QSTR( MP_QSTR_dac_get ), MP_ROM_PTR( &jl_dac_get_obj ) },

    // DAC function aliases
    { MP_ROM_QSTR( MP_QSTR_set_dac ), MP_ROM_PTR( &jl_dac_set_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_dac ), MP_ROM_PTR( &jl_dac_get_obj ) },

    // ADC functions
    { MP_ROM_QSTR( MP_QSTR_adc_get ), MP_ROM_PTR( &jl_adc_get_obj ) },

    // ADC function aliases
    { MP_ROM_QSTR( MP_QSTR_get_adc ), MP_ROM_PTR( &jl_adc_get_obj ) },

    // USB Audio functions (UAC2 microphone - see the note by the wrappers)
    { MP_ROM_QSTR( MP_QSTR_usb_audio_setup ), MP_ROM_PTR( &jl_usb_audio_setup_obj ) },
    { MP_ROM_QSTR( MP_QSTR_usb_audio_enable ), MP_ROM_PTR( &jl_usb_audio_enable_obj ) },
    { MP_ROM_QSTR( MP_QSTR_usb_audio_disable ), MP_ROM_PTR( &jl_usb_audio_disable_obj ) },
    { MP_ROM_QSTR( MP_QSTR_usb_audio_teardown ), MP_ROM_PTR( &jl_usb_audio_disable_obj ) },
    { MP_ROM_QSTR( MP_QSTR_usb_audio_is_enabled ), MP_ROM_PTR( &jl_usb_audio_is_enabled_obj ) },
    { MP_ROM_QSTR( MP_QSTR_usb_audio_active ), MP_ROM_PTR( &jl_usb_audio_active_obj ) },
    { MP_ROM_QSTR( MP_QSTR_usb_audio_status ), MP_ROM_PTR( &jl_usb_audio_status_obj ) },
    { MP_ROM_QSTR( MP_QSTR_usb_audio_set_range ), MP_ROM_PTR( &jl_usb_audio_set_range_obj ) },
    { MP_ROM_QSTR( MP_QSTR_usb_audio_set_rate ), MP_ROM_PTR( &jl_usb_audio_set_rate_obj ) },
    { MP_ROM_QSTR( MP_QSTR_usb_audio_save ), MP_ROM_PTR( &jl_usb_audio_save_obj ) },

    // USB Audio aliases (house style: adc_get/get_adc)
    { MP_ROM_QSTR( MP_QSTR_audio_setup ), MP_ROM_PTR( &jl_usb_audio_setup_obj ) },
    { MP_ROM_QSTR( MP_QSTR_audio_status ), MP_ROM_PTR( &jl_usb_audio_status_obj ) },

    // INA functions
    { MP_ROM_QSTR( MP_QSTR_ina_get_current ), MP_ROM_PTR( &jl_ina_get_current_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ina_get_voltage ), MP_ROM_PTR( &jl_ina_get_voltage_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ina_get_bus_voltage ), MP_ROM_PTR( &jl_ina_get_bus_voltage_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ina_get_power ), MP_ROM_PTR( &jl_ina_get_power_obj ) },

    // INA function aliases
    { MP_ROM_QSTR( MP_QSTR_get_ina_current ), MP_ROM_PTR( &jl_ina_get_current_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_ina_voltage ), MP_ROM_PTR( &jl_ina_get_voltage_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_ina_bus_voltage ), MP_ROM_PTR( &jl_ina_get_bus_voltage_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_ina_power ), MP_ROM_PTR( &jl_ina_get_power_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_current ), MP_ROM_PTR( &jl_ina_get_current_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_voltage ), MP_ROM_PTR( &jl_ina_get_voltage_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_bus_voltage ), MP_ROM_PTR( &jl_ina_get_bus_voltage_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_power ), MP_ROM_PTR( &jl_ina_get_power_obj ) },

    // GPIO functions
    { MP_ROM_QSTR( MP_QSTR_gpio_set ), MP_ROM_PTR( &jl_gpio_set_obj ) },
    { MP_ROM_QSTR( MP_QSTR_gpio_get ), MP_ROM_PTR( &jl_gpio_get_obj ) },
    { MP_ROM_QSTR( MP_QSTR_gpio_set_dir ), MP_ROM_PTR( &jl_gpio_set_dir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_gpio_get_dir ), MP_ROM_PTR( &jl_gpio_get_dir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_gpio_set_pull ), MP_ROM_PTR( &jl_gpio_set_pull_obj ) },
    { MP_ROM_QSTR( MP_QSTR_gpio_get_pull ), MP_ROM_PTR( &jl_gpio_get_pull_obj ) },
    { MP_ROM_QSTR( MP_QSTR_gpio_set_read_floating ), MP_ROM_PTR( &jl_gpio_set_floating_read_obj ) },
    { MP_ROM_QSTR( MP_QSTR_gpio_get_read_floating ), MP_ROM_PTR( &jl_gpio_get_floating_read_obj ) },

    // GPIO function aliases
    { MP_ROM_QSTR( MP_QSTR_set_gpio ), MP_ROM_PTR( &jl_gpio_set_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_gpio ), MP_ROM_PTR( &jl_gpio_get_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_gpio_dir ), MP_ROM_PTR( &jl_gpio_set_dir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_gpio_dir ), MP_ROM_PTR( &jl_gpio_get_dir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_gpio_pull ), MP_ROM_PTR( &jl_gpio_set_pull_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_gpio_pull ), MP_ROM_PTR( &jl_gpio_get_pull_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_gpio_read_floating ), MP_ROM_PTR( &jl_gpio_set_floating_read_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_gpio_read_floating ), MP_ROM_PTR( &jl_gpio_get_floating_read_obj ) },

    // GPIO pin ownership functions (for timing-critical operations like NeoPixels)
    { MP_ROM_QSTR( MP_QSTR_gpio_claim_pin ), MP_ROM_PTR( &jl_gpio_claim_pin_obj ) },
    { MP_ROM_QSTR( MP_QSTR_gpio_release_pin ), MP_ROM_PTR( &jl_gpio_release_pin_obj ) },
    { MP_ROM_QSTR( MP_QSTR_gpio_release_all_pins ), MP_ROM_PTR( &jl_gpio_release_all_pins_obj ) },

    // PWM functions
    { MP_ROM_QSTR( MP_QSTR_pwm ), MP_ROM_PTR( &jl_pwm_obj ) },
    { MP_ROM_QSTR( MP_QSTR_pwm_set_duty_cycle ), MP_ROM_PTR( &jl_pwm_set_duty_cycle_obj ) },
    { MP_ROM_QSTR( MP_QSTR_pwm_set_frequency ), MP_ROM_PTR( &jl_pwm_set_frequency_obj ) },
    { MP_ROM_QSTR( MP_QSTR_pwm_stop ), MP_ROM_PTR( &jl_pwm_stop_obj ) },

    // PWM function aliases
    { MP_ROM_QSTR( MP_QSTR_set_pwm ), MP_ROM_PTR( &jl_pwm_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_pwm_duty_cycle ), MP_ROM_PTR( &jl_pwm_set_duty_cycle_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_pwm_frequency ), MP_ROM_PTR( &jl_pwm_set_frequency_obj ) },
    { MP_ROM_QSTR( MP_QSTR_stop_pwm ), MP_ROM_PTR( &jl_pwm_stop_obj ) },

    // Wavegen API
    { MP_ROM_QSTR( MP_QSTR_wavegen_set_output ), MP_ROM_PTR( &jl_wavegen_set_output_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_wavegen_output ), MP_ROM_PTR( &jl_wavegen_set_output_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_set_freq ), MP_ROM_PTR( &jl_wavegen_set_freq_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_wavegen_freq ), MP_ROM_PTR( &jl_wavegen_set_freq_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_set_wave ), MP_ROM_PTR( &jl_wavegen_set_wave_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_wavegen_wave ), MP_ROM_PTR( &jl_wavegen_set_wave_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_set_sweep ), MP_ROM_PTR( &jl_wavegen_set_sweep_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_wavegen_sweep ), MP_ROM_PTR( &jl_wavegen_set_sweep_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_set_amplitude ), MP_ROM_PTR( &jl_wavegen_set_amplitude_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_wavegen_amplitude ), MP_ROM_PTR( &jl_wavegen_set_amplitude_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_set_offset ), MP_ROM_PTR( &jl_wavegen_set_offset_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_wavegen_offset ), MP_ROM_PTR( &jl_wavegen_set_offset_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_start ), MP_ROM_PTR( &jl_wavegen_start_obj ) },
    { MP_ROM_QSTR( MP_QSTR_start_wavegen ), MP_ROM_PTR( &jl_wavegen_start_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_stop ), MP_ROM_PTR( &jl_wavegen_stop_obj ) },
    { MP_ROM_QSTR( MP_QSTR_stop_wavegen ), MP_ROM_PTR( &jl_wavegen_stop_obj ) },
    // Getters
    { MP_ROM_QSTR( MP_QSTR_wavegen_get_output ), MP_ROM_PTR( &jl_wavegen_get_output_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_wavegen_output ), MP_ROM_PTR( &jl_wavegen_get_output_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_get_freq ), MP_ROM_PTR( &jl_wavegen_get_freq_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_wavegen_freq ), MP_ROM_PTR( &jl_wavegen_get_freq_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_get_wave ), MP_ROM_PTR( &jl_wavegen_get_wave_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_wavegen_wave ), MP_ROM_PTR( &jl_wavegen_get_wave_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_get_amplitude ), MP_ROM_PTR( &jl_wavegen_get_amplitude_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_wavegen_amplitude ), MP_ROM_PTR( &jl_wavegen_get_amplitude_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_get_offset ), MP_ROM_PTR( &jl_wavegen_get_offset_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_wavegen_offset ), MP_ROM_PTR( &jl_wavegen_get_offset_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wavegen_is_running ), MP_ROM_PTR( &jl_wavegen_is_running_obj ) },

    // Node functions
    { MP_ROM_QSTR( MP_QSTR_connect ), MP_ROM_PTR( &jl_nodes_connect_obj ) },
    { MP_ROM_QSTR( MP_QSTR_disconnect ), MP_ROM_PTR( &jl_nodes_disconnect_obj ) },
    { MP_ROM_QSTR( MP_QSTR_fast_connect ), MP_ROM_PTR( &jl_nodes_fast_connect_obj ) },
    { MP_ROM_QSTR( MP_QSTR_fast_disconnect ), MP_ROM_PTR( &jl_nodes_fast_disconnect_obj ) },
    { MP_ROM_QSTR( MP_QSTR_nodes_clear ), MP_ROM_PTR( &jl_nodes_clear_obj ) },
    { MP_ROM_QSTR( MP_QSTR_is_connected ), MP_ROM_PTR( &jl_nodes_is_connected_obj ) },
    { MP_ROM_QSTR( MP_QSTR_nodes_save ), MP_ROM_PTR( &jl_nodes_save_obj ) },

    // Undo / Redo / History
    { MP_ROM_QSTR( MP_QSTR_undo ), MP_ROM_PTR( &jl_undo_obj ) },
    { MP_ROM_QSTR( MP_QSTR_redo ), MP_ROM_PTR( &jl_redo_obj ) },
    { MP_ROM_QSTR( MP_QSTR_history_position ), MP_ROM_PTR( &jl_history_position_obj ) },
    { MP_ROM_QSTR( MP_QSTR_history_size ), MP_ROM_PTR( &jl_history_size_obj ) },
    { MP_ROM_QSTR( MP_QSTR_history_label ), MP_ROM_PTR( &jl_history_label_obj ) },
    { MP_ROM_QSTR( MP_QSTR_history_jump ), MP_ROM_PTR( &jl_history_jump_obj ) },
    { MP_ROM_QSTR( MP_QSTR_history_snapshot ), MP_ROM_PTR( &jl_history_snapshot_obj ) },
    { MP_ROM_QSTR( MP_QSTR_history_snapshot_count ), MP_ROM_PTR( &jl_history_snapshot_count_obj ) },

    // Net Information API - Get/set net names, colors, and info
    { MP_ROM_QSTR( MP_QSTR_get_net_name ), MP_ROM_PTR( &jl_get_net_name_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_net_name ), MP_ROM_PTR( &jl_set_net_name_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_net_color ), MP_ROM_PTR( &jl_get_net_color_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_net_color_name ), MP_ROM_PTR( &jl_get_net_color_name_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_net_color ), MP_ROM_PTR( &jl_set_net_color_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_net_color_hsv ), MP_ROM_PTR( &jl_set_net_color_hsv_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_num_nets ), MP_ROM_PTR( &jl_get_num_nets_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_num_bridges ), MP_ROM_PTR( &jl_get_num_bridges_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_net_nodes ), MP_ROM_PTR( &jl_get_net_nodes_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_bridge ), MP_ROM_PTR( &jl_get_bridge_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_net_info ), MP_ROM_PTR( &jl_get_net_info_obj ) },
    // Fake GPIO path query functions
    { MP_ROM_QSTR( MP_QSTR_get_num_paths ), MP_ROM_PTR( &jl_get_num_paths_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_path_info ), MP_ROM_PTR( &jl_get_path_info_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_all_paths ), MP_ROM_PTR( &jl_get_all_paths_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_path_between ), MP_ROM_PTR( &jl_get_path_between_obj ) },
    // Net current scan queries (background voltage/current sensing)
    { MP_ROM_QSTR( MP_QSTR_get_node_voltage ), MP_ROM_PTR( &jl_get_node_voltage_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_net_current ), MP_ROM_PTR( &jl_get_net_current_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_path_current ), MP_ROM_PTR( &jl_get_path_current_obj ) },
    // Fake GPIO fast toggle
    { MP_ROM_QSTR( MP_QSTR_FakeGpioDisconnect ), MP_ROM_PTR( &fake_gpio_disconnect_type ) },
    // Fake GPIO pin class
    { MP_ROM_QSTR( MP_QSTR_FakeGpioPin ), MP_ROM_PTR( &fake_gpio_pin_type ) },
    // Fake GPIO mode constants
    // Fake GPIO mode constants
    { MP_ROM_QSTR( MP_QSTR_FAKE_GPIO_INPUT ), MP_ROM_INT( FAKE_GPIO_MODE_INPUT ) },
    { MP_ROM_QSTR( MP_QSTR_FAKE_GPIO_OUTPUT ), MP_ROM_INT( FAKE_GPIO_MODE_OUTPUT ) },
    // Short aliases for convenience for fake-gpio modes were intentionally removed
    // to avoid colliding with `INPUT`/`OUTPUT` GPIODirection objects used elsewhere.

    
    // Aliases for net API
    { MP_ROM_QSTR( MP_QSTR_net_name ), MP_ROM_PTR( &jl_get_net_name_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_net_info ), MP_ROM_PTR( &jl_get_net_info_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_all_nets ), MP_ROM_PTR( &jl_get_all_nets_obj ) },
    { MP_ROM_QSTR( MP_QSTR_net_info ), MP_ROM_PTR( &jl_get_net_info_obj ) },
    // Aliases for net current scan queries
    { MP_ROM_QSTR( MP_QSTR_node_voltage ), MP_ROM_PTR( &jl_get_node_voltage_obj ) },
    { MP_ROM_QSTR( MP_QSTR_net_current ), MP_ROM_PTR( &jl_get_net_current_obj ) },
    { MP_ROM_QSTR( MP_QSTR_path_current ), MP_ROM_PTR( &jl_get_path_current_obj ) },

    // Raw hardware functions
    { MP_ROM_QSTR( MP_QSTR_send_raw ), MP_ROM_PTR( &jl_send_raw_obj ) },
    { MP_ROM_QSTR( MP_QSTR_switch_slot ), MP_ROM_PTR( &jl_switch_slot_obj ) },

    // Session management functions
    { MP_ROM_QSTR( MP_QSTR_nodes_discard ), MP_ROM_PTR( &jl_nodes_discard_obj ) },
    { MP_ROM_QSTR( MP_QSTR_nodes_has_changes ), MP_ROM_PTR( &jl_nodes_has_changes_obj ) },
    { MP_ROM_QSTR( MP_QSTR_switch_slot ), MP_ROM_PTR( &jl_switch_slot_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_state ), MP_ROM_PTR( &jl_get_state_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_state ), MP_ROM_PTR( &jl_set_state_obj ) },
    { MP_ROM_QSTR( MP_QSTR_nodes_clear ), MP_ROM_PTR( &jl_nodes_clear_obj ) },

    // OLED functions
    { MP_ROM_QSTR( MP_QSTR_oled_print ), MP_ROM_PTR( &jl_oled_print_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_clear ), MP_ROM_PTR( &jl_oled_clear_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_show ), MP_ROM_PTR( &jl_oled_show_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_connect ), MP_ROM_PTR( &jl_oled_connect_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_disconnect ), MP_ROM_PTR( &jl_oled_disconnect_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_set_text_size ), MP_ROM_PTR( &jl_oled_set_text_size_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_get_text_size ), MP_ROM_PTR( &jl_oled_get_text_size_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_copy_print ), MP_ROM_PTR( &jl_oled_copy_print_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_get_fonts ), MP_ROM_PTR( &jl_oled_get_fonts_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_set_font ), MP_ROM_PTR( &jl_oled_set_font_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_get_current_font ), MP_ROM_PTR( &jl_oled_get_current_font_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_load_bitmap ), MP_ROM_PTR( &jl_oled_load_bitmap_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_display_bitmap ), MP_ROM_PTR( &jl_oled_display_bitmap_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_show_bitmap_file ), MP_ROM_PTR( &jl_oled_show_bitmap_file_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_get_framebuffer ), MP_ROM_PTR( &jl_oled_get_framebuffer_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_set_framebuffer ), MP_ROM_PTR( &jl_oled_set_framebuffer_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_get_framebuffer_size ), MP_ROM_PTR( &jl_oled_get_framebuffer_size_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_set_pixel ), MP_ROM_PTR( &jl_oled_set_pixel_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_get_pixel ), MP_ROM_PTR( &jl_oled_get_pixel_obj ) },

    // OLED GUI (retained screens)
    { MP_ROM_QSTR( MP_QSTR_oled_screen ), MP_ROM_PTR( &jl_oled_screen_new_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_screen_free ), MP_ROM_PTR( &jl_oled_screen_free_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_screen_clear ), MP_ROM_PTR( &jl_oled_screen_clear_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_screen_show ), MP_ROM_PTR( &jl_oled_screen_show_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_screen_hide ), MP_ROM_PTR( &jl_oled_screen_hide_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_screen_reset ), MP_ROM_PTR( &jl_oled_screen_reset_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_add_text ), MP_ROM_PTR( &jl_oled_add_text_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_add_shape ), MP_ROM_PTR( &jl_oled_add_shape_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_set ), MP_ROM_PTR( &jl_oled_set_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_set_var ), MP_ROM_PTR( &jl_oled_set_var_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_screen_save ), MP_ROM_PTR( &jl_oled_screen_save_obj ) },
    { MP_ROM_QSTR( MP_QSTR_oled_screen_load ), MP_ROM_PTR( &jl_oled_screen_load_obj ) },

    // Misc functions
    { MP_ROM_QSTR( MP_QSTR_arduino_reset ), MP_ROM_PTR( &jl_arduino_reset_obj ) },
    { MP_ROM_QSTR( MP_QSTR_pause_core2 ), MP_ROM_PTR( &jl_pause_core2_obj ) },
    { MP_ROM_QSTR( MP_QSTR_run_app ), MP_ROM_PTR( &jl_run_app_obj ) },
    { MP_ROM_QSTR( MP_QSTR_change_terminal_color ), MP_ROM_PTR( &jl_change_terminal_color_obj ) },
    { MP_ROM_QSTR( MP_QSTR_cycle_term_color ), MP_ROM_PTR( &jl_cycle_term_color_obj ) },

    // Status functions
    { MP_ROM_QSTR( MP_QSTR_print_bridges ), MP_ROM_PTR( &jl_nodes_print_bridges_obj ) },
    { MP_ROM_QSTR( MP_QSTR_print_paths ), MP_ROM_PTR( &jl_nodes_print_paths_obj ) },
    { MP_ROM_QSTR( MP_QSTR_print_crossbars ), MP_ROM_PTR( &jl_nodes_print_crossbars_obj ) },
    { MP_ROM_QSTR( MP_QSTR_print_nets ), MP_ROM_PTR( &jl_nodes_print_nets_obj ) },
    { MP_ROM_QSTR( MP_QSTR_print_chip_status ), MP_ROM_PTR( &jl_nodes_print_chip_status_obj ) },

    // Overlay functions
    { MP_ROM_QSTR( MP_QSTR_overlay_set ), MP_ROM_PTR( &jl_overlay_set_obj ) },
    { MP_ROM_QSTR( MP_QSTR_overlay_clear ), MP_ROM_PTR( &jl_overlay_clear_obj ) },
    { MP_ROM_QSTR( MP_QSTR_overlay_clear_all ), MP_ROM_PTR( &jl_overlay_clear_all_obj ) },
    { MP_ROM_QSTR( MP_QSTR_overlay_set_pixel ), MP_ROM_PTR( &jl_overlay_set_pixel_obj ) },
    { MP_ROM_QSTR( MP_QSTR_overlay_count ), MP_ROM_PTR( &jl_overlay_count_obj ) },
    { MP_ROM_QSTR( MP_QSTR_overlay_shift ), MP_ROM_PTR( &jl_overlay_shift_obj ) },
    { MP_ROM_QSTR( MP_QSTR_overlay_place ), MP_ROM_PTR( &jl_overlay_place_obj ) },
    { MP_ROM_QSTR( MP_QSTR_overlay_serialize ), MP_ROM_PTR( &jl_overlay_serialize_obj ) },

    // Probe functions
    { MP_ROM_QSTR( MP_QSTR_probe_tap ), MP_ROM_PTR( &jl_probe_tap_obj ) },
    { MP_ROM_QSTR( MP_QSTR_probe_read_blocking ), MP_ROM_PTR( &jl_probe_read_blocking_obj ) },
    { MP_ROM_QSTR( MP_QSTR_probe_read_nonblocking ), MP_ROM_PTR( &jl_probe_read_nonblocking_obj ) },

    // Probe button functions
    { MP_ROM_QSTR( MP_QSTR_probe_button_blocking ), MP_ROM_PTR( &jl_probe_button_blocking_obj ) },
    { MP_ROM_QSTR( MP_QSTR_probe_button_nonblocking ), MP_ROM_PTR( &jl_probe_button_nonblocking_obj ) },

    // Probe touch/read aliases (parameterized versions support blocking=True/False)
    { MP_ROM_QSTR( MP_QSTR_probe_read ), MP_ROM_PTR( &jl_probe_read_param_obj ) },
    { MP_ROM_QSTR( MP_QSTR_read_probe ), MP_ROM_PTR( &jl_read_probe_param_obj ) },
    { MP_ROM_QSTR( MP_QSTR_probe_wait ), MP_ROM_PTR( &jl_probe_wait_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wait_probe ), MP_ROM_PTR( &jl_wait_probe_obj ) },
    { MP_ROM_QSTR( MP_QSTR_probe_touch ), MP_ROM_PTR( &jl_probe_touch_obj ) },
    { MP_ROM_QSTR( MP_QSTR_wait_touch ), MP_ROM_PTR( &jl_wait_touch_obj ) },

    // Probe button aliases (parameterized versions support blocking=True/False)
    { MP_ROM_QSTR( MP_QSTR_get_button ), MP_ROM_PTR( &jl_get_button_param_obj ) },
    { MP_ROM_QSTR( MP_QSTR_button_read ), MP_ROM_PTR( &jl_button_read_param_obj ) },
    { MP_ROM_QSTR( MP_QSTR_read_button ), MP_ROM_PTR( &jl_read_button_param_obj ) },
    { MP_ROM_QSTR( MP_QSTR_probe_button ), MP_ROM_PTR( &jl_probe_button_param_obj ) },

    // Probe button non-blocking aliases
    { MP_ROM_QSTR( MP_QSTR_check_button ), MP_ROM_PTR( &jl_check_button_obj ) },
    { MP_ROM_QSTR( MP_QSTR_button_check ), MP_ROM_PTR( &jl_button_check_obj ) },

    // Clickwheel functions
    { MP_ROM_QSTR( MP_QSTR_clickwheel_up ), MP_ROM_PTR( &jl_clickwheel_up_obj ) },
    { MP_ROM_QSTR( MP_QSTR_clickwheel_down ), MP_ROM_PTR( &jl_clickwheel_down_obj ) },
    { MP_ROM_QSTR( MP_QSTR_clickwheel_press ), MP_ROM_PTR( &jl_clickwheel_press_obj ) },

    // Service management functions
    { MP_ROM_QSTR( MP_QSTR_force_service ), MP_ROM_PTR( &jl_force_service_obj ) },
    { MP_ROM_QSTR( MP_QSTR_force_service_by_index ), MP_ROM_PTR( &jl_force_service_by_index_obj ) },
    { MP_ROM_QSTR( MP_QSTR_get_service_index ), MP_ROM_PTR( &jl_get_service_index_obj ) },

    // Probe switch functions
    { MP_ROM_QSTR( MP_QSTR_get_switch_position ), MP_ROM_PTR( &jl_get_switch_position_obj ) },
    { MP_ROM_QSTR( MP_QSTR_set_switch_position ), MP_ROM_PTR( &jl_set_switch_position_obj ) },
    { MP_ROM_QSTR( MP_QSTR_check_switch_position ), MP_ROM_PTR( &jl_check_switch_position_obj ) },
    { MP_ROM_QSTR( MP_QSTR_probe_autoconnect ), MP_ROM_PTR( &jl_probe_autoconnect_obj ) },

    // Clickwheel (rotary encoder) functions
    { MP_ROM_QSTR( MP_QSTR_clickwheel_get_position ), MP_ROM_PTR( &jl_clickwheel_get_position_obj ) },
    { MP_ROM_QSTR( MP_QSTR_clickwheel_reset_position ), MP_ROM_PTR( &jl_clickwheel_reset_position_obj ) },
    { MP_ROM_QSTR( MP_QSTR_clickwheel_get_direction ), MP_ROM_PTR( &jl_clickwheel_get_direction_obj ) },
    { MP_ROM_QSTR( MP_QSTR_clickwheel_get_button ), MP_ROM_PTR( &jl_clickwheel_get_button_obj ) },
    { MP_ROM_QSTR( MP_QSTR_clickwheel_is_initialized ), MP_ROM_PTR( &jl_clickwheel_is_initialized_obj ) },

    // Help functions
    { MP_ROM_QSTR( MP_QSTR_help ), MP_ROM_PTR( &jl_help_obj ) },
    { MP_ROM_QSTR( MP_QSTR_nodes_help ), MP_ROM_PTR( &jl_help_nodes_obj ) },

    // Filesystem functions
    { MP_ROM_QSTR( MP_QSTR_fs_exists ), MP_ROM_PTR( &jl_fs_exists_obj ) },
    { MP_ROM_QSTR( MP_QSTR_fs_listdir ), MP_ROM_PTR( &jl_fs_listdir_obj ) },
    { MP_ROM_QSTR( MP_QSTR_fs_read ), MP_ROM_PTR( &jl_fs_read_file_obj ) },
    { MP_ROM_QSTR( MP_QSTR_fs_write ), MP_ROM_PTR( &jl_fs_write_file_obj ) },
    { MP_ROM_QSTR( MP_QSTR_fs_cwd ), MP_ROM_PTR( &jl_fs_get_current_dir_obj ) },

    // JFS Module - Comprehensive filesystem API
    { MP_ROM_QSTR( MP_QSTR_jfs ), MP_ROM_PTR( &jfs_user_cmodule ) },

    // Waveform constants
    { MP_ROM_QSTR( MP_QSTR_SINE ), MP_ROM_INT( 0 ) },
    { MP_ROM_QSTR( MP_QSTR_TRIANGLE ), MP_ROM_INT( 1 ) },
    { MP_ROM_QSTR( MP_QSTR_SAWTOOTH ), MP_ROM_INT( 2 ) },
    { MP_ROM_QSTR( MP_QSTR_SQUARE ), MP_ROM_INT( 3 ) },
    // Aliases and extras for waveforms
    { MP_ROM_QSTR( MP_QSTR_RAMP ), MP_ROM_INT( 2 ) },
    { MP_ROM_QSTR( MP_QSTR_ARBITRARY ), MP_ROM_INT( 4 ) },
};

static MP_DEFINE_CONST_DICT( jumperless_module_globals, jumperless_module_globals_table );

const mp_obj_module_t jumperless_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&jumperless_module_globals,
};
