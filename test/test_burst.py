"""
ESP32 DataLogger - Test Script
Sends test data via serial port to verify data capture.

Usage:
    python test_burst.py COM3 1000    # Send 1000 bytes via COM3
    python test_burst.py COM3 550000  # Send 550KB burst
"""

import serial
import sys
import time
import random

def send_burst(port: str, size: int, baudrate: int = 1000000):
    """Send a burst of test data"""
    print(f"Opening {port} @ {baudrate} bps...")
    
    try:
        ser = serial.Serial(port, baudrate, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening port: {e}")
        return False
    
    # Generate test pattern (incrementing bytes for easy verification)
    print(f"Generating {size} bytes of test data...")
    data = bytes([i % 256 for i in range(size)])
    
    print(f"Sending {size} bytes...")
    start = time.time()
    
    # Send in chunks for progress
    chunk_size = 4096
    sent = 0
    while sent < size:
        chunk = data[sent:sent + chunk_size]
        ser.write(chunk)
        sent += len(chunk)
        progress = (sent * 100) // size
        print(f"\rProgress: {progress}% ({sent}/{size} bytes)", end="", flush=True)
    
    elapsed = time.time() - start
    throughput = size / elapsed / 1000
    
    print(f"\n\nDone!")
    print(f"Time: {elapsed:.2f}s")
    print(f"Throughput: {throughput:.1f} KB/s")
    
    ser.close()
    return True

def main():
    if len(sys.argv) < 3:
        print("Usage: python test_burst.py <PORT> <SIZE>")
        print("Example: python test_burst.py COM3 1000")
        return 1
    
    port = sys.argv[1]
    size = int(sys.argv[2])
    
    if send_burst(port, size):
        return 0
    return 1

if __name__ == "__main__":
    sys.exit(main())
