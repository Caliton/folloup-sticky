idf_component_register(
    SRCS "journal_sync_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_timer json journal_service mbedtls nvs_flash task_config
             transcription_service
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
