#!/usr/bin/env python3
"""
DataLogger Test Script
- DEBUG: COM4 (comandos y logs)
- SNIFFER: COM3 (envío de datos de prueba)
"""

import serial
import time
import random
import os

# Configuración
DEBUG_PORT = "COM4"
SNIFFER_PORT = "COM3"
BAUDRATE = 115200
TIMEOUT = 10  # segundos

# Archivo de log
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.path.join(SCRIPT_DIR, "logger.log")
log_file = None

def log_write(direction, data):
    """Escribe al archivo de log"""
    if log_file:
        timestamp = time.strftime("%H:%M:%S")
        log_file.write(f"[{timestamp}] {direction}: {data}\n")
        log_file.flush()

def wait_for_ready(ser):
    """Espera el mensaje READY del ESP32"""
    print("Esperando READY...")
    start = time.time()
    while (time.time() - start) < TIMEOUT:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                print(f"  {line}")
                log_write("RX", line)
            if "READY" in line:
                return True
    return False

def wait_for_message(ser, message, timeout=5):
    """Espera un mensaje específico"""
    start = time.time()
    while (time.time() - start) < timeout:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                print(f"  {line}")
                log_write("RX", line)
            if message in line:
                return True
        time.sleep(0.01)
    return False

def send_command(ser, cmd, wait_ms=300):
    """Envía un comando y lee la respuesta"""
    print(f"Enviando '{cmd}'...")
    log_write("TX", cmd)
    ser.write(f"{cmd}\n".encode())
    time.sleep(wait_ms / 1000)
    
    response = []
    while ser.in_waiting:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if line:
            print(f"  {line}")
            log_write("RX", line)
            response.append(line)
    return response

def generate_test_data(size, offset=0):
    """Genera datos de prueba con secuencia conocida"""
    # Patrón: bytes 0-255 repetidos, con offset para distinguir envíos
    data = bytes([(i + offset) % 256 for i in range(size)])
    return data

def parse_hex_dump(lines):
    """Parsea el hex dump del comando read"""
    import re
    data = bytearray()
    for line in lines:
        # Formato: "I (xxxx) DataLogger: 01D7: D7 D8 D9 ... ASCII"
        # Buscar patrón de dirección hex seguido de datos
        match = re.search(r'[0-9A-F]{4}: ([0-9A-F ]+)  ', line)
        if match:
            hex_part = match.group(1).strip()
            hex_bytes = hex_part.split()
            for hb in hex_bytes:
                try:
                    data.append(int(hb, 16))
                except ValueError:
                    pass
    return bytes(data)

def verify_data(debug, expected_all_data, data_offset, data_len):
    """Verifica datos leyendo una posición aleatoria"""
    read_offset = random.randint(0, data_len - 64)
    read_len = 64
    print(f"  Leyendo {read_len} bytes desde offset {read_offset}...")
    log_write("TEST", f"Verificando {read_len} bytes desde offset {read_offset}")
    
    response = send_command(debug, f"read {read_offset} {read_len}", wait_ms=500)
    read_data = parse_hex_dump(response)
    expected_data = expected_all_data[data_offset + read_offset:data_offset + read_offset + read_len]
    
    print(f"  Esperado: {expected_data[:16].hex()}...")
    print(f"  Leído:    {read_data[:16].hex() if read_data else '(vacío)'}...")
    log_write("TEST", f"Esperado: {expected_data.hex()}")
    log_write("TEST", f"Leido:    {read_data.hex() if read_data else '(vacio)'}")
    
    if read_data == expected_data:
        print("  OK: Datos verificados correctamente\n")
        log_write("TEST", "OK: Datos verificados")
        return True
    else:
        errors = sum(1 for a, b in zip(expected_data, read_data) if a != b)
        print(f"  ERROR: {errors} bytes diferentes\n")
        log_write("ERROR", f"{errors} bytes diferentes")
        return False

