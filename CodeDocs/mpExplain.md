# Comprehensive MicroPython Configuration Reference Manual

This document provides a comprehensive list of compile-time configuration flags for MicroPython. It is optimized for engineering custom board profiles for high-resource microcontrollers like the **Raspberry Pi RP2350B** featuring expanded external memories (e.g., 16MB Flash, 8MB PSRAM).

To apply these flags to a custom hardware target, define them inside your local `mpconfigboard.h` file. This ensures your definitions cleanly override the target family defaults in `ports/rp2/mpconfigport.h` and the global options in `py/mpconfig.h`.

---

## 📂 1. Feature Profile Levels

These foundational selectors mass-toggle default feature groups before individual exceptions are applied.


| Flag Macro | Type / Values | Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_CONFIG_ROM_LEVEL` | `LEVEL_MINIMUM`<br>`LEVEL_CORE`<br>`LEVEL_BASIC`<br>`LEVEL_FEATURES`<br>`LEVEL_EXTRA_FEATURES`<br>`LEVEL_EVERYTHING` | **Sets the baseline feature profile.** Unlocks or disables groups of core features at once. For an RP2350B with 16MB of Flash, explicitly force this to `MICROPY_CONFIG_ROM_LEVEL_EVERYTHING` to enable all capabilities. |

---

## 🧠 2. RAM Memory Management & External PSRAM Allocation

These variables govern how MicroPython structures its RAM heaps, controls the garbage collection (GC) execution cycles, and interfaces with external PSRAM IC hardware blocks.


| Flag Macro | Recommended Setting | Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_ENABLE_GC` | `(1)` | **Activates the Garbage Collector.** Must be enabled to prevent memory leaks and track runtime heap usage. |
| `MICROPY_GC_SPLIT_HEAP` | `(1)` | **Enables multi-region memory pools.** Essential for systems with separate physical RAM arrays. It allows MicroPython to treat internal SRAM and external 8MB PSRAM as a continuous, unified pool. |
| `MICROPY_ALLOC_GC_STACK_SIZE` | `(1024)` | **Sets the memory traceback stack size.** Defines the number of entries allocated inside the core engine to track nested variables during active collection swept phases. |
| `MICROPY_MALLOC_USES_ALLOCATED_SIZE` | `(1)` | **Passes sizes directly to free/realloc.** Enhances overall memory performance if using customized standard library heap implementations. |
| `MICROPY_MEM_STATS` | `(0)` | **Enables tracking of raw memory diagnostic states.** If enabled `(1)`, allows script invocation of memory statistics utilities, though it adds minor performance overhead. |
| `MICROPY_STACK_CHECK` | `(1)` | **Enables C call stack bounds checking.** Catches out-of-memory stack overflows early, raising a `RuntimeError` rather than letting the hardware silently crash. |

---

## 🗂️ 3. Flash Memory Allocations & Filesystems (VFS)

These parameters control code layout mapping across your 16MB physical Flash chip and define which storage filesystems are mounted at boot.


