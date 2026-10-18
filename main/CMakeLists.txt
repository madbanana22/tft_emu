idf_component_register(
    SRCS
        "main.c"
        "nvs_store.c"
        "panel_fb.c"
        "proto_st7735.c"
        "spi_capture.c"
        "wifi_mgr.c"
        "web_ui.c"
        "test_gen.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_spi esp_driver_gpio nvs_flash esp_wifi esp_event esp_netif esp_http_server esp_timer
    PRIV_REQUIRES mbedtls
    EMBED_FILES "www/index.html"
)
