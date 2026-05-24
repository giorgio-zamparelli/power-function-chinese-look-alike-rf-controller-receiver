#!/usr/bin/env python3
"""
lego-rf control panel — drive BOTH outputs of the cloned LEGO-PF receiver.

The Arduino runs arduino/lego_tx (XN297 transmitter, serial-settable payload).
Server holds the serial port open and serves:
    /        dual-output control panel (Output A + Output B, combined)
    /grid    raw 0-255 payload explorer

DECODED PROTOCOL — one payload byte carries BOTH outputs:
    bits 4-7 = Output A : dir [5:4] (01 CW / 10 CCW / 11 brake), quad encoder [7:6]
    bits 0-3 = Output B : dir [1:0] (01 CW / 10 CCW / 11 brake), quad encoder [3:2]
    Speed is INCREMENTAL (the wheel is a quadrature encoder): send a phase sequence.
      A: start CW 0x10, faster 0x50->0x90 ; start CCW 0x20, faster 0x60->0xA0 ; brake 0x30
      B: start CW 0x01, faster 0x05->0x09 ; start CCW 0x02, faster 0x06->0x0A ; brake 0x03
    Combined byte sent each packet = A_nibble | B_nibble (e.g. 0x11 = both forward).

Run:  python3 webapp/control_server.py [/dev/cu.usbmodemXXXX] [port]
Then open http://localhost:8080
"""
import os, sys, time, termios, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT_DEV = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1122301"
HTTP_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

ser_fd = None
ser_lock = threading.Lock()
aState = 0   # Output A nibble (high)
bState = 0   # Output B nibble (low)

