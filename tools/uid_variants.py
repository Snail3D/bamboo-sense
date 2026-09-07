import ssl, time, json, base64
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
import paho.mqtt.client as mqttc

SER="22E8AJ612200029"; IP="192.168.1.81"
KEY=serialization.load_pem_private_key(open("/tmp/bambu-api/mykey.pem","rb").read(),None)
CERT_ID="5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a"+"CN=GLOF3813734089.bambulab.com"

def send(c, signed_bytes, header):
    # wire = signed bytes with header spliced in as last top-level key
    assert signed_bytes.endswith(b'}')
    wire = signed_bytes[:-1] + b',"header":' + json.dumps(header,separators=(',',':')).encode() + b'}'
    c.publish(f"device/{SER}/request", wire)

def envelope(cmd_obj, uid=None, num_seq=False):
    if num_seq: cmd_obj=dict(cmd_obj); cmd_obj["sequence_id"]=int(cmd_obj["sequence_id"])
    if uid is not None:
        inner = '{"user_id":"%s","print":%s}' % (uid, json.dumps(cmd_obj,separators=(',',':')))
    else:
        inner = '{"print":%s}' % json.dumps(cmd_obj,separators=(',',':'))
    return inner.encode()

acks=[]
def on_msg(cc,u,m):
    try:
        d=json.loads(m.payload); p=d.get("print")
        if p and p.get("command")=="print_speed" and int(str(p.get("sequence_id",0)))>=6001:
            acks.append(p.get("sequence_id")); print("ACK:", {k:p.get(k) for k in ("result","err_code","sequence_id")})
    except: pass

c=mqttc.Client(client_id="uidvar", protocol=mqttc.MQTTv311)
ctx=ssl.create_default_context(); ctx.check_hostname=False; ctx.verify_mode=ssl.CERT_NONE
ctx.load_cert_chain("/tmp/bambu-api/mycert.pem","/tmp/bambu-api/mykey.pem")
c.tls_set_context(ctx); c.username_pw_set("bblp","ac555123"); c.on_message=on_msg
c.connect(IP,8883,keepalive=30); c.loop_start(); c.subscribe(f"device/{SER}/report",0)
time.sleep(2)

tests=[
 ("uid-first",   {"sequence_id":"6001","command":"print_speed","param":"2"}, "1089663942", False),
 ("uid-last",    {"sequence_id":"6002","command":"print_speed","param":"2"}, "1089663942X", False),  # placeholder replaced below
]
# uid-last: build manually
cmd2={"sequence_id":"6002","command":"print_speed","param":"2"}
inner2=('{"print":'+json.dumps(cmd2,separators=(',',':'))+',"user_id":"1089663942"}').encode()
cmd3={"sequence_id":6003,"command":"print_speed","param":"2"}
inner3=('{"print":'+json.dumps(cmd3,separators=(',',':'))+',"user_id":"1089663942"}').encode()

for name, inner in [("uid-first", envelope({"sequence_id":"6001","command":"print_speed","param":"2"},"1089663942")),
                    ("uid-last", inner2),
                    ("uid-last-numseq", inner3)]:
    sig=base64.b64encode(KEY.sign(inner, padding.PKCS1v15(), hashes.SHA256())).decode()
    hdr={"sign_ver":"v1.0","sign_alg":"RSA_SHA256","sign_string":sig,"cert_id":CERT_ID,"payload_len":len(inner)}
    send(c, inner, hdr); time.sleep(3.5)
time.sleep(6)
print("DONE")
