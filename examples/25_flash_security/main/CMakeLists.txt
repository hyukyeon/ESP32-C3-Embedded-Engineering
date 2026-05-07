idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES bootloader_support efuse esp_partition spi_flash
)
