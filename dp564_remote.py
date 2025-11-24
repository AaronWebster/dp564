import socket
import time
import sys
import threading

# --- Protocol Messages ---
# These constant byte sequences were identified from Wireshark captures.

# Handshake messages sent by the client after connecting.
HANDSHAKE_MSG_1 = b'\x00\x00\x00\x05'
HANDSHAKE_MSG_2 = b'\x03'

# A preliminary command sent before the main volume/dim command.
PRE_CMD = b'\x00\x00\x00\x0a'
# The prefix for the volume set command. The final byte is the volume level.
VOLUME_CMD_PREFIX = b'\x02\x03\x12\x00\x00'
# The prefix for the server's acknowledgment of a volume change command.
VOLUME_ACK_PREFIX = b'\x00\x00\x00\x0b\x04\x03\x12\x01\x02\x00'
# The prefix for unsolicited volume updates from the device (e.g., physical knob).
VOLUME_DEVICE_UPDATE_PREFIX = b'\x00\x00\x00\x0b\x04\x03\x14\x01\x02\x00'


# Commands for the DIM (mute) feature.
DIM_CMD_PREFIX = b'\x02\x05\x13\x00\x00'
DIM_ON_PAYLOAD = b'\x01'
DIM_OFF_PAYLOAD = b'\x00'
# The prefix for the server's acknowledgment of a DIM state change.
DIM_ACK_PREFIX = b'\x00\x00\x00\x0b\x04\x05\x13\x01\x02\x00'

# Commands for input source selection.
SOURCE_CMD_PREFIX = b'\x02\x03\x01\x00\x00'
SOURCE_ACK_PREFIX = b'\x00\x00\x00\x0b\x04\x03\x01\x01\x02\x00'
SOURCE_MAP = {
    'aes1': b'\x00',
    'aes2': b'\x01',
    'optical': b'\x02',
    'streaming': b'\x03',
}

# The recurring heartbeat packet sent by both the server and client.
HEARTBEAT_PACKET = b'\x00\x00\x00\x05\x04'
HEARTBEAT_INTERVAL = 10 # Seconds


