idf_build_get_property(target IDF_TARGET)

set(_main_sources
    "main.cpp"
    "epd_driver.cpp"
    "ft6336.cpp"
    "assets.cpp"
)

set(_tinyusb_supported_targets esp32s2 esp32s3 esp32c3 esp32h2 esp32p4)
list(FIND _tinyusb_supported_targets "${target}" _tinyusb_index)

if(_tinyusb_index EQUAL -1)
    list(APPEND _main_sources "usb_cdc_stub.cpp")
    idf_component_register(
        SRCS ${_main_sources}
        INCLUDE_DIRS
            "."
    )
else()
    list(APPEND _main_sources "usb_cdc.cpp")
    idf_component_register(
        SRCS ${_main_sources}
        INCLUDE_DIRS
            "."
        PRIV_REQUIRES
            tinyusb
    )
endif()
