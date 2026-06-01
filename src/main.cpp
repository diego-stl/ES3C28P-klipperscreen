#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <lvgl.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"

// Instancia del controlador de la pantalla
TFT_eSPI tft = TFT_eSPI();

// Pines del Hardware
#define TFT_BL 45         // Retroiluminación (Backlight)
#define TP_SDA 16         // I2C SDA para panel táctil
#define TP_SCL 15         // I2C SCL para panel táctil
#define TP_RST 18         // Pin de Reset del panel táctil
#define I2C_ADDR_FT6336 0x38 // Dirección I2C del panel táctil

// Buffers para LVGL
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[320 * 10]; // Buffer para 10 líneas de la pantalla

// Variables de Estado del Servidor Klipper Impresora 3D
static float pos_x = 125.0;
static float pos_y = 98.4;
static float pos_z = 0.20;
static float move_step = 10.0;

static int nozzle_temp = 0;
static int nozzle_target = 0;
static int bed_temp = 0;
static int bed_target = 0;

static int print_progress = 0;
static String current_gcode_file = "Ninguno";

// Variables de Configuración de Pantalla
static int screen_brightness = 100; // Porcentaje de brillo (0-100)
static bool colors_inverted = true;

// Contenedores globales de la UI
static lv_obj_t *content_area;
static lv_obj_t *btn_status;
static lv_obj_t *btn_move;
static lv_obj_t *btn_temp;
static lv_obj_t *btn_config;
static lv_obj_t *btn_files;

// Punteros a elementos de la UI para actualización dinámica en tiempo real
static lv_obj_t *lbl_badge = NULL;
static lv_obj_t *lbl_file = NULL;
static lv_obj_t *bar_prog = NULL;
static lv_obj_t *lbl_prog = NULL;
static lv_obj_t *lbl_time = NULL;
static lv_obj_t *lbl_noz = NULL;
static lv_obj_t *lbl_bed = NULL;
static lv_obj_t *coord_label = NULL;
static lv_obj_t *lbl_wifi_state = NULL;
static lv_obj_t *lbl_printer_state = NULL;
static lv_obj_t *lbl_gui_pause = NULL;
static lv_obj_t *lbl_gui_estop = NULL;

// Estado de Conectividad y HTTP Polling
bool wifi_connected = false;
bool ws_connected = false; // Se mantiene con true/false para representar la conexión HTTP exitosa a Moonraker
static String klipper_state = "ready";
static String klipper_state_message = "";
static String last_print_state = "";
static unsigned long print_duration = 0;
static unsigned long total_duration = 0;

// Variables de Configuración Dinámica (modificables vía teclado)
static String wifi_ssid_str = WIFI_SSID;
static String wifi_pass_str = WIFI_PASSWORD;
static String moonraker_host_str = MOONRAKER_HOST;
static int moonraker_port_int = MOONRAKER_PORT;
static bool reconnect_requested = false;

// Paleta de Colores Klipper Oscura y Rojiza (Cybertech / Dark Crimson)
#define COLOR_BG        lv_color_make(10, 10, 15)     // Fondo Negro/Carbón Profundo
#define COLOR_SIDEBAR   lv_color_make(18, 18, 24)     // Fondo Barra Lateral Oscura
#define COLOR_CARD      lv_color_make(24, 24, 34)     // Fondo Tarjetas
#define COLOR_RED_ACC   lv_color_make(220, 20, 60)    // Rojo Carmesí Brillante (Crimson)
#define COLOR_RED_DARK  lv_color_make(130, 10, 30)    // Rojo Oscuro / Sangre
#define COLOR_GREY_TEXT lv_color_make(140, 145, 160)  // Texto Secundario
#define COLOR_GREEN     lv_color_make(39, 174, 96)    // Verde listo

/*---------------------------------------------------------------------------
 * DECLARACIÓN DE PROTOTIPOS DE INTERFAZ
 *---------------------------------------------------------------------------*/
void create_status_screen(lv_obj_t *parent);
void create_move_screen(lv_obj_t *parent);
void create_temp_screen(lv_obj_t *parent);
void create_config_screen(lv_obj_t *parent);
void create_files_screen(lv_obj_t *parent);
void update_sidebar_selection(int active_tab);
void update_status_display();
static void update_temp_display();
void sendGcodeToMoonraker(const String &gcode);
void create_cyd_klipper_ui();
void refresh_status_screen();
static void restart_event_cb(lv_event_t *e);
String sendHttpRequest(const String& method, const String& path);
void create_filament_popup();
static void filament_event_cb(lv_event_t *e);
String formatTime(unsigned long seconds);
static void action_confirm_cb(lv_event_t *e);
static void action_cancel_cb(lv_event_t *e);
static void create_confirm_dialog(const char *title_text, const char *msg_text, long action_type);
static void gui_pause_estop_cb(lv_event_t *e);

/*---------------------------------------------------------------------------
 * HARDWARE CALLBACKS PARA LVGL
 *---------------------------------------------------------------------------*/
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  uint8_t i2cdat[16];
  bool touched = false;
  uint16_t rawX = 0, rawY = 0;

  Wire.beginTransmission(I2C_ADDR_FT6336);
  Wire.write((byte)0);
  if (Wire.endTransmission() == 0) {
    Wire.requestFrom((byte)I2C_ADDR_FT6336, (byte)16);
    for (uint8_t i = 0; i < 16; i++) {
      i2cdat[i] = Wire.available() ? Wire.read() : 0;
    }
    uint8_t touches = i2cdat[0x02] & 0x0F;
    if (touches > 0 && touches <= 2) {
      touched = true;
      rawX = ((i2cdat[0x03] & 0x0F) << 8) | i2cdat[0x04];
      rawY = ((i2cdat[0x05] & 0x0F) << 8) | i2cdat[0x06];
    }
  }

  if (touched) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = rawY;
    data->point.y = 240 - rawX;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

/*---------------------------------------------------------------------------
 * EVENTOS DE NAVEGACIÓN
 *---------------------------------------------------------------------------*/
static void nav_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_clean(content_area); // Limpiar pantalla actual
    lbl_badge = NULL;
    lbl_file = NULL;
    bar_prog = NULL;
    lbl_prog = NULL;
    lbl_time = NULL;
    lbl_noz = NULL;
    lbl_bed = NULL;
    coord_label = NULL;
    lbl_wifi_state = NULL;
    lbl_printer_state = NULL;
    lbl_gui_pause = NULL;
    lbl_gui_estop = NULL;

    if (btn == btn_status) {
      update_sidebar_selection(0);
      create_status_screen(content_area);
    } else if (btn == btn_move) {
      update_sidebar_selection(1);
      create_move_screen(content_area);
    } else if (btn == btn_temp) {
      update_sidebar_selection(2);
      create_temp_screen(content_area);
    } else if (btn == btn_config) {
      update_sidebar_selection(3);
      create_config_screen(content_area);
    } else if (btn == btn_files) {
      update_sidebar_selection(4);
      create_files_screen(content_area);
    }
  }
}

void update_sidebar_selection(int active_tab) {
  // Resetear estilos a inactivos (Fondo barra lateral oscuro)
  lv_obj_set_style_bg_color(btn_status, COLOR_SIDEBAR, 0);
  lv_obj_set_style_bg_color(btn_move, COLOR_SIDEBAR, 0);
  lv_obj_set_style_bg_color(btn_temp, COLOR_SIDEBAR, 0);
  lv_obj_set_style_bg_color(btn_config, COLOR_SIDEBAR, 0);
  lv_obj_set_style_bg_color(btn_files, COLOR_SIDEBAR, 0);

  // Pintar el botón activo en rojo carmesí brillante
  if (active_tab == 0) lv_obj_set_style_bg_color(btn_status, COLOR_RED_ACC, 0);
  if (active_tab == 1) lv_obj_set_style_bg_color(btn_move, COLOR_RED_ACC, 0);
  if (active_tab == 2) lv_obj_set_style_bg_color(btn_temp, COLOR_RED_ACC, 0);
  if (active_tab == 3) lv_obj_set_style_bg_color(btn_config, COLOR_RED_ACC, 0);
  if (active_tab == 4) lv_obj_set_style_bg_color(btn_files, COLOR_RED_ACC, 0);
}

void refresh_status_screen() {
  if (btn_status != NULL && lv_obj_get_style_bg_color(btn_status, 0).full == COLOR_RED_ACC.full) {
    lv_obj_clean(content_area);
    lbl_badge = NULL;
    lbl_file = NULL;
    bar_prog = NULL;
    lbl_prog = NULL;
    lbl_time = NULL;
    lbl_gui_pause = NULL;
    lbl_gui_estop = NULL;
    create_status_screen(content_area);
  }
}

static void overlay_click_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    lv_obj_t *overlay = lv_event_get_target(e);
    lv_obj_del(overlay);
  }
}

