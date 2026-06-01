/**
 * @file lv_conf.h
 * Configuration file for LVGL v8.3.11
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#if 1 /* Set to 1 to enable the configuration */

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
/* Color depth: 16 (RGB565) is perfect for ESP32 and ILI9341 */
#define LV_COLOR_DEPTH 16

/* Swap the 2 bytes of RGB565 color. Needed for TFT_eSPI SPI transmission */
#define LV_COLOR_16_SWAP 0

/*====================
   MEMORY SETTINGS
 *====================*/
/* Size of the memory used by `lv_mem_alloc` in bytes (48 KBytes) */
#define LV_MEM_SIZE (48U * 1024U)

/* Set an address where the memory pool should be placed. 0: allocate dynamically */
#define LV_MEM_ADR 0

/*====================
   HAL SETTINGS
 *====================*/
/* Use a custom tick source (millis() from Arduino is extremely reliable on ESP32) */
#define LV_TICK_CUSTOM 1
#if LV_TICK_CUSTOM
    #define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

/* Default display refresh period in milliseconds */
#define LV_DISP_DEF_REFR_PERIOD 30

/* Input device read period in milliseconds */
#define LV_INDEV_DEF_READ_PERIOD 30

/*====================
   FEATURE USAGE
 *====================*/
/* Enable basic fonts */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1

/* Use the default font */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*====================
   THEME USAGE
 *====================*/
/* Enable the default active theme (Material-like theme) */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1 /* Use Dark Mode by default for a modern look */
#define LV_THEME_DEFAULT_GROW 1 /* Buttons and items grow slightly when focused */

/*====================
   WIDGETS & SYSTEM
 *====================*/
/* Enable basic widget classes */
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1

#endif /* End of LV_CONF_H */
#endif
