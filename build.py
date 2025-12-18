"""
ESP32 DataLogger - Build Helper Script
Run this script to build and optionally upload the firmware.

Usage:
    python build.py          # Build only
    python build.py upload   # Build and upload
    python build.py monitor  # Build, upload and monitor
    python build.py clean    # Clean build files
"""

import subprocess
import sys
import os

PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))
ENV = "esp32dev"  # Change to "esp32-s3" for ESP32-S3

def run_pio(args):
    """Run PlatformIO command"""
    cmd = ["pio"] + args
    print(f"\n>>> Running: {' '.join(cmd)}")
    print("-" * 60)
    result = subprocess.run(cmd, cwd=PROJECT_DIR)
    return result.returncode == 0

def build():
    """Build the project"""
    print("\n=== Building DataLogger ===")
    return run_pio(["run", "-e", ENV])

def upload():
    """Build and upload to ESP32"""
    print("\n=== Uploading to ESP32 ===")
    return run_pio(["run", "-e", ENV, "-t", "upload"])

def monitor():
    """Open serial monitor"""
    print("\n=== Opening Serial Monitor ===")
    print("Press Ctrl+C to exit")
    return run_pio(["device", "monitor", "-e", ENV])

def clean():
    """Clean build files"""
    print("\n=== Cleaning build files ===")
    return run_pio(["run", "-e", ENV, "-t", "clean"])

def main():
    action = sys.argv[1] if len(sys.argv) > 1 else "build"
    
    if action == "build":
        success = build()
    elif action == "upload":
        success = upload()
    elif action == "monitor":
        if upload():
            success = monitor()
        else:
            success = False
    elif action == "clean":
        success = clean()
    else:
        print(f"Unknown action: {action}")
        print("Usage: python build.py [build|upload|monitor|clean]")
        return 1
    
    if success:
        print("\n✓ Done!")
        return 0
    else:
        print("\n✗ Failed!")
        return 1

if __name__ == "__main__":
    sys.exit(main())
