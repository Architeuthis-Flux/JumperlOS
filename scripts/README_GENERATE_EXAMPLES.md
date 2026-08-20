# MicroPython Examples Generator

This script automatically converts Python example files into C string literals for embedding in the Jumperless firmware.

## 🎯 Purpose

The `generate_micropython_examples.py` script:
1. Scans `scripts/ex/` for Python example files
2. Converts each `.py` file to a C raw string literal
3. Generates `src/snakes/micropythonExamples.h` with all examples embedded
4. Includes the `jumperless_module.py` wrapper and `jumperless.pyi` stub

This allows MicroPython examples to be:
- Embedded directly in firmware (no SD card needed)
- Accessed from the MicroPython filesystem
- Updated easily by editing Python files and regenerating

## 🚀 Usage

### Basic Usage

```bash
# From project root
python3 scripts/generate_micropython_examples.py
```

### Full Workflow

```bash
# 1. Edit Python examples
vim scripts/ex/my_example.py

# 2. Regenerate header
python3 scripts/generate_micropython_examples.py

# 3. Rebuild firmware
pio run -t upload
```

## 📁 File Locations

| Path | Description |
|------|-------------|
| `scripts/ex/*.py` | Source Python examples |
| `pythonStuff/jumperless_module.py` | Main module wrapper |
| `pythonStuff/jumperless.pyi` | Type stub for IDEs |
| `src/snakes/micropythonExamples.h` | Generated C header (output) |
| `scripts/generate_micropython_examples.py` | This generator script |

## ⚙️ Configuration

### Excluding Files

Edit the `exclude_files` set in the script to skip specific files:

```python
exclude_files = {
    '__init__.py',
    'fuck.py',           # Old version of interaction_demo
    'batt.py',           # Incomplete battery test
    'lipo_char.py',      # Incomplete battery test
    'listFiles.py',      # ViperIDE utility, not a demo
    'my_test.py',        # Add your own exclusions here
}
```

### Adding New Categories

To add a new category for compile-time disabling:

1. Edit the `categories` dict in `generate_header_from_directory()`:

```python
categories = {
    "HARDWARE": ["dac_basics", "adc_basics", "gpio_basics", "node_connections"],
    "DEMO": ["led_brightness_control", "voltage_monitor", "stylophone"],
    "UTILITY": ["readme", "test_runner"],
    "MACHINE": ["uart_basics", "uart_loopback"],
    "MYCATEGORY": ["my_example1", "my_example2"],  # Add here
}
```

2. Regenerate the header
3. Use `#define DISABLE_MYCATEGORY_EXAMPLES` to disable the group

## 📝 Output Format

The generated header has this structure:

```c
#ifndef MICROPYTHON_EXAMPLES_H
#define MICROPYTHON_EXAMPLES_H

// Compile-time configuration
#define INCLUDE_ADC_BASICS
#define INCLUDE_DAC_BASICS
// ... more defines

// Category disable macros
#ifdef DISABLE_ALL_EXAMPLES
#undef INCLUDE_ADC_BASICS
// ... more undefs
#endif

// Individual example strings
#ifdef INCLUDE_ADC_BASICS
const char* ADC_BASICS_PY = R"(
"""Example docstring"""
import jumperless as j
# ... Python code
)";
#endif

// Jumperless module wrapper
#ifdef INCLUDE_JUMPERLESS_MODULE
const char* JUMPERLESS_MODULE_PY = R"(
# ... jumperless_module.py content
)";
#endif

// Type stub
#ifdef INCLUDE_JUMPERLESS_STUB
const char* JUMPERLESS_STUB_PYI = R"(
# ... jumperless.pyi content
)";
#endif

#endif
```

## 🔍 How It Works

### 1. File Scanning

```python
py_files = sorted([f for f in source_dir.glob('*.py') 
                  if f.name not in exclude_files])
```

Scans `scripts/ex/` for all `.py` files except those in `exclude_files`.

### 2. C String Conversion

