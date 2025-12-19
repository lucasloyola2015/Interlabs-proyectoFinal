import subprocess
import sys

# Run PlatformIO build and capture all output
result = subprocess.run(
    ['pio', 'run', '-e', 'esp32dev'],
    cwd=r'c:\Users\loyol\Documents\Interlabs\ProyectoFinal\DataLogger',
    capture_output=True,
    text=True
)

# Print stdout
print("=== STDOUT ===")
print(result.stdout)

# Print stderr
print("\n=== STDERR ===")
print(result.stderr)

# Print last 100 lines of combined output
print("\n=== LAST 100 LINES ===")
all_output = result.stdout + "\n" + result.stderr
lines = all_output.split('\n')
for line in lines[-100:]:
    print(line)

sys.exit(result.returncode)