static void create_toast(const char *title, const char *text) {
  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, 320, 240);
  lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_make(0, 0, 0), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_set_style_radius(overlay, 0, 0);
  lv_obj_add_event_cb(overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *mbox = lv_msgbox_create(overlay, title, text, NULL, false);
  lv_obj_align(mbox, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(mbox, COLOR_CARD, 0);
  lv_obj_set_style_border_color(mbox, COLOR_RED_ACC, 0);
  lv_obj_set_style_border_width(mbox, 1, 0);
  
  // Propagar clics del msgbox al overlay padre
  lv_obj_add_flag(mbox, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(mbox, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void restart_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    long type = (long)lv_event_get_user_data(e);
    if (type == 1) {
      Serial.println("[Moonraker] Enviando RESTART...");
      sendHttpRequest("POST", "/printer/restart");
    } else if (type == 2) {
      Serial.println("[Moonraker] Enviando FIRMWARE_RESTART...");
      sendHttpRequest("POST", "/printer/firmware_restart");
    }
    
    // Crear toast con cierre al tocar en cualquier lado de la pantalla
    create_toast("Klipper", "Enviando comando de reinicio...");
  }
}

static void filament_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    long act = (long)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    // El parent del parent del botón es el overlay
    lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(btn));
    
    if (act == 1) { // Retraer
      Serial.println("[Klipper] Retrayendo filamento...");
      sendGcodeToMoonraker("M83\nG1 E-50 F1200"); // Retraer 50mm
    } else if (act == 2) { // Extruir
      Serial.println("[Klipper] Extruyendo filamento...");
      sendGcodeToMoonraker("M83\nG1 E50 F300");  // Extruir 50mm
    } else if (act == 3) { // Continuar
      Serial.println("[Klipper] Reanudando impresion...");
      sendGcodeToMoonraker("RESUME");
      lv_obj_del(overlay);
    } else if (act == 4) { // Cerrar
      lv_obj_del(overlay);
    }
  }
}

String formatTime(unsigned long seconds) {
  unsigned long h = seconds / 3600;
  unsigned long m = (seconds % 3600) / 60;
  unsigned long s = seconds % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h, m, s);
  return String(buf);
}

static void action_confirm_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    long action = (long)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    // El parent del parent del botón es el overlay
    lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(btn));
    
    if (action == 1) { // Pausar/Reanudar
      if (last_print_state == "paused") {
        Serial.println("[Klipper] Confirmado: REANUDAR");
        sendGcodeToMoonraker("RESUME");
      } else {
        Serial.println("[Klipper] Confirmado: PAUSAR");
        sendGcodeToMoonraker("PAUSE");
      }
    } else if (action == 2) { // Parada de emergencia / Cancelar Impresion
      if (last_print_state == "paused") {
        Serial.println("[Klipper] Confirmado: CANCELAR IMPRESION");
        sendGcodeToMoonraker("CANCEL_PRINT");
      } else {
        Serial.println("[Klipper] Confirmado: PARADA DE EMERGENCIA");
        sendHttpRequest("POST", "/printer/emergency_stop");
      }
    }
    
    // Borrar el dialog overlay
    if (lv_obj_is_valid(overlay)) {
      lv_obj_del(overlay);
    }
  }
}

static void action_cancel_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *overlay = lv_obj_get_parent(lv_obj_get_parent(btn));
    if (lv_obj_is_valid(overlay)) {
      lv_obj_del(overlay);
    }
  }
}

static void create_confirm_dialog(const char *title_text, const char *msg_text, long action_type) {
  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, 320, 240);
  lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_make(0, 0, 0), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_set_style_radius(overlay, 0, 0);

  // Tarjeta central
  lv_obj_t *card = lv_obj_create(overlay);
  lv_obj_set_size(card, 220, 160);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
  lv_obj_set_style_border_color(card, COLOR_RED_ACC, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_pad_all(card, 8, 0);

  // Título
  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, title_text);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(title, COLOR_RED_ACC, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  // Mensaje
  lv_obj_t *msg = lv_label_create(card);
  lv_label_set_text(msg, msg_text);
  lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(msg, lv_color_make(255, 255, 255), 0);
  lv_obj_set_width(msg, 200);
  lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
  lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 24);

  // Botón Aceptar
  lv_obj_t *btn_ok = lv_btn_create(card);
  lv_obj_set_size(btn_ok, 90, 36);
  lv_obj_align(btn_ok, LV_ALIGN_BOTTOM_LEFT, 2, -2);
  lv_obj_set_style_bg_color(btn_ok, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(btn_ok, 4, 0);
  lv_obj_add_event_cb(btn_ok, action_confirm_cb, LV_EVENT_CLICKED, (void*)action_type);

  lv_obj_t *lbl_ok = lv_label_create(btn_ok);
  lv_label_set_text(lbl_ok, "Aceptar");
  lv_obj_set_style_text_font(lbl_ok, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_ok, LV_ALIGN_CENTER, 0, 0);

  // Botón Cancelar
  lv_obj_t *btn_cancel = lv_btn_create(card);
  lv_obj_set_size(btn_cancel, 90, 36);
  lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
  lv_obj_set_style_bg_color(btn_cancel, COLOR_SIDEBAR, 0);
  lv_obj_set_style_radius(btn_cancel, 4, 0);
  lv_obj_add_event_cb(btn_cancel, action_cancel_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
  lv_label_set_text(lbl_cancel, "Cancelar");
  lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_cancel, LV_ALIGN_CENTER, 0, 0);
}

static void gui_pause_estop_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    long act = (long)lv_event_get_user_data(e);
    if (act == 1) { // Click en Pausar/Reanudar
      if (last_print_state == "paused") {
        create_confirm_dialog("REANUDAR IMPRESIÓN", "¿Desea REANUDAR la impresion?", 1);
      } else {
        create_confirm_dialog("PAUSAR IMPRESIÓN", "¿Desea PAUSAR la impresion?", 1);
      }
    } else if (act == 2) { // Click en Parada de Emergencia / Cancelar Impresion
      if (last_print_state == "paused") {
        create_confirm_dialog("CANCELAR IMPRESIÓN", "¿Desea CANCELAR la impresion?", 2);
      } else {
        create_confirm_dialog("¡ALERTA DE PARADA!", "¿Desea realizar una PARADA DE EMERGENCIA?", 2);
      }
    }
  }
}

