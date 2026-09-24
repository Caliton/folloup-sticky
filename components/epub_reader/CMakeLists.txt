idf_component_register(
    SRCS "epub_zip.cpp" "epub_book.cpp" "epub_html.cpp" "epub_cover.cpp"
    INCLUDE_DIRS "include"
    REQUIRES heap
    PRIV_REQUIRES esp_rom log
)
