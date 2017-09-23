// Include all headers for any enabled TCPIP Stack functions
#include "tcpip/src/tcpip_private.h"
#include "crypto/src/hash.h"
#include "websocket.h"

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

#define ProtocolErrorClose(skt) TCPIP_TCP_ArrayPut((skt), (uint8_t *) "\210\002\003\352", 4); TCPIP_TCP_Flush((skt)) // sends a close frame with status code 1002
#define MessageTooBigClose(skt) TCPIP_TCP_ArrayPut((skt), (uint8_t *) "\210\002\003\361", 4); TCPIP_TCP_Flush((skt)) // sends a close frame with status code 1009
#define UnsupportedClose(skt)   TCPIP_TCP_ArrayPut((skt), (uint8_t *) "\210\002\003\363", 4); TCPIP_TCP_Flush((skt)) // sends a close frame with status code 1011

static uint8_t payloadBuffer[MAX_PAYLOAD_LENGTH];

uint8_t HTTPUpgradeHeader[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
uint8_t WebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

int TCPIP_WS_doHandShake( HTTP_CONN* pHttpCon ) {
    if ( TCPIP_TCP_PutIsReady( pHttpCon->socket ) < sizeof (HTTPUpgradeHeader ) + 43 ) return 1;

    wc_HashAlg hash;
    uint8_t sha1Result[SHA_DIGEST_SIZE];
    uint8_t resultBase64[40];
    uint32_t resultLength = sizeof (resultBase64 );

    TCPIP_TCP_ArrayPut( pHttpCon->socket, ( uint8_t * )HTTPUpgradeHeader, sizeof (HTTPUpgradeHeader ) - 1 ); // without terminating \0 

    // SHA1 Initialize
    wc_HashInit( &hash, WC_HASH_TYPE_SHA );
    wc_HashUpdate( &hash, WC_HASH_TYPE_SHA, pHttpCon->webSocketKey, WS_KEY_LENGTH );
    wc_HashUpdate( &hash, WC_HASH_TYPE_SHA, WebSocketGuid, ( uint16_t )sizeof (WebSocketGuid ) - 1 );
    wc_HashFinal( &hash, WC_HASH_TYPE_SHA, sha1Result );

    Base64_Encode_NoNl( sha1Result, SHA_DIGEST_SIZE, resultBase64, &resultLength );
    TCPIP_TCP_ArrayPut( pHttpCon->socket, resultBase64, resultLength );
    TCPIP_TCP_ArrayPut( pHttpCon->socket, ( uint8_t * )"\r\n\r\n", 4 );

    return 0;
}

int TCPIP_WS_SendPayload( HTTP_CONN* pHttpCon, WS_OPCODE opcode, uint16_t length ) {
    if ( length > TCPIP_HTTP_MAX_DATA_LEN ) length = TCPIP_HTTP_MAX_DATA_LEN;

    uint16_t txLength = TCPIP_TCP_PutIsReady( pHttpCon->socket );
    uint16_t frameLength = length + HEADER_LENGTH;

    if ( length >= EXT_PAYLOAD_BOUNDARY ) frameLength += EXT_PAYLOAD_LENGTH;

    if ( txLength < frameLength ) return 1; // not enough space in TX buffer to send frame

    TCPIP_TCP_Put( pHttpCon->socket, FIN_MASK | opcode );

    if ( length >= EXT_PAYLOAD_BOUNDARY ) {
        TCPIP_TCP_Put( pHttpCon->socket, EXT_PAYLOAD_BOUNDARY );
        // swap length into network byte order
        uint16_t payloadLength = ( length << 8 ) | ( length >> 8 );
        TCPIP_TCP_ArrayPut( pHttpCon->socket, ( uint8_t * ) & payloadLength, EXT_PAYLOAD_LENGTH );
    } else {
        TCPIP_TCP_Put( pHttpCon->socket, length );
    }

    if ( length > 0 ) TCPIP_TCP_ArrayPut( pHttpCon->socket, pHttpCon->data, length );

    TCPIP_TCP_Flush( pHttpCon->socket );
    return 0;
}

int TCPIP_WS_Close( HTTP_CONN* pHttpCon, uint16_t status, uint8_t *reason, uint16_t length ) {
    if ( status == 0 ) {
        length = 0;
    } else {
        length += STATUS_CODE_LENGTH;
    }

    if ( reason == NULL ) length = status ? STATUS_CODE_LENGTH : 0;
    if ( length >= EXT_PAYLOAD_BOUNDARY ) length = EXT_PAYLOAD_BOUNDARY - 1; // truncate reason

    uint16_t txLength = TCPIP_TCP_PutIsReady( pHttpCon->socket );
    uint16_t frameLength = length + HEADER_LENGTH;

    if ( txLength < frameLength ) return 1; // not enough space in TX buffer to send frame

    TCPIP_TCP_Put( pHttpCon->socket, FIN_MASK | WS_OPCODE_CLOSE );
    TCPIP_TCP_Put( pHttpCon->socket, length );

    if ( length > 0 ) TCPIP_TCP_ArrayPut( pHttpCon->socket, ( uint8_t * ) & status, STATUS_CODE_LENGTH );
    if ( length > STATUS_CODE_LENGTH ) TCPIP_TCP_ArrayPut( pHttpCon->socket, reason, length - STATUS_CODE_LENGTH );

    TCPIP_TCP_Flush( pHttpCon->socket );
    return 0;
}

SM_HTTP2 TCPIP_WS_Process( HTTP_CONN* pHttpCon ) {
    uint16_t txLength = TCPIP_TCP_PutIsReady( pHttpCon->socket );

    if ( txLength < HEADER_LENGTH + EXT_PAYLOAD_LENGTH ) return pHttpCon->sm; // not enough space in TX buffer to even send a control frame

    uint16_t rxLength = TCPIP_TCP_GetIsReady( pHttpCon->socket );

    if ( rxLength >= HEADER_LENGTH ) {
        uint8_t frameHeader;
        uint16_t payloadLength;
        uint8_t mask[MASK_WIDTH];
        WS_OPCODE opcode;
        uint16_t index;

        frameHeader = TCPIP_TCP_Peek( pHttpCon->socket, 0 );
        opcode = frameHeader & OPCODE_MASK;

        // check header
        if ( ( frameHeader & HEADER_MASK ) != FIN_MASK ) {
            // FIN bit not set or RSV bits set --> unsupported for now
            UnsupportedClose( pHttpCon->socket );
            return SM_HTTP_DISCONNECT;
        }

        // check opcodes
        if ( opcode != WS_OPCODE_TEXT && opcode != WS_OPCODE_BINARY && ( opcode < WS_OPCODE_CLOSE || opcode > WS_OPCODE_PONG ) ) {
            // only text, binary, close frame, ping and pong frames are supported
            UnsupportedClose( pHttpCon->socket );
            return SM_HTTP_DISCONNECT;
        }

        if ( opcode >= WS_OPCODE_CLOSE && ( frameHeader & FIN_MASK ) == 0 ) {
            // control opcodes are not allowed with fragmented frames
            ProtocolErrorClose( pHttpCon->socket );
            return SM_HTTP_DISCONNECT;
        }

        // check payload length
        payloadLength = TCPIP_TCP_Peek( pHttpCon->socket, 1 );

        // check if mask bit is set.
        if ( ( payloadLength & MASK_MASK ) == 0 ) {
            ProtocolErrorClose( pHttpCon->socket );
            return SM_HTTP_DISCONNECT;
        } else {
            // remove the mask bit
            payloadLength &= ~MASK_MASK;
        }

        if ( payloadLength > EXT_PAYLOAD_BOUNDARY ) {
            // extended payload length not supported yet
            MessageTooBigClose( pHttpCon->socket );
            return SM_HTTP_DISCONNECT;
        }

        if ( payloadLength == EXT_PAYLOAD_BOUNDARY ) {
            if ( opcode >= WS_OPCODE_CLOSE ) {
                // control opcodes with extended payloads are not allowed
                ProtocolErrorClose( pHttpCon->socket );
                return SM_HTTP_DISCONNECT;
            } else if ( rxLength < HEADER_LENGTH + EXT_PAYLOAD_LENGTH ) {
                return pHttpCon->sm;
            } else {
                TCPIP_TCP_ArrayPeek( pHttpCon->socket, ( uint8_t * ) & payloadLength, EXT_PAYLOAD_LENGTH, HEADER_LENGTH );
                if ( payloadLength > MAX_PAYLOAD_LENGTH ) {
                    MessageTooBigClose( pHttpCon->socket );
                    return SM_HTTP_DISCONNECT;
                }

                if ( rxLength < payloadLength + MASK_WIDTH - HEADER_LENGTH - EXT_PAYLOAD_LENGTH ) return pHttpCon->sm;

                TCPIP_TCP_ArrayGet( pHttpCon->socket, payloadBuffer, HEADER_LENGTH + EXT_PAYLOAD_LENGTH ); // remove header from TCP RX buffer
            }
        } else {
            if ( rxLength < payloadLength + MASK_WIDTH - HEADER_LENGTH ) return pHttpCon->sm;

            // check if there is enough room in the TX buffer to handle control opcodes
            if ( ( opcode == WS_OPCODE_CLOSE || opcode == WS_OPCODE_PING ) && txLength < payloadLength + HEADER_LENGTH ) return pHttpCon->sm;

            TCPIP_TCP_ArrayGet( pHttpCon->socket, payloadBuffer, HEADER_LENGTH ); // remove header from TCP RX buffer
        }

        // decode message
        TCPIP_TCP_ArrayGet( pHttpCon->socket, mask, MASK_WIDTH );

        if ( payloadLength > 0 ) {
            TCPIP_TCP_ArrayGet( pHttpCon->socket, payloadBuffer, payloadLength );

            // unmask
            for ( index = 0; index < payloadLength; index++ ) {
                payloadBuffer[index] ^= mask[index % MASK_WIDTH];
            }
        }

        // handle control frames
        if ( opcode == WS_OPCODE_CLOSE ) { // connection close
            TCPIP_TCP_Put( pHttpCon->socket, FIN_MASK | WS_OPCODE_CLOSE );
            // echo the received status code (if available)
            if ( payloadLength >= STATUS_CODE_LENGTH ) {
                TCPIP_TCP_Put( pHttpCon->socket, STATUS_CODE_LENGTH );
                TCPIP_TCP_ArrayPut( pHttpCon->socket, payloadBuffer, STATUS_CODE_LENGTH );
            } else {
                TCPIP_TCP_Put( pHttpCon->socket, 0 ); // zero payload length
            }

            TCPIP_TCP_Flush( pHttpCon->socket );
            return SM_HTTP_DISCONNECT;
        }

        if ( opcode == WS_OPCODE_PING ) { // ping
            // reply with a pong
            TCPIP_TCP_Put( pHttpCon->socket, FIN_MASK | WS_OPCODE_PONG );
            TCPIP_TCP_Put( pHttpCon->socket, payloadLength );
            // echo the received payload (if any)
            if ( payloadLength > 0 ) TCPIP_TCP_ArrayPut( pHttpCon->socket, payloadBuffer, payloadLength );

            TCPIP_TCP_Flush( pHttpCon->socket );
        }

        if ( opcode == WS_OPCODE_PONG ) {
            // ignore pongs
        }

        // handle regular frames
        if ( opcode == WS_OPCODE_TEXT || opcode == WS_OPCODE_BINARY ) {
            TCPIP_WS_IncomingDataCallback( pHttpCon, opcode, payloadBuffer, payloadLength );
        }
    }

    // This function is not called while a (partial, incomplete) frame is being received.
    if ( TCPIP_WS_TaskCallback( pHttpCon ) == 0 ) {
        return pHttpCon->sm;
    } else {
        return SM_HTTP_DISCONNECT;
    }
}