| Flag Macro | Recommended Setting | Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_VFS` | `(1)` | **Enables the Virtual Filesystem layer.** Provides abstract file handling infrastructure so multiple drives can share the root directory structure. |
| `MICROPY_VFS_FAT` | `(1)` | **Enables FAT file system support.** Necessary if you want your device to appear as a standard external flash drive when plugged into a PC. |
| `MICROPY_VFS_LFS2` | `(1)` | **Enables LittleFS2 support.** A power-safe filesystem optimized for microcontrollers. It features robust wear-leveling algorithms to extend the life of your raw flash memory. |
| `MICROPY_READER_VFS` | `(1)` | **Enables the VFS import loader.** Allows the core Python interpreter to look up, parse, and load source files directly from your active filesystem directories. |
| `MICROPY_HW_FLASH_STORAGE_BASE` | `(0x10000000)` | **Sets the physical starting address of the flash memory.** Matches the memory-mapped flash address space of the RP2350 microcontroller. |
| `MICROPY_HW_FLASH_STORAGE_BYTES` | `(14 * 1024 * 1024)` | **Sets the internal file storage size.** Dedicates a massive 14MB segment of your 16MB chip strictly to user scripts, reserving the rest for the core firmware binary. |

---

## ⚙️ 4. Compiler, Execution Speed, & Runtime Optimizations

These values fine-tune the performance of the virtual machine (VM). They swap software logic loops for optimized hardware execution branches at the cost of firmware size.


| Flag Macro | Recommended Setting | Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_ENABLE_COMPILER` | `(1)` | **Compiles plain Python code on-device.** If disabled `(0)`, the chip can only execute pre-compiled `.mpy` files, saving roughly 80KB of flash space. Leave enabled on high-resource boards. |
| `MICROPY_OPT_COMPUTED_GOTO` | `(1)` | **Uses indirect jump tables for opcode loops.** Replaces slow, standard C `switch-case` loops with direct GNU C pointer jumps. This delivers up to a 15% increase in code execution speed. |
| `MICROPY_OPT_CACHE_MAP_LOOKUP_IN_BYTECODE` | `(1)` | **Caches dictionary and class attribute lookups.** Speeds up repeated data calls inside performance-heavy loops. |
| `MICROPY_OPT_LOAD_ATTR_FAST_PATH` | `(1)` | **Bypasses full method lookup routines.** Speeds up variable access when an object's attribute arrangement matches predictable, standardized patterns. |
| `MICROPY_COMP_CONST` | `(1)` | **Inlines constant expressions at compile-time.** Optimizes code structure by replacing expressions flagged with `const()` with their direct literal values. |
| `MICROPY_COMP_DOUBLE_TUPLE_ASSIGN` | `(1)` | **Optimizes double variable swaps.** Accelerates assignments like `X, Y = Y, X` by using fast, direct register manipulation. |

---

## 💻 5. Native Code Generation (Emitters & Assemblers)

MicroPython can compile Python syntax down to raw, native machine code. This bypasses the virtual machine layer entirely for speed-critical scripts.


| Flag Macro | Recommended Setting | Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_EMIT_THUMB` | `(1)` | **Enables the `@micropython.native` decorator.** Compiles targeted Python functions directly into native ARM Thumb assembly instructions. |
| `MICROPY_EMIT_INLINE_THUMB` | `(1)` | **Enables the `@micropython.asm_thumb` decorator.** Allows you to write raw inline assembly routines directly inside your standard Python scripts. |
| `MICROPY_EMIT_ARMV7M` | `(1)` | **Enables ARMv7-M hardware instruction pipelining.** Optimizes code execution for modern Cortex-M cores (including the Cortex-M33 found on the RP2350). |
| `MICROPY_EMIT_INLINE_THUMB_FLOAT` | `(1)` | **Passes floats directly to the hardware FPU.** Allows inline assembly to interact directly with the hardware floating-point unit registers. |

---

## 🔢 6. Core Data Types & Language Representation

These settings configure how Python variables are stored in RAM registers and determine the precision of mathematical operations.


| Flag Macro | Recommended Setting | Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_OBJ_REPR` | `MICROPY_OBJ_REPR_A` | **Sets the pointer representation model.** `REPR_A` is the standard choice for 32-bit platforms, packing object references and small integers into single 32-bit registers. |
| `MICROPY_FLOAT_IMPL` | `MICROPY_FLOAT_IMPL_FLOAT` | **Enables single-precision hardware floats.** Matches the native capabilities of the RP2350 hardware FPU. Set to `DOUBLE` only if your application requires 64-bit precision via software emulation. |
| `MICROPY_LONGINT_IMPL` | `MICROPY_LONGINT_IMPL_MPZ` | **Enables arbitrary-precision integers (bignums).** Allows integers to grow dynamically based on available RAM, removing standard 32-bit or 64-bit overflow limits. |
| `MICROPY_STREAMS_NON_BLOCK` | `(1)` | **Enables non-blocking stream attributes.** Essential for handling asynchronous network sockets and responsive peripheral communications. |
| `MICROPY_MODULE_BUILTIN_INIT` | `(1)` | **Auto-initializes core C modules.** Runs background setup routines for embedded libraries automatically during the initial system boot phase. |

---

## 💬 7. REPL, Shell Interaction, & Debugging Diagnostics

These variables control user interaction through the serial terminal interface and manage error logging verbosity.


