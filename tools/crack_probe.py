import ssl, time, json, base64
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
import paho.mqtt.client as mqttc

SER="22E8AJ612200029"; IP="192.168.1.81"
CERT=open("/tmp/bambu-api/blk2.pem").read()
KEY=open("/tmp/bambu-api/blk1.pem","rb").read()
CHAIN=CERT+open("/tmp/bambu-api/blk3.pem").read()+open("/tmp/bambu-api/blk4.pem").read()
CRL=open("/tmp/bambu-api/dylib/crl.pem").read()
leaf=x509.load_pem_x509_certificate(CERT.encode())
cert_id = format(leaf.serial_number,'x').zfill(32) + "CN=GLOF3813734089-524a37c80000"
key=serialization.load_pem_private_key(KEY,None)

def sign_env(cmd_obj, top="print"):
    to_sign=('{"%s":'%top + json.dumps(cmd_obj,sort_keys=True,separators=(",",":")) + '}').encode()
    sig=base64.b64encode(key.sign(to_sign, padding.PKCS1v15(), hashes.SHA256())).decode()
    env={top:cmd_obj, "header":{"sign_ver":"v1.0","sign_alg":"RSA_SHA256","sign_string":sig,"cert_id":cert_id,"payload_len":len(to_sign)}}
    return json.dumps(env)

sec_replies=[]; acks=[]
def on_msg(c,u,m):
    try:
        d=json.loads(m.payload)
        s=d.get("security"); p=d.get("print")
        if s: sec_replies.append(s); print("SEC:", json.dumps(s)[:400])
        if p and p.get("sequence_id")=="9001":
            acks.append({k:p.get(k) for k in ("command","result","reason","err_code")}); print("ACK:",acks[-1])
    except: pass

c=mqttc.Client(client_id="crack1", protocol=mqttc.MQTTv311)
ctx=ssl.create_default_context(); ctx.check_hostname=False; ctx.verify_mode=ssl.CERT_NONE
ctx.load_cert_chain("/tmp/bambu-api/blk2.pem","/tmp/bambu-api/blk1.pem")
c.tls_set_context(ctx); c.username_pw_set("bblp","ac555123"); c.on_message=on_msg
c.connect(IP,8883,keepalive=30); c.loop_start(); c.subscribe(f"device/{SER}/report",0)
time.sleep(2)
t=int(time.time()*1000)
# 1. install
inst={"security":{"sequence_id":"9002","command":"app_cert_install","timestamp":t,"type":"app","app_cert":CHAIN,"crl":CRL}}
c.publish(f"device/{SER}/request", json.dumps(inst))
time.sleep(5)
# 2. list again
c.publish(f"device/{SER}/request", json.dumps({"security":{"sequence_id":"9003","command":"app_cert_list","timestamp":int(time.time()*1000),"type":"app"}}))
time.sleep(4)
# 3. signed command
cmd={"sequence_id":"9001","command":"print_speed","param":"2"}
to_sign=('{"print":'+json.dumps(cmd,sort_keys=True,separators=(",",":"))+'}').encode()
sig=base64.b64encode(key.sign(to_sign, padding.PKCS1v15(), hashes.SHA256())).decode()
c.publish(f"device/{SER}/request", json.dumps({"print":cmd,"header":{"sign_ver":"v1.0","sign_alg":"RSA_SHA256","sign_string":sig,"cert_id":cert_id,"payload_len":len(to_sign)}}))
time.sleep(8)
print("FINAL ACKS:", acks if acks else "NONE")
