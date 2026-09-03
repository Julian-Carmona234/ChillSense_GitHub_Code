idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES esp-tls esp_wifi esp_event nvs_flash esp_driver_gpio
    INCLUDE_DIRS "."
    EMBED_TXTFILES "mosquitto.org.crt"
)
