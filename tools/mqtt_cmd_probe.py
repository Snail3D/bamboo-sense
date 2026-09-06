import ssl, time, json
import paho.mqtt.client as mqttc
SER="22E8AJ612200029"; IP="192.168.1.81"
acks=[]
def on_msg(c,u,m):
    try:
        d=json.loads(m.payload)
        p=d.get("print",{})
        if p.get("command") and p.get("sequence_id")=="9001":
            acks.append({k:p.get(k) for k in ("command","result","reason","err_code","msg","sequence_id")})
            print("ACK:", acks[-1])
    except Exception: pass
c=mqttc.Client(client_id="cmdprobe", protocol=mqttc.MQTTv311)
c.tls_set(cert_reqs=ssl.CERT_NONE); c.tls_insecure_set(True)
c.username_pw_set("bblp","ac555123"); c.on_message=on_msg
c.connect(IP,8883,keepalive=30); c.loop_start()
c.subscribe(f"device/{SER}/report",0)
time.sleep(2)
cmd={"print":{"sequence_id":"9001","command":"print_speed","param":"2"}}
info=c.publish(f"device/{SER}/request", json.dumps(cmd))
print("published:", info.rc)
time.sleep(10)
print("ACKS:", acks if acks else "NONE SEEN")
