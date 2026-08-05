cmake_minimum_required(VERSION 3.20)

project(signal_capture_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 启用 Qt 的自动代码生成机制
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_AUTOUIC_SEARCH_PATHS
    "${CMAKE_CURRENT_SOURCE_DIR}/ui"
)

# 查找 Qt Widgets 模块
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

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
    ui/main_window.ui
    include/ui/main_window.hpp
    src/ui/main_window.cpp
    include/sdr_device.hpp
    src/sdr_device.cpp
    include/rx_receiver.hpp
    include/rx_config.hpp
    src/rx_worker.cpp
    include/signal_plot_widgets.hpp
    src/signal_plot_widgets.cpp
    include/spectrum_processor.hpp
    src/spectrum_processor.cpp
)

target_include_directories(signal_capture_app
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(signal_capture_app
    PRIVATE
        Qt6::Widgets
        Qt6::Concurrent
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

# Logo 作为可替换的外部资源复制到 exe 旁。将 PNG 命名为
# assets/company_logo.png 后重新构建即可，无需修改 C++ 或 UI。
add_custom_command(
    TARGET signal_capture_app
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:signal_capture_app>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:signal_capture_app>/assets"
)
