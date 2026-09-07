#!/usr/bin/env python3
"""Bambu LAN file tunnel client (port 6000) — LIST_INFO / FILE_UPLOAD / FILE_DEL.
Auth: 64-byte packet: 'bblp\\0\\0\\0\\0' + access code zero-padded to 32 + 24 zero bytes.
Framing: 16-byte LE header (payload_size, sequence, cmdtype, mtype) + JSON body."""
import socket, ssl, struct, json, sys, time

AUTH_PKT = b"bblp\x00\x00\x00\x00" + b"cf972ede".ljust(32, b"\x00") + b"\x00" * 24

def frame(seq, cmdtype, mtype, body: bytes) -> bytes:
    return struct.pack("<IIII", len(body), seq, cmdtype, mtype) + body

def read_frame(sock):
    hdr = b""
    while len(hdr) < 16:
        c = sock.recv(16 - len(hdr))
        if not c: raise ConnectionError("eof")
        hdr += c
    size, seq, cmdtype, mtype = struct.unpack("<IIII", hdr)
    body = b""
    while len(body) < size:
        c = sock.recv(size - len(body))
        if not c: raise ConnectionError("eof mid-body")
        body += c
    return seq, cmdtype, mtype, body

def connect(ip, code, timeout=15):
    raw = socket.create_connection((ip, 6000), timeout=timeout)
    ctx = ssl.create_default_context(); ctx.check_hostname=False; ctx.verify_mode=ssl.CERT_NONE
    s = ctx.wrap_socket(raw)
    s.sendall(AUTH_PKT)          # docs: no reply expected
    return s

def list_info(s, path="/", seq=1):
    body = json.dumps({"cmdtype":1,"sequence":seq,"req":{"path":path}}).encode()
    s.sendall(frame(seq, 1, 0x3001, body))
    # collect replies until one carries our sequence with reply data (or 3s silence)
    out=[]
    s.settimeout(4)
    try:
        while True:
            rseq, cmdtype, mtype, b = read_frame(s)
            try: d=json.loads(b)
            except Exception: d={"raw":b[:80].hex()}
            out.append((rseq,mtype,d))
            if rseq==seq and mtype==0x3001: break
    except (socket.timeout, TimeoutError):
        pass
    return out

def upload_file(s, local_path, remote_path, chunk=65536, seq0=10):
    import os
    total=os.path.getsize(local_path)
    # FILE_UPLOAD start frame (mtype 0x3003 per docs)
    body=json.dumps({"cmdtype":5,"sequence":seq0,"req":{"path":remote_path,"file_size":total,"chunk_size":chunk}}).encode()
    s.sendall(frame(seq0,5,0x3003,body))
    with open(local_path,"rb") as f:
        chunk_seq=seq0+1; sent=0
        while True:
            data=f.read(chunk)
            if not data: break
            s.sendall(frame(chunk_seq,5,0x3003,data))
            chunk_seq+=1; sent+=len(data)
            print(f"\r  {sent}/{total}", end="", flush=True)
    print()
    # wait for completion reply
    s.settimeout(20)
    try:
        while True:
            rseq,cmdtype,mtype,b=read_frame(s)
            d=json.loads(b)
            print("REPLY:", json.dumps(d)[:200])
            if rseq==seq0: break
    except (socket.timeout, TimeoutError):
        print("(no completion reply within 20s)")

def file_del(s, remote_path, seq=90):
    body=json.dumps({"cmdtype":3,"sequence":seq,"req":{"path":remote_path}}).encode()
    s.sendall(frame(seq,3,0x3001,body))
    s.settimeout(8)
    try:
        rseq,cmdtype,mtype,b=read_frame(s)
        return json.loads(b)
    except (socket.timeout, TimeoutError):
        return {"timeout":True}

if __name__=="__main__":
    IP="192.168.1.154"
    s=connect(IP,"cf972ede")
    print("TLS+auth OK to",IP)
    print("== LIST_INFO / ==")
    for r in list_info(s,"/"): print(" ",r[1],json.dumps(r[2])[:300])
    print("== LIST_INFO /cache ==")
    for r in list_info(s,"/cache"): print(" ",r[1],json.dumps(r[2])[:300])
