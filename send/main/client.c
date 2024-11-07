#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_eth_enc28j60.h"
#include "driver/spi_master.h"
#include "lwip/sockets.h"  // TCP/IP işlemleri için eklenen kütüphane

static const char *TAG = "eth_example";

#define SERVER_IP "1192.168.1.100" // Hedef cihazın IP adresi
#define SERVER_PORT 1234         // Bağlanılacak port

/** Ethernet olayları için event handler (olay işleyici) */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0}; // MAC adresi için dizi tanımlanır
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data; // Ethernet işleyicisi

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

/* IP_EVENT_ETH_GOT_IP olayını işleyen event handler */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info; // IP bilgisi

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw)); // Gateway adresini yazdır
    ESP_LOGI(TAG, "~~~~~~~~~~~");
}

/** TCP istemci fonksiyonu */
void tcp_client_task(void *pvParameters)
{
    char *payload = "HELLO"; // Gönderilecek mesaj
    while (1) {
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);// Hedef IP adresini ayarla
        dest_addr.sin_family = AF_INET; // IPv4 adres ailesi
        dest_addr.sin_port = htons(SERVER_PORT); // Hedef port numarasını ayarla

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP); // TCP soketi oluştur
        if (sock < 0) {
            ESP_LOGE(TAG, "Socket oluşturulamadı: errno %d", errno);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "Sunucuya bağlanmaya çalışılıyor...");
        int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err != 0) {
            ESP_LOGE(TAG, "Bağlantı başarısız: errno %d", errno);
            close(sock);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "Bağlantı başarılı, mesaj gönderiliyor...");
        int sent = send(sock, payload, strlen(payload), 0);
        if (sent < 0) {
            ESP_LOGE(TAG, "Mesaj gönderilemedi: errno %d", errno);
        } else {
            ESP_LOGI(TAG, "Mesaj gönderildi: %s", payload);
        }

        ESP_LOGI(TAG, "Bağlantı kapatılıyor...");
        close(sock);

        vTaskDelay(5000 / portTICK_PERIOD_MS); // Mesajı her 5 saniyede bir göndermek için
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(gpio_install_isr_service(0));  // GPIO kesme hizmetini başlat
    ESP_ERROR_CHECK(esp_netif_init());  // Ağ arabirimi yapılandırmasını başlat
    ESP_ERROR_CHECK(esp_event_loop_create_default());  // Varsayılan olay döngüsünü başlat
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();  // Ethernet yapılandırması
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);  // Yeni ağ arabirimi oluştur

   // SPI ayarları
    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_EXAMPLE_ENC28J60_MISO_GPIO,  // SPI MISO pin'i
        .mosi_io_num = CONFIG_EXAMPLE_ENC28J60_MOSI_GPIO,  // SPI MOSI pin'i
        .sclk_io_num = CONFIG_EXAMPLE_ENC28J60_SCLK_GPIO,  // SPI SCLK pin'i
        .quadwp_io_num = -1,  // Quad SPI yazma pin'i
        .quadhd_io_num = -1,  // Quad SPI okuma pin'i
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CONFIG_EXAMPLE_ENC28J60_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // SPI cihaz yapılandırması
    spi_device_interface_config_t spi_devcfg = {
        .mode = 0, // SPI modunu ayarla
        .clock_speed_hz = CONFIG_EXAMPLE_ENC28J60_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num = CONFIG_EXAMPLE_ENC28J60_CS_GPIO, // SPI Chip Select pini
        .queue_size = 20,
        .cs_ena_posttrans = enc28j60_cal_spi_cs_hold_time(CONFIG_EXAMPLE_ENC28J60_SPI_CLOCK_MHZ),// SPI Chip Select tutma süresi
    };

    // ENC28J60 yapılandırması
    eth_enc28j60_config_t enc28j60_config = ETH_ENC28J60_DEFAULT_CONFIG(CONFIG_EXAMPLE_ENC28J60_SPI_HOST, &spi_devcfg);
    enc28j60_config.int_gpio_num = CONFIG_EXAMPLE_ENC28J60_INT_GPIO; // Interrupt pini

    // MAC yapılandırması
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_enc28j60(&enc28j60_config, &mac_config);

    // PHY yapılandırması
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 0;  // Otomatik pazarlık zaman aşımını ayarla
    phy_config.reset_gpio_num = -1;  // PHY sıfırlama pini
    esp_eth_phy_t *phy = esp_eth_phy_new_enc28j60(&phy_config);  // PHY oluştur

    // Ethernet yapılandırması
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));// Ethernet sürücüsünü kur

    mac->set_addr(mac, (uint8_t[]) { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 });// MAC adresini ayarla

    // ENC28J60 çip revizyon kontrolü
    if (emac_enc28j60_get_chip_info(mac) < ENC28J60_REV_B5 && CONFIG_EXAMPLE_ENC28J60_SPI_CLOCK_MHZ < 8) {
        ESP_LOGE(TAG, "SPI frequency must be at least 8 MHz for chip revision less than 5");
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

  /*Ethernet bağlantısı Full-Duplex modunda çalıştırılacaktır.Full-duplex modunun avantajı,
    veri iletimi ve alımının aynı anda yapılabilmesidir,
    bu da ağ bağlantısının daha hızlı ve verimli olmasını sağlar.*/ 
#if CONFIG_EXAMPLE_ENC28J60_DUPLEX_FULL
    eth_duplex_t duplex = ETH_DUPLEX_FULL;
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_DUPLEX_MODE, &duplex));
#endif

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));// Ethernet sürücüsünü başlat

    // TCP istemci görevini başlat
    xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);



}
