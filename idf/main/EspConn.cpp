#include "EspConn.h"
#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_ipc.h>

#include "hardware.h"
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <lwip/dns.h>
#include <esp_log.h>

namespace bitfixer 
{

static bool _connected = false;

bool EspConn::isConnected() {
    return _connected;
}

bool EspConn::initWithParams(uint8_t* buffer, uint16_t* bufferSize)
{
    _serialBuffer = buffer;
    _serialBufferSize = bufferSize;
    return true;
}

static esp_netif_t *s_example_sta_netif = NULL;
static SemaphoreHandle_t s_semph_get_ip_addrs = NULL;
static int s_retry_num = 0;

static void example_handler_on_wifi_disconnect(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    _connected = false;
    s_retry_num++;
    if (s_retry_num > 5) {
        log_i("WiFi Connect failed %d times, stop reconnect.", s_retry_num);
        /* let example_wifi_sta_do_connect() return */
        if (s_semph_get_ip_addrs) {
            xSemaphoreGive(s_semph_get_ip_addrs);
        }
        return;
    }
    log_i("Wi-Fi disconnected, trying to reconnect...");
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        return;
    }
    ESP_ERROR_CHECK(err);
}

static void example_handler_on_wifi_connect(void *esp_netif, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    log_i("connect");
}

static void example_handler_on_sta_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    s_retry_num = 0;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    //if (!example_is_our_netif(EXAMPLE_NETIF_DESC_STA, event->esp_netif)) {
    //    return;
    //}
    log_i("Got IPv4 event: Interface \"%s\" address: " IPSTR, esp_netif_get_desc(event->esp_netif), IP2STR(&event->ip_info.ip));
    if (s_semph_get_ip_addrs) {
        xSemaphoreGive(s_semph_get_ip_addrs);
    } else {
        log_i("- IPv4 address: " IPSTR ",", IP2STR(&event->ip_info.ip));
    }

    _connected = true;
}

