# The watchdog HAL component is hal through IDF 5, and esp_hal_wdt from IDF 6.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(sota_priv_wdt esp_hal_wdt)
else()
    set(sota_priv_wdt hal)
endif()

# src/core is the shared core, vendored as plain files by its sync script.
file(GLOB sota_core_srcs "${CMAKE_CURRENT_LIST_DIR}/src/core/*.c")

idf_component_register(
    SRCS "src/simpleota.c"
         "src/sota_net_idf.c"
         ${sota_core_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "src/core"
    PRIV_REQUIRES esp_http_client
                  mbedtls
                  app_update
                  esp_partition
                  nvs_flash
                  esp_timer
                  esp_hw_support
                  ${sota_priv_wdt}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SOTA_DL_CHUNK=${CONFIG_SIMPLEOTA_DOWNLOAD_CHUNK}u
    SOTA_LOG_LEVEL=${CONFIG_LOG_MAXIMUM_LEVEL})
