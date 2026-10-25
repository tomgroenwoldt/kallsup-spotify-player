#include "captive_dns.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace captive_dns {

static const char *TAG = "captive_dns";

constexpr uint8_t AP_IP[4] = {192, 168, 4, 1};  // ESP-IDF's default softAP address
constexpr size_t MAX_PACKET = 512;
constexpr size_t HEADER = 12;
constexpr size_t ANSWER = 16;  // name pointer, type, class, TTL, length, IPv4

static std::atomic<bool> s_running{false};
static std::atomic<int> s_sock{-1};

// Turns the query in `buf` into a response in place. Returns its length, or 0
// to drop the packet.
static size_t answer(uint8_t *buf, size_t len) {
    if (len < HEADER || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) return 0;  // one question, not a response
    size_t pos = HEADER;
    while (pos < len && buf[pos] != 0) {
        if (buf[pos] & 0xC0) return 0;  // queries don't compress names
        pos += buf[pos] + 1;
    }
    pos += 1 + 4;  // terminating zero, QTYPE, QCLASS
    if (pos > len) return 0;
    const bool type_a = buf[pos - 4] == 0 && buf[pos - 3] == 1;

    buf[2] = 0x84 | (buf[2] & 0x01);  // response, authoritative, keep RD
    buf[3] = 0x00;                    // no error
    buf[6] = 0;
    buf[7] = type_a ? 1 : 0;  // other types get an empty answer
    memset(buf + 8, 0, 4);   // no authority or additional records
    if (!type_a) return pos;
    if (pos + ANSWER > MAX_PACKET) return 0;

    const uint8_t record[ANSWER] = {0xC0, HEADER,  // name: pointer to the question
                                    0,    1,       // type A
                                    0,    1,       // class IN
                                    0,    0,    0, 10,  // TTL 10 s
                                    0,    4,    AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]};
    memcpy(buf + pos, record, ANSWER);
    return pos + ANSWER;
}

static void dns_task(void *) {
    const int sock = s_sock;
    uint8_t buf[MAX_PACKET];
    while (s_running) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
        if (n <= 0) continue;  // timeout: check s_running again
        const size_t out = answer(buf, n);
        if (out) sendto(sock, buf, out, 0, reinterpret_cast<sockaddr *>(&from), from_len);
    }
    close(sock);
    s_sock = -1;
    ESP_LOGI(TAG, "stopped");
    vTaskDelete(nullptr);
}

void start() {
    if (s_running.exchange(true)) return;
    if (s_sock >= 0) {
        // The previous task hasn't noticed the stop yet; it keeps serving.
        return;
    }
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        s_running = false;
        return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    memcpy(&addr.sin_addr.s_addr, AP_IP, 4);
    timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind failed");
        close(sock);
        s_running = false;
        return;
    }
    s_sock = sock;
    if (xTaskCreate(dns_task, "captive_dns", 3072, nullptr, 4, nullptr) != pdPASS) {
        close(sock);
        s_sock = -1;
        s_running = false;
        return;
    }
    ESP_LOGI(TAG, "answering all names with 192.168.4.1");
}

void stop() {
    s_running = false;
}

}  // namespace captive_dns
