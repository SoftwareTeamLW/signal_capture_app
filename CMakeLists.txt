cmake_minimum_required(VERSION 3.20)

project(signal_capture_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# UHD 和 Boost 安装目录
set(UHD_ROOT "C:/Program Files/UHD" CACHE PATH "UHD installation directory")
set(BOOST_ROOT "C:/local/boost_1_85_0" CACHE PATH "Boost installation directory")

if(NOT EXISTS "${BOOST_ROOT}/boost/config.hpp")
    message(FATAL_ERROR "Boost headers were not found under: ${BOOST_ROOT}")
endif()

if(NOT EXISTS "${UHD_ROOT}/include/uhd/usrp/multi_usrp.hpp")
    message(FATAL_ERROR "UHD header files were not found under: ${UHD_ROOT}")
endif()

if(NOT EXISTS "${UHD_ROOT}/lib/uhd.lib")
    message(FATAL_ERROR "UHD library was not found under: ${UHD_ROOT}")
endif()

# 手动创建 UHD 导入目标
add_library(UHD::uhd SHARED IMPORTED)

set_target_properties(UHD::uhd PROPERTIES
    IMPORTED_IMPLIB "${UHD_ROOT}/lib/uhd.lib"
    IMPORTED_LOCATION "${UHD_ROOT}/bin/uhd.dll"
    INTERFACE_INCLUDE_DIRECTORIES
    "${UHD_ROOT}/include;${BOOST_ROOT}"
)

add_executable(signal_capture_app
    src/main.cpp
)

target_link_libraries(signal_capture_app
    PRIVATE
        UHD::uhd
)

if(MSVC)
    target_compile_options(signal_capture_app PRIVATE /utf-8)
endif()

# 编译后将 uhd.dll 复制到 exe 所在目录
add_custom_command(
    TARGET signal_capture_app
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UHD_ROOT}/bin/uhd.dll"
        "$<TARGET_FILE_DIR:signal_capture_app>"
)