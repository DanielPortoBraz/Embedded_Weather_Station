/**
 * Autor: Daniel Porto Braz
 * 
 * Este código contém a lógica e integração de um projeto que mede dados meteorológicos (umidade, temperatura e pressão)
 * atmosférica, e os exibe na BitDogLab por meio dos seus periféricos, além de contar com uma interface responsiva em
 * HTML/ AJAX para exibir as medições de forma dinâmica com gráficos, um para cada dado (temperatura, umidade e pressão atmosférica).
 * A interface responsiva possibilita ao usuário a definição de níveis limite de cada dado, também. Os dados obtidos são 
 * medidos pelos sensores: AHT10 e BMP280.
 * 
 * O código desenvolvido é baseado no mesmo apresentado por Wilton Lacerda, no repositório a seguir:
 * https://github.com/wiltonlacerda/EmbarcaTechResU3Ex05
 * 
 * */


// Bibliotecas 
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/cyw43_arch.h"
#include "lib/webserver.h" 
#include "hardware/pio.h"
#include "ws2812.pio.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "aht20.h"
#include "bmp280.h"
#include "ssd1306.h"
#include "font.h"
#include <math.h>


// =========== PINOS E DEFINIÇÔES =============

// IP da placa Raspberry Pi Pico W
static char ip_str[24];

// Buzzer
const uint8_t BUZZER_PIN = 21;
const uint16_t PERIOD = 59609; // WRAP
const float DIVCLK = 16.0; // Divisor inteiro
static uint slice_21;
const uint16_t dc_values[] = {PERIOD * 0.3, 0}; // Duty Cycle de 30% e 0%

#define MATRIX_PIN 7 // Matriz de LEDs
#define NUM_LEDS 25

#define BUTTON_A 5 // Botão A
#define BUTTON_B 6 // Botão B
volatile uint32_t last_time = 0; // Para debounce

#define LED_RED_PIN 13 // LED vermelho
#define LED_GREEN_PIN 11 // LED verde

// Pinos e definições para componentes com interface I2C
#define I2C_PORT i2c0               // i2c0 pinos 0 e 1, i2c1 pinos 2 e 3
#define I2C_SDA 0                   // 0 ou 2
#define I2C_SCL 1                   // 1 ou 3
#define SEA_LEVEL_press_buffer 101325.0 // Pressão ao nível do mar em Pa
// Display na I2C
#define I2C_PORT_DISP i2c1
#define I2C_SDA_DISP 14
#define I2C_SCL_DISP 15
#define endereco 0x3C

// Níveis limite padrão de umidade em %
#define HUM_MAX 90.0f
#define HUM_MIN 70.0f
// Definidos pelo usuário
volatile float hum_max_user = HUM_MAX; 
volatile float hum_min_user = HUM_MIN; 

// Níveis limite padrão de temperatura em °C
#define TEMP_MAX 35.0f
#define TEMP_MIN 20.0f
// Definidos pelo usuário
volatile float temp_max_user = TEMP_MAX; 
volatile float temp_min_user = TEMP_MIN; 

// Níveis limite padrão de pressão atmosférica em kPa
#define PRESS_MAX 20.0f
#define PRESS_MIN 0.0f
// Definidos pelo usuário
volatile float press_max_user = PRESS_MAX;
volatile float press_min_user = PRESS_MIN;

// Seletor de tela no display
// 0 para todos os dados, 1 para temperatura, 2 para umidade e 3 para pressão atmosférica 
static volatile int select_screen = 0;

// Dados de temperatura (BMP280), umidade (AHT20) e pressão atmosférica (BMP280) lidos
#define MAX_BUFFER_SIZE 20 // Tamanho do buffer de leitura. Armazena até 20 amostras
float temp_buffer[MAX_BUFFER_SIZE] = {0};
float temperature; // Medição atual de temperatura
float hum_buffer[MAX_BUFFER_SIZE] = {0};
float humidity; // Medição atual de umidade relativa do ar
float press_buffer[MAX_BUFFER_SIZE] = {0};
float pressure; // Medição atual de pressão atmosférica
int buffer_index = 0; // Indíce do buffer. Atualiza para indicar o número da amostra atual



// =========== FUNÇÔES =============

// WEBSERVER
/**
 * Inicializa o servidor web e mostra informações no display
 */
