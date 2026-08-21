set(EXECUTABLE_COMPONENT_NAME ".dummy" CACHE STRING
    "PIOArduino hybrid-build executable component" FORCE)

# ESP-Matter 1.4.1 targets C++20. GCC 14's C++23 mode exposes a bug in this
# release's std::to_underlying compatibility branch. Replace IDF 5.5's global
# C++23 option instead of merely appending C++20: PlatformIO sorts extracted
# CMake flags and would otherwise place C++23 last during its SCons pass.
if(COMMAND idf_build_set_property)
    idf_build_get_property(cxx_compile_options CXX_COMPILE_OPTIONS)
    list(FILTER cxx_compile_options EXCLUDE REGEX "^-std=gnu\\+\\+")
    list(APPEND cxx_compile_options
        "-std=gnu++2a" "-Os" "-DCHIP_HAVE_CONFIG_H")
    idf_build_set_property(CXX_COMPILE_OPTIONS "${cxx_compile_options}")
endif()

idf_component_register(SRCS "sketch.cpp" "arduino-lib-builder-gcc.c" "arduino-lib-builder-cpp.cpp" "arduino-lib-builder-as.S" INCLUDE_DIRS ".")