```python
def py_to_c_string(py_content: str) -> str:
    if ')"' in py_content:
        # Use delimited raw string literal
        return f'R"===({py_content})==="'
    else:
        return f'R"({py_content})"'
```

Uses C++11 raw string literals:
- Standard format: `R"(...)"` for most content
- Delimited format: `R"===(...)==="` when content contains `)"` sequence

This automatically handles Python docstrings, comments, and all special characters without manual escaping.

### 3. Variable Naming

```python
def filename_to_var_name(filename: str) -> str:
    base = filename.replace('.py', '').upper()
    return f"{base}_PY"
```

Converts filenames to C variable names:
- `adc_basics.py` → `ADC_BASICS_PY`
- `uart_loopback.py` → `UART_LOOPBACK_PY`

### 4. Define Names

```python
def filename_to_define_name(filename: str) -> str:
    base = filename.replace('.py', '').upper()
    return f"INCLUDE_{base}"
```

Converts filenames to preprocessor defines:
- `adc_basics.py` → `INCLUDE_ADC_BASICS`
- `uart_loopback.py` → `INCLUDE_UART_LOOPBACK`

## 🐛 Troubleshooting

### Script fails with "No such file or directory"

**Problem:** Can't find source or output directories

**Solution:** Run from project root:
```bash
cd /path/to/JumperlOS
python3 scripts/generate_micropython_examples.py
```

### Generated header has syntax errors

**Problem:** ~~Python code contains `)"` sequence which breaks raw string literal~~

**Solution:** ✅ **FIXED** - The script now automatically uses delimited raw string literals (`R"===(...)==="`) when it detects `)"` in the content. This handles all Python code including docstrings and type hints.

### Warning: "No jumperless module found"

**Problem:** Can't find `jumperless_module.py` or `jumperless.py`

**Solution:** Ensure `pythonStuff/jumperless_module.py` exists

### Examples not appearing in firmware

**Problem:** Generated header not being compiled

**Solution:**
1. Check that `src/snakes/micropythonExamples.h` was updated
2. Verify the header is included in the build
3. Clean and rebuild: `pio run -t clean && pio run`

## 🔧 Advanced Usage

### Custom Source Directory

Modify the script to use a different source directory:

```python
# In main()
examples_dir = project_root / 'my_custom_examples'
```

### Multiple Output Headers

Generate separate headers for different categories:

```python
# Generate hardware examples
generate_header_from_directory(
    examples_dir / 'hardware',
    output_dir / 'hardwareExamples.h'
)

# Generate demo examples
generate_header_from_directory(
    examples_dir / 'demos',
    output_dir / 'demoExamples.h'
)
```

### Pre-processing Python Files

Add validation or transformation before conversion:

```python
# In generate_header_from_directory()
for py_file in py_files:
    with open(py_file, 'r') as f:
        py_content = f.read()
    
    # Validate syntax
    try:
        compile(py_content, py_file.name, 'exec')
    except SyntaxError as e:
        print(f"Warning: Syntax error in {py_file.name}: {e}")
        continue
    
    # Transform content (e.g., strip comments)
    # py_content = transform(py_content)
    
    c_string = py_to_c_string(py_content)
    # ... continue
```

## 📊 Statistics

The script outputs useful statistics:

```
Processing adc_basics.py...
Processing dac_basics.py...
...
Found jumperless module at /path/to/jumperless_module.py
Found type stub at /path/to/jumperless.pyi

✓ Generated /path/to/micropythonExamples.h
✓ Processed 10 example files
```

## 🔗 Related Files

- `scripts/ex/README.md` - Documentation for example files
- `src/snakes/micropythonExamples.h` - Generated output (do not edit manually!)
- `platformio.ini` - Build configuration

## 📚 References

- [C++ Raw String Literals](https://en.cppreference.com/w/cpp/language/string_literal)
- [Python pathlib](https://docs.python.org/3/library/pathlib.html)
- [MicroPython Documentation](https://docs.micropython.org/)

