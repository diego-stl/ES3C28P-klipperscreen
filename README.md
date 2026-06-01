# Bitácora de Desarrollo: Pantalla Capacitiva ESP32-S3 2.8" (ES3C28P)

Esta bitácora resume de forma estructurada toda la evolución técnica, hitos y configuraciones de hardware que hemos desarrollado a lo largo de este proyecto en PlatformIO.

---

## 🔌 1. Resumen de Hardware y Pinout (Confirmado)

La pantalla está basada en el microcontrolador **ESP32-S3** con un panel LCD IPS **ILI9341V** (SPI) de $320 \times 240$ píxeles y un panel táctil capacitivo **FT6336G** (I2C).

| Periférico | Función | Pin GPIO (ESP32-S3) |
| :--- | :--- | :--- |
| **Pantalla LCD** | `TFT_MISO` | `GPIO 13` |
| | `TFT_MOSI` | `GPIO 11` |
| | `TFT_SCLK` | `GPIO 12` |
| | `TFT_CS` (Chip Select) | `GPIO 10` |
| | `TFT_DC` (Data/Command) | `GPIO 46` |
| | `TFT_RST` (TFT Reset) | Conectado a `CHIP_PU` (Reset general) |
| | `TFT_BL` (Backlight / Retroiluminación) | `GPIO 45` (Control PWM analógico activo en ALTO) |
| **Táctil Capacitivo** | `TP_SDA` (I2C Data) | `GPIO 16` |
| | `TP_SCL` (I2C Clock) | `GPIO 15` |
| | `TP_RST` (Touch Reset) | `GPIO 18` (Activo en BAJO para inicialización) |
| | `TP_INT` (Touch Interrupt) | `GPIO 17` |

---

## 📈 2. Fases de Evolución del Proyecto

### Fase 1: Encendido y Parpadeo Básico (Blink)
* **Objetivo:** Inicializar la pantalla y hacer parpadear la retroiluminación en color blanco cada 5 segundos.
* **Hito de Ingeniería:** Configuramos el proyecto en PlatformIO inyectando todos los pines y directivas del driver `TFT_eSPI` directamente mediante *build flags* en el archivo `platformio.ini`. Esto **eliminó la necesidad** de editar archivos internos de la librería (`User_Setup.h`), logrando un entorno 100% portable y limpio.

### Fase 2: Interactividad Táctil y Corrección IPS
* **Objetivo:** Hacer que la pantalla cambie cíclicamente entre Rojo, Verde y Azul al detectar toques físicos.
* **Hito de Ingeniería:** 
  * Desarrollamos un lector I2C nativo de bajo nivel (`Wire.h`) para comunicarse directamente con la dirección `0x38` del chip **FT6336G**, eliminando la necesidad de librerías externas propensas a fallos de compilación en ESP32-S3.
  * Solucionamos un comportamiento clásico de inversión de colores en el hardware del panel IPS (donde el Rojo se veía Cian y el Azul Amarillo) aplicando la instrucción correctora `tft.invertDisplay(true)`.
  * Diseñamos una máquina de estados con retardo antirebote para que el color cambiara **exactamente una vez por pulso táctil (flanco de subida)**.

### Fase 3: Lector de Coordenadas y Dibujo Inteligente
* **Objetivo:** Mostrar las coordenadas exactas (X, Y) del toque en consola y dibujar un círculo blanco debajo del dedo que desaparece al levantarlo.
* **Hito de Ingeniería:**
  * Implementamos la traslación de ejes en la pantalla horizontal (`320x240`) a partir de la salida nativa vertical (`240x320`) del táctil capacitivo.
  * Diseñamos un **algoritmo de borrado inteligente libre de parpadeo (Zero-Flicker)**: en lugar de refrescar toda la pantalla (lo que causa interferencias visuales), el programa recuerda la posición anterior del dedo, borra *únicamente* la zona del círculo viejo pintándola en negro y redibuja el círculo en la nueva posición.

