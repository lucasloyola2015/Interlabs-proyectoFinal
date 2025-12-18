#!/usr/bin/env python3
"""
DataLogger Stress Test - Prueba de límites del sistema
Encuentra los parámetros que hacen fallar el sistema:
- Baudrate máximo
- Tamaño máximo de burst
- Velocidad máxima sostenida
"""

import serial
import time
import os

# Configuración
DEBUG_PORT = "COM4"
SNIFFER_PORT = "COM3"
TIMEOUT = 10

# Archivo de log
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.path.join(SCRIPT_DIR, "stress_test.log")
log_file = None

def log_write(tag, data):
    """Escribe al archivo de log"""
    if log_file:
        timestamp = time.strftime("%H:%M:%S")
        log_file.write(f"[{timestamp}] {tag}: {data}\n")
        log_file.flush()
    print(f"[{tag}] {data}")

def wait_for_ready(ser, timeout=10):
    """Espera el mensaje READY del ESP32"""
    start = time.time()
    while (time.time() - start) < timeout:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if "READY" in line:
                return True
    return False

def drain_serial(ser, timeout=0.5):
    """Vacía el buffer serial"""
    start = time.time()
    lines = []
    while (time.time() - start) < timeout:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                lines.append(line)
            start = time.time()  # Reset timeout si hay datos
        time.sleep(0.01)
    return lines

def send_command(ser, cmd, wait_ms=300):
    """Envía un comando y lee la respuesta"""
    ser.write(f"{cmd}\n".encode())
    time.sleep(wait_ms / 1000)
    return drain_serial(ser, 0.3)

def get_stats(debug):
    """Obtiene estadísticas del ESP32"""
    response = send_command(debug, "stats", 200)
    stats = {"total": 0, "bursts": 0, "overflows": 0, "dropped": 0}
    for line in response:
        if "total=" in line:
            try:
                stats["total"] = int(line.split("total=")[1].split(",")[0])
                stats["bursts"] = int(line.split("bursts=")[1].split(",")[0])
                stats["overflows"] = int(line.split("overflows=")[1].split(")")[0].split(",")[0])
            except:
                pass
        if "dropped=" in line:
            try:
                stats["dropped"] = int(line.split("dropped=")[1].split(",")[0].split(")")[0])
            except:
                pass
    return stats

def generate_data(size):
    """Genera datos de prueba"""
    return bytes([i % 256 for i in range(size)])

def set_esp_baudrate(debug, baudrate):
    """Cambia el baudrate del ESP32 y espera confirmación"""
    debug.write(f"baud {baudrate}\n".encode())
    time.sleep(0.3)
    response = drain_serial(debug, 0.5)
    for line in response:
        if "BAUD_OK" in line:
            return True
    return False

def test_baudrate_limit(sniffer_port, debug):
    """
    TEST 1: Encontrar el baudrate máximo soportado
    Cambia dinámicamente el baudrate del ESP32 y del sniffer
    """
    log_write("TEST", "=" * 60)
    log_write("TEST", "TEST 1: LÍMITE DE BAUDRATE")
    log_write("TEST", "=" * 60)
    
    # Baudrates a probar (de menor a mayor)
    baudrates = [115200, 230400, 460800, 576000, 921600, 1000000, 1500000, 2000000]
    
    test_size = 5000  # Bytes a enviar por prueba
    results = []
    max_working_baud = 115200
    
    for baud in baudrates:
        log_write("TEST", f"\nProbando baudrate: {baud} bps...")
        
        # Cambiar baudrate del ESP32
        if not set_esp_baudrate(debug, baud):
            log_write("WARN", f"No se pudo configurar baudrate {baud} en ESP32")
            continue
        
        # Formatear y resetear stats
        send_command(debug, "format", 500)
        drain_serial(debug, 0.5)
        
        # Abrir sniffer con nuevo baudrate
        try:
            sniffer = serial.Serial(sniffer_port, baud, timeout=1)
        except Exception as e:
            log_write("ERROR", f"No se pudo abrir sniffer a {baud}: {e}")
            continue
        
        # Enviar datos
        data = generate_data(test_size)
        start_time = time.time()
        sniffer.write(data)
        sniffer.flush()
        
        # Esperar procesamiento
        wait_time = max(1.0, test_size / (baud / 10) + 0.5)
        time.sleep(wait_time)
        drain_serial(debug, 0.5)
        
        elapsed = time.time() - start_time
        stats = get_stats(debug)
        sniffer.close()
        
        success = stats['total'] == test_size and stats['overflows'] == 0
        actual_speed = stats['total'] / elapsed if elapsed > 0 else 0
        theoretical_speed = baud / 10
        
        results.append({
            'baud': baud,
            'sent': test_size,
            'received': stats['total'],
            'overflows': stats['overflows'],
            'success': success,
            'speed': actual_speed
        })
        
        status = "OK" if success else "FAIL"
        log_write("RESULT", f"  {baud} bps: enviados={test_size}, recibidos={stats['total']}, overflows={stats['overflows']}, velocidad={actual_speed:.0f} B/s [{status}]")
        
        if success:
            max_working_baud = baud
        else:
            log_write("LIMIT", f"Baudrate máximo encontrado: {max_working_baud} bps")
            break
    
    # Restaurar baudrate original
    log_write("TEST", "\nRestaurando baudrate a 115200...")
    set_esp_baudrate(debug, 115200)
    
    log_write("RESULT", f"\nBaudrate máximo funcional: {max_working_baud} bps")
    log_write("RESULT", f"Velocidad máxima teórica: {max_working_baud/10:.0f} bytes/s")
    
    return max_working_baud

