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
static int screen_timeout_minutes = 5;   // 0 = Nunca, 1 = 1 Minuto, 5 = 5 Minutos
static unsigned long last_touch_time = 0;
static bool screen_is_off = false;
static lv_obj_t *wifi_setup_overlay = NULL;
static lv_obj_t *printer_setup_overlay = NULL;
static lv_obj_t *active_kb = NULL;
static bool colors_inverted = true;

// Contenedores globales de la UI
static lv_obj_t *content_area;
static lv_obj_t *btn_status;
static lv_obj_t *btn_move;
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
void create_config_screen(lv_obj_t *parent);
void create_files_screen(lv_obj_t *parent);
void create_temp_adjust_popup(bool is_nozzle);
void update_sidebar_selection(int active_tab);
void update_status_display();
static void update_temp_display();
static void update_coord_display();
static void temp_event_cb(lv_event_t *e);
static void temp_popup_cb(lv_event_t *e);
static void temp_popup_overlay_cb(lv_event_t *e);
static void temp_card_clicked_cb(lv_event_t *e);
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

void create_wifi_setup_popup();
void create_printer_setup_popup();
static void wifi_card_clicked_cb(lv_event_t *e);
static void printer_card_clicked_cb(lv_event_t *e);
static void timeout_dd_event_cb(lv_event_t *e);
static void wifi_save_cb(lv_event_t *e);
static void wifi_cancel_cb(lv_event_t *e);
static void printer_save_cb(lv_event_t *e);
static void printer_cancel_cb(lv_event_t *e);

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
    last_touch_time = millis(); // Registrar actividad táctil

    if (screen_is_off) {
      // 1. Despertar la pantalla física al nivel de brillo anterior
      analogWrite(TFT_BL, map(screen_brightness, 0, 100, 0, 255));
      screen_is_off = false;
      Serial.println("[Touch] Despertando pantalla por toque.");

      // 2. Consumir el toque (ignorar primer toque preventivo)
      data->state = LV_INDEV_STATE_REL;
      return;
    }

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
    
    // Limpieza de teclados y diálogos emergentes activos para evitar persistencia huérfana
    if (active_kb != NULL) {
      lv_obj_del(active_kb);
      active_kb = NULL;
    }
    if (wifi_setup_overlay != NULL) {
      lv_obj_del(wifi_setup_overlay);
      wifi_setup_overlay = NULL;
    }
    if (printer_setup_overlay != NULL) {
      lv_obj_del(printer_setup_overlay);
      printer_setup_overlay = NULL;
    }

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
    } else if (btn == btn_config) {
      update_sidebar_selection(2);
      create_config_screen(content_area);
    } else if (btn == btn_files) {
      update_sidebar_selection(3);
      create_files_screen(content_area);
    }
  }
}

void update_sidebar_selection(int active_tab) {
  // Resetear estilos a inactivos (Fondo barra lateral oscuro)
  lv_obj_set_style_bg_color(btn_status, COLOR_SIDEBAR, 0);
  lv_obj_set_style_bg_color(btn_move, COLOR_SIDEBAR, 0);
  lv_obj_set_style_bg_color(btn_config, COLOR_SIDEBAR, 0);
  lv_obj_set_style_bg_color(btn_files, COLOR_SIDEBAR, 0);

  // Pintar el botón activo en rojo carmesí brillante
  if (active_tab == 0) lv_obj_set_style_bg_color(btn_status, COLOR_RED_ACC, 0);
  if (active_tab == 1) lv_obj_set_style_bg_color(btn_move, COLOR_RED_ACC, 0);
  if (active_tab == 2) lv_obj_set_style_bg_color(btn_config, COLOR_RED_ACC, 0);
  if (active_tab == 3) lv_obj_set_style_bg_color(btn_files, COLOR_RED_ACC, 0);
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
  String path = "/printer/objects/query?extruder&heater_bed&virtual_sdcard&print_stats&gcode_move";
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

      // Parsear Coordenadas Reales (gcode_move)
      if (status.containsKey("gcode_move")) {
        JsonObject gmove = status["gcode_move"].as<JsonObject>();
        if (gmove.containsKey("gcode_position")) {
          JsonArray gpos = gmove["gcode_position"].as<JsonArray>();
          if (gpos.size() >= 3) {
            pos_x = gpos[0].as<float>();
            pos_y = gpos[1].as<float>();
            pos_z = gpos[2].as<float>();
          }
        }
      }
      
      // Actualizar interfaz LVGL
      update_temp_display();
      update_status_display();
      update_coord_display();
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
  if (coord_label != NULL) {
    String text = "X: " + String(pos_x, 1) + "   Y: " + String(pos_y, 1) + "   Z: " + String(pos_z, 2);
    lv_label_set_text(coord_label, text.c_str());
  }
}

static void jog_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    long dir = (long)lv_event_get_user_data(e);
    
    if (dir == 1) { pos_y += move_step; sendGcodeToMoonraker("G91\nG0 Y" + String(move_step) + "\nG90"); }   // Y+
    if (dir == 2) { pos_y -= move_step; sendGcodeToMoonraker("G91\nG0 Y-" + String(move_step) + "\nG90"); }  // Y-
    if (dir == 3) { pos_x -= move_step; sendGcodeToMoonraker("G91\nG0 X-" + String(move_step) + "\nG90"); }  // X-
    if (dir == 4) { pos_x += move_step; sendGcodeToMoonraker("G91\nG0 X" + String(move_step) + "\nG90"); }   // X+
    if (dir == 5) { // Z+ (safety-capped step size)
      float z_step = move_step > 10.0 ? 10.0 : move_step;
      pos_z += z_step;
      sendGcodeToMoonraker("G91\nG0 Z" + String(z_step) + "\nG90");
    }
    if (dir == 6) { // Z- (safety-capped step size)
      float z_step = move_step > 10.0 ? 10.0 : move_step;
      pos_z -= z_step;
      sendGcodeToMoonraker("G91\nG0 Z-" + String(z_step) + "\nG90");
    }
    if (dir == 7) { // Home All
      pos_x = 0.0; pos_y = 0.0; pos_z = 0.0;
      sendGcodeToMoonraker("G28");
    }
    if (dir == 8) { // Home X
      pos_x = 0.0;
      sendGcodeToMoonraker("G28 X");
    }
    if (dir == 9) { // Home Y
      pos_y = 0.0;
      sendGcodeToMoonraker("G28 Y");
    }
    if (dir == 10) { // Home Z
      pos_z = 0.0;
      sendGcodeToMoonraker("G28 Z");
    }
    if (dir == 11) { // Home XY
      pos_x = 0.0; pos_y = 0.0;
      sendGcodeToMoonraker("G28 X Y");
    }
    if (dir == 12) { // Desactivar Motores
      sendGcodeToMoonraker("M84");
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
    if (id == 0) move_step = 0.1;
    if (id == 1) move_step = 1.0;
    if (id == 2) move_step = 10.0;
    if (id == 3) move_step = 25.0;
    if (id == 4) move_step = 50.0;
    if (id == 5) move_step = 100.0;
  }
}

