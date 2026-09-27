idf_component_register(
    SRCS "journal_period.cpp" "journal_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_hw_support heap json storage_service
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