void create_filament_popup() {
  lv_obj_t *overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(overlay, 320, 240);
  lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_make(0, 0, 0), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_set_style_radius(overlay, 0, 0);

  // Tarjeta central
  lv_obj_t *card = lv_obj_create(overlay);
  lv_obj_set_size(card, 230, 195);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
  lv_obj_set_style_border_color(card, COLOR_RED_ACC, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_pad_all(card, 8, 0);

  String title_str = "IMPRESORA PAUSADA";
  String msg_str = "Impresora Pausada.\n¿Que deseas hacer?";

  // Título
  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, title_str.c_str());
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(title, COLOR_RED_ACC, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  // Mensaje descriptivo
  lv_obj_t *msg = lv_label_create(card);
  lv_label_set_text(msg, msg_str.c_str());
  lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(msg, lv_color_make(255, 255, 255), 0);
  lv_obj_set_width(msg, 210);
  lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
  lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 24);

  // Botón Retraer
  lv_obj_t *btn_retract = lv_btn_create(card);
  lv_obj_set_size(btn_retract, 98, 38);
  lv_obj_align(btn_retract, LV_ALIGN_TOP_LEFT, 2, 70);
  lv_obj_set_style_bg_color(btn_retract, COLOR_SIDEBAR, 0);
  lv_obj_set_style_border_color(btn_retract, COLOR_RED_ACC, 0);
  lv_obj_set_style_border_width(btn_retract, 1, 0);
  lv_obj_set_style_radius(btn_retract, 4, 0);
  lv_obj_add_event_cb(btn_retract, filament_event_cb, LV_EVENT_CLICKED, (void*)1);

  lv_obj_t *lbl_retract = lv_label_create(btn_retract);
  lv_label_set_text(lbl_retract, "Retraer 50mm");
  lv_obj_set_style_text_font(lbl_retract, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_retract, LV_ALIGN_CENTER, 0, 0);

  // Botón Extruir
  lv_obj_t *btn_extrude = lv_btn_create(card);
  lv_obj_set_size(btn_extrude, 98, 38);
  lv_obj_align(btn_extrude, LV_ALIGN_TOP_RIGHT, -2, 70);
  lv_obj_set_style_bg_color(btn_extrude, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(btn_extrude, 4, 0);
  lv_obj_add_event_cb(btn_extrude, filament_event_cb, LV_EVENT_CLICKED, (void*)2);

  lv_obj_t *lbl_extrude = lv_label_create(btn_extrude);
  lv_label_set_text(lbl_extrude, "Extruir 50mm");
  lv_obj_set_style_text_font(lbl_extrude, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_extrude, LV_ALIGN_CENTER, 0, 0);

  // Botón Continuar (Resume)
  lv_obj_t *btn_resume = lv_btn_create(card);
  lv_obj_set_size(btn_resume, 98, 38);
  lv_obj_align(btn_resume, LV_ALIGN_BOTTOM_LEFT, 2, -2);
  lv_obj_set_style_bg_color(btn_resume, COLOR_GREEN, 0);
  lv_obj_set_style_radius(btn_resume, 4, 0);
  lv_obj_add_event_cb(btn_resume, filament_event_cb, LV_EVENT_CLICKED, (void*)3);

  lv_obj_t *lbl_resume = lv_label_create(btn_resume);
  lv_label_set_text(lbl_resume, "Reanudar");
  lv_obj_set_style_text_font(lbl_resume, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_resume, LV_ALIGN_CENTER, 0, 0);

  // Botón Cerrar (Dismiss)
  lv_obj_t *btn_close = lv_btn_create(card);
  lv_obj_set_size(btn_close, 98, 38);
  lv_obj_align(btn_close, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
  lv_obj_set_style_bg_color(btn_close, COLOR_SIDEBAR, 0);
  lv_obj_set_style_radius(btn_close, 4, 0);
  lv_obj_add_event_cb(btn_close, filament_event_cb, LV_EVENT_CLICKED, (void*)4);

  lv_obj_t *lbl_close = lv_label_create(btn_close);
  lv_label_set_text(lbl_close, "Cerrar");
  lv_obj_set_style_text_font(lbl_close, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_close, LV_ALIGN_CENTER, 0, 0);
}

void update_status_display() {
  if (lbl_file != NULL) {
    lv_label_set_text_fmt(lbl_file, "Archivo: %s", current_gcode_file.c_str());
  }
  if (bar_prog != NULL) {
    lv_bar_set_value(bar_prog, print_progress, LV_ANIM_OFF);
  }
  if (lbl_prog != NULL) {
    lv_label_set_text_fmt(lbl_prog, "Progreso: %d%%", print_progress);
  }
  if (lbl_time != NULL) {
    String elapsedStr = formatTime(print_duration);
    String totalStr = formatTime(total_duration);
    lv_label_set_text_fmt(lbl_time, "T: %s / E: %s", elapsedStr.c_str(), totalStr.c_str());
  }
  if (lbl_gui_pause != NULL) {
    if (last_print_state == "paused") {
      lv_label_set_text(lbl_gui_pause, "Reanudar");
    } else {
      lv_label_set_text(lbl_gui_pause, "Pausar");
    }
  }
  if (lbl_gui_estop != NULL) {
    if (last_print_state == "paused") {
      lv_label_set_text(lbl_gui_estop, "Cancelar");
    } else {
      lv_label_set_text(lbl_gui_estop, "Parar (ESTOP)");
    }
  }
}

void create_status_screen(lv_obj_t *parent) {
  // Tarjeta de Estado principal, posicionada en la parte superior
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, 215, 150);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 5);
  lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE); // Eliminar barra de scroll de la tarjeta

  // Badge de Estado Impresión (Rojo Carmesí / Verde / Gris)
  lv_obj_t *badge = lv_obj_create(card);
  lv_obj_set_size(badge, 199, 28);
  lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_border_width(badge, 0, 0);
  lv_obj_set_style_radius(badge, 4, 0);
  lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE); // Eliminar barra de scroll del badge

  lbl_badge = lv_label_create(badge);
  if (!ws_connected) {
    lv_label_set_text(lbl_badge, "SIN CONEXION");
    lv_obj_set_style_bg_color(badge, COLOR_RED_DARK, 0);
  } else if (klipper_state == "error") {
    lv_label_set_text(lbl_badge, "ERROR MCU");
    lv_obj_set_style_bg_color(badge, COLOR_RED_ACC, 0);
  } else if (klipper_state == "shutdown") {
    lv_label_set_text(lbl_badge, "SHUTDOWN");
    lv_obj_set_style_bg_color(badge, COLOR_RED_DARK, 0);
  } else if (klipper_state == "startup") {
    lv_label_set_text(lbl_badge, "INICIANDO...");
    lv_obj_set_style_bg_color(badge, COLOR_SIDEBAR, 0);
  } else {
    lv_label_set_text(lbl_badge, "CONECTADO");
    lv_obj_set_style_bg_color(badge, COLOR_GREEN, 0);
  }
  lv_obj_set_style_text_font(lbl_badge, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_badge, lv_color_make(255, 255, 255), 0);
  lv_obj_align(lbl_badge, LV_ALIGN_CENTER, 0, 0);

  if (ws_connected && (klipper_state == "error" || klipper_state == "shutdown" || klipper_state == "startup")) {
    // Mostrar mensaje descriptivo del estado de Klipper
    lv_obj_t *lbl_err_msg = lv_label_create(card);
    String msg = klipper_state_message;
    if (msg.length() == 0) {
      if (klipper_state == "error") msg = "Error de conexion con MCU.";
      else if (klipper_state == "shutdown") msg = "La impresora se ha apagado (Shutdown).";
      else msg = "Klipper se esta iniciando...";
    }
    lv_label_set_text(lbl_err_msg, msg.c_str());
    lv_obj_set_style_text_font(lbl_err_msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_err_msg, COLOR_GREY_TEXT, 0);
    lv_obj_set_width(lbl_err_msg, 199);
    lv_label_set_long_mode(lbl_err_msg, LV_LABEL_LONG_WRAP); // Ajuste automático de texto
    lv_obj_align(lbl_err_msg, LV_ALIGN_TOP_LEFT, 2, 35);
    
    // Crear botones de reinicio si está en error o shutdown
    if (klipper_state != "startup") {
      // Botón Firmware Restart
      lv_obj_t *btn_fw_restart = lv_btn_create(card);
      lv_obj_set_size(btn_fw_restart, 95, 34);
      lv_obj_align(btn_fw_restart, LV_ALIGN_BOTTOM_LEFT, 2, -2);
      lv_obj_set_style_bg_color(btn_fw_restart, COLOR_RED_ACC, 0);
      lv_obj_set_style_radius(btn_fw_restart, 4, 0);
      lv_obj_add_event_cb(btn_fw_restart, restart_event_cb, LV_EVENT_CLICKED, (void*)2);
      
      lv_obj_t *lbl_fw_restart = lv_label_create(btn_fw_restart);
      lv_label_set_text(lbl_fw_restart, "Fw Restart");
      lv_obj_set_style_text_font(lbl_fw_restart, &lv_font_montserrat_14, 0);
      lv_obj_align(lbl_fw_restart, LV_ALIGN_CENTER, 0, 0);
      
      // Botón Restart (Klipper Restart)
      lv_obj_t *btn_k_restart = lv_btn_create(card);
      lv_obj_set_size(btn_k_restart, 95, 34);
      lv_obj_align(btn_k_restart, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
      lv_obj_set_style_bg_color(btn_k_restart, COLOR_SIDEBAR, 0);
      lv_obj_set_style_border_color(btn_k_restart, COLOR_RED_ACC, 0);
      lv_obj_set_style_border_width(btn_k_restart, 1, 0);
      lv_obj_set_style_radius(btn_k_restart, 4, 0);
      lv_obj_add_event_cb(btn_k_restart, restart_event_cb, LV_EVENT_CLICKED, (void*)1);
      
      lv_obj_t *lbl_k_restart = lv_label_create(btn_k_restart);
      lv_label_set_text(lbl_k_restart, "Restart");
      lv_obj_set_style_text_font(lbl_k_restart, &lv_font_montserrat_14, 0);
      lv_obj_align(lbl_k_restart, LV_ALIGN_CENTER, 0, 0);
    }
  } else {
    // Archivo actual gcode
    lbl_file = lv_label_create(card);
    lv_label_set_text_fmt(lbl_file, "Archivo: %s", current_gcode_file.c_str());
    lv_obj_set_style_text_font(lbl_file, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_file, lv_color_make(255, 255, 255), 0);
    lv_obj_align(lbl_file, LV_ALIGN_TOP_LEFT, 2, 40);

    // Barra de Progreso
    bar_prog = lv_bar_create(card);
    lv_obj_set_size(bar_prog, 199, 14);
    lv_obj_align(bar_prog, LV_ALIGN_CENTER, 0, 15);
    lv_bar_set_value(bar_prog, print_progress, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_prog, COLOR_RED_ACC, LV_PART_INDICATOR);

    // Etiqueta del Progreso
    lbl_prog = lv_label_create(card);
    lv_label_set_text_fmt(lbl_prog, "Progreso: %d%%", print_progress);
    lv_obj_set_style_text_font(lbl_prog, &lv_font_montserrat_14, 0);
    lv_obj_align_to(lbl_prog, bar_prog, LV_ALIGN_OUT_TOP_LEFT, 0, -2);

    // Tiempos reales
    lbl_time = lv_label_create(card);
    String elapsedStr = formatTime(print_duration);
    String totalStr = formatTime(total_duration);
    lv_label_set_text_fmt(lbl_time, "T: %s / E: %s", elapsedStr.c_str(), totalStr.c_str());
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_time, COLOR_GREY_TEXT, 0);
    lv_obj_align(lbl_time, LV_ALIGN_BOTTOM_LEFT, 2, -2);
  }

  // --- BOTONERA INFERIOR EXTERNA FUERA DEL CUADRO ---
  // Botón 1: Pausar / Reanudar (dinámico)
  lv_obj_t *btn_pause = lv_btn_create(parent);
  lv_obj_set_size(btn_pause, 100, 36);
  lv_obj_align(btn_pause, LV_ALIGN_BOTTOM_LEFT, 10, -8);
  lv_obj_set_style_bg_color(btn_pause, COLOR_SIDEBAR, 0);
  lv_obj_set_style_border_color(btn_pause, COLOR_RED_ACC, 0);
  lv_obj_set_style_border_width(btn_pause, 1, 0);
  lv_obj_set_style_radius(btn_pause, 4, 0);
  lv_obj_add_event_cb(btn_pause, gui_pause_estop_cb, LV_EVENT_CLICKED, (void*)1);

  lbl_gui_pause = lv_label_create(btn_pause);
  if (last_print_state == "paused") {
    lv_label_set_text(lbl_gui_pause, "Reanudar");
  } else {
    lv_label_set_text(lbl_gui_pause, "Pausar");
  }
  lv_obj_set_style_text_font(lbl_gui_pause, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_gui_pause, LV_ALIGN_CENTER, 0, 0);

  // Botón 2: Parada de Emergencia / Cancelar Impresion (dinámico)
  lv_obj_t *btn_estop = lv_btn_create(parent);
  lv_obj_set_size(btn_estop, 100, 36);
  lv_obj_align(btn_estop, LV_ALIGN_BOTTOM_RIGHT, -10, -8);
  lv_obj_set_style_bg_color(btn_estop, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(btn_estop, 4, 0);
  lv_obj_add_event_cb(btn_estop, gui_pause_estop_cb, LV_EVENT_CLICKED, (void*)2);

  lbl_gui_estop = lv_label_create(btn_estop);
  if (last_print_state == "paused") {
    lv_label_set_text(lbl_gui_estop, "Cancelar");
  } else {
    lv_label_set_text(lbl_gui_estop, "Parar (ESTOP)");
  }
  lv_obj_set_style_text_font(lbl_gui_estop, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_gui_estop, LV_ALIGN_CENTER, 0, 0);
}

