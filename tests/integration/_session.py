import socket, struct, sys
port = int(sys.argv[1])
cmds = sys.argv[2].split("|")
s = socket.create_connection(("127.0.0.1", port), timeout=5)
def send(m):
    s.sendall(struct.pack("!I", len(m)) + m.encode())
    h = s.recv(4)
    if len(h) < 4: return "<closed>"
    n = struct.unpack("!I", h)[0]; b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d: break
        b += d
    return b.decode(errors="replace").strip()
tok = ""
for c in cmds:
    c = c.replace("TOKEN", tok)
    r = send(c)
    if c.startswith("login") and r.startswith("OK "):
        tok = r[3:]
    print(r.replace("\n", " ")[:70])