| Flag Macro | Recommended Setting | Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_HELPER_REPL` | `(1)` | **Enables the interactive command-line interface (REPL).** Provides the standard command line interface over serial connections. |
| `MICROPY_REPL_EMACS_KEYS` | `(1)` | **Enables standard shell terminal navigation keys.** Enables keyboard shortcuts like `Ctrl+A` (jump to line start) and `Ctrl+E` (jump to line end). |
| `MICROPY_REPL_AUTO_INDENT` | `(1)` | **Enables automatic text indentation.** Automatically formats code structure blocks during multiline script input inside the REPL terminal. |
| `MICROPY_ERROR_REPORTING` | `MICROPY_ERROR_REPORTING_DETAILED` | **Enables verbose error tracing.** Provides detailed exception error reports and complete stack trace logs instead of simple one-word alerts. |
| `MICROPY_WARNINGS` | `(1)` | **Enables the runtime warning system.** Allows the interpreter to catch and display non-fatal development issues through `sys.warnoptions`. |
| `MICROPY_ENABLE_SOURCE_LINE` | `(1)` | **Retains script line numbers in flash.** Ensures error traceback logs point directly to the exact source code line that triggered the exception. |

---

## 🔀 8. Multicore Threads & Concurrency Control

These flags manage background tasks and control how MicroPython leverages the RP2350's dual-core architecture.


| Flag Macro | Recommended Setting | Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_PY_THREAD` | `(1)` | **Enables the low-level `_thread` module.** Allows the execution of separate background tasks on the second CPU core. |
| `MICROPY_PY_THREAD_GIL` | `(1)` | **Enables the Global Interpreter Lock.** Protects memory integrity by ensuring only one thread accesses the core MicroPython VM state at a time. |

## 📦 9. Built-In Standard Python Modules (`MICROPY_PY_*`)

These settings control the inclusion of built-in libraries. Setting a flag to `(1)` compiles the module directly into the firmware binary, making it instantly available for import.