/*---------------------------------------------------------------------------
 * SERVICIOS DE COMUNICACIÓN HTTP Y MOONRAKER
 *---------------------------------------------------------------------------*/
// Variables de temporización de sondeo HTTP
static unsigned long last_poll_time = 0;
static unsigned long current_poll_interval = 1500; // Sondeo dinámico (1.5s conectado, 5.0s desconectado)

String sendHttpRequest(const String& method, const String& path) {
  if (WiFi.status() != WL_CONNECTED) {
    return "";
  }
  
  String url = "http://" + moonraker_host_str + ":" + String(moonraker_port_int) + path;
  url.replace(" ", "%20"); // Reemplazar espacios por seguridad
  
  HTTPClient http;
  http.begin(url);
  http.setTimeout(600); // 600ms de timeout (localmente Moonraker responde en <50ms)
  
  int httpCode = http.sendRequest(method.c_str(), "");
  String response = "";
  
  if (httpCode > 0) {
    if (httpCode >= 200 && httpCode < 300) {
      response = http.getString();
    } else {
      Serial.printf("[HTTP] Peticion %s a %s devolvio codigo: %d\n", method.c_str(), path.c_str(), httpCode);
    }
  } else {
    Serial.printf("[HTTP] Fallo de red %s a %s. Error: %s\n", method.c_str(), path.c_str(), http.errorToString(httpCode).c_str());
  }
  
  http.end();
  return response;
}

void update_connection_status() {
  if (lbl_badge != NULL) {
    if (ws_connected) {
      if (klipper_state == "error") {
        lv_label_set_text(lbl_badge, "ERROR MCU");
        lv_obj_set_style_bg_color(lv_obj_get_parent(lbl_badge), COLOR_RED_ACC, 0);
      } else if (klipper_state == "shutdown") {
        lv_label_set_text(lbl_badge, "SHUTDOWN");
        lv_obj_set_style_bg_color(lv_obj_get_parent(lbl_badge), COLOR_RED_DARK, 0);
      } else if (klipper_state == "startup") {
        lv_label_set_text(lbl_badge, "INICIANDO...");
        lv_obj_set_style_bg_color(lv_obj_get_parent(lbl_badge), COLOR_SIDEBAR, 0);
      } else {
        lv_label_set_text(lbl_badge, "CONECTADO");
        lv_obj_set_style_bg_color(lv_obj_get_parent(lbl_badge), COLOR_GREEN, 0);
      }
    } else {
      lv_label_set_text(lbl_badge, "SIN CONEXION");
      lv_obj_set_style_bg_color(lv_obj_get_parent(lbl_badge), COLOR_RED_DARK, 0);
    }
  }
  
  if (lbl_printer_state != NULL) {
    if (ws_connected) {
      if (klipper_state != "ready") {
        lv_label_set_text_fmt(lbl_printer_state, "Impresora: %s", klipper_state.c_str());
        lv_obj_set_style_text_color(lbl_printer_state, COLOR_RED_ACC, 0);
      } else {
        lv_label_set_text(lbl_printer_state, "Impresora: CONECTADA");
        lv_obj_set_style_text_color(lbl_printer_state, COLOR_GREEN, 0);
      }
    } else {
      lv_label_set_text(lbl_printer_state, "Impresora: SIN CONEXION");
      lv_obj_set_style_text_color(lbl_printer_state, COLOR_RED_ACC, 0);
    }
  }
}

void update_print_state_badge(const String &state) {
  if (lbl_badge != NULL) {
    if (state == "printing") {
      lv_label_set_text(lbl_badge, "IMPRIMIENDO...");
      lv_obj_set_style_bg_color(lv_obj_get_parent(lbl_badge), COLOR_RED_ACC, 0);
    } else if (state == "paused") {
      lv_label_set_text(lbl_badge, "PAUSADO");
      lv_obj_set_style_bg_color(lv_obj_get_parent(lbl_badge), COLOR_SIDEBAR, 0);
    } else if (state == "standby" || state == "ready") {
      lv_label_set_text(lbl_badge, "LISTO");
      lv_obj_set_style_bg_color(lv_obj_get_parent(lbl_badge), COLOR_GREEN, 0);
    } else {
      String stateUpper = state;
      stateUpper.toUpperCase();
      lv_label_set_text(lbl_badge, stateUpper.c_str());
      lv_obj_set_style_bg_color(lv_obj_get_parent(lbl_badge), COLOR_GREEN, 0);
    }
  }
}

void pollMoonraker() {
  // 1. Consultar estado general de Klipper primero
  String infoPayload = sendHttpRequest("GET", "/printer/info");
  
  if (infoPayload.isEmpty()) {
    if (ws_connected) {
      ws_connected = false;
      current_poll_interval = 5000; // Sondeo lento (5.0s) para evitar bloquear el GUI si Moonraker no está listo
      Serial.println("[HTTP] Servidor Moonraker inalcanzable.");
      update_connection_status();
      refresh_status_screen();
    }
    return;
  }
  
  JsonDocument docInfo;
  DeserializationError errInfo = deserializeJson(docInfo, infoPayload);
  String newState = klipper_state;
  String newMsg = klipper_state_message;
  
  if (!errInfo && docInfo.containsKey("result")) {
    JsonObject result = docInfo["result"].as<JsonObject>();
    if (result.containsKey("state")) {
      newState = result["state"].as<String>();
    }
    if (result.containsKey("state_message")) {
      newMsg = result["state_message"].as<String>();
    }
  }
  
  if (newState != klipper_state || newMsg != klipper_state_message || !ws_connected) {
    klipper_state = newState;
    klipper_state_message = newMsg;
    ws_connected = true;
    current_poll_interval = 1500; // Sondeo normal rápido (1.5s) al estar conectado
    Serial.printf("[HTTP] Servidor Moonraker conectado con exito. Estado Klipper: %s\n", klipper_state.c_str());
    update_connection_status();
    refresh_status_screen();
  }
  
  // Si Klipper no está en estado "ready", no intentamos consultar los objetos de telemetría
  if (klipper_state != "ready") {
    nozzle_temp = 0;
    nozzle_target = 0;
    bed_temp = 0;
    bed_target = 0;
    print_progress = 0;
    print_duration = 0;
    total_duration = 0;
    update_temp_display();
    return;
  }

  // 2. Si Klipper está listo, hacer el query de los objetos de telemetría como de costumbre
  String path = "/printer/objects/query?extruder&heater_bed&virtual_sdcard&print_stats";
  String payload = sendHttpRequest("GET", path);
  
  if (payload.isEmpty()) {
    return;
  }
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("[HTTP] Error parseando JSON de telemetria: %s\n", error.c_str());
    return;
  }
  
  if (doc.containsKey("result")) {
    JsonObject result = doc["result"].as<JsonObject>();
    if (result.containsKey("status")) {
      JsonObject status = result["status"].as<JsonObject>();
      
      // Parsear Extrusor
      if (status.containsKey("extruder")) {
        JsonObject ext = status["extruder"].as<JsonObject>();
        if (ext.containsKey("temperature")) nozzle_temp = (int)ext["temperature"].as<float>();
        if (ext.containsKey("target")) nozzle_target = (int)ext["target"].as<float>();
      }
      
      // Parsear Cama Caliente
      if (status.containsKey("heater_bed")) {
        JsonObject bed = status["heater_bed"].as<JsonObject>();
        if (bed.containsKey("temperature")) bed_temp = (int)bed["temperature"].as<float>();
        if (bed.containsKey("target")) bed_target = (int)bed["target"].as<float>();
      }
      
      // Parsear Progreso de Impresión
      if (status.containsKey("virtual_sdcard")) {
        JsonObject sd = status["virtual_sdcard"].as<JsonObject>();
        if (sd.containsKey("progress")) {
          print_progress = (int)(sd["progress"].as<float>() * 100.0);
        }
      }
      
      // Parsear Archivo y Estado de Impresión
      if (status.containsKey("print_stats")) {
        JsonObject stats = status["print_stats"].as<JsonObject>();
        if (stats.containsKey("filename")) {
          current_gcode_file = stats["filename"].as<String>();
          if (current_gcode_file.isEmpty() || current_gcode_file == "null") {
            current_gcode_file = "Ninguno";
          }
        }
        if (stats.containsKey("print_duration")) {
          print_duration = (unsigned long)stats["print_duration"].as<float>();
        } else {
          print_duration = 0;
        }
        if (print_progress > 0) {
          total_duration = (print_duration * 100) / print_progress;
        } else {
          total_duration = 0;
        }
        if (stats.containsKey("state")) {
          String state = stats["state"].as<String>();
          if (state == "paused" && last_print_state != "paused") {
            create_filament_popup();
          }
          last_print_state = state;
          update_print_state_badge(state);
        }
      }
      
      // Actualizar interfaz LVGL
      update_temp_display();
      update_status_display();
    }
  }
}

