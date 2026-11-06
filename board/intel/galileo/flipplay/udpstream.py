import socket, subprocess, sys, time, struct
ip, port, dur, src = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
rate_mb = float(sys.argv[5]) if len(sys.argv) > 5 else 6.0
W, H, PAY = 640, 480, 1400
fsz = W * H * 4
total = (fsz + PAY - 1) // PAY
cmd = ["ffmpeg","-v","error","-t",dur,"-i",src,"-vf",f"scale={W}:{H}",
       "-pix_fmt","bgra","-f","rawvideo","-"]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
s.bind(("0.0.0.0", 0))
s.setblocking(False)
p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
rate = rate_mb * 1048576
PKT_T = (PAY + 16) / rate
keep = {}                     # frame -> buffer, for NACK retransmits
resent = 0

def pump(until):
    global resent
    first = True
    while first or time.time() < until:
        first = False
        try:
            data, addr = s.recvfrom(2048)
        except BlockingIOError:
            if time.time() >= until:
                break
            time.sleep(0.002)
            continue
        if len(data) < 12 or data[:4] != b"NACK":
            continue
        fr, cnt = struct.unpack("<II", data[4:12])
        buf = keep.get(fr)
        if buf is None or cnt == 0 or cnt > 900:
            continue
        idxs = struct.unpack("<%dH" % cnt, data[12:12 + 2 * cnt])
        for i in idxs:
            chunk = buf[i * PAY:(i + 1) * PAY]
            s.sendto(struct.pack("<IIII", fr, i, total, len(chunk)) + chunk, addr)
            resent += 1

t0 = time.time(); sent = 0; frame = 0; pkts = 0
while True:
    buf = p.stdout.read(fsz)
    if len(buf) < fsz:
        break
    keep[frame] = buf
    for idx in range(total):
        chunk = buf[idx * PAY:(idx + 1) * PAY]
        s.sendto(struct.pack("<IIII", frame, idx, total, len(chunk)) + chunk, (ip, port))
        pkts += 1
        target = t0 + pkts * PKT_T
        d = target - time.time()
        if d > 0:
            time.sleep(d)
        if pkts % 64 == 0:
            pump(time.time())    # answer NACKs without stalling the stream
    frame += 1; sent += fsz
    if len(keep) > 4:
        keep.pop(min(keep))
    nxt = t0 + sent / rate
    pump(max(time.time(), nxt - 0.002))
    if frame % 100 == 0:
        print(f"\r{frame} frames {sent/1048576:.0f} MB {sent/1048576/(time.time()-t0):.1f} MB/s resent={resent}", end="", flush=True)
print(f"\ndone {frame} frames {sent/1048576:.0f} MB in {time.time()-t0:.0f}s, resent={resent}")
