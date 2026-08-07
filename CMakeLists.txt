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
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent LinguistTools)

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
    include/iq_file_writer.hpp
    src/iq_file_writer.cpp
    src/rx_worker.cpp
    include/signal_plot_widgets.hpp
    src/signal_plot_widgets.cpp
    include/spectrum_processor.hpp
    src/spectrum_processor.cpp
    resources/resources.qrc
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

# Parse the common Qt/STL headers once instead of once per translation unit.
# Disable with -DSIGNAL_CAPTURE_USE_PCH=OFF when diagnosing toolchain issues.
option(SIGNAL_CAPTURE_USE_PCH "Use precompiled headers to shorten C++ builds" ON)
if(SIGNAL_CAPTURE_USE_PCH)
    target_precompile_headers(signal_capture_app PRIVATE
        <QObject>
        <QString>
        <QVector>
        <QWidget>
        <algorithm>
        <memory>
        <vector>
    )
endif()

qt_add_translations(signal_capture_app
    TS_FILES
        translations/signal_capture_app_zh_CN.ts
        translations/signal_capture_app_en.ts
        translations/signal_capture_app_ru.ts
    RESOURCE_PREFIX "/i18n"
)

if(MSVC)
    target_compile_options(signal_capture_app PRIVATE /utf-8)
    target_compile_definitions(signal_capture_app PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

# 编译后将 uhd.dll 复制到 exe 所在目录
add_custom_command(
    TARGET signal_capture_app
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UHD_ROOT}/bin/uhd.dll"
        "$<TARGET_FILE_DIR:signal_capture_app>"
)

# Logo 同时作为可替换外部资源复制到 exe 旁。resources.qrc 中还保留
# 同一文件的内嵌兜底；即使复制步骤未执行，左上角也不会退回占位文字。
add_custom_command(
    TARGET signal_capture_app
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:signal_capture_app>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:signal_capture_app>/assets"
)
