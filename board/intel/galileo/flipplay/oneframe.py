import socket, struct, time, sys
ip, port = sys.argv[1], int(sys.argv[2])
W,H,PAY = 640,480,1400
fsz = W*H*4
data = bytes([i % 251 for i in range(fsz)])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
total = (fsz+PAY-1)//PAY
for idx in range(total):
    chunk = data[idx*PAY:(idx+1)*PAY]
    s.sendto(struct.pack('<IIII', 0, idx, total, len(chunk)) + chunk, (ip, port))
    time.sleep(0.001)
print("sent 1 frame", total, "packets")