def test_burst_size_limit(sniffer_port, debug):
    """
    TEST 2: Encontrar el tamaño máximo de burst sin pérdida
    """
    log_write("TEST", "=" * 60)
    log_write("TEST", "TEST 2: LÍMITE DE TAMAÑO DE BURST")
    log_write("TEST", "=" * 60)
    
    # Tamaños de burst a probar
    burst_sizes = [1000, 2000, 4000, 8000, 16000, 24000, 32000, 40000, 48000]
    
    results = []
    
    for burst_size in burst_sizes:
        log_write("TEST", f"\nProbando burst de {burst_size} bytes...")
        
        # Formatear
        send_command(debug, "format", 500)
        drain_serial(debug, 1)
        
        sniffer = serial.Serial(sniffer_port, 115200, timeout=1)
        
        # Enviar burst
        data = generate_data(burst_size)
        sniffer.write(data)
        sniffer.flush()
        
        # Esperar procesamiento (más tiempo para bursts grandes)
        wait_time = max(2, burst_size / 5000)
        time.sleep(wait_time)
        drain_serial(debug, 0.5)
        
        stats = get_stats(debug)
        sniffer.close()
        
        success = stats['total'] == burst_size and stats['overflows'] == 0
        results.append({
            'size': burst_size,
            'received': stats['total'],
            'overflows': stats['overflows'],
            'success': success
        })
        
        status = "OK" if success else "FAIL"
        log_write("RESULT", f"  {burst_size} bytes: recibidos={stats['total']}, overflows={stats['overflows']} [{status}]")
        
        if not success:
            log_write("LIMIT", f"Límite de burst encontrado: {burst_sizes[burst_sizes.index(burst_size)-1] if burst_sizes.index(burst_size) > 0 else 0} bytes")
            break
    
    # Resumen
    log_write("TEST", "\nResumen de burst sizes:")
    max_ok = 0
    for r in results:
        if r['success']:
            max_ok = r['size']
    log_write("RESULT", f"Tamaño máximo de burst sin pérdida: {max_ok} bytes")
    
    return max_ok

