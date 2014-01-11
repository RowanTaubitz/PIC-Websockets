// Include all headers for any enabled TCPIP Stack functions
#include "TCPIP Stack/TCPIP.h"

// bit masks for frame header bits
#define OPCODE_MASK 0x0F
#define HEADER_MASK 0xF0
#define FIN_MASK    0x80
#define MASK_MASK   0x80

// frame field lengths
#define MAX_PAYLOAD_LENGTH   204 // 200 bytes payload + 4 bytes mask
#define EXT_PAYLOAD_BOUNDARY 126
#define MASK_WIDTH             4
#define HEADER_LENGTH          2
#define EXT_PAYLOAD_LENGTH     2
#define STATUS_CODE_LENGTH     2

#define ProtocolErrorClose(skt) TCPPutArray((skt), (BYTE *) "\210\002\003\352", 4); TCPFlush((skt)) // sends a close frame with status code 1002
#define MessageTooBigClose(skt) TCPPutArray((skt), (BYTE *) "\210\002\003\361", 4); TCPFlush((skt)) // sends a close frame with status code 1009
#define UnsupportedClose(skt)   TCPPutArray((skt), (BYTE *) "\210\002\003\363", 4); TCPFlush((skt)) // sends a close frame with status code 1011

static BYTE payloadBuffer[MAX_PAYLOAD_LENGTH];

