idf_component_register(
    SRCS "src/mdaEPiano.cpp" "src/MdaPianoBank.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common heap log esp_rom freertos
)