| Flag Macro | Recommended Setting | Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_PY_BUILTINS_HELP` | `(1)` | **Enables interactive documentation.** Adds the `help()` command to the REPL for quick on-device reference. |
| `MICROPY_PY_BUILTINS_HELP_TEXT` | `mp_help_default_text` | **Defines the help reference text.** Points to the core documentation string array printed when calling `help()`. |
| `MICROPY_PY_BUILTINS_SET` | `(1)` | **Enables mutable Python set types.** Provides native support for unique value arrays and set math operations. |
| `MICROPY_PY_BUILTINS_FROZENSET` | `(1)` | **Enables immutable frozenset types.** Allows sets to be hashed and used as dictionary keys. |
| `MICROPY_PY_BUILTINS_SLICE` | `(1)` | **Enables data array slicing.** Supports standard sequence indexing techniques like `data[1:10:2]`. |
| `MICROPY_PY_BUILTINS_PROPERTY` | `(1)` | **Enables object properties.** Allows classes to use custom getter, setter, and deleter methods via the `@property` decorator. |
| `MICROPY_PY_BUILTINS_MIN_MAX` | `(1)` | **Compiles optimized min/max routines.** Speeds up calculations using built-in `min()` and `max()` methods. |
| `MICROPY_PY_BUILTINS_STR_COUNT` | `(1)` | **Enables the string count method.** Compiles native support for `str.count()` algorithms. |
| `MICROPY_PY_BUILTINS_STR_OP_MODULO` | `(1)` | **Enables string modulo formatting.** Supports classic C-style string interpolation using the `%` operator. |
| `MICROPY_PY_MATH` | `(1)` | **Enables the standard `math` library.** Provides full access to trigonometric, logarithmic, and floating-point math functions. |
| `MICROPY_PY_MATH_SPECIAL_FUNCTIONS` | `(1)` | **Enables advanced math functions.** Compiles operations like `erf`, `erfc`, `gamma`, and `lgamma` into the `math` module. |
| `MICROPY_PY_CMATH` | `(1)` | **Enables the `cmath` library.** Adds mathematical function support specifically for complex number handling. |
| `MICROPY_PY_UJSON` | `(1)` | **Enables the `json` parsing library.** Provides fast serialization and deserialization functions for JSON data strings. |
| `MICROPY_PY_URE` | `(1)` | **Enables the `re` regex engine.** Provides advanced string manipulation using regular expression pattern matching. |
| `MICROPY_PY_URE_SUB` | `(1)` | **Enables regex match substitution.** Adds support for the `re.sub()` function to replace string patterns. |
| `MICROPY_PY_UBINASCII` | `(1)` | **Enables the `binascii` conversion library.** Provides methods for encoding data blocks into Hex, Base64, or binary strings. |
| `MICROPY_PY_UHASHLIB` | `(1)` | **Enables the `hashlib` cryptographic hashing engine.** Provides secure hashing algorithms including MD5, SHA-1, and SHA-256. |
| `MICROPY_PY_UHASHLIB_SHA256` | `(1)` | **Enables explicit SHA-256 hardware acceleration.** Forces optimization hooks using the RP2350's internal security pipelines if available. |
| `MICROPY_PY_UCRYPTOLIB` | `(1)` | **Enables the `cryptolib` cipher engine.** Adds native support for raw block encryption algorithms such as AES. |
| `MICROPY_PY_FRAMEBUF` | `(1)` | **Enables the display `framebuf` library.** Provides a simple pixel-drawing canvas used to render text and shapes on external displays. |
| `MICROPY_PY_SYS` | `(1)` | **Enables the core `sys` system module.** Provides direct access to interpreter configurations, paths, and platform metadata. |
| `MICROPY_PY_SYS_MAXSIZE` | `(1)` | **Exposes the standard platform integer limit.** Adds the `sys.maxsize` integer attribute to runtime environment queries. |
| `MICROPY_PY_SYS_EXIT` | `(1)` | **Enables runtime exit routines.** Compiles the `sys.exit()` command to programmatically stop code loops. |
| `MICROPY_PY_SYS_STDFILES` | `(1)` | **Exposes standard system stream paths.** Maps `sys.stdin`, `sys.stdout`, and `sys.stderr` pipelines out to terminal hooks. |
| `MICROPY_PY_IO` | `(1)` | **Enables the core `io` stream library.** Standardizes reading and writing behaviors for text and binary file operations. |
| `MICROPY_PY_IO_FILEIO` | `(1)` | **Enables the raw `FileIO` class pipeline.** Permits working directly with target hardware disk drives at the block-by-block storage layer. |
| `MICROPY_PY_COLLECTIONS` | `(1)` | **Enables the `collections` data structure module.** Provides advanced containers including `namedtuple`, `deque`, and `OrderedDict`. |

---

## 🔌 10. Hardware Peripheral & Driver Integration Interfaces

These parameters configure MicroPython's direct access to the microcontroller's physical input/output hardware.


| Flag Macro | Recommended Setting | Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_PY_MACHINE` | `(1)` | **Enables the `machine` module.** Provides direct Python access to low-level hardware peripherals like GPIO, I2C, SPI, UART, and PWM. |
| `MICROPY_PY_MACHINE_I2C` | `(1)` | **Enables hardware I2C support.** Compiles the native peripheral driver code into the `machine.I2C` module namespace. |
| `MICROPY_PY_MACHINE_SPI` | `(1)` | **Enables hardware SPI support.** Compiles the native master/slave peripheral controller driver code into `machine.SPI`. |
| `MICROPY_HW_PIN_EXT_COUNT` | `(48)` | **Sets the total physical GPIO pin count.** Configures the pin database to match the expanded 48-pin layout of the RP2350B package variant. |
| `MICROPY_HW_ENABLE_USBDEV` | `(1)` | **Enables the onboard USB driver stack.** Allows the chip to act as a native USB device when connected to a host computer. |
| `MICROPY_HW_USB_CDC` | `(1)` | **Routes the REPL over USB serial.** Standardizes console communication over a virtual USB serial port connection. |
| `MICROPY_HW_USB_MSC` | `(1)` | **Enables USB Mass Storage.** Automatically mounts the internal flash file system as a plug-and-play USB drive on host operating systems. |
| `MICROPY_HW_USB_VID` | `(0x2E8A)` | **Sets the USB Vendor ID registration number.** `0x2E8A` corresponds to the official Raspberry Pi identifier. |
| `MICROPY_HW_USB_PID` | `(0x0005)` | **Sets the USB Product ID registration number.** Tells the host computer exactly what type of active device profile is interfacing with the port. |

---

---


# MicroPython Deep-Architectural & Obscure Configuration Manual