BYTE HTTPUpgradeHeader[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
BYTE WebSocketGuid[]     = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

int doHandShake(BYTE *key) {
    if (TCPIsPutReady(sktHTTP) < sizeof(HTTPUpgradeHeader) + 43) return 1;
    
    TCP_SOCKET socket = sktHTTP;
    HASH_SUM   hash;
    BYTE       sha1Result[20];
    BYTE       resultBase64[40];
    int        resultLength;
    
    TCPPutArray(socket, (BYTE *)HTTPUpgradeHeader, sizeof(HTTPUpgradeHeader) - 1); // without terminating \0 
    
    SHA1Initialize(&hash);
    HashAddData(&hash, key, WS_KEY_LENGTH);
    HashAddData(&hash, WebSocketGuid, (WORD)sizeof(WebSocketGuid) - 1);
    SHA1Calculate(&hash, sha1Result);
    
    resultLength = Base64Encode(sha1Result, 20, resultBase64, 40);
    TCPPutArray(socket, resultBase64, resultLength);
    TCPPutArray(socket, (BYTE *) "\r\n\r\n", 4);
    
    return 0;
}

int WebSocketSendPayload(WS_OPCODE opcode, WORD length) {
    if (length > HTTP_MAX_DATA_LEN) length = HTTP_MAX_DATA_LEN;
    
    TCP_SOCKET socket      = sktHTTP;
    WORD       txLength    = TCPIsPutReady(socket);
    WORD       frameLength = length + HEADER_LENGTH;
    
    if (length >= EXT_PAYLOAD_BOUNDARY) frameLength += EXT_PAYLOAD_LENGTH;
    
    if (txLength < frameLength) return 1; // not enough space in TX buffer to send frame
    
    TCPPut(socket, FIN_MASK | opcode);
    
    if (length >= EXT_PAYLOAD_BOUNDARY) {
        TCPPut(socket, EXT_PAYLOAD_BOUNDARY);
        // swap length into network byte order
        WORD payloadLength = (length << 8) | (length >> 8);
        TCPPutArray(socket, (BYTE *)&payloadLength, EXT_PAYLOAD_LENGTH);
    } else {
        TCPPut(socket, length);
    }
    
    if (length > 0) TCPPutArray(socket, curHTTP.data, length);
    
    TCPFlush(socket);
    return 0;
}

int WebSocketClose(WORD status, BYTE *reason, WORD length) {
    if (status == 0) {
        length  = 0;
    } else {
        length += STATUS_CODE_LENGTH;
    }
    
    if (reason == NULL)                 length  = status ? STATUS_CODE_LENGTH : 0;
    if (length >= EXT_PAYLOAD_BOUNDARY) length  = EXT_PAYLOAD_BOUNDARY - 1; // truncate reason
    
    WORD txLength    = TCPIsPutReady(sktHTTP);
    WORD frameLength = length + HEADER_LENGTH;
    
    if (txLength < frameLength) return 1; // not enough space in TX buffer to send frame
    
    TCPPut(sktHTTP, FIN_MASK | WS_OPCODE_CLOSE);
    TCPPut(sktHTTP, length);
    
    if (length > 0)                  TCPPutArray(sktHTTP, (BYTE *)&status, STATUS_CODE_LENGTH);
    if (length > STATUS_CODE_LENGTH) TCPPutArray(sktHTTP, reason, length - STATUS_CODE_LENGTH);
    
    TCPFlush(sktHTTP);
    return 0;
}

int WebSocketProcess(int status) {
    TCP_SOCKET socket = sktHTTP;
    WORD txLength     = TCPIsPutReady(socket);
    
    if (txLength < HEADER_LENGTH + EXT_PAYLOAD_LENGTH) return status; // not enough space in TX buffer to even send a control frame

    WORD rxLength = TCPIsGetReady(socket);
    
    if (rxLength >= HEADER_LENGTH) {
        BYTE      frameHeader;
        WORD      payloadLength;
        BYTE      mask[MASK_WIDTH];
        WS_OPCODE opcode;
        WORD      index;

        frameHeader = TCPPeek(socket, 0);
        opcode = frameHeader & OPCODE_MASK;
        
        // check header
        if ((frameHeader & HEADER_MASK) != FIN_MASK) {
            // FIN bit not set or RSV bits set --> unsupported for now
            UnsupportedClose(socket);
            return SM_HTTP_DISCONNECT;
        } 
        
        // check opcodes
        if (opcode != WS_OPCODE_TEXT && opcode != WS_OPCODE_BINARY && (opcode < WS_OPCODE_CLOSE || opcode > WS_OPCODE_PONG)) {
            // only text, binary, close frame, ping and pong frames are supported
            UnsupportedClose(socket);
            return SM_HTTP_DISCONNECT;
        } 
        
        if (opcode >= WS_OPCODE_CLOSE && (frameHeader & FIN_MASK) == 0) {
            // control opcodes are not allowed with fragmented frames
            ProtocolErrorClose(socket);
            return SM_HTTP_DISCONNECT;
        }
        
        // check payload length
        payloadLength = TCPPeek(socket, 1);
        
        // check if mask bit is set.
        if ((payloadLength & MASK_MASK) == 0) {
            ProtocolErrorClose(socket);
            return SM_HTTP_DISCONNECT;
        } else {
            // remove the mask bit
            payloadLength &= ~MASK_MASK;
        }
        
        if (payloadLength > EXT_PAYLOAD_BOUNDARY) {
            // extended payload length not supported yet
            MessageTooBigClose(socket);
            return SM_HTTP_DISCONNECT;
        } 
        
        if (payloadLength == EXT_PAYLOAD_BOUNDARY) {
            if (opcode >= WS_OPCODE_CLOSE) {
                // control opcodes with extended payloads are not allowed
                ProtocolErrorClose(socket);
                return SM_HTTP_DISCONNECT;
            } else if (rxLength < HEADER_LENGTH + EXT_PAYLOAD_LENGTH) {
                return status;
            } else {
                TCPPeekArray(socket, (BYTE *)&payloadLength, EXT_PAYLOAD_LENGTH, HEADER_LENGTH);
                if (payloadLength > MAX_PAYLOAD_LENGTH) {
                    MessageTooBigClose(socket);
                    return SM_HTTP_DISCONNECT;
                }
                
                if (rxLength < payloadLength + MASK_WIDTH - HEADER_LENGTH - EXT_PAYLOAD_LENGTH) return status;
                
                TCPGetArray(socket, payloadBuffer, HEADER_LENGTH + EXT_PAYLOAD_LENGTH); // remove header from TCP RX buffer
            }
        } else {
            if (rxLength < payloadLength + MASK_WIDTH - HEADER_LENGTH) return status;

            // check if there is enough room in the TX buffer to handle control opcodes
            if ((opcode == WS_OPCODE_CLOSE || opcode == WS_OPCODE_PING) && txLength < payloadLength + HEADER_LENGTH) return status;
            
            TCPGetArray(socket, payloadBuffer, HEADER_LENGTH); // remove header from TCP RX buffer
        }
        
        // decode message
        TCPGetArray(socket, mask, MASK_WIDTH);
        
        if (payloadLength > 0) {
            TCPGetArray(socket, payloadBuffer, payloadLength);
            
            // unmask
            for (index = 0; index < payloadLength; index++) {
                payloadBuffer[index] ^= mask[index % MASK_WIDTH];
            }
        }
        
        // handle control frames
        if (opcode == WS_OPCODE_CLOSE) { // connection close
            TCPPut(socket, FIN_MASK | WS_OPCODE_CLOSE);
            // echo the received status code (if available)
            if (payloadLength >= STATUS_CODE_LENGTH) {
                TCPPut(socket, STATUS_CODE_LENGTH);
                TCPPutArray(socket, payloadBuffer, STATUS_CODE_LENGTH);
            } else {
                TCPPut(socket, 0); // zero payload length
            }
            
            TCPFlush(socket);
            return SM_HTTP_DISCONNECT;
        } 
        
        if (opcode == WS_OPCODE_PING) { // ping
            // reply with a pong
            TCPPut(socket, FIN_MASK | WS_OPCODE_PONG);
            TCPPut(socket, payloadLength);
            // echo the received payload (if any)
            if (payloadLength > 0) TCPPutArray(socket, payloadBuffer, payloadLength);
            
            TCPFlush(socket);
        }
        
        if (opcode == WS_OPCODE_PONG) {
            // ignore pongs
        }
        
        // handle regular frames
        if (opcode == WS_OPCODE_TEXT || opcode == WS_OPCODE_BINARY) {
            WebSocketIncomingDataCallback(opcode, payloadBuffer, payloadLength);
        }
    }

    // This function is not called while a (partial, incomplete) frame is being received.
    if (WebSocketTaskCallback() == 0) {
        return status;
    } else {
        return SM_HTTP_DISCONNECT;
    }
}