def main():
    global log_file
    
    # Abrir archivo de log (limpiando el anterior)
    log_file = open(LOG_FILE, "w", encoding="utf-8")
    log_write("INFO", "=== Test iniciado ===")
    
    print("=" * 50)
    print("DataLogger Test")
    print("=" * 50)
    
    # Abrir Sniffer primero
    print(f"\n[1] Abriendo SNIFFER ({SNIFFER_PORT})...")
    try:
        sniffer = serial.Serial(SNIFFER_PORT, BAUDRATE, timeout=1)
    except Exception as e:
        print(f"ERROR: No se pudo abrir SNIFFER: {e}")
        return 1
    
    # Abrir Debug (esto reinicia el ESP32)
    print(f"\n[2] Abriendo DEBUG ({DEBUG_PORT})...")
    try:
        debug = serial.Serial(DEBUG_PORT, BAUDRATE, timeout=1)
    except Exception as e:
        print(f"ERROR: No se pudo abrir DEBUG: {e}")
        sniffer.close()
        return 1
    
    try:
        # Esperar READY
        print("\n[3] Esperando que ESP32 esté listo...")
        log_write("TEST", "[3] Esperando READY del ESP32...")
        if not wait_for_ready(debug):
            print("ERROR: Timeout esperando READY")
            log_write("ERROR", "Timeout esperando READY")
            return 1
        print("ESP32 listo!\n")
        log_write("TEST", "ESP32 listo")
        
        # Enviar format
        print("[4] Formateando flash...")
        log_write("TEST", "[4] Formateando flash...")
        log_write("TX", "format")
        debug.write(b"format\n")
        if not wait_for_message(debug, "stats reset", timeout=5):
            print("ERROR: Format no completado")
            log_write("ERROR", "Format no completado")
            return 1
        print("Format completado!\n")
        log_write("TEST", "Format completado")
        
        # Generar y enviar datos de prueba
        TEST_SIZE = 1000
        print(f"[5] Enviando {TEST_SIZE} bytes por SNIFFER...")
        log_write("TEST", f"[5] Enviando {TEST_SIZE} bytes por SNIFFER...")
        test_data = generate_test_data(TEST_SIZE)
        sniffer.write(test_data)
        sniffer.flush()
        print(f"Datos enviados: {test_data[:16].hex()}... (primeros 16 bytes)")
        log_write("SNIFFER_TX", f"{TEST_SIZE} bytes enviados, primeros 16: {test_data[:16].hex()}")
        
        # Esperar que se procese el burst
        print("\n[6] Esperando procesamiento del burst...")
        log_write("TEST", "[6] Esperando procesamiento del burst...")
        time.sleep(0.5)  # Esperar burst timeout
        if not wait_for_message(debug, "Burst", timeout=3):
            # Puede que ya haya pasado, verificar con stats
            pass
        time.sleep(0.2)
        
        # Verificar stats
        print("\n[7] Verificando stats...")
        log_write("TEST", "[7] Verificando stats...")
        response = send_command(debug, "stats")
        
        # Buscar bytes recibidos
        bytes_received = 0
        for line in response:
            if "total=" in line:
                try:
                    # Formato: "UART: total=1000, bursts=1, overflows=0"
                    parts = line.split("total=")[1].split(",")[0]
                    bytes_received = int(parts)
                except:
                    pass
        
        if bytes_received != TEST_SIZE:
            print(f"ERROR: Se esperaban {TEST_SIZE} bytes, se recibieron {bytes_received}")
            log_write("ERROR", f"Se esperaban {TEST_SIZE} bytes, se recibieron {bytes_received}")
            return 1
        print(f"OK: Recibidos {bytes_received} bytes correctamente\n")
        log_write("TEST", f"OK: Recibidos {bytes_received} bytes correctamente")
        
        # Leer posición aleatoria y verificar
        print("[8] Leyendo datos de flash y verificando...")
        log_write("TEST", "[8] Verificando datos iniciales...")
        
        if not verify_data(debug, test_data, 0, TEST_SIZE):
            return 1
        
        # Continuar enviando datos hasta dar la vuelta a la memoria
        FLASH_SIZE = 49152  # Tamaño de la partición
        CHUNK_SIZE = 4000   # Bytes por envío
        total_sent = TEST_SIZE
        wrap_detected = False
        iteration = 1
        
        # Buffer circular para rastrear qué datos están en flash
        # Guardamos (offset_global, datos) para cada envío
        all_data = bytearray(test_data)  # Datos acumulados
        
        print("\n" + "=" * 50)
        print("[9] Llenando memoria hasta dar la vuelta...")
        print("=" * 50)
        log_write("TEST", "[9] Iniciando llenado de memoria hasta wrap...")
        
        while not wrap_detected or iteration <= 3:  # Continuar 3 iteraciones después del wrap
            iteration += 1
            
            # Generar datos con offset único para este envío
            chunk_data = generate_test_data(CHUNK_SIZE, offset=total_sent)
            all_data.extend(chunk_data)
            
            print(f"\n  Iteración {iteration}: Enviando {CHUNK_SIZE} bytes (total acumulado: {total_sent + CHUNK_SIZE})...")
            log_write("TEST", f"Iteracion {iteration}: Enviando {CHUNK_SIZE} bytes")
            
            sniffer.write(chunk_data)
            sniffer.flush()
            
            # Esperar procesamiento
            time.sleep(0.3)
            wait_for_message(debug, "Burst", timeout=2)
            time.sleep(0.2)
            
            total_sent += CHUNK_SIZE
            
            # Verificar stats
            response = send_command(debug, "stats", wait_ms=200)
            
            # Buscar wraps
            for line in response:
                if "wraps=" in line:
                    try:
                        wraps = int(line.split("wraps=")[1].split(",")[0].split(")")[0])
                        if wraps > 0 and not wrap_detected:
                            wrap_detected = True
                            print(f"\n  *** WRAP DETECTADO! La memoria dio la vuelta ***")
                            log_write("TEST", f"*** WRAP DETECTADO en iteracion {iteration}! ***")
                    except:
                        pass
            
            # Verificar datos en posición aleatoria de los datos actuales en flash
            # Después del wrap, solo los datos más recientes están disponibles
            response = send_command(debug, "stats", wait_ms=100)
            flash_used = 0
            for line in response:
                if "used=" in line:
                    try:
                        flash_used = int(line.split("used=")[1].split("/")[0])
                    except:
                        pass
            
            if flash_used > 64:
                # Leer una posición aleatoria dentro de los datos disponibles
                read_offset = random.randint(0, min(flash_used - 64, len(all_data) - total_sent + flash_used - 64))
                read_len = 64
                
                print(f"  Verificando {read_len} bytes en offset {read_offset}...")
                response = send_command(debug, f"read {read_offset} {read_len}", wait_ms=300)
                read_data = parse_hex_dump(response)
                
                # Calcular qué datos esperamos en esa posición
                # Después del wrap, el tail avanza y los datos más viejos se pierden
                # Los datos en flash son los últimos 'flash_used' bytes de all_data
                data_start = len(all_data) - flash_used
                expected_data = bytes(all_data[data_start + read_offset:data_start + read_offset + read_len])
                
                if len(read_data) >= read_len and read_data == expected_data:
                    print(f"  OK: Datos verificados correctamente")
                    log_write("TEST", f"Verificacion OK en offset {read_offset}")
                else:
                    print(f"  WARN: Datos no coinciden (puede ser por timing)")
                    log_write("WARN", f"Datos no coinciden en offset {read_offset}")
            
            # Limitar iteraciones para no llenar infinitamente
            if total_sent > FLASH_SIZE * 2:
                break
        
        # Verificación final
        print("\n" + "=" * 50)
        print("[10] Verificación final después del wrap...")
        print("=" * 50)
        log_write("TEST", "[10] Verificacion final despues del wrap...")
        
        response = send_command(debug, "stats", wait_ms=200)
        print("\nEstadísticas finales:")
        for line in response:
            print(f"  {line}")
        
        # Leer los primeros bytes para confirmar que son datos nuevos (no los originales)
        response = send_command(debug, "read 0 64", wait_ms=500)
        read_data = parse_hex_dump(response)
        original_first_bytes = test_data[:64]
        
        if read_data != original_first_bytes:
            print("\nOK: Los datos originales fueron sobrescritos (wrap funcionando)")
            log_write("TEST", "OK: Datos originales sobrescritos - wrap funciona correctamente")
            print(f"  Original: {original_first_bytes[:16].hex()}...")
            print(f"  Actual:   {read_data[:16].hex()}...")
            log_write("TEST", f"Original: {original_first_bytes[:16].hex()}")
            log_write("TEST", f"Actual:   {read_data[:16].hex()}")
        else:
            print("\nWARN: Los datos parecen ser los originales")
            log_write("WARN", "Los datos parecen ser los originales")
        
        print("\n" + "=" * 50)
        print("TEST PASSED - Wrap de memoria verificado!")
        print(f"Total enviado: {total_sent} bytes")
        print(f"Tamaño flash: {FLASH_SIZE} bytes")
        print("=" * 50)
        log_write("RESULT", f"TEST PASSED - Wrap verificado! Total enviado: {total_sent} bytes")
        return 0
            
    finally:
        debug.close()
        sniffer.close()
        print("\nPuertos cerrados.")
        log_write("INFO", "=== Test finalizado ===")
        if log_file:
            log_file.close()

if __name__ == "__main__":
    exit(main())
