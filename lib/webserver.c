#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"

#include "webserver.h"

#define MAX_BUFFER_SIZE 20


extern volatile float hum_max_user;
extern volatile float hum_min_user;
extern volatile float temp_max_user;
extern volatile float temp_min_user;
extern volatile float press_max_user;
extern volatile float press_min_user;

extern volatile float temperature;
extern volatile float pressure;
extern volatile float humidity;
extern float temp_buffer[MAX_BUFFER_SIZE];
extern float hum_buffer[MAX_BUFFER_SIZE];
extern float press_buffer[MAX_BUFFER_SIZE];
extern int buffer_index;


#define WIFI_SSID "senha"
#define WIFI_PASS "wifi"

const char HTML_PART1[] =
"<!DOCTYPE html><html lang=\"pt-BR\"><head><meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
"<title>Weather Station</title><style>"
"body { font-family: 'Arial', sans-serif; background-color: #87ceeb; margin: 0; padding: 0; }"
"h1 { text-align: center; padding: 1rem; }"
".section { background: white; margin: 1rem auto; padding: 1rem; border-radius: 12px; width: 90%; max-width: 600px; box-shadow: 0 0 10px rgba(0,0,0,0.1); }"
".input-group { display: flex; justify-content: space-between; margin-top: 10px; }"
".input-group label { font-weight: bold; }"
"input[type=number] { width: 45%; padding: 0.5rem; border: 1px solid #ccc; border-radius: 5px; }"
"canvas { width: 100%; max-width: 100%; height: auto; margin-top: 1rem; }"
"</style></head><body><h1>WEATHER STATION</h1>";

const char HTML_PART2[] =
"<div class=\"section\">"
"<h2>Temperatura (°C)</h2>"
"<canvas id=\"tempChart\"></canvas>"
"<div class=\"input-group\">"
"<input type=\"number\" id=\"temp_max\" placeholder=\"Máximo\" title=\"Temperatura máxima\" onchange=\"enviarLimites()\">"
"<input type=\"number\" id=\"temp_min\" placeholder=\"Mínimo\" title=\"Temperatura mínima\" onchange=\"enviarLimites()\">"
"</div></div>";

const char HTML_PART3[] =
"<div class=\"section\">"
"<h2>Umidade (%)</h2>"
"<canvas id=\"humChart\"></canvas>"
"<div class=\"input-group\">"
"<input type=\"number\" id=\"hum_max\" placeholder=\"Máximo\" title=\"Umidade máxima\" onchange=\"enviarLimites()\">"
"<input type=\"number\" id=\"hum_min\" placeholder=\"Mínimo\" title=\"Umidade mínima\" onchange=\"enviarLimites()\">"
"</div></div>";

const char HTML_PART4[] =
"<div class=\"section\">"
"<h2>Pressão (Pa)</h2>"
"<canvas id=\"pressChart\"></canvas>"
"<div class=\"input-group\">"
"<input type=\"number\" id=\"press_max\" placeholder=\"Máximo\" title=\"Pressão máxima\" onchange=\"enviarLimites()\">"
"<input type=\"number\" id=\"press_min\" placeholder=\"Mínimo\" title=\"Pressão mínima\" onchange=\"enviarLimites()\">"
"</div></div>";

const char HTML_PART5[] =
"<script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script>"
"<script>"
"const tempCtx = document.getElementById('tempChart').getContext('2d');"
"const humCtx = document.getElementById('humChart').getContext('2d');"
"const pressCtx = document.getElementById('pressChart').getContext('2d');"

"let tempChart = new Chart(tempCtx, {"
"  type: 'line',"
"  data: { labels: [], datasets: [{ label: '°C', data: [], borderColor: 'red', borderWidth: 2, fill: false }] },"
"  options: { scales: { y: { beginAtZero: false } } }"
"});"

"let humChart = new Chart(humCtx, {"
"  type: 'line',"
"  data: { labels: [], datasets: [{ label: '%', data: [], borderColor: 'blue', borderWidth: 2, fill: false }] },"
"  options: { scales: { y: { beginAtZero: false } } }"
"});"

"let pressChart = new Chart(pressCtx, {"
"  type: 'line',"
"  data: { labels: [], datasets: [{ label: 'Pa', data: [], borderColor: 'green', borderWidth: 2, fill: false }] },"
"  options: { scales: { y: { beginAtZero: false } } }"
"});"
;

const char HTML_PART6[] =
"function atualizarGraficos() {"
"fetch('/estado')"
".then(res => res.json())"
".then(data => {"
"  const labels = Array.from({length: data.temperaturas.length}, (_, i) => i + 1);"
"  tempChart.data.labels = labels;"
"  humChart.data.labels = labels;"
"  pressChart.data.labels = labels;"
"  tempChart.data.datasets[0].data = data.temperaturas;"
"  humChart.data.datasets[0].data = data.umidades;"
"  pressChart.data.datasets[0].data = data.pressoes;"
"  tempChart.update();"
"  humChart.update();"
"  pressChart.update();"
"});"
"}"
"setInterval(atualizarGraficos, 5000);"
"window.onload = atualizarGraficos;";