### Fase 4: Integración Completa de LVGL v8
* **Objetivo:** Dotar al proyecto del motor de interfaz gráfica profesional LVGL.
* **Hito de Ingeniería:**
  * Creamos `include/lv_conf.h` configurado óptimamente para ESP32: profundidad de color de 16 bits, intercambio de bytes activo (`LV_COLOR_16_SWAP = 1`), fuentes Montserrat activadas y el temporizador interno de LVGL mapeado al reloj del sistema mediante `millis()`.
  * Desarrollamos los controladores intermedios (*callbacks*) `my_disp_flush` (vuelco de video rápido vía `pushColors`) y `my_touchpad_read` (lectura táctil capacitiva mapeada a LVGL).

### Fase 5: Recreación de Interfaz CYD-Klipper (Cybertech Red Edition)
* **Objetivo:** Diseñar un panel interactivo inspirado en la interfaz del firmware `suchmememanyskill/CYD-Klipper` para impresoras 3D, bajo una estética oscura futurista y detalles rojo carmesí brillante.
* **Hito de Ingeniería:**
  * **Barra lateral de navegación deslizante:** Dado que se implementaron 5 pestañas de control, convertimos el menú en un panel deslizable vertical con soporte para toques de arrastre.
  * **Pestaña `Estado`:** Muestra progresos de impresión ($68\%$), tiempos estimados y archivos cargados.
  * **Pestaña `Mover`:** Panel D-pad interactivo en cruz que **actualiza numéricamente en tiempo real** la posición X/Y/Z en la pantalla al pulsar las flechas, simulando los límites de movimiento físicos y permitiendo ajustar el tamaño del paso (`1 mm`, `10 mm` y `50 mm`).
  * **Pestaña `Temps`:** Permite subir y bajar la temperatura objetivo de boquilla y cama de 5°C en 5°C, con presets táctiles de calentamiento rápido de PLA y enfriamiento.
  * **Pestaña `Config`:** Integra control del **brillo físico de la pantalla** mediante modulación PWM analógica real sobre el GPIO 45 (`analogWrite`) y un switch que activa/desactiva la inversión de colores del panel en vivo.
  * **Pestaña `Gcodes`:** Biblioteca de archivos deslizable verticalmente que permite cargar archivos G-code de forma dinámica con un cartel emergente de confirmación de carga (*MessageBox*).

### Fase 6: Teclado Virtual y Configuración Wi-Fi/IP en Caliente
* **Objetivo:** Permitir al usuario ingresar y actualizar en tiempo real el SSID, la contraseña de Wi-Fi y la dirección IP/puerto de Klipper directamente desde la pantalla táctil de 2.8", haciendo el dispositivo independiente del código.
* **Hito de Ingeniería:**
  * Diseñamos callbacks dinámicos (`ta_event_cb` y `kb_event_cb`) para crear un teclado virtual flotante en pantalla (`lv_keyboard`) al enfocar los campos de entrada de texto (`lv_textarea`), el cual se auto-destruye y libera la memoria RAM del ESP32-S3 de inmediato al finalizar.
  * Añadimos un parser inteligente de URL para limpiar prefijos (como `http://`) y extraer de forma dinámica el puerto si se especifica en la IP ingresada (ej. `192.168.0.111:7125`).
  * Implementamos lógica de reconexión dinámica de red no bloqueante al pulsar el botón "Conectar y Guardar".

### Fase 7: Migración a HTTP REST API (Conexión Ultra Estable)
* **Objetivo:** Resolver las desconexiones inalámbricas del WebSocket causadas por desbordamiento de búfer en LwIP (error `flush() errno 11 / EWOULDBLOCK`) y discrepancias de mayúsculas/minúsculas en el handshake de Moonraker.
* **Hito de Ingeniería:**
  * Migramos la arquitectura completa a peticiones **HTTP REST (GET/POST)** utilizando la librería estándar optimizada `HTTPClient.h`, eliminando por completo la dependencia inestable de `Links2004/WebSockets`.
  * Diseñamos un sondeo (*polling*) no bloqueante basado en temporización de `millis()` que consulta en una sola llamada GET unificada a `/printer/objects/query?extruder&heater_bed&virtual_sdcard&print_stats` cada 1.5 segundos. Esto extrae instantáneamente las temperaturas reales y objetivo, el archivo cargado, el progreso de impresión y el estado de la máquina.
  * Rediseñamos el despacho de comandos G-code mediante peticiones HTTP POST a `/printer/gcode/script?script=...`, logrando una comunicación robusta, directa y compatible nativamente sin requerir modificaciones en la configuración de Klipper/Moonraker.