// Declaración de los botones de temperatura clickable
static lv_obj_t *btn_noz_card = NULL;
static lv_obj_t *btn_bed_card = NULL;

static void temp_card_clicked_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    long is_nozzle = (long)lv_event_get_user_data(e);
    create_temp_adjust_popup(is_nozzle == 1);
  }
}

void create_move_screen(lv_obj_t *parent) {
  // Convertir parent en scrollable con flex
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(parent, 8, 0);
  lv_obj_set_style_pad_row(parent, 8, 0);
  lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);

  // 1. Barra de Coordenadas Superior (Ancho 215px, Alto 24px)
  lv_obj_t *coord_card = lv_obj_create(parent);
  lv_obj_set_size(coord_card, 215, 24);
  lv_obj_set_style_bg_color(coord_card, COLOR_CARD, 0);
  lv_obj_set_style_border_width(coord_card, 1, 0);
  lv_obj_set_style_border_color(coord_card, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(coord_card, 4, 0);
  lv_obj_set_style_pad_all(coord_card, 0, 0);
  
  coord_label = lv_label_create(coord_card);
  lv_obj_set_style_text_font(coord_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(coord_label, lv_color_make(255, 255, 255), 0);
  lv_obj_align(coord_label, LV_ALIGN_CENTER, 0, 0);
  update_coord_display();

  // 2. Contenedor principal de controles de movimiento (D-pad + Z)
  lv_obj_t *ctrl_box = lv_obj_create(parent);
  lv_obj_set_size(ctrl_box, 215, 105);
  lv_obj_set_style_bg_color(ctrl_box, COLOR_BG, 0);
  lv_obj_set_style_border_width(ctrl_box, 0, 0);
  lv_obj_set_style_pad_all(ctrl_box, 0, 0);

  int btn_size = 35;
  int dpad_offset_x = 15;
  int z_offset_x = 155;
  
  // --- D-PAD X/Y ---
  // Y+ (Arriba)
  lv_obj_t *btn_yp = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_yp, btn_size, btn_size);
  lv_obj_align(btn_yp, LV_ALIGN_TOP_LEFT, dpad_offset_x + btn_size, 0);
  lv_obj_set_style_bg_color(btn_yp, COLOR_CARD, 0);
  lv_obj_set_style_radius(btn_yp, 4, 0);
  lv_obj_set_style_pad_all(btn_yp, 0, 0);
  lv_obj_add_event_cb(btn_yp, jog_event_cb, LV_EVENT_CLICKED, (void*)1);
  lv_obj_t *lbl_yp = lv_label_create(btn_yp);
  lv_label_set_text(lbl_yp, LV_SYMBOL_UP);
  lv_obj_align(lbl_yp, LV_ALIGN_CENTER, 0, 0);

  // X- (Izquierda)
  lv_obj_t *btn_xm = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_xm, btn_size, btn_size);
  lv_obj_align(btn_xm, LV_ALIGN_TOP_LEFT, dpad_offset_x, btn_size);
  lv_obj_set_style_bg_color(btn_xm, COLOR_CARD, 0);
  lv_obj_set_style_radius(btn_xm, 4, 0);
  lv_obj_set_style_pad_all(btn_xm, 0, 0);
  lv_obj_add_event_cb(btn_xm, jog_event_cb, LV_EVENT_CLICKED, (void*)3);
  lv_obj_t *lbl_xm = lv_label_create(btn_xm);
  lv_label_set_text(lbl_xm, LV_SYMBOL_LEFT);
  lv_obj_align(lbl_xm, LV_ALIGN_CENTER, 0, 0);

  // Home XY (Centro) - Rojo carmesí para mantener la estética
  lv_obj_t *btn_home_xy = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_home_xy, btn_size, btn_size);
  lv_obj_align(btn_home_xy, LV_ALIGN_TOP_LEFT, dpad_offset_x + btn_size, btn_size);
  lv_obj_set_style_bg_color(btn_home_xy, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(btn_home_xy, 4, 0);
  lv_obj_set_style_pad_all(btn_home_xy, 0, 0);
  lv_obj_add_event_cb(btn_home_xy, jog_event_cb, LV_EVENT_CLICKED, (void*)11);
  lv_obj_t *lbl_home_xy = lv_label_create(btn_home_xy);
  lv_label_set_text(lbl_home_xy, LV_SYMBOL_HOME);
  lv_obj_align(lbl_home_xy, LV_ALIGN_CENTER, 0, 0);

  // X+ (Derecha)
  lv_obj_t *btn_xp = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_xp, btn_size, btn_size);
  lv_obj_align(btn_xp, LV_ALIGN_TOP_LEFT, dpad_offset_x + btn_size * 2, btn_size);
  lv_obj_set_style_bg_color(btn_xp, COLOR_CARD, 0);
  lv_obj_set_style_radius(btn_xp, 4, 0);
  lv_obj_set_style_pad_all(btn_xp, 0, 0);
  lv_obj_add_event_cb(btn_xp, jog_event_cb, LV_EVENT_CLICKED, (void*)4);
  lv_obj_t *lbl_xp = lv_label_create(btn_xp);
  lv_label_set_text(lbl_xp, LV_SYMBOL_RIGHT);
  lv_obj_align(lbl_xp, LV_ALIGN_CENTER, 0, 0);

  // Y- (Abajo)
  lv_obj_t *btn_ym = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_ym, btn_size, btn_size);
  lv_obj_align(btn_ym, LV_ALIGN_TOP_LEFT, dpad_offset_x + btn_size, btn_size * 2);
  lv_obj_set_style_bg_color(btn_ym, COLOR_CARD, 0);
  lv_obj_set_style_radius(btn_ym, 4, 0);
  lv_obj_set_style_pad_all(btn_ym, 0, 0);
  lv_obj_add_event_cb(btn_ym, jog_event_cb, LV_EVENT_CLICKED, (void*)2);
  lv_obj_t *lbl_ym = lv_label_create(btn_ym);
  lv_label_set_text(lbl_ym, LV_SYMBOL_DOWN);
  lv_obj_align(lbl_ym, LV_ALIGN_CENTER, 0, 0);

  // --- CONTROLES DE Z ---
  // Z Up (Arriba)
  lv_obj_t *btn_zp = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_zp, btn_size, btn_size);
  lv_obj_align(btn_zp, LV_ALIGN_TOP_LEFT, z_offset_x, 0);
  lv_obj_set_style_bg_color(btn_zp, COLOR_CARD, 0);
  lv_obj_set_style_radius(btn_zp, 4, 0);
  lv_obj_set_style_pad_all(btn_zp, 0, 0);
  lv_obj_add_event_cb(btn_zp, jog_event_cb, LV_EVENT_CLICKED, (void*)5);
  lv_obj_t *lbl_zp = lv_label_create(btn_zp);
  lv_label_set_text(lbl_zp, LV_SYMBOL_UP);
  lv_obj_align(lbl_zp, LV_ALIGN_CENTER, 0, 0);

  // Z Home (Centro) - Rojo carmesí para mantener la estética
  lv_obj_t *btn_home_z = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_home_z, btn_size, btn_size);
  lv_obj_align(btn_home_z, LV_ALIGN_TOP_LEFT, z_offset_x, btn_size);
  lv_obj_set_style_bg_color(btn_home_z, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(btn_home_z, 4, 0);
  lv_obj_set_style_pad_all(btn_home_z, 0, 0);
  lv_obj_add_event_cb(btn_home_z, jog_event_cb, LV_EVENT_CLICKED, (void*)10);
  lv_obj_t *lbl_home_z = lv_label_create(btn_home_z);
  lv_label_set_text(lbl_home_z, LV_SYMBOL_HOME);
  lv_obj_align(lbl_home_z, LV_ALIGN_CENTER, 0, 0);

  // Z Down (Abajo)
  lv_obj_t *btn_zm = lv_btn_create(ctrl_box);
  lv_obj_set_size(btn_zm, btn_size, btn_size);
  lv_obj_align(btn_zm, LV_ALIGN_TOP_LEFT, z_offset_x, btn_size * 2);
  lv_obj_set_style_bg_color(btn_zm, COLOR_CARD, 0);
  lv_obj_set_style_radius(btn_zm, 4, 0);
  lv_obj_set_style_pad_all(btn_zm, 0, 0);
  lv_obj_add_event_cb(btn_zm, jog_event_cb, LV_EVENT_CLICKED, (void*)6);
  lv_obj_t *lbl_zm = lv_label_create(btn_zm);
  lv_label_set_text(lbl_zm, LV_SYMBOL_DOWN);
  lv_obj_align(lbl_zm, LV_ALIGN_CENTER, 0, 0);

  // 3. Matriz de Pasos Step (Ancho 215px, Alto 30px)
  static const char *btnm_map[] = {"0.1", "1", "10", "25", "50", "100", ""};
  lv_obj_t *btnm = lv_btnmatrix_create(parent);
  lv_obj_set_size(btnm, 215, 30);
  lv_btnmatrix_set_map(btnm, btnm_map);
  lv_obj_set_style_bg_color(btnm, COLOR_BG, 0);
  lv_obj_set_style_border_width(btnm, 0, 0);
  lv_obj_set_style_pad_all(btnm, 0, 0);
  
  // Establecer el botón inicial checked (por defecto 10 mm)
  lv_btnmatrix_set_btn_ctrl(btnm, 2, LV_BTNMATRIX_CTRL_CHECKED);
  lv_btnmatrix_set_one_checked(btnm, true);
  lv_obj_add_event_cb(btnm, step_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // 4. Homing General y Motores Off (Fila horizontal de dos botones)
  lv_obj_t *homing_box = lv_obj_create(parent);
  lv_obj_set_size(homing_box, 215, 34);
  lv_obj_set_style_bg_color(homing_box, COLOR_BG, 0);
  lv_obj_set_style_border_width(homing_box, 0, 0);
  lv_obj_set_style_pad_all(homing_box, 0, 0);

  // Botón Home Todos (Rojo Carmesí)
  lv_obj_t *btn_home_all = lv_btn_create(homing_box);
  lv_obj_set_size(btn_home_all, 102, 34);
  lv_obj_align(btn_home_all, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(btn_home_all, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(btn_home_all, 4, 0);
  lv_obj_set_style_pad_all(btn_home_all, 0, 0);
  lv_obj_add_event_cb(btn_home_all, jog_event_cb, LV_EVENT_CLICKED, (void*)7);
  lv_obj_t *lbl_home_all = lv_label_create(btn_home_all);
  lv_label_set_text(lbl_home_all, LV_SYMBOL_HOME " TODOS");
  lv_obj_set_style_text_font(lbl_home_all, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_home_all, LV_ALIGN_CENTER, 0, 0);

  // Botón Desactivar Motores (Fondo oscuro con borde rojo carmesí)
  lv_obj_t *btn_motors_off = lv_btn_create(homing_box);
  lv_obj_set_size(btn_motors_off, 102, 34);
  lv_obj_align(btn_motors_off, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(btn_motors_off, COLOR_SIDEBAR, 0);
  lv_obj_set_style_border_width(btn_motors_off, 1, 0);
  lv_obj_set_style_border_color(btn_motors_off, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(btn_motors_off, 4, 0);
  lv_obj_set_style_pad_all(btn_motors_off, 0, 0);
  lv_obj_add_event_cb(btn_motors_off, jog_event_cb, LV_EVENT_CLICKED, (void*)12);
  lv_obj_t *lbl_motors_off = lv_label_create(btn_motors_off);
  lv_label_set_text(lbl_motors_off, "Motores Off");
  lv_obj_set_style_text_font(lbl_motors_off, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_motors_off, LV_ALIGN_CENTER, 0, 0);

  // Separador Visual e Indicador de Sección de Temperatura
  lv_obj_t *sep = lv_obj_create(parent);
  lv_obj_set_size(sep, 215, 1);
  lv_obj_set_style_bg_color(sep, COLOR_RED_DARK, 0);
  lv_obj_set_style_border_width(sep, 0, 0);
  lv_obj_set_style_pad_all(sep, 0, 0);

  // 5. Tarjetas de Temperatura Clickable (Fila horizontal de dos botones)
  lv_obj_t *temp_box = lv_obj_create(parent);
  lv_obj_set_size(temp_box, 215, 36);
  lv_obj_set_style_bg_color(temp_box, COLOR_BG, 0);
  lv_obj_set_style_border_width(temp_box, 0, 0);
  lv_obj_set_style_pad_all(temp_box, 0, 0);

  // Tarjeta Boquilla
  btn_noz_card = lv_btn_create(temp_box);
  lv_obj_set_size(btn_noz_card, 102, 36);
  lv_obj_align(btn_noz_card, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(btn_noz_card, COLOR_CARD, 0);
  lv_obj_set_style_border_width(btn_noz_card, 0, 0);
  lv_obj_set_style_radius(btn_noz_card, 6, 0);
  lv_obj_set_style_pad_all(btn_noz_card, 4, 0);
  lv_obj_add_event_cb(btn_noz_card, temp_card_clicked_cb, LV_EVENT_CLICKED, (void*)1);
  
  lbl_noz = lv_label_create(btn_noz_card);
  lv_obj_set_style_text_font(lbl_noz, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_noz, lv_color_make(255, 255, 255), 0);
  lv_obj_align(lbl_noz, LV_ALIGN_CENTER, 0, 0);

  // Tarjeta Cama
  btn_bed_card = lv_btn_create(temp_box);
  lv_obj_set_size(btn_bed_card, 102, 36);
  lv_obj_align(btn_bed_card, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(btn_bed_card, COLOR_CARD, 0);
  lv_obj_set_style_border_width(btn_bed_card, 0, 0);
  lv_obj_set_style_radius(btn_bed_card, 6, 0);
  lv_obj_set_style_pad_all(btn_bed_card, 4, 0);
  lv_obj_add_event_cb(btn_bed_card, temp_card_clicked_cb, LV_EVENT_CLICKED, (void*)2);
  
  lbl_bed = lv_label_create(btn_bed_card);
  lv_obj_set_style_text_font(lbl_bed, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_bed, lv_color_make(255, 255, 255), 0);
  lv_obj_align(lbl_bed, LV_ALIGN_CENTER, 0, 0);

  // 6. Presets Térmicos PLA / Enfriar (Fila horizontal de dos botones)
  lv_obj_t *preset_box = lv_obj_create(parent);
  lv_obj_set_size(preset_box, 215, 36);
  lv_obj_set_style_bg_color(preset_box, COLOR_BG, 0);
  lv_obj_set_style_border_width(preset_box, 0, 0);
  lv_obj_set_style_pad_all(preset_box, 0, 0);

  lv_obj_t *btn_pre = lv_btn_create(preset_box);
  lv_obj_set_size(btn_pre, 102, 36);
  lv_obj_align(btn_pre, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(btn_pre, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(btn_pre, 4, 0);
  lv_obj_add_event_cb(btn_pre, temp_event_cb, LV_EVENT_CLICKED, (void*)5);
  lv_obj_t *lbl_pre = lv_label_create(btn_pre);
  lv_label_set_text(lbl_pre, "Precal PLA");
  lv_obj_set_style_text_font(lbl_pre, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_pre, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *btn_cool = lv_btn_create(preset_box);
  lv_obj_set_size(btn_cool, 102, 36);
  lv_obj_align(btn_cool, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(btn_cool, COLOR_RED_DARK, 0);
  lv_obj_set_style_radius(btn_cool, 4, 0);
  lv_obj_add_event_cb(btn_cool, temp_event_cb, LV_EVENT_CLICKED, (void*)6);
  lv_obj_t *lbl_cool = lv_label_create(btn_cool);
  lv_label_set_text(lbl_cool, "Enfriar");
  lv_obj_set_style_text_font(lbl_cool, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_cool, LV_ALIGN_CENTER, 0, 0);

  update_temp_display();
}

/*---------------------------------------------------------------------------
 * 3. PANTALLA DE TEMPERATURA (TEMPERATURAS)
 *---------------------------------------------------------------------------*/

static void update_temp_display() {
  if (lbl_noz != NULL && lbl_bed != NULL) {
    lv_label_set_text_fmt(lbl_noz, "Boq: %d° / %d°", nozzle_temp, nozzle_target);
    lv_label_set_text_fmt(lbl_bed, "Cama: %d° / %d°", bed_temp, bed_target);
  }
}

// Variables para rastrear el popup activo
static lv_obj_t *temp_popup_overlay = NULL;
static lv_obj_t *lbl_popup_val = NULL;
static bool popup_for_nozzle = true;

static void temp_popup_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    long val = (long)lv_event_get_user_data(e);
    
    if (val == 999) { // Cerrar / Aceptar
      if (temp_popup_overlay != NULL) {
        lv_obj_del(temp_popup_overlay);
        temp_popup_overlay = NULL;
        lbl_popup_val = NULL;
      }
      return;
    }
    
    if (popup_for_nozzle) {
      if (val == 0) {
        nozzle_target = 0;
      } else {
        nozzle_target += val;
      }
      if (nozzle_target < 0) nozzle_target = 0;
      if (nozzle_target > 280) nozzle_target = 280;
      
      sendGcodeToMoonraker("M104 S" + String(nozzle_target));
      if (lbl_popup_val != NULL) {
        lv_label_set_text_fmt(lbl_popup_val, "%d°C", nozzle_target);
      }
    } else {
      if (val == 0) {
        bed_target = 0;
      } else {
        bed_target += val;
      }
      if (bed_target < 0) bed_target = 0;
      if (bed_target > 110) bed_target = 110;
      
      sendGcodeToMoonraker("M140 S" + String(bed_target));
      if (lbl_popup_val != NULL) {
        lv_label_set_text_fmt(lbl_popup_val, "%d°C", bed_target);
      }
    }
    update_temp_display();
  }
}

// Clic en el fondo backdrop para cerrar
static void temp_popup_overlay_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    lv_obj_t *target = lv_event_get_target(e);
    if (target == temp_popup_overlay) {
      lv_obj_del(temp_popup_overlay);
      temp_popup_overlay = NULL;
      lbl_popup_val = NULL;
    }
  }
}

void create_temp_adjust_popup(bool is_nozzle) {
  popup_for_nozzle = is_nozzle;
  
  // 1. Backdrop overlay
  temp_popup_overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(temp_popup_overlay, 320, 240);
  lv_obj_align(temp_popup_overlay, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(temp_popup_overlay, lv_color_make(0, 0, 0), 0);
  lv_obj_set_style_bg_opa(temp_popup_overlay, LV_OPA_60, 0);
  lv_obj_set_style_border_width(temp_popup_overlay, 0, 0);
  lv_obj_set_style_radius(temp_popup_overlay, 0, 0);
  lv_obj_add_event_cb(temp_popup_overlay, temp_popup_overlay_cb, LV_EVENT_CLICKED, NULL);
  
  // 2. Tarjeta del Diálogo
  lv_obj_t *card = lv_obj_create(temp_popup_overlay);
  lv_obj_set_size(card, 220, 140);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_pad_all(card, 6, 0);
  
  // Título del Dialogo
  lv_obj_t *lbl_title = lv_label_create(card);
  lv_label_set_text(lbl_title, is_nozzle ? "Ajustar Boquilla" : "Ajustar Cama");
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_title, COLOR_GREY_TEXT, 0);
  lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 2);
  
  // Valor objetivo destacado
  lbl_popup_val = lv_label_create(card);
  lv_label_set_text_fmt(lbl_popup_val, "%d°C", is_nozzle ? nozzle_target : bed_target);
  lv_obj_set_style_text_font(lbl_popup_val, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lbl_popup_val, COLOR_RED_ACC, 0);
  lv_obj_align(lbl_popup_val, LV_ALIGN_TOP_MID, 0, 20);
  
  // Botonera de incrementos
  int btn_w = 42;
  int btn_h = 30;
  int btn_y = 52;
  
  // -10
  lv_obj_t *b_m10 = lv_btn_create(card);
  lv_obj_set_size(b_m10, btn_w, btn_h);
  lv_obj_align(b_m10, LV_ALIGN_TOP_LEFT, 4, btn_y);
  lv_obj_set_style_bg_color(b_m10, COLOR_SIDEBAR, 0);
  lv_obj_set_style_radius(b_m10, 4, 0);
  lv_obj_add_event_cb(b_m10, temp_popup_cb, LV_EVENT_CLICKED, (void*)-10);
  lv_obj_t *l_m10 = lv_label_create(b_m10);
  lv_label_set_text(l_m10, "-10");
  lv_obj_set_style_text_font(l_m10, &lv_font_montserrat_14, 0);
  lv_obj_align(l_m10, LV_ALIGN_CENTER, 0, 0);
  
  // -5
  lv_obj_t *b_m5 = lv_btn_create(card);
  lv_obj_set_size(b_m5, btn_w, btn_h);
  lv_obj_align(b_m5, LV_ALIGN_TOP_LEFT, 52, btn_y);
  lv_obj_set_style_bg_color(b_m5, COLOR_SIDEBAR, 0);
  lv_obj_set_style_radius(b_m5, 4, 0);
  lv_obj_add_event_cb(b_m5, temp_popup_cb, LV_EVENT_CLICKED, (void*)-5);
  lv_obj_t *l_m5 = lv_label_create(b_m5);
  lv_label_set_text(l_m5, "-5");
  lv_obj_set_style_text_font(l_m5, &lv_font_montserrat_14, 0);
  lv_obj_align(l_m5, LV_ALIGN_CENTER, 0, 0);
  
  // +5
  lv_obj_t *b_p5 = lv_btn_create(card);
  lv_obj_set_size(b_p5, btn_w, btn_h);
  lv_obj_align(b_p5, LV_ALIGN_TOP_LEFT, 110, btn_y);
  lv_obj_set_style_bg_color(b_p5, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(b_p5, 4, 0);
  lv_obj_add_event_cb(b_p5, temp_popup_cb, LV_EVENT_CLICKED, (void*)5);
  lv_obj_t *l_p5 = lv_label_create(b_p5);
  lv_label_set_text(l_p5, "+5");
  lv_obj_set_style_text_font(l_p5, &lv_font_montserrat_14, 0);
  lv_obj_align(l_p5, LV_ALIGN_CENTER, 0, 0);
  
  // +10
  lv_obj_t *b_p10 = lv_btn_create(card);
  lv_obj_set_size(b_p10, btn_w, btn_h);
  lv_obj_align(b_p10, LV_ALIGN_TOP_LEFT, 158, btn_y);
  lv_obj_set_style_bg_color(b_p10, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(b_p10, 4, 0);
  lv_obj_add_event_cb(b_p10, temp_popup_cb, LV_EVENT_CLICKED, (void*)10);
  lv_obj_t *l_p10 = lv_label_create(b_p10);
  lv_label_set_text(l_p10, "+10");
  lv_obj_set_style_text_font(l_p10, &lv_font_montserrat_14, 0);
  lv_obj_align(l_p10, LV_ALIGN_CENTER, 0, 0);
  
  // Fila inferior (Apagar y Aceptar)
  lv_obj_t *b_off = lv_btn_create(card);
  lv_obj_set_size(b_off, 75, 32);
  lv_obj_align(b_off, LV_ALIGN_BOTTOM_LEFT, 10, -4);
  lv_obj_set_style_bg_color(b_off, COLOR_RED_DARK, 0);
  lv_obj_set_style_radius(b_off, 4, 0);
  lv_obj_add_event_cb(b_off, temp_popup_cb, LV_EVENT_CLICKED, (void*)0);
  lv_obj_t *l_off = lv_label_create(b_off);
  lv_label_set_text(l_off, "Apagar");
  lv_obj_set_style_text_font(l_off, &lv_font_montserrat_14, 0);
  lv_obj_align(l_off, LV_ALIGN_CENTER, 0, 0);
  
  lv_obj_t *b_ok = lv_btn_create(card);
  lv_obj_set_size(b_ok, 95, 32);
  lv_obj_align(b_ok, LV_ALIGN_BOTTOM_RIGHT, -10, -4);
  lv_obj_set_style_bg_color(b_ok, COLOR_SIDEBAR, 0);
  lv_obj_set_style_border_color(b_ok, COLOR_RED_ACC, 0);
  lv_obj_set_style_border_width(b_ok, 1, 0);
  lv_obj_set_style_radius(b_ok, 4, 0);
  lv_obj_add_event_cb(b_ok, temp_popup_cb, LV_EVENT_CLICKED, (void*)999);
  lv_obj_t *l_ok = lv_label_create(b_ok);
  lv_label_set_text(l_ok, "Aceptar");
  lv_obj_set_style_text_font(l_ok, &lv_font_montserrat_14, 0);
  lv_obj_align(l_ok, LV_ALIGN_CENTER, 0, 0);
}

static void temp_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    long act = (long)lv_event_get_user_data(e);
    
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

    update_temp_display();
  }
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
    if (kb == active_kb) {
      active_kb = NULL;
    }
  }
}

static void ta_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = lv_event_get_target(e);
  if (code == LV_EVENT_FOCUSED) {
    if (active_kb != NULL) {
      lv_obj_del(active_kb);
      active_kb = NULL;
    }
    active_kb = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(active_kb, ta);
    lv_obj_add_event_cb(active_kb, kb_event_cb, LV_EVENT_ALL, NULL);
  }
}

static lv_obj_t *ta_ssid;
static lv_obj_t *ta_pass;
static lv_obj_t *ta_ip;

// Wi-Fi Popup cancel/save callbacks
static void wifi_cancel_cb(lv_event_t *e) {
  if (active_kb != NULL) {
    lv_obj_del(active_kb);
    active_kb = NULL;
  }
  if (wifi_setup_overlay != NULL) {
    lv_obj_del(wifi_setup_overlay);
    wifi_setup_overlay = NULL;
  }
}

static void wifi_save_cb(lv_event_t *e) {
  if (active_kb != NULL) {
    lv_obj_del(active_kb);
    active_kb = NULL;
  }
  if (ta_ssid != NULL && ta_pass != NULL) {
    wifi_ssid_str = String(lv_textarea_get_text(ta_ssid));
    wifi_pass_str = String(lv_textarea_get_text(ta_pass));
    wifi_ssid_str.trim();
    wifi_pass_str.trim();
    reconnect_requested = true;
    create_toast("Wi-Fi", "Guardando y reconectando...");
  }
  if (wifi_setup_overlay != NULL) {
    lv_obj_del(wifi_setup_overlay);
    wifi_setup_overlay = NULL;
  }
}

// Printer Popup cancel/save callbacks
static void printer_cancel_cb(lv_event_t *e) {
  if (active_kb != NULL) {
    lv_obj_del(active_kb);
    active_kb = NULL;
  }
  if (printer_setup_overlay != NULL) {
    lv_obj_del(printer_setup_overlay);
    printer_setup_overlay = NULL;
  }
}

static void printer_save_cb(lv_event_t *e) {
  if (active_kb != NULL) {
    lv_obj_del(active_kb);
    active_kb = NULL;
  }
  if (ta_ip != NULL) {
    moonraker_host_str = String(lv_textarea_get_text(ta_ip));
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
    create_toast("Impresora", "Guardando y reconectando...");
  }
  if (printer_setup_overlay != NULL) {
    lv_obj_del(printer_setup_overlay);
    printer_setup_overlay = NULL;
  }
}

// Timeout dropdown callback
static void timeout_dd_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED) {
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dd);
    if (selected == 0) {
      screen_timeout_minutes = 0; // Nunca
    } else if (selected == 1) {
      screen_timeout_minutes = 1; // 1 Min
    } else if (selected == 2) {
      screen_timeout_minutes = 5; // 5 Min
    }
    last_touch_time = millis(); // Refrescar timer
    Serial.printf("[Config] Timeout cambiado a: %d min\n", screen_timeout_minutes);
  }
}

// Card Clicked callbacks
static void wifi_card_clicked_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    create_wifi_setup_popup();
  }
}

