import os
import ssl, time, sys
import paho.mqtt.client as mqttc
SER=os.environ.get("BAMBU_SERIAL","")
got=[]
def on_msg(c,u,m):
    got.append(len(m.payload))
    if len(got)<=2: print("MSG", m.topic[:50], len(m.payload), m.payload[:120])
def on_sub(c,u,mid,rc,*a): print("SUBSCRIBED rc=",rc)
def on_conn(c,u,fl,rc,*a): print("CONNECTED rc=",rc)
def on_disc(c,u,rc,*a): print("DISCONNECTED rc=",rc,"after",len(got),"msgs")
c=mqttc.Client(client_id="macprobe-"+str(time.time())[-5:], protocol=mqttc.MQTTv311)
c.tls_set(cert_reqs=ssl.CERT_NONE); c.tls_insecure_set(True)
c.username_pw_set("bblp",os.environ.get("BAMBU_ACCESS_CODE",""))
c.on_message=on_msg; c.on_subscribe=on_sub; c.on_connect=on_conn; c.on_disconnect=on_disc
c.connect(os.environ.get("BAMBU_IP",""),8883,keepalive=30)
c.loop_start()
c.subscribe(f"device/{SER}/report",0)
t0=time.time()
while time.time()-t0<25: time.sleep(0.5)
print("TOTAL MSGS:",len(got))