def test_continuous_rate(sniffer_port, debug):
    """
    TEST 3: Velocidad máxima sostenida sin pérdida
    Envía datos continuamente y mide cuándo empieza a perder
    """
    log_write("TEST", "=" * 60)
    log_write("TEST", "TEST 3: VELOCIDAD MÁXIMA SOSTENIDA")
    log_write("TEST", "=" * 60)
    
    # Formatear
    send_command(debug, "format", 500)
    drain_serial(debug, 1)
    
    sniffer = serial.Serial(sniffer_port, 115200, timeout=1)
    
    chunk_size = 1000
    total_sent = 0
    total_time = 10  # segundos de prueba
    
    log_write("TEST", f"Enviando datos continuamente por {total_time} segundos...")
    
    start_time = time.time()
    chunks_sent = 0
    
    while (time.time() - start_time) < total_time:
        data = generate_data(chunk_size)
        sniffer.write(data)
        total_sent += chunk_size
        chunks_sent += 1
        
        # Pequeña pausa para no saturar el buffer del OS
        time.sleep(0.05)
    
    sniffer.flush()
    elapsed = time.time() - start_time
    
    # Esperar que termine de procesar
    time.sleep(2)
    drain_serial(debug, 1)
    
    stats = get_stats(debug)
    sniffer.close()
    
    loss = total_sent - stats['total']
    loss_percent = (loss / total_sent) * 100 if total_sent > 0 else 0
    actual_rate = stats['total'] / elapsed
    
    log_write("RESULT", f"Tiempo de prueba: {elapsed:.1f}s")
    log_write("RESULT", f"Bytes enviados: {total_sent}")
    log_write("RESULT", f"Bytes recibidos: {stats['total']}")
    log_write("RESULT", f"Bytes perdidos: {loss} ({loss_percent:.2f}%)")
    log_write("RESULT", f"Velocidad sostenida: {actual_rate:.0f} bytes/s")
    log_write("RESULT", f"Overflows: {stats['overflows']}")
    log_write("RESULT", f"Bursts detectados: {stats['bursts']}")
    
    return loss == 0

def test_rapid_bursts(sniffer_port, debug):
    """
    TEST 4: Ráfagas rápidas - tiempo mínimo entre bursts
    """
    log_write("TEST", "=" * 60)
    log_write("TEST", "TEST 4: TIEMPO MÍNIMO ENTRE BURSTS")
    log_write("TEST", "=" * 60)
    
    burst_size = 1000
    num_bursts = 20
    
    # Diferentes intervalos entre bursts (ms)
    intervals = [200, 150, 120, 100, 80, 60, 50, 40, 30, 20]
    
    results = []
    
    for interval_ms in intervals:
        log_write("TEST", f"\nProbando intervalo de {interval_ms}ms entre bursts...")
        
        # Formatear
        send_command(debug, "format", 500)
        drain_serial(debug, 1)
        
        sniffer = serial.Serial(sniffer_port, 115200, timeout=1)
        
        total_sent = 0
        for i in range(num_bursts):
            data = generate_data(burst_size)
            sniffer.write(data)
            total_sent += burst_size
            time.sleep(interval_ms / 1000)
        
        sniffer.flush()
        
        # Esperar procesamiento
        time.sleep(2)
        drain_serial(debug, 0.5)
        
        stats = get_stats(debug)
        sniffer.close()
        
        success = stats['total'] == total_sent and stats['overflows'] == 0
        results.append({
            'interval': interval_ms,
            'sent': total_sent,
            'received': stats['total'],
            'overflows': stats['overflows'],
            'success': success
        })
        
        status = "OK" if success else "FAIL"
        log_write("RESULT", f"  {interval_ms}ms: enviados={total_sent}, recibidos={stats['total']}, overflows={stats['overflows']} [{status}]")
        
        if not success:
            log_write("LIMIT", f"Intervalo mínimo encontrado: {intervals[intervals.index(interval_ms)-1] if intervals.index(interval_ms) > 0 else intervals[0]}ms")
            break
    
    min_interval = intervals[0]
    for r in results:
        if r['success']:
            min_interval = r['interval']
    
    log_write("RESULT", f"\nIntervalo mínimo seguro entre bursts: {min_interval}ms")
    
    return min_interval

