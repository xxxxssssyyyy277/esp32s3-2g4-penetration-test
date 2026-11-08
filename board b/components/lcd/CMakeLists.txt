# 1.54in ST7789 240x240 LCD driver (4-wire software SPI + frame buffer).
# Only the four files required by this project are compiled here; the 258 KB
# cat animation (LCD_CatFrames.c) is deliberately NOT copied.
idf_component_register(
    SRCS "LCD.c" "LCD_Data.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)
