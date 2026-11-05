import socket, subprocess, sys, time, os
ip, port, dur, src = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
W, H = 640, 480
cmd = ["ffmpeg","-v","error","-t",dur,"-i",src,"-vf",f"scale={W}:{H}",
       "-pix_fmt","bgra","-f","rawvideo","-"]
s = socket.create_connection((ip, port))
p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
t0 = time.time(); sent = 0; last = 0
while True:
    b = p.stdout.read(1 << 20)
    if not b: break
    s.sendall(b); sent += len(b)
    now = time.time()
    if now - last > 1:
        print(f"\r{sent/1048576:7.0f} MB  {sent/1048576/(now-t0):5.1f} MB/s", end="", flush=True)
        last = now
s.shutdown(socket.SHUT_WR)
p.wait()
dt = time.time() - t0
print(f"\ndone {sent/1048576:.0f} MB in {dt:.1f}s = {sent/1048576/dt:.1f} MB/s")
