#ifndef __esp_conn_h__
#define __esp_conn_h__

#include <stdint.h>
#include <stddef.h>

namespace bitfixer
{

class EspConn {
public:
    EspConn()
    : _serialBuffer(NULL)
    , _serialBufferSize(NULL)
    {

    }

    // Large enough for a 512 byte block plus generous HTTP headers.
    static const int RECEIVE_BUFFER_SIZE = 2048;

    bool initWithParams(uint8_t* buffer, uint16_t* bufferSize);
    bool connect(const char* ssid, const char* passphrase);
    bool startClient(const char* host, uint16_t port);
    // Sends a request and stores the response in receiveBuffer().
    // Returns false if the request failed or the response didn't fit.
    bool sendData(uint8_t sock, unsigned char* data, int len);
    bool isConnected();

    uint8_t* receiveBuffer() { return _receiveBuffer; }
    int receivedBytes() { return _receivedBytes; }
private:

    uint8_t* _serialBuffer;
    uint16_t* _serialBufferSize;

    // Responses get their own buffer. They used to be written into the
    // shared 1K buffer with no length limit, over the network drives'
    // file names and URLs.
    uint8_t _receiveBuffer[RECEIVE_BUFFER_SIZE + 1];
    int _receivedBytes = 0;

    char _host[256];
    int _port;

    bool _connected = false;

    bool wifi_start();
    bool wifi_stop();
};

}

#endif