class Dp564Remote:
    """
    A class to control a Dolby DP564 reference decoder over a network.
    It handles the connection, handshake, and listens for heartbeats in a
    background thread.
    """

    def __init__(self, host, port=4444):
        """
        Initializes the Dp564Remote object and connects to the device.

        Args:
            host (str): The IP address of the DP564 unit.
            port (int): The port number for the remote protocol (default 4444).
        """
        self.host = host
        self.port = port
        self.sock = None
        self.listener_thread = None
        self.heartbeat_thread = None
        self.is_connected = False
        
        # --- Internal State Variables ---
        self.volume_db = 0.0 # Assume 0.0dB initially
        self.dim_state = False # Assume DIM is off initially
        self.source = 'aes1' # Assume AES1 is the default source
        
        # Threading events for handling acknowledgments and stopping threads
        self.stop_event = threading.Event()
        self.ack_event = threading.Event()
        self.last_ack = None
        self.ack_lock = threading.Lock()

        self.connect()

    def connect(self):
        """
        Establishes the connection to the DP564, performs the handshake,
        and starts the background listener and heartbeat threads.
        """
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(5.0)
            print(f"Connecting to DP564 at {self.host}:{self.port}...")
            self.sock.connect((self.host, self.port))
            self.is_connected = True
            print("Connection successful.")

            print("Sending handshake...")
            self.sock.sendall(HANDSHAKE_MSG_1)
            time.sleep(0.1)
            self.sock.sendall(HANDSHAKE_MSG_2)
            print("Handshake complete.")

            print("Receiving initial data dump from server...")
            initial_data_buffer = b''
            self.sock.settimeout(10.0) 
            while HEARTBEAT_PACKET not in initial_data_buffer:
                chunk = self.sock.recv(4096)
                if not chunk:
                    print("Connection closed during initial data dump.")
                    self.is_connected = False
                    return
                initial_data_buffer += chunk
            self.sock.settimeout(1.0)
            print("Initial data received. Starting background threads.")

            self.stop_event.clear()
            
            self.listener_thread = threading.Thread(target=self._listen_for_packets)
            self.listener_thread.daemon = True
            self.listener_thread.start()

            self.heartbeat_thread = threading.Thread(target=self._send_heartbeats)
            self.heartbeat_thread.daemon = True
            self.heartbeat_thread.start()

        except Exception as e:
            print(f"Failed to connect: {e}")
            self.is_connected = False

    def _listen_for_packets(self):
        """
        (Private) Runs in a thread to listen for incoming packets and update state.
        """
        while not self.stop_event.is_set():
            try:
                data = self.sock.recv(1024)
                if not data:
                    print("\n[Listener] Connection closed by the DP564.")
                    self.is_connected = False
                    self.stop_event.set()
                    break

                if data == HEARTBEAT_PACKET:
                    continue
                
                # Check for volume status updates from the device (physical knob)
                elif data.startswith(VOLUME_DEVICE_UPDATE_PREFIX):
                    value = data[-1]
                    db_level = (value - 192) / 2.0
                    if self.volume_db != db_level:
                        self.volume_db = db_level
                        print(f"\n[Listener] Volume updated by device to: {self.volume_db:.1f} dB")
                
                # Handle other ACKs or status messages
                else:
                    # Check for volume command ACK
                    if data.startswith(VOLUME_ACK_PREFIX):
                         value = data[-1]
                         self.volume_db = (value - 192) / 2.0

                    with self.ack_lock:
                        self.last_ack = data
                        self.ack_event.set()

            except socket.timeout:
                continue
            except Exception as e:
                if not self.stop_event.is_set():
                    print(f"\n[Listener] An error occurred: {e}")
                self.is_connected = False
                self.stop_event.set()
                break
    
    def _send_heartbeats(self):
        """
        (Private) Runs in a thread to send keep-alive heartbeats to the device.
        """
        while not self.stop_event.wait(HEARTBEAT_INTERVAL):
            if not self.is_connected:
                break
            try:
                self.sock.sendall(HEARTBEAT_PACKET)
            except Exception as e:
                print(f"\n[Heartbeat] Failed to send heartbeat: {e}")
                self.is_connected = False
                self.stop_event.set()
                break

    def SetVolumeDb(self, db_level):
        """
        Sends the command to set the master volume on the DP564 and waits for acknowledgment.
        """
        if not self.is_connected: return
        if not -95.0 <= db_level <= 0.0:
            print("Invalid volume. Please enter a value between -95.0 and 0.0.")
            return

        value = int(192 + (db_level * 2))
        volume_command = VOLUME_CMD_PREFIX + bytes([value])
        expected_ack = VOLUME_ACK_PREFIX + bytes([value])

        try:
            with self.ack_lock:
                self.ack_event.clear()
                self.last_ack = None

            self.sock.sendall(PRE_CMD)
            time.sleep(0.1)
            self.sock.sendall(volume_command)
            print(f"Sent command to set volume to {db_level} dB")

            if self.ack_event.wait(timeout=2.0):
                with self.ack_lock:
                    if self.last_ack and self.last_ack.endswith(expected_ack):
                         print(f"Volume change to {db_level} dB acknowledged.")
                         self.volume_db = db_level
                    else:
                         print(f"Warning: Received unexpected ACK: {self.last_ack.hex() if self.last_ack else 'None'}")
            else:
                print("Warning: No acknowledgment received for volume change.")
        except Exception as e:
            print(f"Failed to send volume command: {e}")
            self.disconnect()

    def Dim(self, value=None):
        """
        Sets or toggles the DIM (mute) state.
        """
        if not self.is_connected: return
        target_state = not self.dim_state if value is None else bool(value)
        dim_payload = DIM_ON_PAYLOAD if target_state else DIM_OFF_PAYLOAD
        state_str = "ON" if target_state else "OFF"
        dim_command = DIM_CMD_PREFIX + dim_payload
        expected_ack = DIM_ACK_PREFIX + dim_payload

        try:
            with self.ack_lock:
                self.ack_event.clear()
                self.last_ack = None
            self.sock.sendall(PRE_CMD)
            time.sleep(0.1)
            self.sock.sendall(dim_command)
            print(f"Sent command to turn DIM {state_str}")

            if self.ack_event.wait(timeout=2.0):
                with self.ack_lock:
                    if self.last_ack and self.last_ack.endswith(expected_ack):
                        print(f"DIM state change to {state_str} acknowledged.")
                        self.dim_state = target_state
                    else:
                        print(f"Warning: Received unexpected ACK for DIM command: {self.last_ack.hex() if self.last_ack else 'None'}")
            else:
                print("Warning: No acknowledgment received for DIM command.")
        except Exception as e:
            print(f"Failed to send DIM command: {e}")
            self.disconnect()

    def SetSource(self, source_name):
        """
        Sets the input source.
        """
        if not self.is_connected: return
        source_name = source_name.lower()
        if source_name not in SOURCE_MAP:
            print(f"Invalid source name. Choose from: {', '.join(SOURCE_MAP.keys())}")
            return

        source_payload = SOURCE_MAP[source_name]
        source_command = SOURCE_CMD_PREFIX + source_payload
        expected_ack = SOURCE_ACK_PREFIX + source_payload

        try:
            with self.ack_lock:
                self.ack_event.clear()
                self.last_ack = None
            self.sock.sendall(PRE_CMD)
            time.sleep(0.1)
            self.sock.sendall(source_command)
            print(f"Sent command to set source to {source_name}")

            if self.ack_event.wait(timeout=2.0):
                with self.ack_lock:
                    if self.last_ack and self.last_ack.endswith(expected_ack):
                        print(f"Source change to {source_name} acknowledged.")
                        self.source = source_name
                    else:
                        print(f"Warning: Received unexpected ACK for source command: {self.last_ack.hex() if self.last_ack else 'None'}")
            else:
                print("Warning: No acknowledgment received for source change.")
        except Exception as e:
            print(f"Failed to send source command: {e}")
            self.disconnect()

    def disconnect(self):
        """
        Cleanly disconnects from the DP564.
        """
        print("Disconnecting...")
        self.stop_event.set()
        if self.listener_thread and self.listener_thread.is_alive():
            self.listener_thread.join()
        if self.heartbeat_thread and self.heartbeat_thread.is_alive():
            self.heartbeat_thread.join()
        if self.sock:
            self.sock.close()
        self.is_connected = False
        print("Disconnected.")

    def __del__(self):
        """
        Destructor to ensure disconnection when the object is garbage collected.
        """
        if self.is_connected:
            self.disconnect()


