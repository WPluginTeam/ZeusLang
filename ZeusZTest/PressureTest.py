
import socket

for i in range(100000):
    if i % 1000 == 0:
        print(f"Progress: {i}/100000")
        import time
        time.sleep(0.1)
        
    try:
        a = socket.socket()
        a.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        a.settimeout(5)
        a.connect(('localhost', 8081))
        a.send(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
        a.recv(1024)
        a.close()
    except Exception as e:
        print(f"Error at {i}: {e}")
        time.sleep(2)

print("Done!")