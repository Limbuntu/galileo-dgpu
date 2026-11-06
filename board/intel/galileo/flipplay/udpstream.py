import socket, subprocess, sys, time, struct
ip, port, dur, src = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
W, H, PAY = 640, 480, 1400
fsz = W*H*4
cmd = ["ffmpeg","-v","error","-t",dur,"-i",src,"-vf",f"scale={W}:{H}","-pix_fmt","bgra","-f","rawvideo","-"]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
t0 = time.time(); sent = 0; frame = 0; rate = 6.0*1048576
while True:
    buf = p.stdout.read(fsz)
    if len(buf) < fsz: break
    total = (fsz + PAY - 1)//PAY
    for idx in range(total):
        chunk = buf[idx*PAY:(idx+1)*PAY]
        s.sendto(struct.pack('<IIII', frame, idx, total, len(chunk)) + chunk, (ip, port))
    frame += 1; sent += fsz
    target = t0 + sent/rate
    d = target - time.time()
    if d > 0: time.sleep(d)
    if frame % 100 == 0:
        print(f"\r{frame} frames {sent/1048576:.0f} MB {sent/1048576/(time.time()-t0):.1f} MB/s", end="", flush=True)
print(f"\ndone {frame} frames {sent/1048576:.0f} MB in {time.time()-t0:.0f}s")
