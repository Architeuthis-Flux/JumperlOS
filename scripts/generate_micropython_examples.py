#!/usr/bin/env python3
"""
Generate micropythonExamples.h from Python files
Converts .py files into C string literals for embedding in firmware
"""

import json
import os
import sys
from pathlib import Path


def py_to_c_string(py_content: str) -> str:
    """
    Convert Python source to C raw string literal.
    Uses R\"(...)\" format, or R\"delimiter(...)delimiter\" if content contains )\"
    """
    # Check if content contains the )" sequence that would break raw string literal
    if ')"' in py_content:
        # Use delimited raw string literal
        # Find a delimiter that doesn't appear in the content
        delimiter = "==="
        attempt = 0
        while f'){delimiter}"' in py_content and attempt < 10:
            delimiter += "="
            attempt += 1

        if attempt >= 10:
            raise ValueError("Cannot find suitable delimiter for raw string literal")

        return f'R"{delimiter}({py_content}){delimiter}"'
    else:
        # Standard raw string literal
        return f'R"({py_content})"'


def fnv1a32(data: bytes) -> int:
    """
    Compute 32-bit FNV-1a hash of the given bytes.
    This must match the C++ implementation used at runtime.
    """
    fnv_offset_basis = 0x811C9DC5
    fnv_prime = 0x01000193

    h = fnv_offset_basis
    for b in data:
        h ^= b
        h = (h * fnv_prime) & 0xFFFFFFFF
    return h