This document details the low-level, hidden, and highly specialized compile-time configuration macros in MicroPython. These flags are typically left at default settings but can be overridden in your `mpconfigboard.h` file to tweak VM performance, control string allocation, or hack internal engine behaviors for advanced chips like the RP2350B.

---

## 🔬 1. VM Tweaks & Micro-Optimizations

These switches alter how the internal Virtual Machine loop processes opcodes, routes internal functions, and optimizes hardware register allocations.


| Flag Macro | Recommended Setting | Deep-Dive Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_OPT_LOAD_ATTR_FAST_PATH` | `(1)` | **Bypasses full dictionary lookups for object attributes.** It uses a single-entry cache mechanism in the bytecode stream to guess the attribute slot. If the object layout has not changed, it resolves instantly. |
| `MICROPY_OPT_STORE_ATTR_FAST_PATH` | `(1)` | **Applies the same single-entry cache logic to attribute writes.** Speeds up repeated variables assignments like `self.x = value` inside tight execution loops. |
| `MICROPY_PERSISTENT_CODE_LOAD` | `(1)` | **Enables native loading of pre-compiled bytecode scripts.** Allows the runtime engine to parse and execute cross-compiled `.mpy` files straight out of memory or a flash stream. |
| `MICROPY_PERSISTENT_CODE_SAVE` | `(0)` | **Allows saving generated bytecode back out as an .mpy file.** Typically used only on full Unix ports. Leave disabled `(0)` on microcontrollers to save internal firmware space unless building a native on-device IDE. |
| `MICROPY_COMP_MODULE_CONST` | `(1)` | **Enables cross-module constant folding.** Allows access optimization for constants across separate modules at compile time if they are explicitly imported using the `from module import const` syntax. |
| `MICROPY_PREV_ALLOC_TYPES` | `(1)` | **Caches previous memory allocation types.** Speeds up the internal C function `gc_alloc` by allowing it to prioritize recycling blocks of the exact same size that were just discarded. |

---

## 🔤 2. String Interning (QSTR) & Internal Hashing Mechanics

MicroPython stores string constants and variable identifiers as unique integer tokens called "QSTRs" (Qualified Strings). These flags control how the internal hashing engine handles text.


| Flag Macro | Recommended Setting | Deep-Dive Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_QSTR_BYTES_IN_LEN` | `(1)` | **Allocates the byte size field for string length tracking.** For an RP2350 with plenty of memory, setting this to `(1)` limits individual string variable sizes to 255 characters. Set to `(2)` if you must manipulate continuous string literals up to 65,535 bytes long. |
| `MICROPY_QSTR_BYTES_IN_HASH` | `(1)` | **Sets the size of the internal QSTR lookup hash table values.** A setting of `(1)` uses an 8-bit hash. On a high-resource 16MB board with thousands of strings, changing this to `(2)` switches to a 16-bit hash, drastically reducing hash collisions and speeding up variable lookups at the expense of minor RAM overhead. |
| `MICROPY_ALLOC_QSTR_CHUNK_INIT` | `(128)` | **Defines the baseline allocation block size for new string tokens.** Sets the amount of heap space grabbed at once when building the runtime string pool, reducing heap fragmentation as scripts run. |

---

## 🧠 3. Advanced Memory Hooks & Allocation Policies

These configuration definitions modify the lower boundaries of the garbage collection allocator and allow you to tweak how code tokens are safely evaluated.