void inicializar_webserver(ssd1306_t *ssd) {
    ssd1306_fill(ssd, false);
    ssd1306_draw_string(ssd, "Conectando WiFi", 6, 22);
    ssd1306_send_data(ssd);

    if (!webserver_init()) {
        printf("Falha ao iniciar o servidor web.\n");
        ssd1306_fill(ssd, false);
        ssd1306_draw_string(ssd, "WiFi: FALHA", 8, 22);
        ssd1306_send_data(ssd);
        while(true); // Para execução em caso de falha
    }
    
    // Obtém e exibe o IP
    uint8_t *ip = (uint8_t *)&(cyw43_state.netif[0].ip_addr.addr);
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    printf("IP: %s\n", ip_str);

    ssd1306_fill(ssd, false);
    ssd1306_draw_string(ssd, "IP:", 8, 6);
    ssd1306_draw_string(ssd, ip_str, 8, 22);
    ssd1306_send_data(ssd);
    sleep_ms(3000); // Mostra o IP por 3 segundos
}


// BUZZER
// Configura o buzzer
void setup_buzzer(){
    gpio_init(BUZZER_PIN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);

    // PWM do BUZZER
    // Configura para soar 440 Hz
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    slice_21 = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_clkdiv(slice_21, DIVCLK);
    pwm_set_wrap(slice_21, PERIOD);
    pwm_set_gpio_level(BUZZER_PIN, 0);
    pwm_set_enabled(slice_21, true);
}


// MATRIZ DE LEDS
/**
 * Envia um pixel para a matriz WS2812
 */
void ws2812_put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

/**
 * Converte valores RGB para formato u32 (GRB)
 */
uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)(g) << 16) | ((uint32_t)(r) << 8) | (uint32_t)(b);
}

/**
 * Atualiza a matriz de LEDs baseada no nível percentual
 */
void update_matrix(float nivel_percentual) {
    uint32_t frame[NUM_LEDS] = {0};  // Buffer com 25 LEDs apagados
    uint32_t cor_azul = urgb_u32(0, 0, 4);
    uint32_t cor_vermelha = urgb_u32(4, 0, 0);

    // Define quais LEDs acender baseado no nível
    if (nivel_percentual >= 20.0 && nivel_percentual <= 30.0) {
        // Primeira linha (0-4) - vermelha (nível baixo)
        for (int i = 0; i <= 4; i++) {
            frame[i] = cor_vermelha;
        }
    }
    else if (nivel_percentual > 30.0 && nivel_percentual <= 39.0) {
        // Primeira linha - azul (nível OK)
        for (int i = 0; i <= 4; i++) {
            frame[i] = cor_azul;
        }
    }
    else if (nivel_percentual > 39.0 && nivel_percentual <= 59.0) {
        // Duas primeiras linhas - azul
        for (int i = 0; i <= 9; i++) {
            frame[i] = cor_azul;
        }
    }
    else if (nivel_percentual > 59.0 && nivel_percentual <= 70.0) {
        // Três primeiras linhas - azul
        for (int i = 0; i <= 14; i++) {
            frame[i] = cor_azul;
        }
    }
    else if (nivel_percentual > 70.0 && nivel_percentual <= 79.0) {
        // Três primeiras linhas - vermelha (nível alto)
        for (int i = 0; i <= 14; i++) {
            frame[i] = cor_azul;
        }
    }
    else if (nivel_percentual > 79.0 && nivel_percentual <= 99.0) {
        // Quatro primeiras linhas - vermelha
        for (int i = 0; i <= 19; i++) {
            frame[i] = cor_vermelha;
        }
    }
    else if (nivel_percentual > 99.0) {
        // Todas as linhas - vermelha (nível crítico)
        for (int i = 0; i <= 24; i++) {
            frame[i] = cor_vermelha;
        }
    }

    // Envia o frame completo para a matriz WS2812
    for (int i = 0; i < NUM_LEDS; i++) {
        ws2812_put_pixel(frame[i]);
    }
    sleep_us(70);
}

