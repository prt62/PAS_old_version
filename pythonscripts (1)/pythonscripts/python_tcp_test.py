import socket
import os
import time
import threading
import re
import numpy as np

try:
    import audioop
    _HAS_AUDIOOP = True
except ImportError:
    _HAS_AUDIOOP = False

# ==========================================
# GLOBAL CONFIGURATION
# ==========================================

target_ip = '192.168.0.250'
PORT = 5500
FILE_NAME = 'song.mp3'

COMMAND_TIMEOUT = 4
UPLOAD_TIMEOUT = 60
RETRY_DELAY = 1.5
MAX_RETRIES = 5

LIVE_MIC_BOOST = 2.5

# ==========================================
# NUMPY RESAMPLER FALLBACK (no audioop)
# ==========================================

def numpy_ratecv(data, width, nchannels, inrate, outrate, state=None):
    """Linear-interpolation resampler — fallback when audioop unavailable."""
    samples = np.frombuffer(data, dtype=np.int16).astype(np.float32)
    in_len = len(samples)
    out_len = int(in_len * outrate / inrate)
    if out_len == 0:
        return b'', None
    indices = np.linspace(0, in_len - 1, out_len)
    left = np.floor(indices).astype(np.int32)
    right = np.minimum(left + 1, in_len - 1)
    frac = (indices - left).astype(np.float32)
    resampled = samples[left] + frac * (samples[right] - samples[left])
    return resampled.astype(np.int16).tobytes(), None

def ratecv(data, width, nchannels, inrate, outrate, state=None):
    if _HAS_AUDIOOP:
        return audioop.ratecv(data, width, nchannels, inrate, outrate, state)
    return numpy_ratecv(data, width, nchannels, inrate, outrate, state)

# ==========================================
# TCP SOCKET FUNCTIONS
# ==========================================

def create_socket(timeout):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
    sock.settimeout(timeout)
    return sock

def safe_close(sock):
    if sock is None:
        return
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    finally:
        try:
            sock.close()
        except OSError:
            pass

def receive_response(sock, max_bytes=2048, timeout=6):
    response = b""
    STOP_KEYWORDS = (b"ACK", b"UPLOAD_OK", b"ERR", b"ACK_PLAY",
                     b"ACK_STOP", b"ACK_LIVE", b"BUSY", b"READY")
    try:
        sock.settimeout(timeout)
        while len(response) < max_bytes:
            try:
                part = sock.recv(1024)
            except socket.timeout:
                break
            if not part:
                break
            response += part
            for keyword in STOP_KEYWORDS:
                if keyword in response:
                    return response.decode(errors="ignore").strip()
    except OSError as e:
        print(f"[!] receive_response error: {e}")
    return response.decode(errors="ignore").strip()

def is_valid_ip(ip_str):
    pattern = r'^(\d{1,3}\.){3}\d{1,3}$'
    if not re.match(pattern, ip_str):
        return False
    try:
        return all(0 <= int(p) <= 255 for p in ip_str.split('.'))
    except ValueError:
        return False

def send_command(cmd):
    sock = create_socket(COMMAND_TIMEOUT)
    try:
        sock.connect((target_ip, PORT))
        sock.sendall(cmd.strip().encode())
        response = receive_response(sock)
        if "ACK" in response:
            return True
        elif "BUSY" in response:
            print("[-] ESP32 is busy")
            return False
        else:
            print(f"[-] Unexpected response: '{response}'")
            return False
    except OSError as e:
        print(f"[-] Network error: {e}")
        return False
    finally:
        safe_close(sock)

def reliable_send(cmd, retries=MAX_RETRIES):
    for attempt in range(1, retries + 1):
        if send_command(cmd):
            return True
        print(f"[!] Retry {attempt}/{retries} for '{cmd}'...")
        time.sleep(RETRY_DELAY)
    print(f"[-] Command '{cmd}' failed after {retries} retries.")
    return False

# ==========================================
# FILE UPLOAD — ORIGINAL LOGIC (TCP)
# ==========================================

def upload_file():
    if not os.path.exists(FILE_NAME):
        return
    file_size = os.path.getsize(FILE_NAME)

    sock = create_socket(UPLOAD_TIMEOUT * 2)
    try:
        sock.connect((target_ip, PORT))
        sock.sendall("UPLD".encode())
        time.sleep(0.8)

        print(f"\n[*] Uploading {FILE_NAME} ({file_size / 1024:.1f} KB)...")
        bytes_sent = 0
        with open(FILE_NAME, 'rb') as f:
            while True:
                chunk = f.read(2048)
                if not chunk:
                    break
                sock.sendall(chunk)
                bytes_sent += len(chunk)
                print(f"\r[*] {(bytes_sent / file_size) * 100:.1f}%", end='', flush=True)

        sock.shutdown(socket.SHUT_WR)
        response = receive_response(sock, max_bytes=2048)
        
        # Sirf "UPLOAD_OK" ka wait mat karo, "READY" bhi accept karo
        if "UPLOAD_OK" in response or "READY" in response:
            print("\n[+] ✅ Upload/Handshake Successful!")
        else:
            print(f"\n[-] Upload Failed: {response}")
    except OSError as e:
        print(f"\n[-] Upload Error: {e}")
    finally:
        safe_close(sock)

# ==========================================
# SIMPLE COMMANDS
# ==========================================