### Fase 8: Panel de Estado Optimizado, Seguridad Activa y Telemetría de Tiempos
* **Objetivo:** Refinar la interfaz de usuario en la pantalla de Estado, corregir las lecturas de los cronómetros de impresión y añadir controles de seguridad obligatorios antes de enviar acciones críticas (pausa/parada de emergencia) a la impresora 3D.
* **Hito de Ingeniería:**
  * **Optimización de Fluidez de Interfaz:** Ajustamos el timeout de peticiones HTTP a `600ms` y configuramos un intervalo de sondeo dinámico (1.5s activo / 5.0s cuando Moonraker no está listo), eliminando cualquier tipo de lag en la respuesta táctil o refresco visual si la impresora está desconectada.
  * **Notificaciones de Pausa Integradas (Toasts):** Implementamos un sistema de alertas emergentes interactivas basadas en overlays transparentes a pantalla completa con cierre rápido al tocar cualquier zona táctil. Al pausar la impresión, se despliega una ventana unificada de "IMPRESORA PAUSADA" que incluye controles directos para extruir y retraer filamento de forma rápida y controlada.
  * **Botonera Inferior Externa de Seguridad:** Limpiamos el espacio vertical eliminando la cabecera fija de estado y rediseñamos el panel a `150` píxeles de altura libre de barras de scroll, permitiendo ubicar dos botones táctiles dinámicos exteriores en la parte inferior: **Pausar/Reanudar** y **Parada de Emergencia (ESTOP) / Cancelar Impresión**.
  * **Confirmación por Superposición Modal:** Para evitar toques involuntarios, ambos botones de control e integraciones críticas muestran una ventana de diálogo flotante (modal) difuminada al 50% de opacidad que solicita validación obligatoria (**Aceptar** o **Cancelar**). Al estar la impresora pausada, el botón de parada de emergencia cambia automáticamente a "Cancelar", permitiendo anular la impresión de forma segura mediante el comando `CANCEL_PRINT` de Moonraker/Klipper tras confirmar la acción.
  * **Cronómetros de Impresión Precisos:** Mapeamos el campo `print_duration` directamente desde Moonraker y desarrollamos un motor de estimación dinámico:
    $$\text{Duración Estimada Total} = \frac{\text{print\_duration} \times 100}{\text{Progreso Impresión}}$$
    Mostrando la telemetría en formato `T: HH:MM:SS` (Tiempo Transcurrido) y `E: HH:MM:SS` (Tiempo Estimado Total) en tiempo real sin congelarse en ceros.

### Fase 9: Fusión de Pestañas (Ajus), Coordenadas Reales y D-pad Carmesí
* **Objetivo:** Fusionar movimiento y temperaturas en una sola pestaña scrollable, mejorar los botones direccionales e incorporar lectura física de coordenadas.
* **Hito de Ingeniería:**
  * **Fusión de Pantallas:** Unificamos los controles de Movimiento y Temperatura en una sola pestaña deslizante llamada `"Ajus"`, optimizando el menú de la barra lateral vertical a 4 pestañas limpias.
  * **Coordenadas en Tiempo Real:** Agregamos `gcode_move` al sondeo no bloqueante a Moonraker para extraer la posición física (`pos_x`, `pos_y`, `pos_z`) de la boquilla en tiempo real cada 1.5 segundos. Resolvimos fallas de formato tipográfico del compilador implementando concatenación explícita mediante `String` con precisión decimal controlada.
  * **Botonera Direccional Redesigned:** Diseñamos botones direccionales carmesí (`COLOR_RED_ACC`) a escala `35x35` px basados en el layout de `botones.png`.
  * **Controles Unificados de Homing:** Añadimos una hilera horizontal inferior con botones para `Home TODOS` y `Motores Off` (`M84`), y expandimos la matriz de selección de pasos a 6 valores: `0.1`, `1`, `10`, `25`, `50`, `100` mm.

