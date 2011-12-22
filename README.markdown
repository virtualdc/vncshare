# Simple VNC proxy server.

Allows to connect from some machine in the internet to listening viewers in
local network. Works with UltraVNC single click executable.

Sample settings in the UltraVNC's helpdesk.txt:

```[DIRECT]

[HOST]
<Your name>
-id 12345 -connect <your host>:<your port> -noregistry

[ENTERCODE]
Enter code:
```

Work logic:

1. User calls to operator and decribes problem
2. Operator tells the his number to client
3. Client launches "single click" and enters number
4. VNC connects to the vncshare and sends number
5. vncshare looks up number in the operators.conf
6. If number found it connects to the specified host & port
7. If connection successful it proxies data between server & client