void sendGcodeToMoonraker(const String &gcode) {
  if (wifi_connected) {
    String encodedGcode = gcode;
    encodedGcode.replace(" ", "%20");
    encodedGcode.replace("\n", "%0A");
    
    String path = "/printer/gcode/script?script=" + encodedGcode;
    String response = sendHttpRequest("POST", path);
    Serial.printf("[HTTP] G-Code enviado: %s. Rpta: %s\n", gcode.c_str(), response.c_str());
  } else {
    Serial.printf("[HTTP] No conectado a Wi-Fi. Comando G-code omitido: %s\n", gcode.c_str());
  }
}

/*---------------------------------------------------------------------------
 * 2. PANTALLA DE MOVIMIENTO (MOVIMIENTO)
 *---------------------------------------------------------------------------*/

static void update_coord_display() {
  lv_label_set_text_fmt(coord_label, "X: %.1f  Y: %.1f  Z: %.2f", pos_x, pos_y, pos_z);
}

static void jog_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    long dir = (long)lv_event_get_user_data(e);
    
    if (dir == 1) { pos_y += move_step; sendGcodeToMoonraker("G91\nG0 Y" + String(move_step) + "\nG90"); }   // Y+
    if (dir == 2) { pos_y -= move_step; sendGcodeToMoonraker("G91\nG0 Y-" + String(move_step) + "\nG90"); }  // Y-
    if (dir == 3) { pos_x -= move_step; sendGcodeToMoonraker("G91\nG0 X-" + String(move_step) + "\nG90"); }  // X-
    if (dir == 4) { pos_x += move_step; sendGcodeToMoonraker("G91\nG0 X" + String(move_step) + "\nG90"); }   // X+
    if (dir == 5) { pos_z += 1.0;       sendGcodeToMoonraker("G91\nG0 Z1.0\nG90"); }                         // Z+
    if (dir == 6) { pos_z -= 1.0;       sendGcodeToMoonraker("G91\nG0 Z-1.0\nG90"); }                        // Z-
    if (dir == 7) {                     // Home All
      pos_x = 0.0; pos_y = 0.0; pos_z = 0.0;
      sendGcodeToMoonraker("G28");
    }
    
    // Validar límites lógicos de cama de impresión
    if (pos_x < 0) pos_x = 0; if (pos_x > 220) pos_x = 220;
    if (pos_y < 0) pos_y = 0; if (pos_y > 220) pos_y = 220;
    if (pos_z < 0) pos_z = 0; if (pos_z > 250) pos_z = 250;

    update_coord_display();
  }
}

static void step_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED) {
    lv_obj_t *btnm = lv_event_get_target(e);
    uint32_t id = lv_btnmatrix_get_selected_btn(btnm);
    if (id == 0) move_step = 1.0;
    if (id == 1) move_step = 10.0;
    if (id == 2) move_step = 50.0;
  }
}

void create_move_screen(lv_obj_t *parent) {
  // Label Coordenadas
  coord_label = lv_label_create(parent);
  lv_obj_set_style_text_font(coord_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(coord_label, lv_color_make(255, 255, 255), 0);
  lv_obj_align(coord_label, LV_ALIGN_TOP_MID, 0, 5);
  update_coord_display();

  // Contenedor principal de controles
  lv_obj_t *ctrl_box = lv_obj_create(parent);
  lv_obj_set_size(ctrl_box, 230, 185);
  lv_obj_align(ctrl_box, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_set_style_bg_color(ctrl_box, COLOR_BG, 0);
  lv_obj_set_style_border_width(ctrl_box, 0, 0);
  lv_obj_set_style_pad_all(ctrl_box, 0, 0);

  int btn_size = 35;
  
  // Y+
  lv_obj_t *btn_yp = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_yp, btn_size, btn_size);
  lv_obj_align(btn_yp, LV_ALIGN_TOP_LEFT, 45, 5);
  lv_obj_set_style_bg_color(btn_yp, COLOR_CARD, 0);
  lv_obj_add_event_cb(btn_yp, jog_event_cb, LV_EVENT_CLICKED, (void*)1);
  lv_obj_t *lbl_yp = lv_label_create(btn_yp);
  lv_label_set_text(lbl_yp, "Y+");
  lv_obj_align(lbl_yp, LV_ALIGN_CENTER, 0, 0);

  // Y-
  lv_obj_t *btn_ym = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_ym, btn_size, btn_size);
  lv_obj_align(btn_ym, LV_ALIGN_TOP_LEFT, 45, 85);
  lv_obj_set_style_bg_color(btn_ym, COLOR_CARD, 0);
  lv_obj_add_event_cb(btn_ym, jog_event_cb, LV_EVENT_CLICKED, (void*)2);
  lv_obj_t *lbl_ym = lv_label_create(btn_ym);
  lv_label_set_text(lbl_ym, "Y-");
  lv_obj_align(lbl_ym, LV_ALIGN_CENTER, 0, 0);

  // X-
  lv_obj_t *btn_xm = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_xm, btn_size, btn_size);
  lv_obj_align(btn_xm, LV_ALIGN_TOP_LEFT, 5, 45);
  lv_obj_set_style_bg_color(btn_xm, COLOR_CARD, 0);
  lv_obj_add_event_cb(btn_xm, jog_event_cb, LV_EVENT_CLICKED, (void*)3);
  lv_obj_t *lbl_xm = lv_label_create(btn_xm);
  lv_label_set_text(lbl_xm, "X-");
  lv_obj_align(lbl_xm, LV_ALIGN_CENTER, 0, 0);

  // X+
  lv_obj_t *btn_xp = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_xp, btn_size, btn_size);
  lv_obj_align(btn_xp, LV_ALIGN_TOP_LEFT, 85, 45);
  lv_obj_set_style_bg_color(btn_xp, COLOR_CARD, 0);
  lv_obj_add_event_cb(btn_xp, jog_event_cb, LV_EVENT_CLICKED, (void*)4);
  lv_obj_t *lbl_xp = lv_label_create(btn_xp);
  lv_label_set_text(lbl_xp, "X+");
  lv_obj_align(lbl_xp, LV_ALIGN_CENTER, 0, 0);

  // Home All (Centro del D-pad)
  lv_obj_t *btn_home = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_home, btn_size, btn_size);
  lv_obj_align(btn_home, LV_ALIGN_TOP_LEFT, 45, 45);
  lv_obj_set_style_bg_color(btn_home, COLOR_RED_ACC, 0);
  lv_obj_add_event_cb(btn_home, jog_event_cb, LV_EVENT_CLICKED, (void*)7);
  lv_obj_t *lbl_home = lv_label_create(btn_home);
  lv_label_set_text(lbl_home, "H");
  lv_obj_align(lbl_home, LV_ALIGN_CENTER, 0, 0);

  // --- CONTROLES DE Z ---
  // Z+
  lv_obj_t *btn_zp = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_zp, btn_size + 5, btn_size);
  lv_obj_align(btn_zp, LV_ALIGN_TOP_RIGHT, -15, 10);
  lv_obj_set_style_bg_color(btn_zp, COLOR_CARD, 0);
  lv_obj_add_event_cb(btn_zp, jog_event_cb, LV_EVENT_CLICKED, (void*)5);
  lv_obj_t *lbl_zp = lv_label_create(btn_zp);
  lv_label_set_text(lbl_zp, "Z+");
  lv_obj_align(lbl_zp, LV_ALIGN_CENTER, 0, 0);

  // Z-
  lv_obj_t *btn_zm = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_zm, btn_size + 5, btn_size);
  lv_obj_align(btn_zm, LV_ALIGN_TOP_RIGHT, -15, 65);
  lv_obj_set_style_bg_color(btn_zm, COLOR_CARD, 0);
  lv_obj_add_event_cb(btn_zm, jog_event_cb, LV_EVENT_CLICKED, (void*)6);
  lv_obj_t *lbl_zm = lv_label_create(btn_zm);
  lv_label_set_text(lbl_zm, "Z-");
  lv_obj_align(lbl_zm, LV_ALIGN_CENTER, 0, 0);

  // --- BOTONERA DE PASO (STEP) ---
  static const char *btnm_map[] = {"1 mm", "10 mm", "50 mm", ""};
  lv_obj_t *btnm = lv_btnmatrix_create(ctrl_box);
  lv_obj_set_size(btnm, 210, 32);
  lv_obj_align(btnm, LV_ALIGN_BOTTOM_MID, 0, -5);
  lv_btnmatrix_set_map(btnm, btnm_map);
  lv_obj_set_style_bg_color(btnm, COLOR_BG, 0);
  lv_obj_set_style_border_width(btnm, 0, 0);
  
  lv_btnmatrix_set_btn_ctrl(btnm, 1, LV_BTNMATRIX_CTRL_CHECKED);
  lv_btnmatrix_set_one_checked(btnm, true);
  lv_obj_add_event_cb(btnm, step_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/*---------------------------------------------------------------------------
 * 3. PANTALLA DE TEMPERATURA (TEMPERATURAS)
 *---------------------------------------------------------------------------*/

static void update_temp_display() {
  if (lbl_noz != NULL && lbl_bed != NULL) {
    lv_label_set_text_fmt(lbl_noz, "Boquilla: %d°C / %d°C", nozzle_temp, nozzle_target);
    lv_label_set_text_fmt(lbl_bed, "Cama: %d°C / %d°C", bed_temp, bed_target);
  }
}

static void temp_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    long act = (long)lv_event_get_user_data(e);
    
    if (act == 1) nozzle_target += 5; // Noz +5
    if (act == 2) nozzle_target -= 5; // Noz -5
    if (act == 3) bed_target += 5;    // Bed +5
    if (act == 4) bed_target -= 5;    // Bed -5
    
    if (act == 5) { // Preheat PLA
      nozzle_target = 210;
      bed_target = 60;
      sendGcodeToMoonraker("M104 S210");
      sendGcodeToMoonraker("M140 S60");
    }
    
    if (act == 6) { // Cooldown
      nozzle_target = 0;
      bed_target = 0;
      sendGcodeToMoonraker("M104 S0");
      sendGcodeToMoonraker("M140 S0");
    }

    if (nozzle_target < 0) nozzle_target = 0;
    if (bed_target < 0) bed_target = 0;

    if (act == 1 || act == 2) {
      sendGcodeToMoonraker("M104 S" + String(nozzle_target));
    }
    if (act == 3 || act == 4) {
      sendGcodeToMoonraker("M140 S" + String(bed_target));
    }

    update_temp_display();
  }
}