def serial_open():
    global ser_fd
    fd = os.open(PORT_DEV, os.O_RDWR | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[3] = 0
    a[2] = termios.CREAD | termios.CLOCAL | termios.CS8
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    ser_fd = fd
    buf = b""; t0 = time.time()
    while time.time() - t0 < 5:
        try:
            d = os.read(fd, 4096)
            if d: buf += d
        except OSError:
            pass
        if b"ready" in buf:
            break
        time.sleep(0.05)
    print("serial ready on", PORT_DEV, "| banner:", b"ready" in buf)

def emit():
    with ser_lock:
        os.write(ser_fd, (str((aState | bState) & 0xFF) + "\n").encode())

def set_ch(ch, v):
    global aState, bState
    if ch == "a": aState = v & 0xF0
    else:         bState = v & 0x0F
    emit(); print("set", ch, hex(v), "-> byte", hex(aState | bState))

def seq_ch(ch, vals, gap=0.06):
    global aState, bState
    for v in vals:
        if ch == "a": aState = v & 0xF0
        else:         bState = v & 0x0F
        emit(); time.sleep(gap)
    print("seq", ch, [hex(v) for v in vals])

def stop_all():
    global aState, bState
    aState, bState = 0x30, 0x03   # brake both
    emit(); print("STOP both -> 0x33")

def raw_send(v):
    global aState, bState
    v &= 0xFF; aState = v & 0xF0; bState = v & 0x0F
    emit(); print("raw", hex(v))

def set_channel(n):
    n = max(1, min(4, int(n)))
    with ser_lock:
        os.write(ser_fd, ("c%d\n" % n).encode())   # tells the Arduino to switch RF channels + address
    print("channel", n)

STYLE = """
 :root{--bg:#0f1115;--fg:#e8eaed;--mut:#8b909a;--grn:#2ecc71;--blu:#3aa0ff;--red:#ff4d4f;--ylw:#f5c451}
 *{box-sizing:border-box;-webkit-user-select:none;user-select:none}
 body{margin:0;font:16px/1.4 -apple-system,system-ui,sans-serif;background:var(--bg);color:var(--fg);padding:18px;max-width:620px;margin:auto}
 a{color:var(--blu);text-decoration:none;font-size:13px}
 h1{font-size:19px;font-weight:600;margin:8px 0 2px}.sub{color:var(--mut);font-size:13px;margin-bottom:14px}
 button{color:#fff;border:0;border-radius:13px;cursor:pointer;touch-action:manipulation;font:700 16px system-ui}
 button:active{transform:scale(.96)}
 h2{font-size:14px;color:var(--mut);text-transform:uppercase;letter-spacing:.05em;margin:18px 0 6px}
"""

CONTROL_HTML = """<!doctype html><html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>LEGO dual control</title>
<style>%s
 .blk{border:1px solid #232733;border-radius:14px;padding:12px;margin-bottom:12px}
 .row{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px}.row button{padding:20px 0;font-size:15px}
 .cw{background:var(--grn)}.ccw{background:var(--blu)}.st{background:#555c6b}
 .meter{font:700 14px ui-monospace,monospace;color:var(--ylw);margin-bottom:8px}
 .stopall{background:var(--red);width:100%%;padding:20px 0;font-size:19px;margin-top:4px}
 .byte{text-align:center;font:700 14px ui-monospace,monospace;color:var(--mut);margin-top:12px}
 .byte b{color:var(--fg)}.nav{text-align:right}
 .chanrow{display:flex;align-items:center;gap:8px;margin-bottom:14px;color:var(--mut);font-size:13px}
 .ch{padding:9px 16px;background:#2a2f3a;border-radius:9px;font-size:15px}
 .ch.on{background:var(--ylw);color:#0b0d10}
</style></head><body>
<div class=nav><a href="/grid">number grid →</a></div>
<h1>LEGO dual-output control</h1>
<div class=sub>Output A + Output B, sent together (e.g. steering + drive)</div>
<div class=chanrow><span>channel switch:</span><span id=chbtns></span></div>

<div class=blk>
 <h2>Output A (high nibble)</h2>
 <div class=meter>A: <span id=am>stop</span></div>
 <div class=row>
  <button class=cw id=acw>▲ CW</button>
  <button class=ccw id=accw>▼ CCW</button>
  <button class=st id=astop>■ stop</button>
 </div>
</div>

<div class=blk>
 <h2>Output B (low nibble)</h2>
 <div class=meter>B: <span id=bm>stop</span></div>
 <div class=row>
  <button class=cw id=bcw>▲ CW</button>
  <button class=ccw id=bccw>▼ CCW</button>
  <button class=st id=bstop>■ stop</button>
 </div>
</div>

<button class=stopall id=stopall>■ STOP BOTH (brake)</button>
<div class=byte>byte sent: <b id=byte>0x00</b></div>

<script>
 const $=id=>document.getElementById(id);
 let actx=0,bctx=0,al=0,bl=0;
 function disp(){$('am').textContent=al>0?('CW · '+al):al<0?('CCW · '+(-al)):'stop';
   $('bm').textContent=bl>0?('CW · '+bl):bl<0?('CCW · '+(-bl)):'stop';}
 const get=u=>fetch(u).then(r=>r.text()).then(t=>{if(t&&t[0]==='@')$('byte').textContent=t.slice(1);}).catch(()=>{});
 $('acw').onclick=()=>{if(actx!==1){get('/set?ch=a&v=16');actx=1;}get('/seq?ch=a&v=80,144');al++;disp();};
 $('accw').onclick=()=>{if(actx!==2){get('/set?ch=a&v=32');actx=2;}get('/seq?ch=a&v=96,160');al--;disp();};
 $('astop').onclick=()=>{get('/set?ch=a&v=48');actx=0;al=0;disp();};
 $('bcw').onclick=()=>{if(bctx!==1){get('/set?ch=b&v=1');bctx=1;}get('/seq?ch=b&v=5,9');bl++;disp();};
 $('bccw').onclick=()=>{if(bctx!==2){get('/set?ch=b&v=2');bctx=2;}get('/seq?ch=b&v=6,10');bl--;disp();};
 $('bstop').onclick=()=>{get('/set?ch=b&v=3');bctx=0;bl=0;disp();};
 $('stopall').onclick=()=>{get('/stop');actx=bctx=0;al=bl=0;disp();};
 // channel selector 1-4 (must match the receiver's physical switch)
 function setChan(n){get('/channel?n='+n);[...document.querySelectorAll('.ch')].forEach(b=>b.classList.toggle('on',+b.dataset.n===n));}
 (function(){const h=$('chbtns');for(let n=1;n<=4;n++){const b=document.createElement('button');b.className='ch'+(n===1?' on':'');b.dataset.n=n;b.textContent=n;b.onclick=()=>setChan(n);h.appendChild(b);}})();
</script></body></html>""" % STYLE

GRID_HTML = """<!doctype html><html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>payload grid</title>
<style>%s
 .read{text-align:center;font:600 15px ui-monospace,monospace;margin:8px 0}.read b{font-size:24px;color:var(--ylw)}.read .bin{color:var(--blu)}
 .legend{font-size:12px;color:var(--mut);text-align:center;margin-bottom:8px}.legend i{font-style:normal;padding:2px 6px;border-radius:5px;color:#0b0d10;font-weight:700}
 .grid{display:grid;grid-template-columns:repeat(16,1fr);gap:3px}
 .grid button{font:600 10px ui-monospace,monospace;padding:8px 0;border-radius:5px;color:#0b0d10}
 .c1{background:#2ecc71}.c2{background:#3aa0ff}.c3{background:#ff6b6d}.c0{background:#454b57;color:#9aa0aa}
 .stop{background:var(--red);width:100%%;padding:16px 0;margin-top:10px;font-size:17px}.nav{text-align:left}
 .chanrow{display:flex;align-items:center;gap:8px;margin-bottom:12px;color:var(--mut);font-size:13px}
 .ch{padding:8px 14px;background:#2a2f3a;border-radius:9px;font-size:14px}.ch.on{background:var(--ylw);color:#0b0d10}
</style></head><body>
<div class=nav><a href="/">← controls</a></div>
<h1>raw payload grid</h1>
<div class=sub>click any 0–255 · high nibble=Output A, low nibble=Output B</div>
<div class=chanrow><span>channel switch:</span><span id=chbtns></span></div>
<div class=read>value <b id=ev>0</b> <span id=eh>0x00</span> <span class=bin id=eb>0000&nbsp;0000</span></div>
<div class=legend>low-bits: <i class=c1>01 fwd</i> <i class=c2>10 rev</i> <i class=c3>11 stop</i> <i class=c0>00 float</i></div>
<div class=grid id=grid></div>
<button class=stop id=stop>■ STOP / BRAKE (3)</button>
<script>
 const $=id=>document.getElementById(id);
 function bin(v){return v.toString(2).padStart(8,'0').replace(/(....)(....)/,'$1&nbsp;$2');}
 function send(v){fetch('/send?v='+v).catch(()=>{});$('ev').textContent=v;$('eh').textContent='0x'+v.toString(16).padStart(2,'0').toUpperCase();$('eb').innerHTML=bin(v);}
 const g=$('grid');for(let v=0;v<256;v++){const b=document.createElement('button');b.textContent=v;b.className='c'+(v&3);b.onclick=()=>send(v);g.appendChild(b);}
 $('stop').onclick=()=>send(3);
 function setChan(n){fetch('/channel?n='+n).catch(()=>{});[...document.querySelectorAll('.ch')].forEach(b=>b.classList.toggle('on',+b.dataset.n===n));}
 (function(){const h=$('chbtns');for(let n=1;n<=4;n++){const b=document.createElement('button');b.className='ch'+(n===1?' on':'');b.dataset.n=n;b.textContent=n;b.onclick=()=>setChan(n);h.appendChild(b);}})();
</script></body></html>""" % STYLE

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _html(self, html):
        body = html.encode()
        self.send_response(200); self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
    def _ok(self):
        body = ("@0x%02X" % ((aState | bState) & 0xFF)).encode()
        self.send_response(200); self.send_header("Content-Type", "text/plain"); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        if u.path == "/":
            self._html(CONTROL_HTML)
        elif u.path == "/grid":
            self._html(GRID_HTML)
        elif u.path == "/set":
            try: set_ch(q.get("ch", ["b"])[0], int(q.get("v", ["0"])[0]))
            except Exception as e: print("set err", e)
            self._ok()
        elif u.path == "/seq":
            try: seq_ch(q.get("ch", ["b"])[0], [int(x) for x in q.get("v", [""])[0].split(",") if x != ""])
            except Exception as e: print("seq err", e)
            self._ok()
        elif u.path == "/stop":
            stop_all(); self._ok()
        elif u.path == "/send":
            try: raw_send(int(q.get("v", ["0"])[0]))
            except Exception as e: print("send err", e)
            self._ok()
        elif u.path == "/channel":
            try: set_channel(int(q.get("n", ["1"])[0]))
            except Exception as e: print("chan err", e)
            self._ok()
        else:
            self.send_response(404); self.end_headers()

if __name__ == "__main__":
    serial_open()
    srv = ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), H)
    print(f"controls: http://localhost:{HTTP_PORT}/   grid: http://localhost:{HTTP_PORT}/grid")
    srv.serve_forever()
