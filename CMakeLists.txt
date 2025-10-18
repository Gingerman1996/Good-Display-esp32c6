cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_definitions(
    LV_CONF_SKIP=1
    LV_FONT_MONTSERRAT_48=1
    LV_FONT_DEFAULT=&lv_font_montserrat_48
)

project(good_display_esp32c6)