if __name__ == '__main__':
    DP564_IP = '192.168.0.11'
    print("Initializing DP564 Remote...")
    remote = Dp564Remote(DP564_IP)

    if remote.is_connected:
        print("\n--- Dolby DP564 Remote ---")
        print("Commands:")
        print("  volume <dB>      - Set volume from -95.0 to 0.0")
        print("  source <name>    - Set source (aes1, aes2, optical, streaming)")
        print("  dim / dim on / dim off - Toggle or set DIM state")
        print("  status           - Show current device state")
        print("  q / quit         - Exit")

        try:
            while remote.is_connected:
                command_input = input("\nCommand > ").lower().split()
                if not command_input: continue
                cmd = command_input[0]

                if cmd in ['q', 'quit']: break
                elif cmd == 'dim':
                    if len(command_input) > 1 and command_input[1] == 'on': remote.Dim(True)
                    elif len(command_input) > 1 and command_input[1] == 'off': remote.Dim(False)
                    else: remote.Dim()
                elif cmd == 'volume':
                    if len(command_input) > 1:
                        try: remote.SetVolumeDb(float(command_input[1]))
                        except ValueError: print("Invalid volume value.")
                    else: print("Please specify a volume level (e.g., 'volume -20.5').")
                elif cmd == 'source':
                    if len(command_input) > 1: remote.SetSource(command_input[1])
                    else: print("Please specify a source name (e.g., 'source aes1').")
                elif cmd == 'status':
                    print(f"  - Current Volume: {remote.volume_db:.1f} dB")
                    print(f"  - Current Source: {remote.source}")
                    print(f"  - DIM State: {'ON' if remote.dim_state else 'OFF'}")
                else: print("Invalid command.")
        except KeyboardInterrupt: print("\nUser interrupted.")
        finally: remote.disconnect()
    else: print("Could not establish a connection with the DP564.")