esp_err_t example_wifi_sta_do_connect(wifi_config_t wifi_config, bool wait)
{
    if (wait) {
        s_semph_get_ip_addrs = xSemaphoreCreateBinary();
        if (s_semph_get_ip_addrs == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_retry_num = 0;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &example_handler_on_wifi_disconnect, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &example_handler_on_sta_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &example_handler_on_wifi_connect, s_example_sta_netif));

    log_i("Connecting to %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        log_e("WiFi connect failed! ret:%x", ret);
        return ret;
    }
    if (wait) {
        log_i("Waiting for IP(s)");
        xSemaphoreTake(s_semph_get_ip_addrs, portMAX_DELAY);
        if (s_retry_num > 5) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static wifi_config_t _wifi_config;

bool EspConn::connect(const char* ssid, const char* passphrase) {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    s_example_sta_netif = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    memset(&_wifi_config, 0, sizeof(wifi_config_t));
    strcpy((char*)_wifi_config.sta.ssid, ssid);
    strcpy((char*)_wifi_config.sta.password, passphrase);
    _wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    _wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    return wifi_start();
}

bool EspConn::wifi_start() {
    ESP_ERROR_CHECK(esp_wifi_start());
    example_wifi_sta_do_connect(_wifi_config, true);
    return true;
}

bool EspConn::wifi_stop() {
    esp_wifi_stop();
    return true;
}

// Socket send/receive timeout, so a stalled server can't hang the PETdisk.
#define HTTP_TIMEOUT_MS 5000

// check if a string is an ip address already
static bool isIP(const char* host) {
    int f[4];
    int res = sscanf(host, "%d.%d.%d.%d", &f[0], &f[1], &f[2], &f[3]);
    if (res != 4) {
        return false;
    }

    return true;
}

bool EspConn::startClient(const char* host, uint16_t port)
{
    _port = port;
    if (isIP(host)) {
        strncpy(_host, host, sizeof(_host) - 1);
        _host[sizeof(_host) - 1] = 0;
        return true;
    }

    // Resolve the name for every request. The previous dns_gethostbyname()
    // code waited on a flag that was never reset, so after the first lookup
    // every other host name silently reused the first host's address.
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;

    enable_interrupts();
    int err = getaddrinfo(host, NULL, &hints, &res);
    disable_interrupts();

    if (err != 0 || res == NULL) {
        ESP_LOGE("conn", "dns lookup failed for %s (%d)", host, err);
        _host[0] = 0;
        return false;
    }

    struct sockaddr_in* addr = (struct sockaddr_in*)res->ai_addr;
    inet_ntoa_r(addr->sin_addr, _host, sizeof(_host));
    freeaddrinfo(res);
    return true;
}

typedef struct _httpArgs {
    char* host;
    int port;
    uint8_t* sendData;
    int dataLen;
    uint8_t* recvData;
    int recvCapacity;
    int recCount;
    bool truncated;
} httpArgs;

// Send a request and read the whole response (HTTP/1.0: the server closes
// the connection when it is done). Returns false on any failure.
static bool http_fetch(httpArgs* hargs) {
    hargs->recCount = 0;
    hargs->truncated = false;

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    if (inet_pton(AF_INET, hargs->host, &dest_addr.sin_addr) != 1) {
        return false;
    }
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(hargs->port);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return false;
    }

    struct timeval timeout;
    timeout.tv_sec = HTTP_TIMEOUT_MS / 1000;
    timeout.tv_usec = (HTTP_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    bool ok = false;
    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) == 0) {
        // send() may accept only part of the request
        int sent = 0;
        while (sent < hargs->dataLen) {
            int n = send(sock, hargs->sendData + sent, hargs->dataLen - sent, 0);
            if (n <= 0) {
                break;
            }
            sent += n;
        }

        if (sent == hargs->dataLen) {
            // Read until the server closes the connection, never past the
            // end of the receive buffer. Excess data is drained and dropped.
            ok = true;
            while (1) {
                int space = hargs->recvCapacity - hargs->recCount;
                uint8_t discard[64];
                uint8_t* dest = space > 0 ? hargs->recvData + hargs->recCount : discard;
                int len = recv(sock, dest, space > 0 ? space : sizeof(discard), 0);
                if (len == 0) {
                    break;          // server closed the connection: done
                }
                if (len < 0) {
                    ok = false;     // timeout or error
                    break;
                }
                if (space > 0) {
                    hargs->recCount += len;
                } else {
                    hargs->truncated = true;
                }
            }
        }
    }

    // Always release the socket; failed requests used to leak it.
    shutdown(sock, 0);
    close(sock);

    if (hargs->truncated) {
        ESP_LOGE("conn", "response larger than %d bytes, dropped", hargs->recvCapacity);
        ok = false;
    }
    return ok;
}

bool EspConn::sendData(uint8_t sock, unsigned char* data, int len)
{
    httpArgs args;
    args.host = _host;
    args.port = _port;
    args.sendData = data;
    args.dataLen = len;
    args.recvData = _receiveBuffer;
    args.recvCapacity = RECEIVE_BUFFER_SIZE;

    enable_interrupts();
    bool ok = http_fetch(&args);
    // log only the request line; requests can be much longer than a log line
    const char* eol = (const char*)memchr(data, '\r', len);
    int lineLen = eol ? (int)(eol - (const char*)data) : len;
    if (lineLen > 120) {
        lineLen = 120;
    }
    ESP_LOGI("conn", "fetch: %.*s -> %s, %d bytes", lineLen, (const char*)data, ok ? "ok" : "FAILED", args.recCount);
    portYIELD();
    disable_interrupts();

    _receivedBytes = ok ? args.recCount : 0;
    _receiveBuffer[_receivedBytes] = 0;
    if (_serialBufferSize) {
        *_serialBufferSize = _receivedBytes;
    }
    return ok;
}

}