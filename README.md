PIC-Websockets
===================

A WebSocket Server for MicroChip PIC MCUs, built on top of the MicroChip TCP/IP Stack.

How to add this into your own project
-------------------------------------

You'll need to copy a couple of files into your project and implement two callbacks, that's all... :

0. Make sure you have a functional HTTP server on your platform, using the MicroChip TCP/IP stack. (So if you don't know how to do that, learn that first)
1. Replace the files HTTP.c, HTTP.h and TCPIP.h from the TCP/IP Stack with the ones in this repo (but diff them first to avoid any surprises).
2. Copy WebSocket.c and WebSocket.h to the same directory where HTTP.c and HTTP.h live.
3. Check and/or adjust the paths used in the `#include` directives inside all the above files.
3. Compare TCPIPConfig.h from this repo with your version of TCPIPConfig.h and make these necessary changes in your own version:
  - add `#define STACK_USE_WEBSOCKETS`
  - adjust the TX and RX buffer sizes inside the `TCP_CONFIGURATION` block to be able to hold your maximum WebSocket frame size (equals maximum payload size + 4 frame header bytes).
  - add or adjust the `HTTP_MAX_DATA_LEN` value to be same or larger than the largest websocket frame payload you plan to send out (no need to count the frame header).
3. Look into CustomHTTPApp.c in this repo and copy the code inside the `#if defined(HTTP_USE_WEBSOCKETS)` block to an appropriate location in your own project.
4. Compile, program...
5. adjust the `websocketUri` variable in websocket_test.html to point to your board
6. open websocket_test.html in a browser and enjoy...
7. now go implement your own logic inside the functions you copied in step 3.

API
---

The API consists of 4 functions, 2 of them being callbacks that users must implement themselves:

`int WebSocketSendPayload(WS_OPCODE opcode, WORD length)`  
  Sends length bytes of data in the curHTTP.data buffer to the client with given opcode (`WS_OPCODE_TEXT` or `WS_OPCODE_BINARY`)

`int WebSocketClose(WORD status, BYTE *reason, WORD length)`  
  Closes the websocket with given status code and reason

`extern void WebSocketIncomingDataCallback(WS_OPCODE opcode, BYTE *payloadBuffer, WORD payloadLength)`  
  This callback is called after a websocket frame is received. The payloadBuffer contains the payload of the websocket frame. Important: do not send any tcp packet out from this callback (use it to set state instead and do the sending in `WebSocketTaskCallback`).

`extern int WebSocketTaskCallback()`  
  This callback gets called from inside StackApplications() (well actually it is inside HTTPProcess(), which gets called by HTTPServer(), which in turn gets called by StackApplications()). So this gets called on every iteration. Use it to keep state and send frames with WebSocketSendPayload (after first storing the data to send in curHTTP.data).

Unsupported or missing features
-------------------------------

- sending and receiving of fragmented frames is not supported yet
- sending and receiving continuation frames is not supported
- receival of extended payloads is not supported (maximum payload on an incoming WebSocket frame should not exceed 125 bytes)
- sending of 64bit extended payload length is not supported, maximum outgoing payload length is 32767 bytes (so 16bit extended payload _is_ supported on outgoing frames)
- extension frames are not supported
- there is no check for UTF8 validity of incoming or outgoing text frames
- be aware that PICs are little endian. If you send frames with binary data containing shorts or longs, your shorts or longs will be sent in little endian order so you need to take that into account when parsing the buffer on the other side (or you need to swap bytes into big-endian order before sending them).

When an unsupported websocket frame is received, the code closes the connection with an appropriate status code.

LICENSE
-------

License? What license? Do with this code (or better, with the changes that I made to the pre-existing code) whatever you want.
All source code files except WebSocket.c and WebSocket.h are copyright MicroChip Technology and have a header in the file with license information.

