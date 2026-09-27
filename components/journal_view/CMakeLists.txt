idf_component_register(
    SRCS "journal_view.cpp"
    INCLUDE_DIRS "include"
    REQUIRES epaper_ui journal_service project_assets
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