| Flag Macro | Recommended Setting | Deep-Dive Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_TRACK_ALLOCATED_BYTES` | `(0)` | **Enables continuous byte tracking allocations.** Tracks exactly how many bytes are currently allocated by the runtime. Turning this on `(1)` allows scripts to call `gc.mem_alloc()`, but adds minor clock cycles to every single allocation and free routine. |
| `MICROPY_FREE_UNUSED_PARENTS_BEFORE_REALLOC` | `(1)` | **Aggressively forces compiler parser memory cleanup.** Instructs the code compiler parser to aggressively free temporary parental AST parsing nodes before requesting memory reallocations, keeping the temporary RAM footprint minimal during live script compilation. |
| `MICROPY_REUSE_AS_OBJ_IDS` | `(0)` | **Recycles raw memory pointers as direct Python unique object IDs.** Using `(0)` maps unique IDs straight to the literal RAM memory addresses of the variables (`id(obj) == address`). Turning this to `(1)` uses an abstract tracking index table instead. |

---

## 🐛 4. Deep Diagnostic Tracing & VM Debugging Hooks

*Warning: Setting any of these to `(1)` will flood your serial REPL terminal with low-level diagnostic data and drastically slow down execution. Use them only when troubleshooting custom C extensions.*


| Flag Macro | Default Setting | Deep-Dive Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_DEBUG_VM_EXEC` | `(0)` | **Prints every single VM opcode instruction as it executes.** Allows step-by-step assembly-level tracking of the virtual interpreter machine loop. |
| `MICROPY_DEBUG_PARSE` | `(0)` | **Dumps the full Abstract Syntax Tree (AST) layout map.** Prints the internal token parse tree to the terminal as a script is being compiled on the device. |
| `MICROPY_DEBUG_COMPILER` | `(0)` | **Dumps raw compiler bytecode output logs.** Displays the literal hex byte arrays generated by the compiler block before it passes them down to the VM runner. |
| `MICROPY_DEBUG_GC` | `(0)` | **Outputs internal logs for every single Garbage Collection cycle.** Prints structural data maps during the sweep and mark phases, showing exactly which memory blocks are retained or freed. |

---

## 🏗️ 5. Sub-Feature Language Dialect Selectors

These variables allow you to fine-tune Python language compatibility, letting you strip or add minor structural behaviors to change the runtime's footprint.


| Flag Macro | Recommended Setting | Deep-Dive Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_COMP_MODULE_ATTR_DELEGATION` | `(1)` | **Enables advanced module attribute hooks.** Allows modules to implement custom `__getattr__` and `__dir__` logic functions at the root package module scale. |
| `MICROPY_COMP_TRIPLE_TUPLE_ASSIGN` | `(1)` | **Optimizes three-way continuous variable unpacking.** Accelerates unpacking routines like `X, Y, Z = A, B, C` via high-speed stack shifts, building on the basic double-tuple optimizations. |
| `MICROPY_COMP_RETURN_IF_EXPR` | `(1)` | **Optimizes direct return statements.** Optimizes bytecode layout size for code blocks that instantly evaluate and return inline expressions, such as `return x + 1`. |
| `MICROPY_PY_DELATTR_SETATTR` | `(1)` | **Enables runtime module manipulation.** Compiles explicit support for the core Python functions `delattr()` and `setattr()`, allowing scripts to dynamically delete or modify class attributes at runtime. |
| `MICROPY_PY_BUILTINS_EXECFILE` | `(1)` | **Enables the raw script path executor function.** Adds the legacy `execfile("script.py")` built-in function to the global namespace, providing a shortcut to execute local files without reading them manually. |
| `MICROPY_PY_SYS_GETSIZEOF` | `(1)` | **Exposes the exact object RAM size in bytes.** Adds the `sys.getsizeof(object)` method, which queries the memory controller and returns the exact number of bytes an active variable uses in RAM. |


--- 
--- 

# MicroPython User C Modules & VM Dispatch Configuration Manual

This document details the configuration steps, flags, and structure required to relay custom C code directly into the MicroPython core engine, along with obscure flags used to hack the internal Virtual Machine (VM) instruction dispatch layout.

---

## 🛠️ 1. Integrating User C Modules (`USER_C_MODULES`)

User C Modules allow you to write high-performance C code (like specialized DSP algorithms, tight loops, or custom hardware drivers) and expose them natively as standard Python modules. This bypasses the Python interpreter completely for speed-critical tasks.

### External Directory Structure
Instead of placing your custom code inside the main MicroPython repository source tree, maintain an isolated folder structure:

```text
my_project/
├── modules/
│   └── c_math_extension/
│       ├── micropython.mk
│       └── c_math_extension.c
└── ports/
    └── rp2/
        └── mpconfigboard.h
```

### Writing the Configuration Makefile (`micropython.mk`)
MicroPython's build system uses a specific makefile format to discover your source files. Create a `micropython.mk` file inside your module directory:

```make
# Capture the current directory path of this module
USERMODULE_C_MATH_DIR := \$(current_dir)

# Add your C source files to the global compilation list
SRC_USERMOD += \$(USERMODULE_C_MATH_DIR)/c_math_extension.c

