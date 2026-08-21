# Building the Jumperless Native MicroPython Module

This guide explains how to build the Jumperless firmware with the native MicroPython module integration.

## Prerequisites

- PlatformIO installed and configured
- Git (for MicroPython repository cloning)
- Build tools (make, gcc, etc.)

## Build Process

### 1. Build MicroPython with Jumperless Module

First, build MicroPython with the native Jumperless module integrated:

```bash
# From the project root directory
./scripts/build_micropython.sh
```

This script will:
- Clone or update the MicroPython repository
- Build MicroPython embed port with the Jumperless module included
- Generate QSTR definitions for the Jumperless functions
- Verify the module integration

> **Read this before you add a Python binding.** That third bullet is the
> only thing that regenerates QSTRs, and **the ordinary `pio run` firmware
> build does not run it.** See
> [QSTRs are partly hand-maintained](#qstrs-are-partly-hand-maintained)
> below.

### 2. Build Jumperless Firmware

After MicroPython is built, compile the main firmware:

```bash
# Using PlatformIO
pio run

# Or using PlatformIO with specific environment
pio run -e pico
```

### 3. Upload to Device

```bash
# Upload firmware
pio run -t upload

# Or with specific environment
pio run -e pico -t upload
```

## Build Verification

The build script will verify:

1. **QSTR Generation**: Confirms MicroPython QSTR definitions are generated
2. **Jumperless QSTRs**: Checks that Jumperless-specific function names are included
3. **Module Files**: Verifies all required source files are present
4. **Configuration**: Ensures the module is enabled in the configuration

### Expected Output

```
◆ MicroPython embed build successful!
   Generated 1234 QSTR definitions
   Jumperless module QSTRs found: 25
   ◆ Jumperless MicroPython module found
   ◆ Jumperless API wrapper found
   ◆ Jumperless module enabled in configuration
◆ MicroPython is ready for use with Jumperless native module enabled!
```

## QSTRs are partly hand-maintained

`lib/micropython/micropython_embed/genhdr/qstrdefs.generated.h` says
"automatically generated" at the top, and it is **committed to this repo**.
Both are true, and together they are a trap:

- **The firmware build runs no qstr extraction pass.** `pio run` compiles the
  committed header as-is. Only `scripts/build_micropython.sh` regenerates it,
  and that works against an **external MicroPython checkout** that is not
  vendored here — so on most machines, most of the time, nothing regenerates
  it at all.
- **So adding a `MP_QSTR_foo` to `modules/jumperless/modjumperless.c` means
  hand-editing that header.** Skipping it does not fail quietly in an
  interesting way — the build stops with `'MP_QSTR_foo' undeclared`. That is
  the *good* outcome.

### The two rules a hand-added line must obey

1. **Byte-sorted insertion.** The `QDEF1` pool is registered with
   `is_sorted = true` (`py/qstr.c`), and `qstr_find_strn` **binary-searches**
   it with `strncmp`. Insert in byte-sorted position by the string literal —
   not alphabetically-ish, not at the end. A line in the wrong place does not
   error: the binary search simply fails to find some *other* qstr, and that
   name silently resolves as absent at runtime. **This is the silent failure
   mode.** Re-verify the whole pool is still sorted after editing.
2. **Correct hash.** The hash is MicroPython's DJB2-derived qstr hash, *not*
   the FNV-1a used elsewhere in this repo (the `/projects` provisioning
   table). Per byte:

   ```python
   h = 5381
   for b in name.encode():
       h = ((h * 33) ^ b) & 0xFFFF     # MICROPY_QSTR_BYTES_IN_HASH == 2
   h = h or 1                          # 0 is reserved for MP_QSTRnull
   ```

   Recompute the existing entries with this before trusting it — that check
   is how the algorithm and the 2-byte width were confirmed empirically.

The `QDEF1` line format is
`QDEF1(MP_QSTR_<name>, <hash>, <byte length>, "<name>")`.

Precedent: commit `68b93e3` added 28 lines this way; the guided-placement
branch added 10 more (`load_project`, `place_part`, `remove_part`,
`list_parts`, `guide_progress`, `row`, `footprint`, `placed`, `pins`,
`class`). A real regeneration reproduces hand-added entries identically, so
this is a shortcut around a missing build step, not a fork of the file.

## Troubleshooting

### Common Issues

1. **Module Not Found**
   ```
   ◇ Jumperless MicroPython module missing
   ```
   - Check that `lib/micropython/modjumperless.cpp` exists
   - Verify the USER_C_MODULES path is correct

2. **API Wrapper Missing**
   ```
   ◇ Jumperless API wrapper missing
   ```
   - Check that `src/JumperlessMicroPythonAPI.cpp` exists
   - Verify include paths in the source file

3. **No Jumperless QSTRs**
   ```
   Warning: No Jumperless module QSTRs detected
   ```
   - The module may not be properly registered
   - Check `MODULE_JUMPERLESS_ENABLED` is defined
   - Verify `MP_REGISTER_MODULE` call in the module source

4. **Build Errors**
   - Check that all include paths are correct in `platformio.ini`
   - Verify that required headers are available
   - Ensure `MODULE_JUMPERLESS_ENABLED=1` is set in build flags

### Debug Steps

1. **Check Module Registration**:
   ```bash
   grep -r "MP_QSTR_jumperless" lib/micropython/micropython_embed/
   ```

2. **Verify Build Flags**:
   ```bash
   grep -A5 "build_flags" platformio.ini
   ```

3. **Check Include Paths**:
   ```bash
   find . -name "modjumperless.cpp"
   find . -name "JumperlessMicroPythonAPI.cpp"
   ```

## Runtime Testing

After successful build and upload, test the module:

```python
# In MicroPython REPL
import jumperless

# Test basic functions
jumperless.dac_set(0, 2.5)
voltage = jumperless.adc_get(0)
print(f"ADC reading: {voltage}V")

# Test node connections
jumperless.nodes_connect(1, 5)
jumperless.nodes_disconnect(1, 5)
```

Or call the built-in test function from C++:
```cpp
testJumperlessNativeModule();
```

## File Structure

After building, the key files are:

```
lib/micropython/
├── modjumperless.cpp              # Native module implementation
├── micropython.mk                 # Module build configuration
├── mpconfigport.h                 # Module enabled flag
└── micropython_embed/             # Generated MicroPython files
    ├── genhdr/qstrdefs.generated.h    # QSTR definitions
    └── ...

src/
├── JumperlessMicroPythonAPI.cpp   # C++ wrapper functions
├── Python_Proper.cpp             # MicroPython integration
└── Python_Proper.h               # Header file

platformio.ini                    # Build configuration with module flags
```

## Performance Benefits

The native module provides significant improvements over the string-based approach:

- **~10x faster execution** - No string parsing overhead
- **Better memory efficiency** - No intermediate string buffers
- **Type safety** - Native Python type handling
- **Error handling** - Proper Python exceptions
- **IDE support** - Full Python syntax support

## Next Steps

After successful build:

1. Test all hardware functions through the native module
2. Update existing Python code to use direct function calls
3. Remove old string-based command parsing code
4. Add additional hardware functions as needed

For usage examples, see `examples/python_proper_native_demo.py`. 