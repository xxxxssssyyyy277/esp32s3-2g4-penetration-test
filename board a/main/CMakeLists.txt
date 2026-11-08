set(app_srcs
    "main.c"
    "console.c"
    "wifi_link.c"
    "ap_measure.c"
    "uart_cmd.c"
    "udp_sink.c"
    "display.c"
)

# 屏后端按 DISP_KIND 选择：本工程只提供 OLED 后端（板 A）。
# 板 B 的 display_lcd.c 由板 B 提示词补全后再加入下面分支。
if(DISP_KIND STREQUAL "OLED")
    list(APPEND app_srcs "display_oled.c")
    set(app_priv_reqs "oled")
else()
    # 板 B 的 LCD 后端（TODO：由板 B 提示词补全 display_lcd.c）
    list(APPEND app_srcs "display_lcd.c")
    set(app_priv_reqs "")
endif()

idf_component_register(
    SRCS ${app_srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${app_priv_reqs}
)

# 把 CMake 缓存变量翻译成 C 编译宏
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    APP_ROLE_${APP_ROLE}
    DISP_KIND_${DISP_KIND}
)