// Inicializa os periféricos
void initialize_peripherals(ssd1306_t *ssd, struct bmp280_calib_param *params){
    gpio_init(LED_RED_PIN); // LED vermelho
    gpio_set_dir(LED_RED_PIN, GPIO_OUT);
    gpio_put(LED_RED_PIN, false);
    
    gpio_init(LED_GREEN_PIN); // LED verde
    gpio_set_dir(LED_GREEN_PIN, GPIO_OUT);
    gpio_put(LED_GREEN_PIN, false);

    gpio_init(BUTTON_A); // Botão A
    gpio_set_dir(BUTTON_A, GPIO_IN);
    gpio_pull_up(BUTTON_A);

    gpio_init(BUTTON_B); // Botão B
    gpio_set_dir(BUTTON_B, GPIO_IN);
    gpio_pull_up(BUTTON_B);

    setup_buzzer(); // Buzzer

    // Configuração da matriz de LEDs WS2812
    PIO pio = pio0;
    int sm = 0;
    uint offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, MATRIX_PIN, 800000, false);

    // I2C do Display funcionando em 400Khz.
    i2c_init(I2C_PORT_DISP, 400 * 1000);

    gpio_set_function(I2C_SDA_DISP, GPIO_FUNC_I2C);                    // Set the GPIO pin function to I2C
    gpio_set_function(I2C_SCL_DISP, GPIO_FUNC_I2C);                    // Set the GPIO pin function to I2C
    gpio_pull_up(I2C_SDA_DISP);                                        // Pull up the data line
    gpio_pull_up(I2C_SCL_DISP);                                        // Pull up the clock line                                                    // Inicializa a estrutura do display
    ssd1306_init(ssd, WIDTH, HEIGHT, false, endereco, I2C_PORT_DISP); // Inicializa o display
    ssd1306_config(ssd);                                              // Configura o display
    ssd1306_send_data(ssd);                                           // Envia os dados para o display

    // Limpa o display. O display inicia com todos os pixels apagados.
    ssd1306_fill(ssd, false);
    ssd1306_send_data(ssd);

    // Inicializa o I2C
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    // Inicializa o BMP280
    bmp280_init(I2C_PORT);
    bmp280_get_calib_params(I2C_PORT, params);

    // Inicializa o AHT20
    aht20_reset(I2C_PORT);
    aht20_init(I2C_PORT);

}

// Função para calcular a altitude a partir da pressão atmosférica
double calculate_altitude(double press_buffer)
{
    return 44330.0 * (1.0 - pow(press_buffer / SEA_LEVEL_press_buffer, 0.1903));
}

// Sinaliza o estado pelo LED RGB, com base nas medidas obtidas
void state_measures(float temp_buffer, float hum_buffer, float press_buffer){

    // Acende o LED vermelho, caso os dados estejam fora do intervalo limite
    if (temp_buffer > temp_max_user || temp_buffer < temp_min_user 
        || hum_buffer > hum_max_user || hum_buffer < hum_min_user 
        || press_buffer > press_max_user || press_buffer < press_min_user){
        gpio_put(LED_RED_PIN, true);
        gpio_put(LED_GREEN_PIN, false);
        // Emite beep
        pwm_set_gpio_level(BUZZER_PIN, dc_values[0]);
        sleep_ms(100);
        pwm_set_gpio_level(BUZZER_PIN, dc_values[1]);
    }
    else{ // Acende o LED verde, caso contrário
        gpio_put(LED_GREEN_PIN, true);
        gpio_put(LED_RED_PIN, false);
    }
}



// ======== INTERRUPÇÂO ===========

// Interrupção com botão
void gpio_irq_handler(uint gpio, uint32_t events)
{
    uint32_t curr_time = to_ms_since_boot(get_absolute_time());

    if (curr_time - last_time > 200){
        last_time = curr_time;

        if (gpio == BUTTON_B) {
            reset_usb_boot(0, 0);  // Reset para modo BOOTSEL
            return;
        }

        else if (gpio == BUTTON_A) {  
            select_screen =  (select_screen + 1) % 4; // Alterna de 0 a 3
        }
    }
}