### Fase 10: Pestaña Configuración Rediseñada, Modales Superiores y Protector de Pantalla por Inactividad
* **Objetivo:** Reestructurar la pestaña de configuración para albergar inputs de red dinámicos en ventanas emergentes seguras, acotar los límites de brillo para evitar apagados por error e implementar un protector de pantalla automático.
* **Hito de Ingeniería:**
  * **Tarjetas de Conexión Pulsables:** Dividimos el bloque fijo de estado original en dos elegantes botones independientes para **Wi-Fi** e **Impresora**.
  * **Modales Superiores Anti-Solapamiento:** Al pulsar cualquiera de las tarjetas de conexión, se despliega una superposición modal con un cuadro de diálogo centrado en la mitad superior de la pantalla (`Y = 5`). De esta forma, al enfocar los inputs táctiles, el teclado virtual de LVGL se despliega en la mitad inferior sin obstruir la vista de los datos que el usuario está escribiendo.
  * **Límite de Brillo Activo:** Restringimos por software y hardware el rango del slider de control de brillo analógico PWM (GPIO 45) entre **5%** y **100%**, impidiendo la desactivación total accidental del display.
  * **Protector de Pantalla (Screen Sleep/Wake):** Implementamos un selector dropdown en configuración con opciones de temporizador de inactividad (`Nunca`, `1 min`, `5 min` por defecto). Si no se detecta actividad táctil capacitiva durante dicho período, el microcontrolador suspende el PWM al **0%** apantallando a negro el display para ahorrar energía y proteger el panel.
  * **Intercepción Táctil Inteligente:** Al tocar el panel apagado, este se reactiva instantáneamente a su brillo anterior e **ignora/consume el primer toque**, previniendo de forma segura clics involuntarios sobre la interfaz subyacente.

### Fase 11: Control del LED de Estado RGB Integrado y Parpadeo Inteligente
* **Objetivo:** Configurar y controlar el LED RGB direccionable incorporado en la parte inferior de la pantalla ES3C28P (GPIO 42), vinculándolo a la telemetría en vivo de Klipper y diseñando una sección de pruebas táctil interactiva.
* **Hito de Ingeniería:**
  * **Integración del Driver WS2812:** Incorporamos la biblioteca `Adafruit NeoPixel` y encapsulamos el control del LED físico en el pin GPIO 42 a través de una función de utilidad centralizada `set_led_color(r, g, b)`.
  * **Sincronización Automática con Klipper:** Programamos un mapeo de colores para reflejar en vivo los estados del sistema:
    * 🔵 **Azul Rey (`0, 50, 255`):** Impresora en espera / lista (máquina fría).
    * 🟠 **Naranja (`255, 100, 0`):** Impresora imprimiendo de forma activa (requiere atención).
    * 🚨 **Rojo Parpadeante:** Impresión pausada por cualquier motivo para alertar de forma prioritaria. Se implementó mediante un oscilador no bloqueante basado en `millis()` cada 500ms en el bucle principal (`loop()`), impidiendo interrupciones o congelamiento de la interfaz LVGL.
    * 🔴 **Rojo Estático (`220, 20, 60`):** Error de conexión a Moonraker o falla del MCU de la impresora, diferenciándolo de una pausa.
  * **Panel de Prueba en Configuración:** Agregamos una tarjeta adicional en la pestaña de configuración con botones táctiles para cada color (Rojo, Verde, Azul y Apagar) con notificaciones Toast interactivas para verificar físicamente el funcionamiento del LED en caliente.




