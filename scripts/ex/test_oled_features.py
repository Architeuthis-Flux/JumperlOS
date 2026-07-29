"""
Test script for new OLED MicroPython features
Run this on the Jumperless device to test all new OLED functions
"""
import time

def test_text_size_control():
    """Test text size control functions"""
    print("\n=== Testing Text Size Control ===")
    
    # Test setting text size
    print("Setting text size to 0 (small)...")
    oled_set_text_size(0)
    assert oled_get_text_size() == 0, "Text size should be 0"
    oled_print("Small text mode")
    time.sleep(2)
    
    print("Setting text size to 1 (normal)...")
    oled_set_text_size(1)
    assert oled_get_text_size() == 1, "Text size should be 1"
    oled_print("Normal text")
    time.sleep(2)
    
    print("Setting text size to 2 (large)...")
    oled_set_text_size(2)
    assert oled_get_text_size() == 2, "Text size should be 2"
    oled_print("Large text")
    time.sleep(2)
    
    print("✓ Text size control works!")

def test_print_redirection():
    """Test print() redirection to OLED"""
    print("\n=== Testing Print Redirection ===")
    
    # Enable print copying
    print("Enabling print copy to OLED...")
    oled_copy_print(True)
    
    # Test printing
    print("Line 1 on OLED")
    time.sleep(1)
    print("Line 2 on OLED")
    time.sleep(1)
    print("Line 3 on OLED")
    time.sleep(2)
    
    # Disable print copying
    print("Disabling print copy...")
    oled_copy_print(False)
    print("This should NOT appear on OLED")
    time.sleep(2)
    
    print("✓ Print redirection works!")

def test_font_system():
    """Test font enumeration and selection"""
    print("\n=== Testing Font System ===")
    
    # Get available fonts
    fonts = oled_get_fonts()
    print(f"Available fonts: {fonts}")
    assert len(fonts) > 0, "Should have fonts available"
    
    # Test setting fonts
    for font in fonts[:3]:  # Test first 3 fonts
        print(f"Setting font to: {font}")
        result = oled_set_font(font)
        assert result, f"Should be able to set font {font}"
        
        current = oled_get_current_font()
        print(f"Current font: {current}")
        
        oled_print(f"Font: {font}", 2)
        time.sleep(1.5)
    
    print("✓ Font system works!")

def test_bitmap_functions():
    """Test bitmap loading and display"""
    print("\n=== Testing Bitmap Functions ===")
    
    # Test loading a bitmap file (if it exists)
    test_file = "/jogo32h.bin"
    
    print(f"Attempting to load bitmap: {test_file}")
    result = oled_load_bitmap(test_file)
    
    if result:
        print("✓ Bitmap loaded successfully")
        
        # Display the loaded bitmap
        print("Displaying bitmap at (0, 0)...")
        oled_display_bitmap(0, 0, 0, 0)  # Use loaded bitmap
        time.sleep(2)
        
        # Try convenience function
        print("Using convenience function...")
        oled_show_bitmap_file(test_file, 0, 0)
        time.sleep(2)
    else:
        print(f"⚠ Bitmap file not found: {test_file}")
        print("  (This is OK if the file doesn't exist)")
    
    print("✓ Bitmap functions work!")

def test_framebuffer_access():
    """Test framebuffer manipulation"""
    print("\n=== Testing Framebuffer Access ===")
    
    # Get framebuffer size
    width, height, buffer_size = oled_get_framebuffer_size()
    print(f"Framebuffer: {width}x{height}, {buffer_size} bytes")
    
    # Get current framebuffer
    fb = oled_get_framebuffer()
    print(f"Got framebuffer: {len(fb)} bytes")
    assert len(fb) == buffer_size, "Framebuffer size should match"
    
    # Test pixel manipulation
    print("Drawing test pattern...")
    oled_clear()
    
    # Draw a border
    for x in range(width):
        oled_set_pixel(x, 0, 1)  # Top
        oled_set_pixel(x, height-1, 1)  # Bottom
    
    for y in range(height):
        oled_set_pixel(0, y, 1)  # Left
        oled_set_pixel(width-1, y, 1)  # Right
    
    # Draw diagonal lines
    for i in range(min(width, height)):
        oled_set_pixel(i, i, 1)
        oled_set_pixel(width-1-i, i, 1)
    
    oled_show()
    time.sleep(2)
    
    # Test reading pixels
    print("Testing pixel reading...")
    assert oled_get_pixel(0, 0) == 1, "Corner pixel should be set"
    assert oled_get_pixel(width//2, height//2) == 0, "Center should be clear"
    
    # Test framebuffer set/get round-trip
    print("Testing framebuffer round-trip...")
    fb_copy = bytearray(fb)
    result = oled_set_framebuffer(fb_copy)
    assert result, "Should be able to set framebuffer"
    
    print("✓ Framebuffer access works!")

def test_small_text_scrolling():
    """Test small text scrolling mode"""
    print("\n=== Testing Small Text Scrolling ===")
    
    oled_set_text_size(0)  # Small scrolling mode
    oled_clear()
    
    # Print multiple lines to test scrolling
    for i in range(10):
        oled_print(f"Scroll line {i+1}")
        time.sleep(0.3)
    
    time.sleep(2)
    print("✓ Small text scrolling works!")

def run_all_tests():
    """Run all OLED feature tests"""
    print("=" * 50)
    print("OLED MicroPython Features Test Suite")
    print("=" * 50)
    
    try:
        # Ensure OLED is connected
        oled_connect()
        time.sleep(0.5)
        
        # Run tests
        test_text_size_control()
        test_print_redirection()
        test_font_system()
        test_bitmap_functions()
        test_framebuffer_access()
        test_small_text_scrolling()
        
        # Final success message
        oled_clear()
        oled_set_text_size(2)
        oled_print("All tests passed!")
        
        print("\n" + "=" * 50)
        print("✓ ALL TESTS PASSED!")
        print("=" * 50)
        
    except Exception as e:
        print(f"\n✗ TEST FAILED: {e}")
        oled_clear()
        oled_set_text_size(1)
        oled_print("Test failed!")
        raise

# Run tests if executed directly
if __name__ == "__main__":
    run_all_tests()