# Include this directory path so header files can be resolved
CFLAGS_USERMOD += -I\$(USERMODULE_C_MATH_DIR)
```

### Activating the Module at Build Time
You do not use a `#define` macro inside `mpconfigboard.h` to pull the files in. Instead, pass the absolute path of your modules folder to the `make` or `cmake` build command line argument:

```bash
# For CMake-based systems (like the RP2/RP2350 port)
make BOARD=MY_CUSTOM_RP2350B USER_C_MODULES=/path/to/my_project/modules
```

---

## 🏎️ 2. Virtual Machine Dispatch Mechanics

MicroPython's virtual machine is a giant loop that fetches, decodes, and executes bytecode opcodes. You can alter the underlying C execution style of this loop to prioritize either raw processing speed or minimal firmware binary footprint.

These flags change how the main interpreter loop in `py/vm.c` transfers control between individual bytecode instructions.



| Flag Macro | Recommended Setting | Deep-Dive Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_OPT_COMPUTED_GOTO` | `(1)` | **Swaps standard switch-cases for indirect threaded code.** Uses GNU C's label-as-values feature (`&&label`). Instead of a large jump table that bounces back to the top of a `switch` statement, each opcode ends by directly jumping to the memory address of the next opcode handler. This eliminates branch mispredictions and speeds up execution by up to 15% on the RP2350's Cortex-M33 cores. |
| `MICROPY_OPT_COMPUTED_GOTO_SAVE_SPACE` | `(0)` | **Compresses the lookup tables used by Computed GOTOs.** If enabled `(1)`, it forces the compiler to shrink the internal address jump table layout to save flash space, but introduces a minor processing delay when resolving the next instruction. Leave disabled `(0)` on your 16MB flash layout to maximize speed. |
| `MICROPY_VM_HOOK_INIT` | *Macro Expression* | **Relays custom C code execution at VM initialization.** Allows you to insert low-level C instructions inside the interpreter initialization phase just before the bytecode loops execute. |
| `MICROPY_VM_HOOK_LOOP` | *Macro Expression* | **Relays a C code hook at the top of every VM cycle iteration.** Executed before every single instruction fetch. Useful for custom low-level debugging systems, pinning background tasks, or refreshing thread watchers. *Warning: Relaying anything heavy here drastically slows down performance.* |
| `MICROPY_VM_HOOK_RETURN` | *Macro Expression* | **Relays a C code hook right before a function yields control.** Executes custom cleanups or tracking logs immediately before a VM function execution block returns its values. |

### Example Implementation of VM Hooks
To use VM hooks, define them as functional macro expressions directly inside your custom `mpconfigboard.h` file:

```c
// Example: Resetting an external hardware watchdog timer or checking 
// a custom pinned flag directly within the core Python interpreter execution loop
extern void my_custom_hardware_watchdog_pet(void);

#define MICROPY_VM_HOOK_LOOP  my_custom_hardware_watchdog_pet();
```

---

## ⚡ 3. Exception Handling & Stack Unwinding Mechanics

These internal parameters fine-tune how the interpreter processes runtime syntax execution faults, code exceptions, and memory-safe unrolling blocks.



| Flag Macro | Recommended Setting | Deep-Dive Operational Impact |
| :--- | :--- | :--- |
| `MICROPY_ENABLE_DYNRUNTIME` | `(1)` | **Enables native dynamic loading of external binary .mpy modules.** Allows your RP2350B to dynamically load compiled, position-independent machine code modules (`.mpy` compiled from C) over a filesystem at runtime, rather than requiring them to be frozen into the static firmware image at compile time. |
| `MICROPY_STACKLESS` | `(0)` | **Enables a completely heap-allocated, stackless VM footprint.** Changes the interpreter to handle Python-to-Python function calls using the heap instead of the physical C stack. This allows for deep or infinite recursion limits without causing a physical stack overflow, but introduces memory allocation overhead. Keep disabled `(0)` unless your scripts depend heavily on deeply nested asynchronous code loops. |
| `MICROPY_STACKLESS_STRICT` | `(0)` | **Enables strict stackless compliance checking rules.** Forces the heap allocation mechanism to apply aggressively to all expression contexts. It should only be enabled if `MICROPY_STACKLESS` is active. |