void create_temp_screen(lv_obj_t *parent) {
  // Boquilla Card
  lv_obj_t *c_noz = lv_obj_create(parent);
  lv_obj_set_size(c_noz, 220, 52);
  lv_obj_align(c_noz, LV_ALIGN_TOP_MID, 0, 5);
  lv_obj_set_style_bg_color(c_noz, COLOR_CARD, 0);
  lv_obj_set_style_border_width(c_noz, 0, 0);
  lv_obj_set_style_radius(c_noz, 6, 0);
  lv_obj_set_style_pad_all(c_noz, 4, 0);

  lbl_noz = lv_label_create(c_noz);
  lv_obj_set_style_text_font(lbl_noz, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_noz, LV_ALIGN_LEFT_MID, 5, 0);

  lv_obj_t *btn_noz_p = lv_btn_create(c_noz);
  lv_obj_set_size(btn_noz_p, 36, 32);
  lv_obj_align(btn_noz_p, LV_ALIGN_RIGHT_MID, -45, 0);
  lv_obj_set_style_bg_color(btn_noz_p, COLOR_RED_ACC, 0);
  lv_obj_add_event_cb(btn_noz_p, temp_event_cb, LV_EVENT_CLICKED, (void*)1);
  lv_obj_t *l_np = lv_label_create(btn_noz_p);
  lv_label_set_text(l_np, "+");
  lv_obj_align(l_np, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *btn_noz_m = lv_btn_create(c_noz);
  lv_obj_set_size(btn_noz_m, 36, 32);
  lv_obj_align(btn_noz_m, LV_ALIGN_RIGHT_MID, -5, 0);
  lv_obj_set_style_bg_color(btn_noz_m, COLOR_SIDEBAR, 0);
  lv_obj_add_event_cb(btn_noz_m, temp_event_cb, LV_EVENT_CLICKED, (void*)2);
  lv_obj_t *l_nm = lv_label_create(btn_noz_m);
  lv_label_set_text(l_nm, "-");
  lv_obj_align(l_nm, LV_ALIGN_CENTER, 0, 0);

  // Cama Card
  lv_obj_t *c_bed = lv_obj_create(parent);
  lv_obj_set_size(c_bed, 220, 52);
  lv_obj_align(c_bed, LV_ALIGN_TOP_MID, 0, 62);
  lv_obj_set_style_bg_color(c_bed, COLOR_CARD, 0);
  lv_obj_set_style_border_width(c_bed, 0, 0);
  lv_obj_set_style_radius(c_bed, 6, 0);
  lv_obj_set_style_pad_all(c_bed, 4, 0);

  lbl_bed = lv_label_create(c_bed);
  lv_obj_set_style_text_font(lbl_bed, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_bed, LV_ALIGN_LEFT_MID, 5, 0);

  lv_obj_t *btn_bed_p = lv_btn_create(c_bed);
  lv_obj_set_size(btn_bed_p, 36, 32);
  lv_obj_align(btn_bed_p, LV_ALIGN_RIGHT_MID, -45, 0);
  lv_obj_set_style_bg_color(btn_bed_p, COLOR_RED_ACC, 0);
  lv_obj_add_event_cb(btn_bed_p, temp_event_cb, LV_EVENT_CLICKED, (void*)3);
  lv_obj_t *l_bp = lv_label_create(btn_bed_p);
  lv_label_set_text(l_bp, "+");
  lv_obj_align(l_bp, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *btn_bed_m = lv_btn_create(c_bed);
  lv_obj_set_size(btn_bed_m, 36, 32);
  lv_obj_align(btn_bed_m, LV_ALIGN_RIGHT_MID, -5, 0);
  lv_obj_set_style_bg_color(btn_bed_m, COLOR_SIDEBAR, 0);
  lv_obj_add_event_cb(btn_bed_m, temp_event_cb, LV_EVENT_CLICKED, (void*)4);
  lv_obj_t *l_bm = lv_label_create(btn_bed_m);
  lv_label_set_text(l_bm, "-");
  lv_obj_align(l_bm, LV_ALIGN_CENTER, 0, 0);

  // Botones de presets
  lv_obj_t *btn_pre = lv_btn_create(parent);
  lv_obj_set_size(btn_pre, 105, 36);
  lv_obj_align(btn_pre, LV_ALIGN_BOTTOM_LEFT, 5, -10);
  lv_obj_set_style_bg_color(btn_pre, COLOR_RED_ACC, 0);
  lv_obj_add_event_cb(btn_pre, temp_event_cb, LV_EVENT_CLICKED, (void*)5);
  lv_obj_t *lbl_pre = lv_label_create(btn_pre);
  lv_label_set_text(lbl_pre, "Precal PLA");
  lv_obj_set_style_text_font(lbl_pre, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_pre, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *btn_cool = lv_btn_create(parent);
  lv_obj_set_size(btn_cool, 105, 36);
  lv_obj_align(btn_cool, LV_ALIGN_BOTTOM_RIGHT, -5, -10);
  lv_obj_set_style_bg_color(btn_cool, COLOR_RED_DARK, 0);
  lv_obj_add_event_cb(btn_cool, temp_event_cb, LV_EVENT_CLICKED, (void*)6);
  lv_obj_t *lbl_cool = lv_label_create(btn_cool);
  lv_label_set_text(lbl_cool, "Enfriar");
  lv_obj_set_style_text_font(lbl_cool, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_cool, LV_ALIGN_CENTER, 0, 0);

  update_temp_display();
}

/*---------------------------------------------------------------------------
 * 4. CONFIGURACIÓN DE PANTALLA (CONFIGURACION DE PANTALLA)
 *---------------------------------------------------------------------------*/
static lv_obj_t *lbl_bright;

static void bright_slider_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED) {
    lv_obj_t *slider = lv_event_get_target(e);
    screen_brightness = lv_slider_get_value(slider);
    
    // 1. Cambiar dinámicamente el brillo físico mediante control PWM analógico
    analogWrite(TFT_BL, map(screen_brightness, 0, 100, 0, 255));
    
    // 2. Actualizar el valor en la pantalla
    lv_label_set_text_fmt(lbl_bright, "Brillo Pantalla: %d%%", screen_brightness);
  }
}

static void kb_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *kb = lv_event_get_target(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_del(kb);
  }
}

static void ta_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = lv_event_get_target(e);
  if (code == LV_EVENT_FOCUSED) {
    lv_obj_t *kb = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL, NULL);
  }
}

static lv_obj_t *ta_ssid;
static lv_obj_t *ta_pass;
static lv_obj_t *ta_ip;

static void connect_save_btn_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    wifi_ssid_str = String(lv_textarea_get_text(ta_ssid));
    wifi_pass_str = String(lv_textarea_get_text(ta_pass));
    moonraker_host_str = String(lv_textarea_get_text(ta_ip));
    
    wifi_ssid_str.trim();
    wifi_pass_str.trim();
    moonraker_host_str.trim();

    // Limpiar IP de prefijos y sufijos de forma inteligente
    if (moonraker_host_str.startsWith("http://")) {
      moonraker_host_str = moonraker_host_str.substring(7);
    } else if (moonraker_host_str.startsWith("https://")) {
      moonraker_host_str = moonraker_host_str.substring(8);
    }
    
    // Si contiene puerto (ej. 192.168.0.100:7125), extraerlo dinámicamente
    int colon_index = moonraker_host_str.indexOf(':');
    if (colon_index != -1) {
      String port_str = moonraker_host_str.substring(colon_index + 1);
      moonraker_host_str = moonraker_host_str.substring(0, colon_index);
      int parsed_port = port_str.toInt();
      if (parsed_port > 0) {
        moonraker_port_int = parsed_port;
      }
    }
    
    // Quitar rutas si las hay (ej. 192.168.0.100/)
    int slash_index = moonraker_host_str.indexOf('/');
    if (slash_index != -1) {
      moonraker_host_str = moonraker_host_str.substring(0, slash_index);
    }
    
    moonraker_host_str.trim();
    
    reconnect_requested = true;
    Serial.printf("[Config] Nuevos parametros guardados. Host: %s, Puerto: %d. Solicitando reconexion...\n", moonraker_host_str.c_str(), moonraker_port_int);
    
    // Crear toast con cierre al tocar en cualquier lado de la pantalla
    create_toast("Configuracion", "Guardando y reconectando...");
  }
}

