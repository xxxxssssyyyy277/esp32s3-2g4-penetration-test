idf_component_register(
    SRCS "OLED.c" "OLED_Data.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver
)

# 诊断开关：cmake -DOLED_SCAN_ALL=1（或 idf.py -DOLED_SCAN_ALL=1 build）
# 打开后编译进引脚两两扫描，用来定位「屏到底接在哪两个脚上」。
if(OLED_SCAN_ALL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "OLED_SCAN_ALL=1")
    message(STATUS "OLED: pin-pair scan enabled (diagnostic build)")
endif()
