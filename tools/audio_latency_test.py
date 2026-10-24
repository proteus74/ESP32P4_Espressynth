#!/usr/bin/env python3
"""Plays a chord pattern over the board's WebSocket (/ws, "midiTest") and counts DMA underruns /
render overruns from the serial log meanwhile. Prints one summary line.

  python tools/audio_latency_test.py <ip> <com-port> [seconds=25] [settle=20] [--reset] [--stress]

  --reset   reset the board first and count the underruns of the boot phase too
  --stress  two parallel HTTP downloads of /static/js/main.js during the test (like the web UI loading)

Needs pyserial (ESP-IDF venv has it). Notes go over the web server's WebSocket, so the board must be
reachable on Wi-Fi; the serial port is only read. See documentation/audio-latency.md.
"""
import base64, json, os, re, socket, struct, sys, threading, time, urllib.request
import serial

ip, port = sys.argv[1], sys.argv[2]
args = [a for a in sys.argv[3:] if not a.startswith('--')]
duration = float(args[0]) if len(args) > 0 else 25
settle = float(args[1]) if len(args) > 1 else 20
do_reset = '--reset' in sys.argv
do_stress = '--stress' in sys.argv

lines, stop, stats = [], False, []

def serial_reader(ser):
    buf = b''
    while not stop:
        buf += ser.read(4096)
        while b'\n' in buf:
            l, buf = buf.split(b'\n', 1)
            lines.append((time.time(), l.decode('utf-8', 'replace').strip()))

def mem():
    try:
        d = json.load(urllib.request.urlopen(f'http://{ip}/api/diagnostics/memory', timeout=5))
        c = d['current']
        return c['internalFree'], c['internalMinimum'], c['psramFree'], d.get('graph', {}).get('blockBufferBytes')
    except Exception as e:
        return None

class WS:
    def __init__(self, ip):
        self.s = socket.create_connection((ip, 80), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f'GET /ws HTTP/1.1\r\nHost: {ip}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                        f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
        resp = b''
        while b'\r\n\r\n' not in resp:
            resp += self.s.recv(1024)
        assert b' 101 ' in resp.split(b'\r\n')[0], resp[:80]
        self.s.settimeout(None)
        threading.Thread(target=self._drain, daemon=True).start()
    def _recvn(self, n):
        b = b''
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise EOFError
            b += c
        return b
    def _drain(self):
        try:
            while True:
                h = self._recvn(2)
                op, n = h[0] & 0x0F, h[1] & 0x7F
                if n == 126: n = struct.unpack('>H', self._recvn(2))[0]
                elif n == 127: n = struct.unpack('>Q', self._recvn(8))[0]
                if h[1] & 0x80: self._recvn(4)
                data = self._recvn(n) if n else b''
                if op == 1 and b'"cpuLoad"' in data:
                    try: stats.append((time.time(), json.loads(data)))
                    except Exception: pass
        except Exception:
            pass
    def send(self, obj):
        p = json.dumps(obj).encode()
        mask = os.urandom(4)
        hdr = bytes([0x81])
        n = len(p)
        hdr += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n)
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(p)))
    def note(self, on, n, vel=100):
        self.send({'type': 'midiTest', 'midiType': 'note_on' if on else 'note_off', 'channel': 0, 'note': n, 'velocity': vel if on else 0})

ser = serial.Serial(port, 115200, timeout=0.2)
if do_reset:
    ser.dtr = False; ser.rts = True; time.sleep(0.1); ser.rts = False
threading.Thread(target=serial_reader, args=(ser,), daemon=True).start()

# wait until the web server answers, then settle (Wi-Fi/BLE start-up underruns are not part of the test)
t0 = time.time()
while mem() is None:
    if time.time() - t0 > 90:
        print('board not reachable'); sys.exit(2)
    time.sleep(1)
time.sleep(settle)
m0 = mem()
mark = len(lines)
ws = WS(ip)
def stress():
    while not stop:
        try:
            urllib.request.urlopen(f'http://{ip}/static/js/main.js', timeout=10).read()
        except Exception:
            time.sleep(0.2)
if do_stress:
    for _ in range(2): threading.Thread(target=stress, daemon=True).start()
smark = len(stats)
chords = [[48, 55, 60, 64, 67], [53, 57, 60, 65, 69], [55, 59, 62, 67, 71], [48, 55, 60, 64, 67]]
active, t_start, step = [], time.time(), 0
while time.time() - t_start < duration:
    chord = chords[(step // 4) % len(chords)]
    n = chord[step % len(chord)] + (12 if step % 8 >= 5 else 0)
    ws.note(True, n); active.append((time.time() + 1.0, n))
    for rel in [a for a in active if a[0] <= time.time()]:
        ws.note(False, rel[1]); active.remove(rel)
    step += 1
    time.sleep(0.12)
for _, n in active:
    ws.note(False, n)
time.sleep(1.5)
stop = True
m1 = mem()

boot = [l for t, l in lines[:mark] if 'Audio-Underrun' in l]
boot_events = sum(int(re.search(r'\+(\d+)', l).group(1)) for l in boot if re.search(r'\+(\d+)', l))
print(f'BOOT underrun_events_until_test_start={boot_events} (reset={do_reset})')
window = [l for t, l in lines[mark:]]
under = [l for l in window if 'Audio-Underrun' in l]
over = [l for l in window if 'Audio-Overrun' in l]
last_total = None
for l in under:
    m = re.search(r'\+(\d+), (\d+) total', l)
    if m:
        last_total = int(m.group(2))
events = sum(int(re.search(r'\+(\d+)', l).group(1)) for l in under if re.search(r'\+(\d+)', l))
st = [d for _, d in stats[smark:]]
def col(k, sub=None):
    v = [(d[k][sub] if sub else d[k]) for d in st if k in d]
    return v
c1 = col('core1'); pk = col('peakUs'); p99 = col('p99Us'); p50 = col('p50Us'); va = col('voiceActive')
bud = st[-1]['budgetUs'] if st else None
ur = col('underruns'); ro = col('renderOverruns')
if st:
    print(f'LOAD samples={len(st)} audioCore avg={sum(c1)/len(c1):.1f}% max={max(c1):.1f}%  p50={max(p50)}us p99={max(p99)}us peak={max(pk)}us budget={bud}us  '
          f'voices max={max(va) if va else None}  underruns {ur[0]}->{ur[-1]}  renderOverruns {ro[0]}->{ro[-1]}')
else:
    print('LOAD no cpuLoad broadcast received')
print(f'RESULT stress={do_stress} notes={step} duration={duration:.0f}s underrun_events={events} underrun_lines={len(under)} overrun_lines={len(over)} '
      f'mem_before(internalFree,min,psram,blockbuf)={m0} mem_after={m1}')
for l in (under + over)[:6]:
    print('  ', l[:170])