void create_config_screen(lv_obj_t *parent) {
  // Convertir parent en scrollable con flex
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent, 8, 0);
  lv_obj_set_style_pad_row(parent, 8, 0);
  lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);

  // Título
  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, "Configuración del Sistema");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_make(255, 255, 255), 0);

  // Tarjeta de Estados de Conexión
  lv_obj_t *c_status = lv_obj_create(parent);
  lv_obj_set_size(c_status, 215, 60);
  lv_obj_set_style_bg_color(c_status, COLOR_CARD, 0);
  lv_obj_set_style_border_width(c_status, 0, 0);
  lv_obj_set_style_radius(c_status, 6, 0);
  lv_obj_set_style_pad_all(c_status, 6, 0);

  lbl_wifi_state = lv_label_create(c_status);
  lv_obj_align(lbl_wifi_state, LV_ALIGN_TOP_LEFT, 2, 2);
  lv_obj_set_style_text_font(lbl_wifi_state, &lv_font_montserrat_14, 0);
  
  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text_fmt(lbl_wifi_state, "Wi-Fi: SI (IP: %s)", WiFi.localIP().toString().c_str());
    lv_obj_set_style_text_color(lbl_wifi_state, COLOR_GREEN, 0);
  } else {
    lv_label_set_text(lbl_wifi_state, "Wi-Fi: DESCONECTADO");
    lv_obj_set_style_text_color(lbl_wifi_state, COLOR_RED_ACC, 0);
  }

  lbl_printer_state = lv_label_create(c_status);
  lv_obj_align(lbl_printer_state, LV_ALIGN_BOTTOM_LEFT, 2, -2);
  lv_obj_set_style_text_font(lbl_printer_state, &lv_font_montserrat_14, 0);
  
  if (ws_connected) {
    lv_label_set_text(lbl_printer_state, "Impresora: CONECTADA");
    lv_obj_set_style_text_color(lbl_printer_state, COLOR_GREEN, 0);
  } else {
    lv_label_set_text(lbl_printer_state, "Impresora: SIN CONEXION");
    lv_obj_set_style_text_color(lbl_printer_state, COLOR_RED_ACC, 0);
  }

  // --- Campos de texto ---
  
  // SSID Label
  lv_obj_t *lbl_ssid = lv_label_create(parent);
  lv_label_set_text(lbl_ssid, "SSID de Wi-Fi:");
  lv_obj_set_style_text_font(lbl_ssid, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_ssid, COLOR_GREY_TEXT, 0);

  // SSID TextArea
  ta_ssid = lv_textarea_create(parent);
  lv_obj_set_width(ta_ssid, 215);
  lv_textarea_set_one_line(ta_ssid, true);
  lv_textarea_set_text(ta_ssid, wifi_ssid_str.c_str());
  lv_obj_set_style_bg_color(ta_ssid, COLOR_CARD, 0);
  lv_obj_set_style_border_width(ta_ssid, 1, 0);
  lv_obj_set_style_border_color(ta_ssid, COLOR_SIDEBAR, 0);
  lv_obj_set_style_text_color(ta_ssid, lv_color_make(255, 255, 255), 0);
  lv_obj_add_event_cb(ta_ssid, ta_event_cb, LV_EVENT_ALL, NULL);

  // Password Label
  lv_obj_t *lbl_pass = lv_label_create(parent);
  lv_label_set_text(lbl_pass, "Contraseña:");
  lv_obj_set_style_text_font(lbl_pass, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_pass, COLOR_GREY_TEXT, 0);

  // Password TextArea
  ta_pass = lv_textarea_create(parent);
  lv_obj_set_width(ta_pass, 215);
  lv_textarea_set_one_line(ta_pass, true);
  lv_textarea_set_text(ta_pass, wifi_pass_str.c_str());
  lv_obj_set_style_bg_color(ta_pass, COLOR_CARD, 0);
  lv_obj_set_style_border_width(ta_pass, 1, 0);
  lv_obj_set_style_border_color(ta_pass, COLOR_SIDEBAR, 0);
  lv_obj_set_style_text_color(ta_pass, lv_color_make(255, 255, 255), 0);
  lv_obj_add_event_cb(ta_pass, ta_event_cb, LV_EVENT_ALL, NULL);

  // IP Label
  lv_obj_t *lbl_ip = lv_label_create(parent);
  lv_label_set_text(lbl_ip, "IP Servidor Moonraker:");
  lv_obj_set_style_text_font(lbl_ip, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_ip, COLOR_GREY_TEXT, 0);

  // IP TextArea
  ta_ip = lv_textarea_create(parent);
  lv_obj_set_width(ta_ip, 215);
  lv_textarea_set_one_line(ta_ip, true);
  lv_textarea_set_text(ta_ip, moonraker_host_str.c_str());
  lv_obj_set_style_bg_color(ta_ip, COLOR_CARD, 0);
  lv_obj_set_style_border_width(ta_ip, 1, 0);
  lv_obj_set_style_border_color(ta_ip, COLOR_SIDEBAR, 0);
  lv_obj_set_style_text_color(ta_ip, lv_color_make(255, 255, 255), 0);
  lv_obj_add_event_cb(ta_ip, ta_event_cb, LV_EVENT_ALL, NULL);

  // Botón Conectar y Guardar
  lv_obj_t *btn_save = lv_btn_create(parent);
  lv_obj_set_size(btn_save, 215, 38);
  lv_obj_set_style_bg_color(btn_save, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(btn_save, 6, 0);
  lv_obj_add_event_cb(btn_save, connect_save_btn_event_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl_save = lv_label_create(btn_save);
  lv_label_set_text(lbl_save, "Conectar y Guardar");
  lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_save, LV_ALIGN_CENTER, 0, 0);

  // Separador decorativo
  lv_obj_t *sep = lv_obj_create(parent);
  lv_obj_set_size(sep, 215, 1);
  lv_obj_set_style_bg_color(sep, COLOR_CARD, 0);
  lv_obj_set_style_border_width(sep, 0, 0);

  // Tarjeta Brillo (original)
  lv_obj_t *c_bright = lv_obj_create(parent);
  lv_obj_set_size(c_bright, 215, 72);
  lv_obj_set_style_bg_color(c_bright, COLOR_CARD, 0);
  lv_obj_set_style_border_width(c_bright, 0, 0);
  lv_obj_set_style_radius(c_bright, 6, 0);
  lv_obj_set_style_pad_all(c_bright, 8, 0);

  lbl_bright = lv_label_create(c_bright);
  lv_label_set_text_fmt(lbl_bright, "Brillo Pantalla: %d%%", screen_brightness);
  lv_obj_set_style_text_font(lbl_bright, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_bright, lv_color_make(255, 255, 255), 0);
  lv_obj_align(lbl_bright, LV_ALIGN_TOP_LEFT, 2, 2);

  lv_obj_t *slider = lv_slider_create(c_bright);
  lv_obj_set_size(slider, 195, 12);
  lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -5);
  lv_slider_set_value(slider, screen_brightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, COLOR_RED_ACC, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_make(255, 255, 255), LV_PART_KNOB);
  lv_obj_add_event_cb(slider, bright_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);


}

/*---------------------------------------------------------------------------
 * 5. SELECCIÓN DE ARCHIVOS (ARCHIVOS GCODE)
 *---------------------------------------------------------------------------*/
static void file_click_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    const char *filename = lv_label_get_text(label);
    
    // Cambiar el archivo de impresión activo localmente (se sincronizará luego por WebSocket)
    current_gcode_file = String(filename);
    print_progress = 0;
    
    // Enviar comandos Moonraker
    sendGcodeToMoonraker("SDCARD_SELECT_FILE FILENAME=" + String(filename));
    sendGcodeToMoonraker("SDCARD_PRINT_FILE FILENAME=" + String(filename)); // O simplemente dejar que Moonraker notifique al iniciar
    
    Serial.printf("Consola: Solicitado cargar y reproducir el archivo '%s'\n", filename);

    // Crear toast con cierre al tocar en cualquier lado de la pantalla
    create_toast("Carga de G-Code", "Archivo cargado con exito.");
  }
}

