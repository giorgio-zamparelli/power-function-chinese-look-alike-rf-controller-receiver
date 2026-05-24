#!/usr/bin/env python3
"""
lego-rf control panel — local web UI to drive the cloned LEGO-PF receiver.

The Arduino runs arduino/lego_tx (XN297 transmitter, serial-settable payload).
Server holds the serial port open (Uno resets only once) and serves two pages:
    /        control panel (direction + incremental speed)
    /grid    raw 0-255 payload explorer

DECODED PROTOCOL — payload byte:
    bits 0-1 = direction: 01 forward/CW, 10 reverse/CCW, 11 brake/stop, 00 float
    bits 2-3 = quadrature encoder channels A,B (the wheel) -> speed is INCREMENTAL
    CW notch +1: 5 then 9   CCW notch +1: 6 then 10
    start CW = 1, start CCW = 2, stop/brake = 3   (Gray CW order: 1,5,13,9)

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

def _w(value: int):
    value = max(0, min(255, int(value)))
    with ser_lock:
        os.write(ser_fd, (str(value) + "\n").encode())

def serial_send(value: int):
    _w(value); print("payload =", value)

def serial_seq(vals, gap=0.06):
    for v in vals:
        _w(v); time.sleep(gap)
    print("seq =", vals)

STYLE = """
 :root{--bg:#0f1115;--fg:#e8eaed;--mut:#8b909a;--grn:#2ecc71;--blu:#3aa0ff;--red:#ff4d4f;--ylw:#f5c451}
 *{box-sizing:border-box;-webkit-user-select:none;user-select:none}
 body{margin:0;font:16px/1.4 -apple-system,system-ui,sans-serif;background:var(--bg);color:var(--fg);padding:18px;max-width:620px;margin:auto}
 a{color:var(--blu);text-decoration:none;font-size:13px}
 h1{font-size:19px;font-weight:600;margin:8px 0 2px}.sub{color:var(--mut);font-size:13px;margin-bottom:16px}
 .state{text-align:center;font-size:15px;color:var(--mut);margin-bottom:12px}.state b{color:var(--fg);font-size:18px}
 button{color:#fff;border:0;border-radius:13px;cursor:pointer;touch-action:manipulation;font:700 16px system-ui}
 button:active{transform:scale(.96)}
 h2{font-size:14px;color:var(--mut);text-transform:uppercase;letter-spacing:.05em;margin:22px 0 8px}
"""

CONTROL_HTML = """<!doctype html><html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>LEGO control</title>
<style>%s
 .spd{display:grid;grid-template-columns:1fr 1fr;gap:12px}.spd button{padding:34px 0;font-size:18px}
 .fwd{background:var(--grn)}.rev{background:var(--blu)}
 .stoprow{margin-top:12px}.stop{background:var(--red);width:100%%;padding:22px 0;font-size:19px}
 .meter{text-align:center;font:700 22px ui-monospace,monospace;margin:6px 0 14px;color:var(--ylw)}
 .hint{color:var(--mut);font-size:13px;text-align:center;margin-top:14px}
 .nav{text-align:right}
</style></head><body>
<div class=nav><a href="/grid">number grid →</a></div>
<h1>LEGO motor control</h1>
<div class=sub>cloned Power Functions receiver · channel 1</div>

<div class=meter>speed: <span id=lvl>0 (stopped)</span></div>
<div class=spd>
 <button class=fwd id=cw>▲ faster CW</button>
 <button class=rev id=ccw>▼ faster CCW</button>
</div>
<div class=stoprow><button class=stop id=stop>■ STOP / BRAKE</button></div>
<div class=hint>Tap <b>faster CW</b> to speed up clockwise. Tap <b>faster CCW</b> to slow it / reverse.<br>
(CW wheel = 1 then 5→9 · CCW wheel = 2 then 6→10 · tapping the other side switches wheels.)</div>

<script>
 const $=id=>document.getElementById(id);let ctx=0,lvl=0;
 function disp(){$('lvl').textContent = lvl>0?('CW · '+lvl):lvl<0?('CCW · '+(-lvl)):'0 (stopped)';}
 function cw(){ if(ctx!==1){fetch('/send?v=1').catch(()=>{});ctx=1;} fetch('/seq?v=5,9').catch(()=>{}); lvl++; disp(); }
 function ccw(){ if(ctx!==2){fetch('/send?v=2').catch(()=>{});ctx=2;} fetch('/seq?v=6,10').catch(()=>{}); lvl--; disp(); }
 function stop(){ fetch('/send?v=3').catch(()=>{}); ctx=0; lvl=0; disp(); }
 $('cw').onclick=cw;$('ccw').onclick=ccw;$('stop').onclick=stop;
</script></body></html>""" % STYLE

GRID_HTML = """<!doctype html><html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>payload grid</title>
<style>%s
 .read{text-align:center;font:600 15px ui-monospace,monospace;margin:8px 0}.read b{font-size:24px;color:var(--ylw)}.read .bin{color:var(--blu)}
 .legend{font-size:12px;color:var(--mut);text-align:center;margin-bottom:8px}
 .legend i{font-style:normal;padding:2px 6px;border-radius:5px;color:#0b0d10;font-weight:700}
 .grid{display:grid;grid-template-columns:repeat(16,1fr);gap:3px}
 .grid button{font:600 10px ui-monospace,monospace;padding:8px 0;border-radius:5px;color:#0b0d10}
 .c1{background:#2ecc71}.c2{background:#3aa0ff}.c3{background:#ff6b6d}.c0{background:#454b57;color:#9aa0aa}
 .stop{background:var(--red);width:100%%;padding:16px 0;margin-top:10px;font-size:17px}
 #log{background:#000;color:#7CFC8A;font:12px/1.5 ui-monospace,monospace;border-radius:10px;padding:8px;height:74px;overflow:auto;margin-top:10px;white-space:pre-wrap}
 .nav{text-align:left}
</style></head><body>
<div class=nav><a href="/">← controls</a></div>
<h1>raw payload grid</h1>
<div class=sub>click any value 0–255 · watch the motor</div>
<div class=read>value <b id=ev>0</b> <span id=eh>0x00</span> <span class=bin id=eb>0000&nbsp;0000</span></div>
<div class=legend>low bits: <i class=c1>01 fwd</i> <i class=c2>10 rev</i> <i class=c3>11 stop</i> <i class=c0>00 float</i></div>
<div class=grid id=grid></div>
<button class=stop id=stop>■ STOP / BRAKE (3)</button>
<div id=log></div>
<script>
 const $=id=>document.getElementById(id);
 function bin(v){return v.toString(2).padStart(8,'0').replace(/(....)(....)/,'$1&nbsp;$2');}
 function show(v){$('ev').textContent=v;$('eh').textContent='0x'+v.toString(16).padStart(2,'0').toUpperCase();$('eb').innerHTML=bin(v);}
 function send(v){fetch('/send?v='+v).catch(()=>{});show(v);const l=$('log');l.textContent+=v+' ';l.scrollTop=l.scrollHeight;}
 const g=$('grid');for(let v=0;v<256;v++){const b=document.createElement('button');b.textContent=v;b.className='c'+(v&3);b.onclick=()=>send(v);g.appendChild(b);}
 $('stop').onclick=()=>send(3);
</script></body></html>""" % STYLE

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _html(self, html):
        body = html.encode()
        self.send_response(200); self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/":
            self._html(CONTROL_HTML)
        elif u.path == "/grid":
            self._html(GRID_HTML)
        elif u.path == "/send":
            q = parse_qs(u.query)
            try: serial_send(int(q.get("v", ["0"])[0]))
            except Exception as e: print("send err", e)
            self.send_response(200); self.end_headers(); self.wfile.write(b"ok")
        elif u.path == "/seq":
            q = parse_qs(u.query)
            try: serial_seq([int(x) for x in q.get("v", [""])[0].split(",") if x != ""])
            except Exception as e: print("seq err", e)
            self.send_response(200); self.end_headers(); self.wfile.write(b"ok")
        else:
            self.send_response(404); self.end_headers()

if __name__ == "__main__":
    serial_open()
    srv = ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), H)
    print(f"controls: http://localhost:{HTTP_PORT}/   grid: http://localhost:{HTTP_PORT}/grid")
    srv.serve_forever()