static void printer_card_clicked_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    create_printer_setup_popup();
  }
}

// Create Wi-Fi setup popup
void create_wifi_setup_popup() {
  if (wifi_setup_overlay != NULL) return;

  wifi_setup_overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(wifi_setup_overlay, 320, 240);
  lv_obj_set_style_bg_color(wifi_setup_overlay, lv_color_make(0, 0, 0), 0);
  lv_obj_set_style_bg_opa(wifi_setup_overlay, LV_OPA_70, 0);
  lv_obj_set_style_border_width(wifi_setup_overlay, 0, 0);
  lv_obj_set_style_radius(wifi_setup_overlay, 0, 0);
  lv_obj_align(wifi_setup_overlay, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *dialog = lv_obj_create(wifi_setup_overlay);
  lv_obj_set_size(dialog, 290, 115);
  lv_obj_align(dialog, LV_ALIGN_TOP_MID, 0, 5);
  lv_obj_set_style_bg_color(dialog, COLOR_CARD, 0);
  lv_obj_set_style_border_color(dialog, COLOR_RED_ACC, 0);
  lv_obj_set_style_border_width(dialog, 1, 0);
  lv_obj_set_style_radius(dialog, 8, 0);
  lv_obj_set_style_pad_all(dialog, 6, 0);

  lv_obj_t *lbl_title = lv_label_create(dialog);
  lv_label_set_text(lbl_title, "Ajustes de Wi-Fi");
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_title, lv_color_make(255, 255, 255), 0);
  lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 2);

  ta_ssid = lv_textarea_create(dialog);
  lv_obj_set_size(ta_ssid, 135, 32);
  lv_obj_align(ta_ssid, LV_ALIGN_TOP_LEFT, 2, 24);
  lv_textarea_set_one_line(ta_ssid, true);
  lv_textarea_set_text(ta_ssid, wifi_ssid_str.c_str());
  lv_textarea_set_placeholder_text(ta_ssid, "SSID");
  lv_obj_set_style_bg_color(ta_ssid, COLOR_BG, 0);
  lv_obj_set_style_border_color(ta_ssid, COLOR_SIDEBAR, 0);
  lv_obj_set_style_text_color(ta_ssid, lv_color_make(255, 255, 255), 0);
  lv_obj_add_event_cb(ta_ssid, ta_event_cb, LV_EVENT_ALL, NULL);

  ta_pass = lv_textarea_create(dialog);
  lv_obj_set_size(ta_pass, 135, 32);
  lv_obj_align(ta_pass, LV_ALIGN_TOP_RIGHT, -2, 24);
  lv_textarea_set_one_line(ta_pass, true);
  lv_textarea_set_text(ta_pass, wifi_pass_str.c_str());
  lv_textarea_set_placeholder_text(ta_pass, "Password");
  lv_obj_set_style_bg_color(ta_pass, COLOR_BG, 0);
  lv_obj_set_style_border_color(ta_pass, COLOR_SIDEBAR, 0);
  lv_obj_set_style_text_color(ta_pass, lv_color_make(255, 255, 255), 0);
  lv_obj_add_event_cb(ta_pass, ta_event_cb, LV_EVENT_ALL, NULL);

  lv_obj_t *btn_cancel = lv_btn_create(dialog);
  lv_obj_set_size(btn_cancel, 130, 32);
  lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 2, -2);
  lv_obj_set_style_bg_color(btn_cancel, COLOR_RED_DARK, 0);
  lv_obj_set_style_radius(btn_cancel, 6, 0);
  lv_obj_add_event_cb(btn_cancel, wifi_cancel_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
  lv_label_set_text(lbl_cancel, "Cancelar");
  lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_cancel, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *btn_save = lv_btn_create(dialog);
  lv_obj_set_size(btn_save, 130, 32);
  lv_obj_align(btn_save, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
  lv_obj_set_style_bg_color(btn_save, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(btn_save, 6, 0);
  lv_obj_add_event_cb(btn_save, wifi_save_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl_save = lv_label_create(btn_save);
  lv_label_set_text(lbl_save, "Guardar");
  lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_save, LV_ALIGN_CENTER, 0, 0);
}

// Create Printer setup popup
void create_printer_setup_popup() {
  if (printer_setup_overlay != NULL) return;

  printer_setup_overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(printer_setup_overlay, 320, 240);
  lv_obj_set_style_bg_color(printer_setup_overlay, lv_color_make(0, 0, 0), 0);
  lv_obj_set_style_bg_opa(printer_setup_overlay, LV_OPA_70, 0);
  lv_obj_set_style_border_width(printer_setup_overlay, 0, 0);
  lv_obj_set_style_radius(printer_setup_overlay, 0, 0);
  lv_obj_align(printer_setup_overlay, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *dialog = lv_obj_create(printer_setup_overlay);
  lv_obj_set_size(dialog, 290, 115);
  lv_obj_align(dialog, LV_ALIGN_TOP_MID, 0, 5);
  lv_obj_set_style_bg_color(dialog, COLOR_CARD, 0);
  lv_obj_set_style_border_color(dialog, COLOR_RED_ACC, 0);
  lv_obj_set_style_border_width(dialog, 1, 0);
  lv_obj_set_style_radius(dialog, 8, 0);
  lv_obj_set_style_pad_all(dialog, 6, 0);

  lv_obj_t *lbl_title = lv_label_create(dialog);
  lv_label_set_text(lbl_title, "Ajustes de Impresora (Moonraker)");
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_title, lv_color_make(255, 255, 255), 0);
  lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 2);

  ta_ip = lv_textarea_create(dialog);
  lv_obj_set_size(ta_ip, 270, 32);
  lv_obj_align(ta_ip, LV_ALIGN_TOP_MID, 0, 24);
  lv_textarea_set_one_line(ta_ip, true);
  lv_textarea_set_text(ta_ip, moonraker_host_str.c_str());
  lv_textarea_set_placeholder_text(ta_ip, "IP o Host (ej. 192.168.1.100)");
  lv_obj_set_style_bg_color(ta_ip, COLOR_BG, 0);
  lv_obj_set_style_border_color(ta_ip, COLOR_SIDEBAR, 0);
  lv_obj_set_style_text_color(ta_ip, lv_color_make(255, 255, 255), 0);
  lv_obj_add_event_cb(ta_ip, ta_event_cb, LV_EVENT_ALL, NULL);

  lv_obj_t *btn_cancel = lv_btn_create(dialog);
  lv_obj_set_size(btn_cancel, 130, 32);
  lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 2, -2);
  lv_obj_set_style_bg_color(btn_cancel, COLOR_RED_DARK, 0);
  lv_obj_set_style_radius(btn_cancel, 6, 0);
  lv_obj_add_event_cb(btn_cancel, printer_cancel_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
  lv_label_set_text(lbl_cancel, "Cancelar");
  lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_cancel, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *btn_save = lv_btn_create(dialog);
  lv_obj_set_size(btn_save, 130, 32);
  lv_obj_align(btn_save, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
  lv_obj_set_style_bg_color(btn_save, COLOR_RED_ACC, 0);
  lv_obj_set_style_radius(btn_save, 6, 0);
  lv_obj_add_event_cb(btn_save, printer_save_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl_save = lv_label_create(btn_save);
  lv_label_set_text(lbl_save, "Guardar");
  lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_save, LV_ALIGN_CENTER, 0, 0);
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

  // --- TARJETAS DE CONEXIÓN SEPARADAS Y CLICKABLES ---

  // Tarjeta Wi-Fi Clickable
  lv_obj_t *btn_wifi_card = lv_btn_create(parent);
  lv_obj_set_size(btn_wifi_card, 215, 38);
  lv_obj_set_style_bg_color(btn_wifi_card, COLOR_CARD, 0);
  lv_obj_set_style_border_width(btn_wifi_card, 0, 0);
  lv_obj_set_style_radius(btn_wifi_card, 6, 0);
  lv_obj_set_style_pad_all(btn_wifi_card, 6, 0);
  lv_obj_add_event_cb(btn_wifi_card, wifi_card_clicked_cb, LV_EVENT_CLICKED, NULL);

  lbl_wifi_state = lv_label_create(btn_wifi_card);
  lv_obj_align(lbl_wifi_state, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_text_font(lbl_wifi_state, &lv_font_montserrat_14, 0);
  
  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text_fmt(lbl_wifi_state, "Wi-Fi: SI (IP: %s)", WiFi.localIP().toString().c_str());
    lv_obj_set_style_text_color(lbl_wifi_state, COLOR_GREEN, 0);
  } else {
    lv_label_set_text(lbl_wifi_state, "Wi-Fi: DESCONECTADO");
    lv_obj_set_style_text_color(lbl_wifi_state, COLOR_RED_ACC, 0);
  }

  // Tarjeta Impresora Clickable
  lv_obj_t *btn_printer_card = lv_btn_create(parent);
  lv_obj_set_size(btn_printer_card, 215, 38);
  lv_obj_set_style_bg_color(btn_printer_card, COLOR_CARD, 0);
  lv_obj_set_style_border_width(btn_printer_card, 0, 0);
  lv_obj_set_style_radius(btn_printer_card, 6, 0);
  lv_obj_set_style_pad_all(btn_printer_card, 6, 0);
  lv_obj_add_event_cb(btn_printer_card, printer_card_clicked_cb, LV_EVENT_CLICKED, NULL);

  lbl_printer_state = lv_label_create(btn_printer_card);
  lv_obj_align(lbl_printer_state, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_text_font(lbl_printer_state, &lv_font_montserrat_14, 0);
  
  if (ws_connected) {
    lv_label_set_text(lbl_printer_state, "Impresora: CONECTADA");
    lv_obj_set_style_text_color(lbl_printer_state, COLOR_GREEN, 0);
  } else {
    lv_label_set_text(lbl_printer_state, "Impresora: SIN CONEXION");
    lv_obj_set_style_text_color(lbl_printer_state, COLOR_RED_ACC, 0);
  }

  // Separador decorativo
  lv_obj_t *sep = lv_obj_create(parent);
  lv_obj_set_size(sep, 215, 1);
  lv_obj_set_style_bg_color(sep, COLOR_CARD, 0);
  lv_obj_set_style_border_width(sep, 0, 0);

  // Tarjeta Brillo (con límite mínimo de 5%)
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
  lv_slider_set_range(slider, 5, 100); // Límite mínimo de 5%
  lv_slider_set_value(slider, screen_brightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, COLOR_RED_ACC, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_make(255, 255, 255), LV_PART_KNOB);
  lv_obj_add_event_cb(slider, bright_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // Tarjeta Timeout (Apagado Automático)
  lv_obj_t *c_timeout = lv_obj_create(parent);
  lv_obj_set_size(c_timeout, 215, 66);
  lv_obj_set_style_bg_color(c_timeout, COLOR_CARD, 0);
  lv_obj_set_style_border_width(c_timeout, 0, 0);
  lv_obj_set_style_radius(c_timeout, 6, 0);
  lv_obj_set_style_pad_all(c_timeout, 6, 0);

  lv_obj_t *lbl_timeout = lv_label_create(c_timeout);
  lv_label_set_text(lbl_timeout, "Apagado Pantalla:");
  lv_obj_set_style_text_font(lbl_timeout, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_timeout, lv_color_make(255, 255, 255), 0);
  lv_obj_align(lbl_timeout, LV_ALIGN_TOP_LEFT, 2, 2);

  lv_obj_t *dd_timeout = lv_dropdown_create(c_timeout);
  lv_obj_set_size(dd_timeout, 195, 30);
  lv_obj_align(dd_timeout, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_dropdown_set_options(dd_timeout, "Nunca\n1 Minuto\n5 Minutos");
  
  if (screen_timeout_minutes == 0) {
    lv_dropdown_set_selected(dd_timeout, 0);
  } else if (screen_timeout_minutes == 1) {
    lv_dropdown_set_selected(dd_timeout, 1);
  } else if (screen_timeout_minutes == 5) {
    lv_dropdown_set_selected(dd_timeout, 2);
  }
  lv_obj_add_event_cb(dd_timeout, timeout_dd_event_cb, LV_EVENT_VALUE_CHANGED, NULL);


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
  lv_label_set_text(l_move, "Ajus");
  lv_obj_set_style_text_font(l_move, &lv_font_montserrat_14, 0);
  lv_obj_align(l_move, LV_ALIGN_CENTER, 0, 0);



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

  last_touch_time = millis();

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

  // Control de protector de pantalla (Timeout de inactividad)
  if (screen_timeout_minutes > 0 && !screen_is_off) {
    if (millis() - last_touch_time >= ((unsigned long)screen_timeout_minutes * 60000UL)) {
      screen_is_off = true;
      analogWrite(TFT_BL, 0); // Apagar retroiluminación física de la pantalla
      Serial.println("[System] Pantalla apagada por inactividad prolongada.");
    }
  }
  
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