const char HTML_PART7[] =
"function enviarLimites() {"
"  const params = new URLSearchParams({"
"    temp_max: document.getElementById('temp_max').value,"
"    temp_min: document.getElementById('temp_min').value,"
"    hum_max: document.getElementById('hum_max').value,"
"    hum_min: document.getElementById('hum_min').value,"
"    press_max: document.getElementById('press_max').value,"
"    press_min: document.getElementById('press_min').value"
"  });"
"  fetch('/limites?' + params.toString());"
"}"
"</script></body></html>";


struct http_state {
    char response[4096]; // Aumentado para suportar HTML completo
    size_t len;
    size_t sent;
};

static err_t http_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    struct http_state *hs = (struct http_state *)arg;
    hs->sent += len;

    if (hs->sent >= hs->len) {
        tcp_close(tpcb);
        free(hs);
    } else {
        tcp_write(tpcb, hs->response + hs->sent, hs->len - hs->sent, TCP_WRITE_FLAG_COPY);
    }

    return ERR_OK;
}

static err_t http_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) {
        tcp_close(tpcb);
        return ERR_OK;
    }

    char *req = (char *)p->payload;

    if (strstr(req, "GET /limites")) {
        struct http_state *hs = malloc(sizeof(struct http_state));
        if (!hs) return ERR_MEM;
        hs->sent = 0;

        sscanf(req, "GET /limites?temp_max=%f&temp_min=%f&hum_max=%f&hum_min=%f&press_max=%f&press_min=%f",
               &temp_max_user, &temp_min_user,
               &hum_max_user, &hum_min_user,
               &press_max_user, &press_min_user);

        const char *redir_hdr = "HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n";
        hs->len = snprintf(hs->response, sizeof(hs->response), redir_hdr);

        tcp_arg(tpcb, hs);
        tcp_sent(tpcb, http_sent);
        tcp_write(tpcb, hs->response, hs->len, TCP_WRITE_FLAG_COPY);
        tcp_output(tpcb);
    }

    else if (strstr(req, "GET /estado")) {
        struct http_state *hs = malloc(sizeof(struct http_state));
        if (!hs) return ERR_MEM;
        hs->sent = 0;

        char temp_str[512] = "", hum_str[512] = "", press_str[512] = "";

        for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
            int idx = (buffer_index + i) % MAX_BUFFER_SIZE;

            snprintf(temp_str + strlen(temp_str), sizeof(temp_str) - strlen(temp_str),
                     "%.2f%s", temp_buffer[idx], (i < MAX_BUFFER_SIZE - 1) ? "," : "");

            snprintf(hum_str + strlen(hum_str), sizeof(hum_str) - strlen(hum_str),
                     "%.2f%s", hum_buffer[idx], (i < MAX_BUFFER_SIZE - 1) ? "," : "");

            snprintf(press_str + strlen(press_str), sizeof(press_str) - strlen(press_str),
                     "%.2f%s", press_buffer[idx], (i < MAX_BUFFER_SIZE - 1) ? "," : "");
        }

        char json_payload[2048];
        snprintf(json_payload, sizeof(json_payload),
                 "{\"temperaturas\":[%s],\"umidades\":[%s],\"pressoes\":[%s]}",
                 temp_str, hum_str, press_str);

        hs->len = snprintf(hs->response, sizeof(hs->response),
                           "HTTP/1.1 200 OK\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %d\r\n"
                           "Connection: close\r\n\r\n%s",
                           (int)strlen(json_payload), json_payload);

        tcp_arg(tpcb, hs);
        tcp_sent(tpcb, http_sent);
        tcp_write(tpcb, hs->response, hs->len, TCP_WRITE_FLAG_COPY);
        tcp_output(tpcb);
    }

    else {
        const char* header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n";

        tcp_write(tpcb, header, strlen(header), TCP_WRITE_FLAG_COPY);
        tcp_write(tpcb, HTML_PART1, strlen(HTML_PART1), TCP_WRITE_FLAG_COPY);
        tcp_write(tpcb, HTML_PART2, strlen(HTML_PART2), TCP_WRITE_FLAG_COPY);
        tcp_write(tpcb, HTML_PART3, strlen(HTML_PART3), TCP_WRITE_FLAG_COPY);
        tcp_write(tpcb, HTML_PART4, strlen(HTML_PART4), TCP_WRITE_FLAG_COPY);
        tcp_write(tpcb, HTML_PART5, strlen(HTML_PART5), TCP_WRITE_FLAG_COPY);
        tcp_write(tpcb, HTML_PART6, strlen(HTML_PART6), TCP_WRITE_FLAG_COPY);
        tcp_write(tpcb, HTML_PART7, strlen(HTML_PART7), TCP_WRITE_FLAG_COPY);
        tcp_output(tpcb);
    }

    pbuf_free(p);
    return ERR_OK;
}



static err_t connection_callback(void *arg, struct tcp_pcb *newpcb, err_t err) {
    tcp_recv(newpcb, http_recv);
    return ERR_OK;
}

static void start_http_server(void) {
    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, 80);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, connection_callback);
    printf("Servidor HTTP iniciado na porta 80\n");
}

bool webserver_init(void) {
    if (cyw43_arch_init()) {
        printf("Falha para iniciar o cyw43\n");
        return false;
    }

    cyw43_arch_enable_sta_mode();

    printf("Conectando ao Wi-Fi: %s\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, 15000)) {
        printf("Falha para conectar ao Wi-Fi\n");
        cyw43_arch_deinit();
        return false;
    }

    printf("Conectado com sucesso!\n");
    start_http_server();
    return true;
}                                                
