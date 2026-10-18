"""A fake Tuya 3.4 device (the local protocol, TCP port 6668) for the on-device tests.

The board is told that a device with this computer's IP exists, and talks to this server instead of a socket.
That lets the tests make a device go away and come back, change its readings and switch it, without touching
a real socket. Only protocol 3.4 is implemented (session key handshake, status query 0x10, control 0x0d, UPDATEDPS 0x12,
heartbeat 0x09). One connection may carry many requests; it is closed after `idle_timeout` seconds without one.

It also records what the board does: `connections` (every accepted TCP connection), `commands` (the control
commands received, newest last), `open_connections`, `queries`, `heartbeats`, and it checks the HMAC of every frame the
board sends.
"""
import hashlib
import hmac
import json
import socket
import socketserver
import struct
import threading
import time

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PREFIX, SUFFIX = 0x000055AA, 0x0000AA55
SESS_START, SESS_RESP, SESS_FINISH, CONTROL_NEW, STATUS_PUSH, DP_QUERY_NEW, UPDATEDPS, HEART_BEAT = 3, 4, 5, 0x0D, 0x08, 0x10, 0x12, 0x09


def _ecb(key, data, encrypt):
    c = Cipher(algorithms.AES(key), modes.ECB())
    op = c.encryptor() if encrypt else c.decryptor()
    return op.update(data) + op.finalize()


def _pad(data):
    n = 16 - len(data) % 16
    return data + bytes([n]) * n


def _unpad(data):
    n = data[-1]
    return data[:-n] if 1 <= n <= 16 else data


def _recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


class FakeTuya:
    def __init__(self, key=b"0123456789abcdef", port=6668, dps=None):
        self.key = key
        self.port = port
        self.dps = dict(dps or {"1": True, "18": 300, "19": 480, "20": 2250})
        self.connections = 0
        self.open_connections = 0   # connections open right now
        self.queries = 0            # status queries (0x10) answered
        self.heartbeats = 0         # heartbeats (0x09) answered
        self.idle_timeout = 10.0    # seconds without a request before the fake closes the connection
        self.answer_heartbeat = True  # False: heartbeats are counted but never answered
        self.commands = []          # [(unix time, {dp: value})]
        self.hmac_errors = 0
        self.answer_delay = 0.0     # seconds to wait before answering the handshake (to provoke a timeout)
        self.silent = False         # accept the connection, then never answer
        self.ignore_control = False  # acknowledge control commands but do not change anything
        self.updatedps = 0          # UPDATEDPS (0x12) commands received
        self.no_updatedps = False   # close the connection on UPDATEDPS, like a device that does not know the command
        self.lock = threading.Lock()
        self._server = None
        self._thread = None

    # ---- lifecycle: stop() makes the board's connections fail at once (connection refused) ----------
    def start(self):
        if self._server:
            return
        fake = self

        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                with fake.lock:
                    fake.connections += 1
                    fake.open_connections += 1
                try:
                    fake._serve(self.request)
                except (ConnectionError, OSError, struct.error):
                    pass
                finally:
                    with fake.lock:
                        fake.open_connections -= 1

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True

        self._server = Server(("0.0.0.0", self.port), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()

    def stop(self):
        if self._server:
            self._server.shutdown()
            self._server.server_close()
            self._server = None

    def set(self, **dps):
        with self.lock:
            for k, v in dps.items():
                self.dps[k.lstrip("d")] = v

    def get(self, dp):
        with self.lock:
            return self.dps.get(str(dp))

    # ---- protocol -----------------------------------------------------------------------------
    def _frame(self, seq, cmd, payload_plain, key, retcode=0):
        body = struct.pack(">I", retcode) + (_ecb(key, _pad(payload_plain), True) if payload_plain else b"")
        head = struct.pack(">IIII", PREFIX, seq, cmd, len(body) + 36)
        mac = hmac.new(key, head + body, hashlib.sha256).digest()
        return head + body + mac + struct.pack(">I", SUFFIX)

    def _read(self, sock, key):
        head = _recv_exact(sock, 16)
        prefix, seq, cmd, length = struct.unpack(">IIII", head)
        if prefix != PREFIX:
            raise ConnectionError("bad prefix")
        rest = _recv_exact(sock, length)
        payload, mac, suffix = rest[:-36], rest[-36:-4], rest[-4:]
        if struct.unpack(">I", suffix)[0] != SUFFIX or not hmac.compare_digest(mac, hmac.new(key, head + payload, hashlib.sha256).digest()):
            with self.lock:
                self.hmac_errors += 1
        return seq, cmd, _unpad(_ecb(key, payload, False)) if payload else b""

    def _serve(self, sock):
        sock.settimeout(10)
        if self.silent:
            time.sleep(10)
            return
        seq, cmd, local_nonce = self._read(sock, self.key)
        if cmd != SESS_START:
            return
        if self.answer_delay:
            time.sleep(self.answer_delay)
        device_nonce = b"fakedevicenonce!"
        sock.sendall(self._frame(seq, SESS_RESP, device_nonce + hmac.new(self.key, local_nonce, hashlib.sha256).digest(), self.key))
        seq, cmd, finish = self._read(sock, self.key)
        if cmd != SESS_FINISH or finish != hmac.new(self.key, device_nonce, hashlib.sha256).digest():
            with self.lock:
                self.hmac_errors += 1
            return
        mixed = bytes(a ^ b for a, b in zip(local_nonce, device_nonce))
        session = _ecb(self.key, mixed, True)
        while True:
            sock.settimeout(self.idle_timeout)
            seq, cmd, plain = self._read(sock, session)
            if cmd == HEART_BEAT:
                with self.lock:
                    self.heartbeats += 1
                if self.answer_heartbeat:
                    sock.sendall(self._frame(seq, HEART_BEAT, b"", session))
            elif cmd == DP_QUERY_NEW:
                with self.lock:
                    self.queries += 1
                    body = json.dumps({"dps": self.dps}, separators=(",", ":")).encode()
                sock.sendall(self._frame(seq, DP_QUERY_NEW, body, session))
            elif cmd == CONTROL_NEW:
                data = json.loads(plain[15:].decode())
                changes = data.get("data", {}).get("dps", {})
                with self.lock:
                    if not self.ignore_control:
                        self.dps.update({str(k): v for k, v in changes.items()})
                    self.commands.append((time.time(), changes))
                sock.sendall(self._frame(seq, CONTROL_NEW, b"", session))
            elif cmd == UPDATEDPS:  # like the real sockets: the fresh values are pushed first, the answer follows
                with self.lock:
                    self.updatedps += 1
                if self.no_updatedps:
                    return
                wanted = [str(x) for x in json.loads(plain.decode()).get("dpId", [])]
                with self.lock:
                    fresh = {k: v for k, v in self.dps.items() if k in wanted}
                push = json.dumps({"protocol": 4, "t": int(time.time()), "data": {"dps": fresh}}, separators=(",", ":")).encode()
                sock.sendall(self._frame(0, STATUS_PUSH, push, session))
                sock.sendall(self._frame(seq, UPDATEDPS, b"", session))
            else:
                return


def lan_ip_towards(host):
    """The address of this computer on the network that reaches `host` (the board)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, 80))
        return s.getsockname()[0]
    finally:
        s.close()
