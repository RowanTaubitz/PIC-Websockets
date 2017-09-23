#ifndef __WEBSOCKET_H
#define __WEBSOCKET_H

#include "tcpip/src/common/sys_fs_wrapper.h"

#include "tcpip/src/http_private.h"

#define WS_KEY_LENGTH 24

typedef enum {
    WS_OPCODE_CONT = 0x00,
    WS_OPCODE_TEXT = 0x01,
    WS_OPCODE_BINARY = 0x02,
    WS_OPCODE_CLOSE = 0x08,
    WS_OPCODE_PING = 0x09,
    WS_OPCODE_PONG = 0x0A
} WS_OPCODE;

int TCPIP_WS_doHandShake(HTTP_CONN* pHttpCon);
SM_HTTP2 TCPIP_WS_Process(HTTP_CONN* pHttpCon);

/*****************************************************************************
 Function:
 int TCPIP_WS_SendPayload(uint8_t *buffer, WORD length)

 Description:
 Creates and sends a websocket frame with the given payload.
 
 Precondition:
 An open and active websocket connection is required, the payload data should
 have been copied into curHTTP.data
 
 Parameters:
 opcode        - 0x01 for UTF8 text data or 0x02 for binary data
 payloadLength - yes, indeed, the length of the payload in bytes
 
 Return Values:
 0 on success, 1 if the message could not be sent (possibly due to lack
 of space in the TX buffer).
 
 Remarks:
 payloadLength should be smaller or same as HTTP_MAX_DATA_LEN
 ***************************************************************************/
int TCPIP_WS_SendPayload(HTTP_CONN* pHttpCon, WS_OPCODE opcode, uint16_t length);

/*****************************************************************************
 Function:
 int TCPIP_WS_Close(WORD status, uint8_t *reason, WORD length)

 Description:
 Sends a close connection frame with given reason and status and initiates
 the closing of the underlying HTTP/TCP connection.
 
 Precondition:
 An open and active websocket connection is required.
 
 Parameters:
 status - a status code. Pass 0 if no status code should be sent (only when
          no reason is given)
 reason - buffer containing the closing reason message. Pass NULL if no 
          reason should be sent
 length - length of the reason buffer. Pass 0 if no reason should be sent
 
 Return Values:
 0 on success, 1 if the message could not be sent (possibly due to lack
 of space in the TX buffer).
 
 Remarks:
 None
 ***************************************************************************/
int TCPIP_WS_Close(HTTP_CONN* pHttpCon, uint16_t status, uint8_t *reason, uint16_t length);

/****************************************************************************
 Section:
 User-Implemented Callback Function Prototypes
 ***************************************************************************/

/*****************************************************************************
 Function:
 void TCPIP_WS_IncomingDataCallback(uint8_t opcode, uint8_t *payloadBuffer, WORD payloadLength)

 Summary:
 Processes an Incoming WebSocket Data Frame
 
 Description:
 This function is implemented by the application developer in
 CustomHTTPApp.c.  Its purpose is to parse the payload received from
 a WebSocket Frame and perform any
 application-specific tasks in response to these inputs. 
  
 Precondition:
 None
 
 Parameters:
 opcode        - 0x01 for UTF8 text data or 0x02 for binary data
 payloadBuffer - buffer containing the frame's payload
 payloadLength - length of the buffer
 
 Return Values:
 None
 
 Remarks:
 This function is only called if data is received over an active WebSocket.
 This function may NOT write to the TCP buffer.
 The payload may be considered complete and finished.
 ***************************************************************************/
extern void TCPIP_WS_IncomingDataCallback(HTTP_CONN* pHttpCon, WS_OPCODE opcode, uint8_t *payloadBuffer, uint16_t payloadLength);

/*****************************************************************************
 Function:
 int TCPIP_WS_TaskCallback()
 
 Summary:
 Main stack task for WebSocket requests.
  
 Description:
 This function is implemented by the application developer in
 CustomHTTPApp.c.  Its purpose is to provide data to be sent to the websocket
 client in a new frame.
 
 Precondition:
 None
 
 Parameters:
 None
 
 Return Values:
 0 to keep the connection open or 1 to close the connection.
 
 Remarks:
 This function is not called while a frame is being received.
 Use WebSocketSendPayload() for doing the actual sending of the frames
 
 ***************************************************************************/
extern int TCPIP_WS_TaskCallback(HTTP_CONN* pHttpCon);

#endif