void create_files_screen(lv_obj_t *parent) {
  // Título
  lv_obj_t *title = lv_label_create(parent);
  lv_label_set_text(title, "Biblioteca de Archivos G-Code");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(title, lv_color_make(255, 255, 255), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 5, 5);

  // Contenedor con scroll vertical para listado de archivos
  lv_obj_t *list = lv_obj_create(parent);
  lv_obj_set_size(list, 230, 185);
  lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_set_style_bg_color(list, COLOR_BG, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(list, 2, 0);
  lv_obj_set_style_pad_row(list, 5, 0); // Espacio entre ítems

  // Listado de archivos simulados
  const char *files[] = {
    "benchy_fast.gcode",
    "calibration_cube.gcode",
    "drawer_handle.gcode",
    "filament_spool_holder.gcode",
    "spatula_grip.gcode"
  };

  for (uint8_t i = 0; i < 5; i++) {
    lv_obj_t *btn = lv_btn_create(list);
    lv_obj_set_width(btn, 206);
    lv_obj_set_height(btn, 36);
    lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_event_cb(btn, file_click_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, files[i]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 5, 0);
  }
}

/*---------------------------------------------------------------------------
 * DISEÑO MAESTRO DE LA ESTRUCTURA CYD-KLIPPER (DESLIZANTE/SCROLLABLE)
 *---------------------------------------------------------------------------*/
void create_cyd_klipper_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COLOR_BG, 0);

  // 1. Barra Lateral de Navegación Deslizante/Scrollable (Izquierda)
  lv_obj_t *sidebar = lv_obj_create(scr);
  lv_obj_set_size(sidebar, 80, 240);
  lv_obj_align(sidebar, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_bg_color(sidebar, COLOR_SIDEBAR, 0);
  lv_obj_set_style_border_width(sidebar, 0, 0);
  lv_obj_set_style_radius(sidebar, 0, 0);
  lv_obj_set_scrollbar_mode(sidebar, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN); // Disposición vertical automática
  lv_obj_set_style_pad_all(sidebar, 4, 0);
  lv_obj_set_style_pad_row(sidebar, 6, 0); // Espacio regular entre botones

  int btn_h = 42;
  int btn_w = 72;

  // Botón Tab 1: Estado
  btn_status = lv_btn_create(sidebar);
  lv_obj_set_size(btn_status, btn_w, btn_h);
  lv_obj_set_style_radius(btn_status, 6, 0);
  lv_obj_add_event_cb(btn_status, nav_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l_status = lv_label_create(btn_status);
  lv_label_set_text(l_status, "Estado");
  lv_obj_set_style_text_font(l_status, &lv_font_montserrat_14, 0);
  lv_obj_align(l_status, LV_ALIGN_CENTER, 0, 0);

  // Botón Tab 2: Movimiento
  btn_move = lv_btn_create(sidebar);
  lv_obj_set_size(btn_move, btn_w, btn_h);
  lv_obj_set_style_radius(btn_move, 6, 0);
  lv_obj_add_event_cb(btn_move, nav_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l_move = lv_label_create(btn_move);
  lv_label_set_text(l_move, "Mover");
  lv_obj_set_style_text_font(l_move, &lv_font_montserrat_14, 0);
  lv_obj_align(l_move, LV_ALIGN_CENTER, 0, 0);

  // Botón Tab 3: Temperaturas
  btn_temp = lv_btn_create(sidebar);
  lv_obj_set_size(btn_temp, btn_w, btn_h);
  lv_obj_set_style_radius(btn_temp, 6, 0);
  lv_obj_add_event_cb(btn_temp, nav_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l_temp = lv_label_create(btn_temp);
  lv_label_set_text(l_temp, "Temps");
  lv_obj_set_style_text_font(l_temp, &lv_font_montserrat_14, 0);
  lv_obj_align(l_temp, LV_ALIGN_CENTER, 0, 0);

  // Botón Tab 4: Configuración de Pantalla
  btn_config = lv_btn_create(sidebar);
  lv_obj_set_size(btn_config, btn_w, btn_h);
  lv_obj_set_style_radius(btn_config, 6, 0);
  lv_obj_add_event_cb(btn_config, nav_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l_config = lv_label_create(btn_config);
  lv_label_set_text(l_config, "Config");
  lv_obj_set_style_text_font(l_config, &lv_font_montserrat_14, 0);
  lv_obj_align(l_config, LV_ALIGN_CENTER, 0, 0);

  // Botón Tab 5: Archivos Gcode
  btn_files = lv_btn_create(sidebar);
  lv_obj_set_size(btn_files, btn_w, btn_h);
  lv_obj_set_style_radius(btn_files, 6, 0);
  lv_obj_add_event_cb(btn_files, nav_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l_files = lv_label_create(btn_files);
  lv_label_set_text(l_files, "Gcodes");
  lv_obj_set_style_text_font(l_files, &lv_font_montserrat_14, 0);
  lv_obj_align(l_files, LV_ALIGN_CENTER, 0, 0);

  // 2. Área de Contenido Dinámico (Derecha)
  content_area = lv_obj_create(scr);
  lv_obj_set_size(content_area, 240, 240);
  lv_obj_align(content_area, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(content_area, COLOR_BG, 0);
  lv_obj_set_style_border_width(content_area, 0, 0);
  lv_obj_set_style_radius(content_area, 0, 0);
  lv_obj_set_style_pad_all(content_area, 0, 0);

  // Iniciar por defecto en la pantalla de Estado
  update_sidebar_selection(0);
  create_status_screen(content_area);
}

/*---------------------------------------------------------------------------
 * INICIALIZACIÓN Y BUCLE PRINCIPAL
 *---------------------------------------------------------------------------*/
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=============================================");
  Serial.println("CYD-Klipper Cybertech Red Edition on ESP32-S3");
  Serial.println("=============================================");

  // 1. Inicializar retroiluminación con valor por defecto
  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, map(screen_brightness, 0, 100, 0, 255));

  // 2. Inicializar Pantalla LCD física
  tft.init();
  tft.setRotation(1);       // Modo horizontal
  tft.invertDisplay(colors_inverted);
  tft.fillScreen(TFT_BLACK);

  // 3. Inicializar panel táctil capacitivo (FT6336G)
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(50);
  Wire.begin(TP_SDA, TP_SCL, 400000);

  // 4. Inicializar LVGL
  lv_init();

  // 5. Configurar el buffer de dibujo de LVGL
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, 320 * 10);

  // 6. Configurar el controlador de visualización
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 320;
  disp_drv.ver_res = 240;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // 7. Configurar el controlador táctil
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  // 8. Crear la interfaz interactiva de CYD-Klipper
  create_cyd_klipper_ui();

  // 9. Conectar a la red Wi-Fi configurada dinámicamente de forma estable
  WiFi.persistent(false); // No escribir en flash a cada conexión para no dañar el integrado
  WiFi.mode(WIFI_STA);    // Configurar modo estación explícito (evita rechazos del router)
  WiFi.begin(wifi_ssid_str.c_str(), wifi_pass_str.c_str());
  Serial.printf("CYD-Klipper UI Cybertech Red iniciada. Conectando a Wi-Fi SSID: %s...\n", wifi_ssid_str.c_str());
}

void loop() {
  lv_timer_handler();
  
  // Procesar solicitud de reconexión manual desde la UI
  if (reconnect_requested) {
    reconnect_requested = false;
    Serial.println("[Red] Ejecutando desconexion para aplicar nuevos cambios de SSID/IP...");
    WiFi.disconnect();
    delay(200);
    WiFi.begin(wifi_ssid_str.c_str(), wifi_pass_str.c_str());
    wifi_connected = false;
    ws_connected = false;
    
    // Actualizar estados visuales si la pantalla de config está abierta
    if (lbl_wifi_state != NULL) {
      lv_label_set_text(lbl_wifi_state, "Wi-Fi: CONECTANDO...");
      lv_obj_set_style_text_color(lbl_wifi_state, COLOR_GREY_TEXT, 0);
    }
    if (lbl_printer_state != NULL) {
      lv_label_set_text(lbl_printer_state, "Impresora: CONECTANDO...");
      lv_obj_set_style_text_color(lbl_printer_state, COLOR_GREY_TEXT, 0);
    }
  }

  // Procesar tareas de conectividad
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifi_connected) {
      Serial.println("\n[Wi-Fi] Conectado exitosamente!");
      wifi_connected = true;
      
      // Actualizar etiqueta de Wi-Fi si la pantalla de config está activa
      if (lbl_wifi_state != NULL) {
        lv_label_set_text_fmt(lbl_wifi_state, "Wi-Fi: SI (IP: %s)", WiFi.localIP().toString().c_str());
        lv_obj_set_style_text_color(lbl_wifi_state, COLOR_GREEN, 0);
      }
      
      // Diagnóstico de red rápido
      Serial.printf("[Diagnostico] Probando conexion TCP hacia el host de Klipper %s:%d...\n", moonraker_host_str.c_str(), moonraker_port_int);
      WiFiClient testClient;
      if (testClient.connect(moonraker_host_str.c_str(), moonraker_port_int)) {
        Serial.println("[Diagnostico] ¡EXITO! Conexion TCP cruda establecida.");
        testClient.stop();
      } else {
        Serial.println("[Diagnostico] ¡FALLO! No se pudo abrir socket TCP.");
        Serial.println("[Diagnostico] Posibles causas: 1. IP/Puerto incorrecto. 2. AP Isolation (Aislamiento de AP) en el Router. 3. Firewall en Raspberry Pi.");
      }
      
      // Forzar un sondeo inmediato de telemetría
      last_poll_time = 0;
    }
    
    // Sondeo periódico no bloqueante a Moonraker
    if (millis() - last_poll_time >= current_poll_interval) {
      last_poll_time = millis();
      pollMoonraker();
    }
  } else {
    // Si la conexión Wi-Fi está caída, reintentar activamente cada 10 segundos sin bloquear la pantalla
    static unsigned long last_wifi_retry_time = 0;
    if (millis() - last_wifi_retry_time >= 10000) {
      last_wifi_retry_time = millis();
      Serial.println("[Wi-Fi] Reintentando conexion activa...");
      WiFi.disconnect();
      delay(50);
      WiFi.begin(wifi_ssid_str.c_str(), wifi_pass_str.c_str());
    }

    if (wifi_connected) {
      Serial.println("\n[Wi-Fi] Conexion Wi-Fi perdida!");
      wifi_connected = false;
      ws_connected = false;
      
      if (lbl_badge != NULL) {
        lv_label_set_text(lbl_badge, "SIN CONEXION");
        lv_obj_set_style_bg_color(lv_obj_get_parent(lbl_badge), COLOR_RED_DARK, 0);
      }
      
      if (lbl_wifi_state != NULL) {
        lv_label_set_text(lbl_wifi_state, "Wi-Fi: DESCONECTADO");
        lv_obj_set_style_text_color(lbl_wifi_state, COLOR_RED_ACC, 0);
      }
      if (lbl_printer_state != NULL) {
        lv_label_set_text(lbl_printer_state, "Impresora: SIN CONEXION");
        lv_obj_set_style_text_color(lbl_printer_state, COLOR_RED_ACC, 0);
      }
    }
  }
  
  delay(5);
}