// ============ PROGRAMA PRINCIPAL ==========
int main()
{
    stdio_init_all();
    
    ssd1306_t ssd; // Estrutura do display
    struct bmp280_calib_param params;
    initialize_peripherals(&ssd, &params);

    // Ativação das interrupções
    gpio_set_irq_enabled_with_callback(BUTTON_A, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);  
    gpio_set_irq_enabled_with_callback(BUTTON_B, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler); 

    inicializar_webserver(&ssd); // Permite a conexão via WIFI para o webserver

    // Estrutura para armazenar os dados do sensor
    AHT20_Data data;
    int32_t raw_temp_bmp;
    int32_t raw_press_buffer;

    char str_press[7]; // Buffer para armazenar o valor de pressão atmosférica
    char str_alt[5];  // Buffer para armazenar o valor de altitude
    char str_temp[5];  // Buffer para armazenar o valor de temperatura (AHT20)
    char str_umi[5];  // Buffer para armazenar o valor de umidade

    bool cor = true;

    while (1)
    {
        // Poll do WiFi
        cyw43_arch_poll();

        // Leitura do BMP280
        bmp280_read_raw(I2C_PORT, &raw_temp_bmp, &raw_press_buffer);
        temperature = (float) (bmp280_convert_temp(raw_temp_bmp, &params)) / 100.0;
        pressure = (float) bmp280_convert_pressure(raw_press_buffer, raw_temp_bmp, &params) / 1000.0;

        // Cálculo da altitude
        double altitude = calculate_altitude(pressure * 1000);

        printf("Pressao = %.3f kPa\n", pressure);
        printf("Temperatura BMP: = %.2f C\n", temperature);
        printf("Altitude estimada: %.2f m\n", altitude);

        // Leitura do AHT20
        if (aht20_read(I2C_PORT, &data))
        {
            humidity = data.humidity;
            printf("Temperatura AHT: %.2f C\n", data.temperature);
            printf("Umidade: %.2f %%\n\n\n", data.humidity);
        }
        else
        {
            printf("Erro na leitura do AHT10!\n\n\n");
        }

        // Atualiza os buffers de amostras para os gráficos da interface web
        temp_buffer[buffer_index] = temperature;
        hum_buffer[buffer_index] = humidity;
        press_buffer[buffer_index] = pressure;

        buffer_index = (buffer_index + 1) % MAX_BUFFER_SIZE; // Alterna rotativamente os itens do buffer, permitindo salvar 20 amostras por sequência

        sprintf(str_press, "%.2fkPa", pressure); // Converte o dado de pressão em string
        sprintf(str_alt, "%.0fm", altitude);  // Converte o dado de altitude em string
        sprintf(str_temp, "%.1fC", temperature);  // Converte o dado de temperatura (BMP280) em string
        sprintf(str_umi, "%.1f%%", humidity);  // Converte o dado de umidade em string
    
        state_measures(temperature, humidity, pressure); // Indica o estado do sistema pelo LED RGB
        update_matrix(humidity); // Exibe a porcentagem de umidade na matriz de LEDs

        //  Atualiza o conteúdo do display
        ssd1306_fill(&ssd, !cor);                           // Limpa o display
        ssd1306_rect(&ssd, 3, 3, 122, 60, cor, !cor);       // Desenha um retângulo
        
        ssd1306_draw_string(&ssd, "WEA. STATION", 16, 8);
        ssd1306_draw_string(&ssd, "IP:", 6, 16); 
        ssd1306_draw_string(&ssd, ip_str, 32, 16);   // Escreve o IP da placa
        ssd1306_line(&ssd, 3, 26, 123, 26, cor);          

        // Verifica qual a tela a ser exibida
        switch (select_screen){

            case 1: // Tela de Temperatura
                ssd1306_draw_string(&ssd, "TEMP:", 24, 32); 
                ssd1306_draw_string(&ssd, str_temp, 65, 32); 
                break;

            case 2: // Tela de Umidade
                ssd1306_draw_string(&ssd, "HUM:", 24, 32); 
                ssd1306_draw_string(&ssd, str_umi, 57, 32); 
                break;
               
            case 3: // Tela de Pressão Atmosférica
                ssd1306_draw_string(&ssd, "PRESS:", 8, 32); 
                ssd1306_draw_string(&ssd, str_press, 60, 32);
                
                // Exibe altitude aproximada
                ssd1306_draw_string(&ssd, "ALT:", 8, 42); 
                ssd1306_draw_string(&ssd, str_alt, 49, 42); 
                break;   
            
            default: // Tela Geral
                ssd1306_line(&ssd, 63, 25, 63, 60, cor); // Linha Vertical     

                // Exibição de Temperatura
                ssd1306_draw_string(&ssd, "TEMP:", 12, 30); 
                ssd1306_draw_string(&ssd, str_temp, 73, 30); 

                // Exibição de Umidade
                ssd1306_draw_string(&ssd, "HUM:", 12, 40); 
                ssd1306_draw_string(&ssd, str_umi, 73, 40); 

                // Exibição de Pressão Atmosférica
                ssd1306_draw_string(&ssd, "PRESS:", 12, 50); 
                ssd1306_draw_string(&ssd, str_press, 64, 50); 
                break;
        }

        ssd1306_send_data(&ssd); // Atualiza o display
        
        printf("TempMAX: %.1f\n", temp_max_user);
        printf("TempMIN: %.1f\n", temp_min_user);
        printf("HumMAX: %.1f\n", hum_max_user);
        printf("HumMIN: %.1f\n", hum_min_user);
        printf("PressMAX: %.1f\n", press_max_user);
        printf("PressMIN: %.1f\n", press_min_user);
        sleep_ms(500);
    }

    return 0;
}
