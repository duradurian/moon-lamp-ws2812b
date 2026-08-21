cmake_minimum_required(VERSION 3.16.0)

# PIOArduino's custom-sdkconfig pass builds its generated source component from
# .dummy. ESP-Matter looks up the executable component while configuring, so
# publish that name before project() starts component discovery.
set(EXECUTABLE_COMPONENT_NAME ".dummy" CACHE STRING
    "PIOArduino hybrid-build executable component" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_music_sync)
