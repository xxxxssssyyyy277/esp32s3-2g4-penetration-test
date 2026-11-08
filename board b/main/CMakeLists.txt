set(app_srcs
    "main.c"
    "console.c"
    "wifi_link.c"
    "measure.c"
    "uart_cmd.c"
    "udp_keepalive.c"
    "display.c"
    "rgb_led.c"
)

# IMPORTANT: component requirements must NOT depend on DISP_KIND / APP_ROLE.
# ESP-IDF expands requirements in a separate "script mode" CMake pass
# (tools/cmake/scripts/component_get_requirements.cmake) where those cache
# variables are not defined. A conditional PRIV_REQUIRES therefore silently
# loses the component and the build fails with "LCD.h: No such file or directory".
# Listing "lcd" unconditionally is safe: for the OLED placeholder build the
# library is simply never referenced by the linker.
set(app_priv_reqs "lcd" "driver" "esp_wifi" "esp_netif" "esp_event" "lwip" "nvs_flash" "esp_timer")

# Only ONE display backend may be linked: LCD frame buffer is 115 KB of internal
# RAM, the OLED frame buffer is 1 KB. Linking both wastes 116 KB for nothing.
if(DISP_KIND STREQUAL "LCD")
    list(APPEND app_srcs "display_lcd.c" "backlight.c")
else()
    # TODO: 由板 A 提示词补全 OLED 后端（本工程只保留可编译占位）
    list(APPEND app_srcs "display_oled.c")
endif()

idf_component_register(
    SRCS ${app_srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${app_priv_reqs}
)

# Translate the two CMake cache variables into C compile definitions.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    APP_ROLE_${APP_ROLE}
    DISP_KIND_${DISP_KIND}
)
