idf_component_register(SRCS "tiny_soundfont.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES heap)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
