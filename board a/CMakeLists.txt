# 「2.4 GHz 遮挡穿透损耗测试」双板工程 —— 顶层 CMake
#
# 一套代码、两个编译宏：
#   板 A：-DAPP_ROLE=AP   -DDISP_KIND=OLED
#   板 B：-DAPP_ROLE=STA  -DDISP_KIND=LCD
#
# 示例：
#   idf.py -B build_a -DAPP_ROLE=AP  -DDISP_KIND=OLED build
#   idf.py -B build_b -DAPP_ROLE=STA -DDISP_KIND=LCD  build
cmake_minimum_required(VERSION 3.16)

# NOTE: every string printed by message() must stay pure ASCII.
# idf.py reads cmake output with the host locale codec (GBK on zh-CN Windows)
# and crashes with UnicodeDecodeError if CMake prints UTF-8 Chinese.
set(APP_ROLE "AP" CACHE STRING "Board role: AP (board A, SoftAP) or STA (board B, station)")
set_property(CACHE APP_ROLE PROPERTY STRINGS AP STA)

set(DISP_KIND "OLED" CACHE STRING "Display kind: OLED (board A, 0.96in SSD1306) or LCD (board B, 1.54in ST7789)")
set_property(CACHE DISP_KIND PROPERTY STRINGS OLED LCD)

if(NOT APP_ROLE MATCHES "^(AP|STA)$")
    message(FATAL_ERROR "APP_ROLE must be AP or STA, got '${APP_ROLE}'")
endif()
if(NOT DISP_KIND MATCHES "^(OLED|LCD)$")
    message(FATAL_ERROR "DISP_KIND must be OLED or LCD, got '${DISP_KIND}'")
endif()

message(STATUS "build config: APP_ROLE=${APP_ROLE}  DISP_KIND=${DISP_KIND}")

# components/ 目录下的组件（components/oled）由 IDF 自动发现，无需 EXTRA_COMPONENT_DIRS。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_link)