def load_hash_history(history_path: Path) -> dict:
    """Load the hash history file, returning an empty dict if it doesn't exist."""
    if history_path.exists():
        with open(history_path, "r", encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_hash_history(history_path: Path, history: dict) -> None:
    """Save the hash history file, sorted by filename for stable diffs."""
    with open(history_path, "w", encoding="utf-8") as f:
        json.dump(dict(sorted(history.items())), f, indent=2)
        f.write("\n")


def update_history(history: dict, filename: str, current_hash: int) -> list:
    """
    Ensure current_hash is first in the history list for filename.
    Returns the updated list of hex strings (current first, then older versions).
    """
    current_hex = f"0x{current_hash:08X}"
    existing = history.get(filename, [])
    # Remove current hash from wherever it might be, then prepend it
    deduped = [h for h in existing if h.lower() != current_hex.lower()]
    updated = [current_hex] + deduped
    history[filename] = updated
    return updated


def hashes_to_c_array(hex_list: list) -> str:
    """Convert a list of hex strings to a C array initialiser body."""
    return ", ".join(hex_list)


def filename_to_define_name(filename: str) -> str:
    """
    Convert filename to C preprocessor define name.
    Example: adc_basics.py -> INCLUDE_ADC_BASICS
    """
    base = filename.replace('.py', '').upper()
    return f"INCLUDE_{base}"

def filename_to_var_name(filename: str) -> str:
    """
    Convert filename to C variable name.
    Example: adc_basics.py -> ADC_BASICS_PY
    """
    base = filename.replace('.py', '').upper()
    return f"{base}_PY"

def generate_header_from_directory(source_dir: Path, output_file: Path,
                                   history_path: Path = None,
                                   additional_includes=None):
    """
    Generate complete micropythonExamples.h from a directory of Python files.
    
    Args:
        source_dir: Directory containing .py example files
        output_file: Output .h file path
        additional_includes: List of additional files to always include (like jumperless.py)
    """
    
    if additional_includes is None:
        additional_includes = []

    history = load_hash_history(history_path) if history_path else {}
    
    # Files to exclude from examples (test files, incomplete demos, utilities)
    exclude_files = {
        '__init__.py',
        'fuck.py',           # Old version of interaction_demo
        'batt.py',           # Incomplete battery test
        'lipo_char.py',      # Incomplete battery test
        'listFiles.py',      # ViperIDE utility, not a demo
        'viperide_reinit.py', # ViperIDE connection script, handled separately
        'simple_fake_gpio_test.py', # Simple fake GPIO test, not a demo
        'fake_gpio.py', # Fake GPIO test, not a demo
        
    }
    
    # Scan for Python files
    py_files = sorted([f for f in source_dir.glob('*.py') 
                      if f.name not in exclude_files])
    
    # Start building the header
    lines = []

    # Header guard and intro
    lines.extend([
        "#ifndef MICROPYTHON_EXAMPLES_H",
        "#define MICROPYTHON_EXAMPLES_H",
        "",
        "#include <stdint.h>",
        "",
        "//==============================================================================",
        "// MicroPython Examples - Compile-time Configuration",
        "//==============================================================================",
        "// This file is AUTO-GENERATED by scripts/generate_micropython_examples.py",
        "// Do not edit manually! Edit the .py files in scripts/ex/ instead.",
        "//==============================================================================",
        "",
        "// Define which examples to include at compile time",
        "// Comment out any example you don't want to include",
    ])
    
    # Generate #define directives for each file
    for py_file in py_files:
        define_name = filename_to_define_name(py_file.name)
        lines.append(f"#define {define_name}")
    
    # Always include the jumperless module wrapper
    lines.extend([
        "",
        "// Always include the jumperless Python module wrapper",
        "#define INCLUDE_JUMPERLESS_MODULE",
        "",
        "// Optional: Include type stub and init files for ViperIDE support",
        "#define INCLUDE_JUMPERLESS_STUB // jumperless.pyi for autocomplete",
        "// #define INCLUDE_VIPER_INIT      // viper_init.py for ViperIDE",
        "",
    ])
    
    # Add convenience disable groups
    lines.extend([
        "// Convenience defines to disable groups of examples",
        "// Uncomment any of these to disable entire categories",
        "// #define DISABLE_ALL_EXAMPLES",
        "// #define DISABLE_HARDWARE_EXAMPLES    // Disables DAC, ADC, GPIO, Node examples",
        "// #define DISABLE_DEMO_EXAMPLES        // Disables LED, Voltage, Stylophone demos",
        "// #define DISABLE_UTILITY_EXAMPLES     // Disables README and test runner",
        "// #define DISABLE_MACHINE_EXAMPLES     // Disables UART examples",
        "",
        "#ifdef DISABLE_ALL_EXAMPLES",
    ])
    
    for py_file in py_files:
        define_name = filename_to_define_name(py_file.name)
        lines.append(f"#undef {define_name}")
    
    lines.extend([
        "#endif",
        "",
    ])
    
    # Add category-specific disables
    categories = {
        "HARDWARE": ["dac_basics", "adc_basics", "gpio_basics", "node_connections"],
        "DEMO": ["led_brightness_control", "voltage_monitor", "stylophone"],
        "UTILITY": ["readme", "test_runner"],
        "MACHINE": ["uart_basics", "uart_loopback"],
    }
    
    for category, examples in categories.items():
        lines.append(f"#ifdef DISABLE_{category}_EXAMPLES")
        for example in examples:
            define_name = f"INCLUDE_{example.upper()}"
            lines.append(f"#undef {define_name}")
        lines.extend(["#endif", ""])
    
    # Now generate the actual string constants
    lines.extend([
        "//==============================================================================",
        "// MicroPython Examples - Individual Demo Files",
        "//==============================================================================",
        "",
    ])
    
    # Process each Python file
    for py_file in py_files:
        print(f"Processing {py_file.name}...")

        define_name = filename_to_define_name(py_file.name)
        var_name = filename_to_var_name(py_file.name)

        # Read the Python file
        with open(py_file, "r", encoding="utf-8") as f:
            py_content = f.read()

        # Generate the C string
        c_string = py_to_c_string(py_content)
        # Compute default content hash (must match C++ FNV-1a implementation)
        content_hash = fnv1a32(py_content.encode("utf-8"))

        # Update hash history for this file (current hash is always first)
        all_hashes = update_history(history, py_file.name, content_hash)
        hash_count = len(all_hashes)
        hash_array = hashes_to_c_array(all_hashes)

        # Add to header
        lines.extend([
            f"#ifdef {define_name}",
            f"const char* {var_name} = {c_string};",
            f"const uint32_t {var_name}_HASHES[{hash_count}] = {{ {hash_array} }};",
            f"const int {var_name}_HASH_COUNT = {hash_count};",
            "#endif",
            "",
        ])
    
    # Add the jumperless module wrapper and stub
    # These are kept separately since they're not "examples"
    lines.extend([
        "//==============================================================================",
        "// Jumperless Python Module Wrapper",
        "//==============================================================================",
        "// This section should be maintained manually or from a separate source",
        "",
    ])
    
    # Add the jumperless.py module wrapper
    # Try jumperless_module.py first, then jumperless.py
    jumperless_module_path = source_dir.parent / 'jumperless_module.py'
    if not jumperless_module_path.exists():
        jumperless_module_path = source_dir.parent / 'jumperless.py'
    
    if jumperless_module_path.exists():
        print(f"Found jumperless module at {jumperless_module_path}")
        with open(jumperless_module_path, "r", encoding="utf-8") as f:
            module_content = f.read()

        module_hash = fnv1a32(module_content.encode("utf-8"))
        module_hashes = update_history(history, "jumperless.py", module_hash)
        module_hash_array = hashes_to_c_array(module_hashes)

        lines.extend([
            "#ifdef INCLUDE_JUMPERLESS_MODULE",
            f"const char* JUMPERLESS_MODULE_PY = {py_to_c_string(module_content)};",
            f"const uint32_t JUMPERLESS_MODULE_PY_HASHES[{len(module_hashes)}] = {{ {module_hash_array} }};",
            f"const int JUMPERLESS_MODULE_PY_HASH_COUNT = {len(module_hashes)};",
            "#endif",
            "",
        ])
    else:
        print(f"Warning: No jumperless module found at {source_dir.parent}")
        lines.extend([
            "#ifdef INCLUDE_JUMPERLESS_MODULE",
            "// WARNING: No jumperless_module.py found - module wrapper not included",
            "#endif",
            "",
        ])
    
    # Add stub file if it exists
    stub_path = source_dir.parent / 'jumperless.pyi'
    if stub_path.exists():
        print(f"Found type stub at {stub_path}")
        with open(stub_path, "r", encoding="utf-8") as f:
            stub_content = f.read()

        stub_hash = fnv1a32(stub_content.encode("utf-8"))
        stub_hashes = update_history(history, "jumperless.pyi", stub_hash)
        stub_hash_array = hashes_to_c_array(stub_hashes)

        lines.extend([
            "//==============================================================================",
            "// Jumperless Type Stub (Minimal - For ViperIDE Autocomplete)",
            "//==============================================================================",
            "",
            "#ifdef INCLUDE_JUMPERLESS_STUB",
            f"const char* JUMPERLESS_STUB_PYI = {py_to_c_string(stub_content)};",
            f"const uint32_t JUMPERLESS_STUB_PYI_HASHES[{len(stub_hashes)}] = {{ {stub_hash_array} }};",
            f"const int JUMPERLESS_STUB_PYI_HASH_COUNT = {len(stub_hashes)};",
            "#endif",
            "",
        ])
    
    # Add ViperIDE reinit script - this is executed on USBSer2 connection
    # to send filesystem information to ViperIDE/mpremote tools
    viperide_reinit_path = source_dir / 'viperide_reinit.py'
    viperide_reinit_exists = viperide_reinit_path.exists()
    
    if viperide_reinit_exists:
        print(f"Found ViperIDE reinit script at {viperide_reinit_path}")
        with open(viperide_reinit_path, "r", encoding="utf-8") as f:
            viperide_content = f.read()

        viperide_hash = fnv1a32(viperide_content.encode("utf-8"))
        viperide_hashes = update_history(history, "viperide_reinit.py", viperide_hash)
        viperide_hash_array = hashes_to_c_array(viperide_hashes)

        lines.extend([
            "//==============================================================================",
            "// ViperIDE Reinit Script (Sends filesystem info on USBSer2 connection)",
            "//==============================================================================",
            "",
            "#define INCLUDE_VIPERIDE_REINIT",
            f"const char* VIPERIDE_REINIT_PY = {py_to_c_string(viperide_content)};",
            f"const uint32_t VIPERIDE_REINIT_PY_HASHES[{len(viperide_hashes)}] = {{ {viperide_hash_array} }};",
            f"const int VIPERIDE_REINIT_PY_HASH_COUNT = {len(viperide_hashes)};",
            "",
        ])
    else:
        print(f"Warning: viperide_reinit.py not found at {viperide_reinit_path}")
        lines.extend([
            "//==============================================================================",
            "// ViperIDE Reinit Script",
            "//==============================================================================",
            "",
            "// #define INCLUDE_VIPERIDE_REINIT",
            "// WARNING: viperide_reinit.py not found",
            "",
        ])
    
    # Save updated hash history
    if history_path:
        save_hash_history(history_path, history)
        print(f"✓ Updated hash history: {history_path}")

    # Add helper comment section for FilesystemStuff.cpp array entries
    lines.extend([
        "//==============================================================================",
        "// FilesystemStuff.cpp Integration Guide",
        "//==============================================================================",
        "// Copy the following entries into the examples[] array in FilesystemStuff.cpp",
        "// inside the initializeMicroPythonExamples() function.",
        "// ExampleInfo struct: { path, content, name, knownHashes, hashCount }",
        "//",
        "// The array should include these entries (already sorted alphabetically):",
        "//",
    ])
    
    # Add the lib files first
    if jumperless_module_path.exists():
        lines.append("// #ifdef INCLUDE_JUMPERLESS_MODULE")
        lines.append("//     { \"/python_scripts/lib/jumperless.py\", JUMPERLESS_MODULE_PY, \"jumperless.py\", JUMPERLESS_MODULE_PY_HASHES, JUMPERLESS_MODULE_PY_HASH_COUNT },")
        lines.append("// #endif")
    
    if stub_path.exists():
        lines.append("// #ifdef INCLUDE_JUMPERLESS_STUB")
        lines.append("//     { \"/python_scripts/lib/jumperless.pyi\", JUMPERLESS_STUB_PYI, \"jumperless.pyi\", JUMPERLESS_STUB_PYI_HASHES, JUMPERLESS_STUB_PYI_HASH_COUNT },")
        lines.append("// #endif")
    
    # Add all the example files
    for py_file in py_files:
        define_name = filename_to_define_name(py_file.name)
        var_name = filename_to_var_name(py_file.name)
        filename = py_file.name
        
        lines.append(f"// #ifdef {define_name}")
        lines.append(f"//     {{ \"/python_scripts/examples/{filename}\", {var_name}, \"{filename}\", {var_name}_HASHES, {var_name}_HASH_COUNT }},")
        lines.append("// #endif")
    
    # Add viperide_reinit if it exists
    if viperide_reinit_exists:
        lines.append("// #ifdef INCLUDE_VIPERIDE_REINIT")
        lines.append("//     { \"/python_scripts/examples/viperide_reinit.py\", VIPERIDE_REINIT_PY, \"viperide_reinit.py\", VIPERIDE_REINIT_PY_HASHES, VIPERIDE_REINIT_PY_HASH_COUNT },")
        lines.append("// #endif")
    
    lines.append("//")
    
    # Close the header guard
    lines.extend([
        "",
        "#endif // MICROPYTHON_EXAMPLES_H",
    ])
    
    # Write the output file
    output_content = '\n'.join(lines)
    
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write(output_content)
    
    print(f"\n✓ Generated {output_file}")
    print(f"✓ Processed {len(py_files)} example files")
    return len(py_files)

def main():
    # Determine paths
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    
    # Source directory containing Python examples
    examples_dir = project_root / 'scripts' / 'ex'
    
    # Output header file
    output_header = project_root / 'src' / 'snakes' / 'micropythonExamples.h'

    # Hash history file - accumulates all known firmware hashes per example
    history_file = script_dir / 'hash_history.json'
    
    print("=" * 70)
    print("Generating micropythonExamples.h")
    print("=" * 70)
    print(f"Source directory: {examples_dir}")
    print(f"Output file: {output_header}")
    print()
    
    if not examples_dir.exists():
        print(f"ERROR: Source directory not found: {examples_dir}")
        sys.exit(1)
    
    try:
        num_files = generate_header_from_directory(examples_dir, output_header,
                                                    history_path=history_file)
        print()
        print("=" * 70)
        print("SUCCESS!")
        print("=" * 70)
        print(f"Updated {output_header} with {num_files} examples")
        print()
        print("To use:")
        print("1. Edit Python files in scripts/ex/")
        print("2. Run: python3 scripts/generate_micropython_examples.py")
        print("3. Rebuild firmware")
        
    except Exception as e:
        print(f"\nERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    main()

