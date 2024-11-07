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
/** Ethernet olaylarını işleyen event handler */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};// MAC adresi için tampon
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;// Ethernet sürücüsünün handle'ı

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);// MAC adresini al
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",// MAC adresini logla
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

/** IP adresi alındığında çağrılan event handler */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;// IP olay verisi
    const esp_netif_ip_info_t *ip_info = &event->ip_info;// IP bilgileri

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));// IP adresi
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask)); // Ağ maskesi
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));// Ağ geçidi
    ESP_LOGI(TAG, "~~~~~~~~~~~");
}

/** TCP sunucu görevi */
void tcp_server_task(void *pvParameters)
{
    char rx_buffer[128];// Alınan mesajı tutmak için tampo
    int addr_family = AF_INET;/*IPv4 adres ailesi */ /*hangi türde bir adresin kullanılacağını belirtir. Örnek IPv4 veya IPv6 */
    int ip_protocol = IPPROTO_IP;// IP protokolü
    
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);// Herhangi bir IP adresinden gelen bağlantıları kabul et
    dest_addr.sin_family = addr_family;
    dest_addr.sin_port = htons(1234); // Sunucunun dinleyeceği port (istemciyle aynı olmalı) port numarası 0 ile 65535 arasında bir değer alabilir

    // Dinleme için socket oluştur
    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Socket oluşturulamadı: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // Socket'i hedef adrese bağla
    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Bağlama işlemi başarısız oldu: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    // Yeni bağlantı bekle
    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Dinleme başlatılamadı: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        ESP_LOGI(TAG, "Yeni bağlantı bekleniyor...");
        struct sockaddr_in source_addr;
        uint addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        
        if (sock < 0) {
            ESP_LOGE(TAG, "Bağlantı kabul edilemedi: errno %d", errno);
            break;
        }

        ESP_LOGI(TAG, "Bağlantı kabul edildi");

        // Bağlantı üzerinden gelen veriyi al
        int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (len > 0) {
            rx_buffer[len] = '\0'; // Null-terminator ekle
            ESP_LOGI(TAG, "Alınan mesaj: %s", rx_buffer);// Mesajı logla
        } else if (len == 0) {
            ESP_LOGI(TAG, "Bağlantı kapatıldı");
        } else {
            ESP_LOGE(TAG, "Mesaj alınamadı: errno %d", errno);
        }

        close(sock); // Bağlantıyı kapat
    }
    
    ESP_LOGI(TAG, "Sunucu görevi sonlandırılıyor");
    close(listen_sock); // Dinleme socket'ini kapat
    vTaskDelete(NULL);
}

void app_main(void)
{
        // GPIO interrupt servisini başlat
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
        // ESP netif başlat
    ESP_ERROR_CHECK(esp_netif_init());
        // Varsayılan event loop oluştur
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();    // Ethernet yapılandırması
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);  // Yeni bir ethernet arayüzü oluştur

    // SPI bus yapılandırması
    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_EXAMPLE_ENC28J60_MISO_GPIO,
        .mosi_io_num = CONFIG_EXAMPLE_ENC28J60_MOSI_GPIO,
        .sclk_io_num = CONFIG_EXAMPLE_ENC28J60_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CONFIG_EXAMPLE_ENC28J60_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // SPI cihaz yapılandırması
    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_EXAMPLE_ENC28J60_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num = CONFIG_EXAMPLE_ENC28J60_CS_GPIO,
        .queue_size = 20,
        .cs_ena_posttrans = enc28j60_cal_spi_cs_hold_time(CONFIG_EXAMPLE_ENC28J60_SPI_CLOCK_MHZ),
    };

    // ENC28J60 Ethernet modülü için yapılandırma
    eth_enc28j60_config_t enc28j60_config = ETH_ENC28J60_DEFAULT_CONFIG(CONFIG_EXAMPLE_ENC28J60_SPI_HOST, &spi_devcfg);
    enc28j60_config.int_gpio_num = CONFIG_EXAMPLE_ENC28J60_INT_GPIO;

    // Ethernet MAC ve PHY yapılandırmaları
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_enc28j60(&enc28j60_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 0;
    phy_config.reset_gpio_num = -1;
    esp_eth_phy_t *phy = esp_eth_phy_new_enc28j60(&phy_config);

    // Ethernet sürücüsü yapılandırması
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    // MAC adresi ayarla
    mac->set_addr(mac, (uint8_t[]) { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 });

    // ENC28J60 chip'in frekansını kontrol et
    if (emac_enc28j60_get_chip_info(mac) < ENC28J60_REV_B5 && CONFIG_EXAMPLE_ENC28J60_SPI_CLOCK_MHZ < 8) {
        ESP_LOGE(TAG, "SPI frequency must be at least 8 MHz for chip revision less than 5");
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    // Ethernet bağlantısını başlat
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

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    // TCP server görevini başlat
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);


}
