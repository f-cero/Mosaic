idf_component_register(
  SRCS "ms_port_esp32.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_lcd driver esp_wifi nvs_flash fatfs sdmmc bt)
