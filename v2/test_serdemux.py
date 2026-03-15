import subprocess
import os
import time
import serial
import threading
import select
import errno
import signal
import sys

class SerdemuxTest:
    def __init__(self, bin_path="./serdemux", device="/tmp/ttyV0", pty="/tmp/ttyV1", num_streams=4):
        self.bin_path = bin_path
        self.device = device
        self.pty = pty
        self.num_streams = num_streams
        self.socat_proc = None
        self.serdemux_proc = None
        self.stream_dir = "streams"
        self.rx_fds = {}
        self.tx_fds = {}

    def setup(self):
        # Cleanup
        for f in [self.device, self.pty]:
            if os.path.exists(f): os.remove(f)

        self.socat_proc = subprocess.Popen([
            "socat", "-d", "-d",
            f"PTY,link={self.device},raw,echo=0",
            f"PTY,link={self.pty},raw,echo=0"
        ])

        timeout = 5
        start_time = time.time()
        while not (os.path.exists(self.device) and os.path.exists(self.pty)):
            if time.time() - start_time > timeout:
                raise RuntimeError("Timeout waiting for socat")
            time.sleep(0.1)

        if not os.path.exists(self.bin_path):
            print(f"Binary not found at {self.bin_path}. Attempting to compile...")
            source_path = os.path.join(os.path.dirname(self.bin_path), "serdemux.c")
            subprocess.run(["gcc", "-std=c11", "-O2", "-pthread", "-o", self.bin_path, source_path], check=True)

        self.serdemux_proc = subprocess.Popen([
            self.bin_path, "-d", self.pty, "-n", str(self.num_streams)
        ], stderr=subprocess.PIPE, text=True)
        time.sleep(1)

        for i in range(self.num_streams):
            rx_path = os.path.join(self.stream_dir, f"{i}.rx")
            tx_path = os.path.join(self.stream_dir, f"{i}.tx")
            start_wait = time.time()
            while not (os.path.exists(rx_path) and os.path.exists(tx_path)):
                if time.time() - start_wait > 2:
                    raise RuntimeError(f"FIFOs not created by serdemux")
                time.sleep(0.1)
            self.rx_fds[i] = os.open(rx_path, os.O_RDWR | os.O_NONBLOCK)
            self.tx_fds[i] = os.open(tx_path, os.O_RDWR | os.O_NONBLOCK)

    def teardown(self):
        print("Tearing down...")
        for fd in list(self.rx_fds.values()) + list(self.tx_fds.values()):
            os.close(fd)
        self.rx_fds = {}
        self.tx_fds = {}

        if self.serdemux_proc:
            self.serdemux_proc.send_signal(signal.SIGINT)
            try:
                self.serdemux_proc.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                self.serdemux_proc.kill()
        if self.socat_proc:
            self.socat_proc.terminate()
            self.socat_proc.wait()

        if os.path.exists(self.stream_dir):
            for f in os.listdir(self.stream_dir):
                os.remove(os.path.join(self.stream_dir, f))
            os.rmdir(self.stream_dir)

        for f in [self.device, self.pty]:
            if os.path.exists(f): os.remove(f)

    def test_bidirectional(self):
        print("Testing Bidirectional...")
        ser_fd = os.open(self.device, os.O_RDWR | os.O_NOCTTY)

        stream_id = 0
        rx_fd = self.rx_fds[stream_id]
        tx_fd = self.tx_fds[stream_id]

        # 1. FIFO -> Serial
        msg_to_ser = b"Hi Serial\n"
        os.write(tx_fd, msg_to_ser)

        expected_on_ser = bytes([stream_id]) + msg_to_ser
        received_on_ser = b""
        for _ in range(20):
            r, w, e = select.select([ser_fd], [], [], 0.1)
            if ser_fd in r:
                chunk = os.read(ser_fd, 1024)
                if chunk:
                    received_on_ser += chunk
                    if received_on_ser == expected_on_ser: break

        assert received_on_ser == expected_on_ser, f"Expected {expected_on_ser!r}, got {received_on_ser!r}"
        print("FIFO -> Serial success")

        # 2. Serial -> FIFO
        msg_to_fifo = b"Hi FIFO\n"
        payload = bytes([stream_id]) + msg_to_fifo
        os.write(ser_fd, payload)

        received = b""
        for _ in range(50):
            r, w, e = select.select([rx_fd], [], [], 0.1)
            if rx_fd in r:
                try:
                    chunk = os.read(rx_fd, 1024)
                    if chunk:
                        received += chunk
                        if received == msg_to_fifo: break
                except BlockingIOError: pass
            time.sleep(0.1)

        assert received == msg_to_fifo, f"Expected {msg_to_fifo!r}, got {received!r}"
        print("Serial -> FIFO success")
        os.close(ser_fd)

    def test_multiple_streams(self):
        print("Testing Multiple Streams...")
        ser_fd = os.open(self.device, os.O_RDWR | os.O_NOCTTY)
        for i in range(self.num_streams):
            msg = f"Stream {i} data\n".encode()
            os.write(ser_fd, bytes([i]) + msg)
            received = b""
            for _ in range(50):
                r, w, e = select.select([self.rx_fds[i]], [], [], 0.1)
                if self.rx_fds[i] in r:
                    try:
                        chunk = os.read(self.rx_fds[i], 1024)
                        if chunk:
                            received += chunk
                            if received == msg: break
                    except BlockingIOError: pass
                time.sleep(0.1)
            assert received == msg, f"Stream {i}: Expected {msg!r}, got {received!r}"
        os.close(ser_fd)
        print("Multiple Streams success")

    def test_large_payload(self):
        print("Testing Large Payload...")
        ser_fd = os.open(self.device, os.O_RDWR | os.O_NOCTTY)
        stream_id = 1
        payload = b"X" * 10000 + b"\n"
        full_msg = bytes([stream_id]) + payload

        chunk_size = 1024
        for i in range(0, len(full_msg), chunk_size):
            os.write(ser_fd, full_msg[i:i+chunk_size])

        received = b""
        rx_fd = self.rx_fds[stream_id]
        for _ in range(100):
            r, w, e = select.select([rx_fd], [], [], 0.1)
            if rx_fd in r:
                try:
                    chunk = os.read(rx_fd, 4096)
                    if chunk:
                        received += chunk
                        if len(received) >= len(payload): break
                except BlockingIOError: pass
            time.sleep(0.1)

        assert received == payload, f"Large Payload: expected {len(payload)}, got {len(received)}"
        os.close(ser_fd)
        print("Large Payload success")

    def run_all(self):
        try:
            self.setup()
            self.test_bidirectional()
            self.test_multiple_streams()
            self.test_large_payload()
            print("All tests passed!")
        except Exception as e:
            print(f"Test failed: {e}")
            sys.exit(1)
        finally:
            self.teardown()

if __name__ == "__main__":
    bin_path = "./serdemux"
    if len(sys.argv) > 1:
        bin_path = sys.argv[1]

    tester = SerdemuxTest(bin_path=bin_path)
    tester.run_all()
