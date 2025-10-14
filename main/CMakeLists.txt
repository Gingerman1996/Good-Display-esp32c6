idf_build_get_property(target IDF_TARGET)

set(_main_sources
    "main.cpp"
)

idf_component_register(
    SRCS ${_main_sources}
    INCLUDE_DIRS
        "."
    REQUIRES
        gde_display
)