def test_flash_write_speed(sniffer_port, debug):
    """
    TEST 5: Velocidad de escritura a flash
    Usa baudrate alto para no limitar por UART
    """
    log_write("TEST", "=" * 60)
    log_write("TEST", "TEST 5: VELOCIDAD DE ESCRITURA A FLASH")
    log_write("TEST", "=" * 60)
    
    # Usar baudrate alto para esta prueba
    test_baud = 921600
    log_write("TEST", f"Configurando baudrate a {test_baud} para prueba de flash...")
    set_esp_baudrate(debug, test_baud)
    
    # Formatear
    send_command(debug, "format", 500)
    drain_serial(debug, 1)
    
    sniffer = serial.Serial(sniffer_port, test_baud, timeout=1)
    
    # Llenar la flash completamente - envío rápido sin pausas
    flash_size = 65536  # Tamaño real de partición
    chunk_size = 8000
    total_sent = 0
    
    log_write("TEST", f"Llenando flash ({flash_size} bytes) a máxima velocidad...")
    
    start_time = time.time()
    
    # Enviar todo de una vez
    while total_sent < flash_size:
        data = generate_data(chunk_size)
        sniffer.write(data)
        total_sent += chunk_size
    
    sniffer.flush()
    send_time = time.time() - start_time
    
    # Esperar que termine de escribir
    time.sleep(3)
    drain_serial(debug, 1)
    
    total_time = time.time() - start_time
    
    stats = get_stats(debug)
    sniffer.close()
    
    # Restaurar baudrate
    set_esp_baudrate(debug, 115200)
    
    uart_speed = total_sent / send_time if send_time > 0 else 0
    flash_speed = stats['total'] / total_time if total_time > 0 else 0
    
    log_write("RESULT", f"Bytes enviados: {total_sent}")
    log_write("RESULT", f"Bytes recibidos: {stats['total']}")
    log_write("RESULT", f"Tiempo envío UART: {send_time:.2f}s ({uart_speed/1024:.1f} KB/s)")
    log_write("RESULT", f"Tiempo total (con flush): {total_time:.1f}s")
    log_write("RESULT", f"Velocidad efectiva flash: {flash_speed:.0f} bytes/s ({flash_speed/1024:.1f} KB/s)")
    
    return flash_speed

def main():
    global log_file
    
    # Abrir log
    log_file = open(LOG_FILE, "w", encoding="utf-8")
    
    log_write("INFO", "=" * 60)
    log_write("INFO", "DATALOGGER STRESS TEST - PRUEBA DE LÍMITES")
    log_write("INFO", "=" * 60)
    log_write("INFO", f"Debug port: {DEBUG_PORT}")
    log_write("INFO", f"Sniffer port: {SNIFFER_PORT}")
    log_write("INFO", "")
    
    # Abrir debug y esperar ready
    log_write("INFO", "Conectando al ESP32...")
    try:
        debug = serial.Serial(DEBUG_PORT, 115200, timeout=1)
    except Exception as e:
        log_write("ERROR", f"No se pudo abrir DEBUG: {e}")
        return 1
    
    if not wait_for_ready(debug):
        log_write("ERROR", "ESP32 no responde")
        debug.close()
        return 1
    
    log_write("INFO", "ESP32 conectado!\n")
    
    try:
        # Ejecutar tests
        results = {}
        
        # Test 1: Baudrate
        results['baudrate_ok'] = test_baudrate_limit(SNIFFER_PORT, debug)
        
        # Test 2: Burst size
        results['max_burst'] = test_burst_size_limit(SNIFFER_PORT, debug)
        
        # Test 3: Velocidad sostenida
        results['sustained_ok'] = test_continuous_rate(SNIFFER_PORT, debug)
        
        # Test 4: Intervalo mínimo
        results['min_interval'] = test_rapid_bursts(SNIFFER_PORT, debug)
        
        # Test 5: Velocidad flash
        results['flash_speed'] = test_flash_write_speed(SNIFFER_PORT, debug)
        
        # Resumen final
        log_write("TEST", "\n" + "=" * 60)
        log_write("TEST", "RESUMEN DE LÍMITES DEL SISTEMA")
        log_write("TEST", "=" * 60)
        log_write("RESULT", f"Baudrate 115200: {'OK' if results['baudrate_ok'] else 'FAIL'}")
        log_write("RESULT", f"Tamaño máximo de burst: {results['max_burst']} bytes")
        log_write("RESULT", f"Velocidad sostenida: {'OK' if results['sustained_ok'] else 'Con pérdidas'}")
        log_write("RESULT", f"Intervalo mínimo entre bursts: {results['min_interval']}ms")
        log_write("RESULT", f"Velocidad escritura flash: {results['flash_speed']:.0f} bytes/s ({results['flash_speed']/1024:.1f} KB/s)")
        
        log_write("TEST", "\n" + "=" * 60)
        log_write("TEST", "STRESS TEST COMPLETADO")
        log_write("TEST", "=" * 60)
        
        return 0
        
    finally:
        debug.close()
        if log_file:
            log_file.close()

if __name__ == "__main__":
    exit(main())