def set_volume():
    raw = input("Enter Volume (0-100): ").strip()
    try:
        vol = max(0, min(100, int(raw)))
    except ValueError:
        print("[-] Invalid volume; please enter a number 0-100.")
        return
    if reliable_send(f"VOL:{vol}"):
        print(f"[+] Volume changed to {vol}%")

def change_ip():
    global target_ip
    new_ip = input("Enter new ESP32 IP: ").strip()
    if is_valid_ip(new_ip):
        target_ip = new_ip
        print(f"[+] Target IP updated to {target_ip}")
    else:
        print("[-] Invalid IP address.")

def play_audio():
    if reliable_send("PLAY"):
        print("[+] Play command confirmed!")

def stop_audio():
    if reliable_send("STOP"):
        print("[+] Stop command confirmed!")

# ==========================================
# LIVE STREAMING MODULE (UDP + TCP same port)
# ==========================================

def live_stream():
    try:
        import pyaudio
    except ImportError:
        print("\n[-] ERROR: PyAudio is not installed. Run: pip install pyaudio")
        return

    print("\n[*] Initializing UDP Stream...")

    tcp_sock = None
    udp_sock = None
    p = None
    mic_stream = None

    try:
        # --- TCP handshake on PORT ---
        tcp_sock = create_socket(COMMAND_TIMEOUT)
        tcp_sock.connect((target_ip, PORT))
        tcp_sock.sendall(b"LIVE")

        response = receive_response(tcp_sock, timeout=5)
        if "ACK_LIVE" not in response:
            print(f"[-] Handshake rejected by ESP32: '{response}'")
            return

        safe_close(tcp_sock)
        tcp_sock = None
        time.sleep(0.3)

        # --- Audio device ---
        p = pyaudio.PyAudio()
        try:
            info = p.get_default_input_device_info()
            HW_RATE = int(info['defaultSampleRate'])
        except OSError:
            HW_RATE = 44100
            print(f"[!] Could not query default input device; assuming {HW_RATE} Hz.")

        ESP_RATE = 16000
        CHUNK = 1024

        mic_stream = p.open(
            format=pyaudio.paInt16,
            channels=1,
            rate=HW_RATE,
            input=True,
            frames_per_buffer=CHUNK,
        )

        # --- UDP socket sending to same PORT (5500) ---
        udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp_sock.bind(("0.0.0.0", PORT))

        print("\n=============================================")
        print(f"  🎙️ LIVE MIC ACTIVE - PURE UDP MODE!")
        print(f"  Streaming to  : {target_ip}:{PORT} (UDP)")
        print(f"  Hardware Rate : {HW_RATE} Hz (Native)")
        print(f"  ESP32 Rate    : {ESP_RATE} Hz (MTU Safe Payload)")
        print(f"  Math Strategy : Fixed-Point Integer Fraction (25/10)")
        print(f"  Limiter       : ±30000 (Safe Margin)")
        print("=============================================\n")

        stop_event = threading.Event()

        def wait_for_stop():
            input(" [!] Press [ENTER] to end the live announcement...\n")
            stop_event.set()

        threading.Thread(target=wait_for_stop, daemon=True).start()

        resample_state = None
        BOOST_NUM, BOOST_DEN = 25, 10

        while not stop_event.is_set():
            raw = mic_stream.read(CHUNK, exception_on_overflow=False)

            # DC offset removal
            audio_array = np.frombuffer(raw, dtype=np.int16).astype(np.int32)
            audio_array -= int(np.mean(audio_array))

            # Resampling
            pcm_bytes = audio_array.astype(np.int16).tobytes()
            converted_bytes, resample_state = ratecv(
                pcm_bytes, 2, 1, HW_RATE, ESP_RATE, resample_state
            )

            # Boost & Limiter
            decimated_array = np.frombuffer(converted_bytes, dtype=np.int16).astype(np.int32)
            decimated_array = (decimated_array * BOOST_NUM) // BOOST_DEN
            decimated_array = np.clip(decimated_array, -30000, 30000)

            # Send via UDP to same PORT
            final_dsp_bytes = decimated_array.astype(np.int16).tobytes()
            udp_sock.sendto(final_dsp_bytes, (target_ip, PORT))

        print("\n[*] Stopping live stream gracefully...")

    except OSError as e:
        print(f"\n[-] Live Stream Error: {e}")
    except Exception as e:
        print(f"\n[-] Unexpected Live Stream Error: {e}")
    finally:
        if mic_stream:
            try:
                mic_stream.stop_stream()
                mic_stream.close()
            except Exception:
                pass
        if p:
            p.terminate()
        if udp_sock:
            udp_sock.close()
        safe_close(tcp_sock)
        print("[+] UDP Stream disconnected.")

# ==========================================
# MAIN MENU
# ==========================================

def main_menu():
    while True:
        print("\n===================================")
        print("    AJEEVI AUDIO CONTROLLER")
        print("===================================")
        print(f"  TARGET IP : {target_ip}")
        print("-----------------------------------")
        print("  0. Change Target IP")
        print("  1. Upload New Song")
        print("  2. Play Audio")
        print("  3. Stop Audio")
        print("  4. Set Volume")
        print("  5. Live Announcement (UDP Paging)")
        print("  6. Exit")
        choice = input("Select Option: ").strip()

        if   choice == '0': change_ip()
        elif choice == '1': upload_file()
        elif choice == '2': play_audio()
        elif choice == '3': stop_audio()
        elif choice == '4': set_volume()
        elif choice == '5': live_stream()
        elif choice == '6': break
        else: print("[-] Unknown option.")

if __name__ == "__main__":
    main_menu()
