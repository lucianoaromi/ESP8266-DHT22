#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <time.h>
#include <FS.h>           // SPIFFS para almacenamiento interno

// =============================
// CONFIG WIFI
// =============================
const char* ssid     = "Yolanda Casa";
const char* password = "Dr4Y0l4nd4G0nz4l3z";

// =============================
// CONFIG DHT22
// =============================
#define SENSOR D5
#define DHTTYPE DHT22
DHT dht(SENSOR, DHTTYPE);

// Variables globales
float TEMPERATURA = 0;
float HUMEDAD     = 0;

float tempMax = -100, tempMin = 200;
float humMax  = 0,    humMin  = 100;

unsigned long lastRead = 0;

// =============================
// INTERVALOS DE TIEMPO
// =============================
const unsigned long READ_INTERVAL_MS      = 5000UL;   // 5 segundos
const unsigned long HIST_MINUTE_INTERVAL  = 60000UL;  // 1 minuto
const unsigned long CSV_SAVE_INTERVAL_MS  = 300000UL; // 5 minutos

// =============================
// HISTORIAL 1 HORA (60 puntos)
// =============================
const int HOUR_POINTS = 60;
float histTemp[HOUR_POINTS];
float histHum[HOUR_POINTS];
int   histCount = 0;  // cantidad de muestras válidas
int   histIndex = 0;  // índice circular

unsigned long lastHistMinute = 0;   // para el gráfico (1 minuto)
unsigned long lastCsvSave    = 0;   // para el archivo CSV (5 minutos)

// =============================
// TELEGRAM CONFIG
// =============================
const String TELEGRAM_TOKEN = "8481385433:AAHYb6QwA5Kn_cd7P5IcNKx70Irge8xRHG0";
const String CHAT_ID        = "5144677839";

const float ALERTA_TEMP = 35.0;              // UMBRAL DE ALERTA
unsigned long lastAlert = 0;                 // tiempo del último aviso
const unsigned long ALERT_INTERVAL = 300000; // 5 MINUTOS

WiFiServer server(80);

// =============================
// LOG EN SPIFFS (HASTA ~1 MES)
// =============================
const char* LOG_FILE = "/historial.csv";
const unsigned long MAX_LOG_LINES = 8640UL; // 24 × 60 / 5 * 30 (1 por 5 minutos, 30 días aprox)
bool spiffsOk = false;
unsigned long logLines = 0;

// =============================
// HORA NTP (ARGENTINA GMT-3)
// =============================
String obtenerFechaHora() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  if (!timeinfo) return "Sin hora";

  char buffer[32];
  sprintf(buffer, "%02d/%02d/%04d %02d:%02d:%02d",
          timeinfo->tm_mday,
          timeinfo->tm_mon + 1,
          timeinfo->tm_year + 1900,
          timeinfo->tm_hour,
          timeinfo->tm_min,
          timeinfo->tm_sec);
  return String(buffer);
}

// =============================
// ENCODER URL PARA TELEGRAM
// =============================
String urlEncode(const String &text) {
  String encoded = "";
  char c;
  char buf[4];

  for (int i = 0; i < text.length(); i++) {
    c = text.charAt(i);
    if ( ('a' <= c && c <= 'z') ||
         ('A' <= c && c <= 'Z') ||
         ('0' <= c && c <= '9') ) {
      encoded += c;
    } else {
      sprintf(buf, "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

// =============================
// ENVIAR MENSAJE TELEGRAM
// =============================
void enviarTelegram(const String &mensaje) {
  WiFiClientSecure client;

  client.setInsecure();
  client.setTimeout(5000);
  client.setBufferSizes(512, 512);  // Ajuste importante en ESP8266

  Serial.println("📡 Conectando a Telegram...");

  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("❌ Error conectando a Telegram");
    return;
  }

  delay(150); // Telegram lo necesita

  String url = "/bot" + TELEGRAM_TOKEN +
               "/sendMessage?chat_id=" + CHAT_ID +
               "&text=" + urlEncode(mensaje);

  client.print(
    String("GET ") + url + " HTTP/1.1\r\n" +
    "Host: api.telegram.org\r\n" +
    "User-Agent: ESP8266\r\n" +
    "Connection: close\r\n\r\n"
  );

  Serial.println("📨 Enviando mensaje a Telegram...");

  // Leer solo la primera línea de respuesta
  String line = client.readStringUntil('\n');
  if (line.startsWith("HTTP/1.1 200")) {
    Serial.println("✅ Telegram enviado correctamente");
  } else {
    Serial.println("⚠ Respuesta inesperada: " + line);
  }

  client.stop();
}

// =============================
// LECTURA DEL DHT22
// =============================
bool leerDHT() {
  float sumaT = 0, sumaH = 0;
  int muestras = 5, validas = 0;

  for (int i = 0; i < muestras; i++) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      sumaT += t;
      sumaH += h;
      validas++;
    }
    delay(10);
  }

  if (validas == 0) return false;

  TEMPERATURA = sumaT / validas;
  HUMEDAD     = sumaH / validas;

  // Corrección de sensores
  HUMEDAD     = HUMEDAD - 9.0;
  TEMPERATURA = TEMPERATURA - 0.8;

  // Limitar valores
  if (HUMEDAD < 0)   HUMEDAD = 0;
  if (HUMEDAD > 100) HUMEDAD = 100;

  if (TEMPERATURA > tempMax) tempMax = TEMPERATURA;
  if (TEMPERATURA < tempMin) tempMin = TEMPERATURA;

  if (HUMEDAD > humMax) humMax = HUMEDAD;
  if (HUMEDAD < humMin) humMin = HUMEDAD;

  return true;
}

// =============================
// INICIALIZAR LOG EN SPIFFS
// =============================
void iniciarLog() {
  if (!SPIFFS.exists(LOG_FILE)) {
    File f = SPIFFS.open(LOG_FILE, "w");
    if (f) {
      f.println("FechaHora,Temperatura,Hum.edad");
      f.close();
    }
    logLines = 0;
    Serial.println("Log creado nuevo.");
    return;
  }

  File f = SPIFFS.open(LOG_FILE, "r");
  if (!f) {
    logLines = 0;
    Serial.println("No se pudo abrir log existente.");
    return;
  }

  bool first = true;
  logLines = 0;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (first) {          // saltar cabecera
      first = false;
      continue;
    }
    if (line.length() > 1) logLines++;
  }

  f.close();
  Serial.printf("Log existente con %lu lineas.\n", logLines);
}

// =============================
// AGREGAR REGISTRO AL LOG
// =============================
void agregarRegistroLog(const String &fechaHora, float t, float h) {
  if (!spiffsOk) return;

  File f = SPIFFS.open(LOG_FILE, "a");
  if (!f) {
    Serial.println("No se pudo abrir log para escribir.");
    return;
  }

  f.printf("%s,%.1f,%.1f\r\n", fechaHora.c_str(), t, h);
  f.close();

  logLines++;

  if (logLines >= MAX_LOG_LINES) {
    Serial.println("Limite de 1 mes alcanzado, reiniciando log...");
    SPIFFS.remove(LOG_FILE);
    File nf = SPIFFS.open(LOG_FILE, "w");
    if (nf) {
      nf.println("FechaHora,Temperatura,Hum.edad");
      nf.close();
    }
    logLines = 0;
  }
}

// =============================
// GUARDAR MUESTRA EN HISTORIAL
// =============================
void actualizarHistorial() {
  unsigned long ahora = millis();

  // Historial en RAM (gráfico 1 hora) → cada 1 minuto
  if (ahora - lastHistMinute >= HIST_MINUTE_INTERVAL) {
    lastHistMinute = ahora;

    histTemp[histIndex] = TEMPERATURA;
    histHum[histIndex]  = HUMEDAD;

    histIndex = (histIndex + 1) % HOUR_POINTS;
    if (histCount < HOUR_POINTS) histCount++;
  }

  // Guardar CSV → cada 5 minutos
  if (ahora - lastCsvSave >= CSV_SAVE_INTERVAL_MS) {
    lastCsvSave = ahora;
    String fecha = obtenerFechaHora();
    agregarRegistroLog(fecha, TEMPERATURA, HUMEDAD);
  }
}

// ==========================================================
//                    CÁLCULO DE FASE LUNAR
// ==========================================================
double edadLunar() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);

  int d = t->tm_mday;
  int m = t->tm_mon + 1;
  int y = t->tm_year + 1900;

  if (m < 3) {
    y--;
    m += 12;
  }

  long a = y / 100;
  long b = a / 4;
  long c = 2 - a + b;
  long e = (long)(365.25 * (y + 4716));
  long f = (long)(30.6001 * (m + 1));

  long jd = c + d + e + f - 1524.5;
  double daysSinceNew = jd - 2451549.5;

  double age = fmod(daysSinceNew, 29.53058867);
  if (age < 0) age += 29.53058867;

  return age;
}

String fechaFutura(double dias) {
  time_t now = time(nullptr);
  now += (long)(dias * 86400);

  struct tm* t = localtime(&now);

  char buf[20];
  sprintf(buf, "%02d/%02d/%04d",
          t->tm_mday,
          t->tm_mon + 1,
          t->tm_year + 1900);

  return String(buf);
}

void calcularLunas(String &proximaNueva, String &proximaLlena) {
  double edad  = edadLunar();
  double ciclo = 29.53058867;

  double dn = ciclo - edad;      // Días hasta próxima luna nueva
  double dl = 14.765 - edad;     // Días hasta luna llena

  if (dl < 0) dl += ciclo;

  proximaNueva = fechaFutura(dn);
  proximaLlena = fechaFutura(dl);
}

int faseLunar() {
  double age  = edadLunar();
  double frac = age / 29.53058867;

  if (frac < 0.0625) return 0;
  if (frac < 0.1875) return 1;
  if (frac < 0.3125) return 2;
  if (frac < 0.4375) return 3;
  if (frac < 0.5625) return 4;
  if (frac < 0.6875) return 5;
  if (frac < 0.8125) return 6;
  if (frac < 0.9375) return 7;
  return 0;
}

String faseNombre(int f) {
  switch(f) {
    case 0: return "Luna nueva";
    case 1: return "Creciente cóncava";
    case 2: return "Cuarto creciente";
    case 3: return "Gibosa creciente";
    case 4: return "Luna llena";
    case 5: return "Gibosa menguante";
    case 6: return "Cuarto menguante";
    case 7: return "Menguante cóncava";
  }
  return "Desconocida";
}

String faseIcono(int f) {
  switch(f) {
    case 0: return "🌑";
    case 1: return "🌘";
    case 2: return "🌗";
    case 3: return "🌖";
    case 4: return "🌕";
    case 5: return "🌔";
    case 6: return "🌓";
    case 7: return "🌒";
  }
  return "🌑";
}

// =============================
// SETUP
// =============================
void setup() {
  Serial.begin(115200);
  dht.begin();

  // Conexión WiFi
  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" conectado!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // NTP GMT-3 (Argentina)
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  // Inicializar historial en RAM
  for (int i = 0; i < HOUR_POINTS; i++) {
    histTemp[i] = NAN;
    histHum[i]  = NAN;
  }

  // Montar SPIFFS
  if (SPIFFS.begin()) {
    spiffsOk = true;
    Serial.println("SPIFFS montado correctamente.");
    iniciarLog();
  } else {
    spiffsOk = false;
    Serial.println("❌ Error montando SPIFFS.");
  }

  server.begin();

  // Enviar primer mensaje a Telegram cuando todo ya está listo
  enviarTelegram("🤖 Sistema iniciado correctamente.\nIP: " + WiFi.localIP().toString());
}

// ==========================================================
//      OBTENER FECHA INICIAL Y FINAL DEL CSV EXISTENTE
// ==========================================================
String obtenerNombreCSV() {
  if (!SPIFFS.exists(LOG_FILE)) return "historial_vacio.csv";

  File f = SPIFFS.open(LOG_FILE, "r");
  if (!f) return "historial_error.csv";

  String linea;
  String primeraFecha = "";
  String ultimaFecha  = "";

  bool esCabecera = true;

  while (f.available()) {
    linea = f.readStringUntil('\n');

    // saltar cabecera
    if (esCabecera) {
      esCabecera = false;
      continue;
    }

    // línea con contenido real
    if (linea.length() > 5) {

      // tomar primera fecha válida
      if (primeraFecha == "") {
        primeraFecha = linea.substring(0, 10);  // dd/mm/yyyy
      }

      // actualizar última fecha cada vez
      ultimaFecha = linea.substring(0, 10);
    }
  }

  f.close();

  if (primeraFecha == "" || ultimaFecha == "")
    return "historial_sin_datos.csv";

  // Convertir dd/mm/yyyy → yyyymmdd
  auto normalizar = [](String f) {
    String d = f.substring(0,2);
    String m = f.substring(3,5);
    String y = f.substring(6,10);
    return y + m + d;
  };

  String f1 = normalizar(primeraFecha);
  String f2 = normalizar(ultimaFecha);

  return "historial_" + f1 + "_" + f2 + ".csv";
}


// ==========================================================
//  OBTENER FECHA PRIMERA Y ÚLTIMA LÍNEA DEL CSV
// ==========================================================
void obtenerFechasCSV(String &primera, String &ultima) {
  primera = "";
  ultima  = "";

  if (!SPIFFS.exists(LOG_FILE)) return;

  File f = SPIFFS.open(LOG_FILE, "r");
  if (!f) return;

  String linea;
  bool esCabecera = true;

  while (f.available()) {
    linea = f.readStringUntil('\n');

    if (esCabecera) {
      esCabecera = false;
      continue;
    }

    if (linea.length() > 5) {
      if (primera == "") primera = linea.substring(0,10);
      ultima = linea.substring(0,10);
    }
  }
  f.close();
}

// =============================
// LOOP PRINCIPAL
// =============================
void loop() {
  unsigned long ahora = millis();

  // Leer cada 5 segundos
  if (ahora - lastRead > READ_INTERVAL_MS) {
    lastRead = ahora;

    if (leerDHT()) {
      Serial.printf("Temp: %.1f C | Hum: %.1f %%\n", TEMPERATURA, HUMEDAD);

      // Actualizar historial de 1 hora + log en SPIFFS
      actualizarHistorial();

      // Alerta por temperatura
      if (TEMPERATURA >= ALERTA_TEMP &&
          (lastAlert == 0 || (ahora - lastAlert > ALERT_INTERVAL))) {

        lastAlert = ahora;

        String alerta = "ALERTA: Temperatura alta!\n";
        alerta += "🌡 Temp: " + String(TEMPERATURA) + " °C\n";
        alerta += "💧 Hum: " + String(HUMEDAD) + " %\n";
        alerta += "🕒 " + obtenerFechaHora();

        enviarTelegram(alerta);
      }
    }
  }

  // =============================
  // SERVIDOR WEB
  // =============================
  WiFiClient client = server.available();
  if (!client) return;

  unsigned long timeout = millis();
  while (!client.available()) {
    if (millis() - timeout > 500) {
      client.stop();
      return;
    }
  }

  String requestLine = client.readStringUntil('\r');
  client.read(); // consumir '\n'

  Serial.print(">>> REQUEST LINE: [");
  Serial.print(requestLine);
  Serial.println("]");



  while (client.available()) {
    String header = client.readStringUntil('\r');
    client.read();
    if (header.length() == 1) break;
  }

  // ===== Endpoint JSON /data =====
  if (requestLine.indexOf("GET /data") >= 0) {
    String fechaHora = obtenerFechaHora();

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Connection: close");
    client.println();

    client.print("{\"temp\":");
    client.print(TEMPERATURA, 1);
    client.print(",\"hum\":");
    client.print(HUMEDAD, 1);
    client.print(",\"tmin\":");
    client.print(tempMin, 1);
    client.print(",\"tmax\":");
    client.print(tempMax, 1);
    client.print(",\"hmin\":");
    client.print(humMin, 1);
    client.print(",\"hmax\":");
    client.print(humMax, 1);
    client.print(",\"time\":\"");
    client.print(fechaHora);
    client.print("\",\"hcount\":");
    client.print(histCount);

    // === FASE LUNAR ===
    int f = faseLunar();
    client.print(",\"moonIcon\":\"");
    client.print(faseIcono(f));
    client.print("\",\"moonName\":\"");
    client.print(faseNombre(f));

    String lunaNueva, lunaLlena;
    calcularLunas(lunaNueva, lunaLlena);

    client.print("\",\"nextNewMoon\":\"");
    client.print(lunaNueva);
    client.print("\",\"nextFullMoon\":\"");
    client.print(lunaLlena);
    client.print("\"");

    // Historial 1h
    client.print(",\"histT\":[");
    for (int i = 0; i < histCount; i++) {
      int idx = (histIndex - histCount + i + HOUR_POINTS) % HOUR_POINTS;
      if (i > 0) client.print(",");
      client.print(histTemp[idx], 1);
    }

    client.print("],\"histH\":[");
    for (int i = 0; i < histCount; i++) {
      int idx = (histIndex - histCount + i + HOUR_POINTS) % HOUR_POINTS;
      if (i > 0) client.print(",");
      client.print(histHum[idx], 1);
    }
    client.println("]}");

    client.stop();
    return;
  }

  // ===== Endpoint CSV /csv =====
    if (requestLine.startsWith("GET /csv ")) {
        if (!spiffsOk || !SPIFFS.exists(LOG_FILE)) {
          client.println("HTTP/1.1 500 Internal Server Error");
          client.println("Content-Type: text/plain");
          client.println("Connection: close");
          client.println();
          client.println("Log no disponible");
          client.stop();
          return;
    }

    File f = SPIFFS.open(LOG_FILE, "r");
    if (!f) {
      client.println("HTTP/1.1 500 Internal Server Error");
      client.println("Content-Type: text/plain");
      client.println("Connection: close");
      client.println();
      client.println("No se pudo abrir el log");
      client.stop();
      return;
    }

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/csv");
    client.println("Content-Disposition: attachment; filename=\"" + obtenerNombreCSV() + "\"");
    client.println("Connection: close");
    client.println();

    uint8_t buf[512];
    while (f.available()) {
      size_t len = f.read(buf, sizeof(buf));
      client.write(buf, len);
    }

    f.close();
    client.stop();
    return;
  }

  // ===== Endpoint CLEAR CSV /clearcsv =====
  if (requestLine.startsWith("GET /clearcsv ")) {

      if (spiffsOk && SPIFFS.exists(LOG_FILE)) {
          SPIFFS.remove(LOG_FILE);
      }

      // Crear archivo vacío nuevamente
      File nf = SPIFFS.open(LOG_FILE, "w");
      if (nf) {
          nf.println("FechaHora,Temperatura,Hum.edad");
          nf.close();
      }

      logLines = 0;

      Serial.println("🗑️ Historial CSV borrado manualmente.");

      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: text/plain");
      client.println("Connection: close");
      client.println();
      client.println("CSV borrado correctamente");
      client.stop();
      return;
  }

  // ===== Endpoint CSV info (primer y último registro) =====
  if (requestLine.startsWith("GET /csvinfo ")) {
      String f1, f2;
      obtenerFechasCSV(f1, f2);

      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Connection: close");
      client.println();

      client.print("{\"first\":\"" + f1 + "\",");
      client.print("\"last\":\"" + f2 + "\"}");

      client.stop();
      return;
  }

  // ===== Página principal (HTML) =====
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();

  client.println(R"rawliteral(
<!-- ============================================================================================= -->



<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Monitor Ambiental ESP8266/DHT22</title>

  <link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Material+Symbols+Rounded" />
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>

    <style>
        body{
            background:#0d0f1a;
            color:#e0e0e0;
            font-family:Segoe UI, sans-serif;
            text-align:center;
            padding:25px;
        }
        h1{margin-bottom:5px;}
        h2{margin-top:0;color:#aaa;}

        .panel{
            margin:20px auto;
            display:inline-block;
            text-align:left;
            padding:25px 35px;
            border-radius:16px;
            background:#1b1d2b;
            box-shadow:0 0 18px rgba(0,0,0,0.7);
            min-width:300px;
        }

        .section-title{
            font-size:20px;
            margin-bottom:10px;
            display:flex;
            align-items:center;
            gap:8px;
            font-weight:bold;
        }

        .icon{
            font-family: 'Material Symbols Rounded';
            font-size:28px;
            vertical-align:middle;
        }

        #temp{ color:#FFD700; }
        #hum{  color:#00B4FF; }

        .icon-temp { color:#FFC107; }
        .icon-hum  { color:#03A9F4; }
        .icon-time { color:#4CAF50; }

        .label{margin:8px 0;font-size:17px;}
        .value{font-size:19px;font-weight:bold;}

        .charts{
            max-width:1000px;
            margin:20px auto;
            display:flex;
            flex-wrap:wrap;
            justify-content:space-around;
            gap:20px;
        }

        canvas{
            background:#141622;
            border-radius:10px;
            padding:12px;
        }

        .footer{
            margin-top:25px;
            font-size:16px;
            font-family:Georgia, serif;
            color:#DAA5;
            font-style:italic;
            text-shadow:0 0 3px rgba(255, 215, 0, 0.25);
        }

        .chart-block{
            flex:1;
            min-width:280px;
        }

        /* ======== Estilo iPhone Lunar Card ======== */
        .lunar-ios {
                background: rgba(255,255,255,0.04);
                padding: 28px;
                border-radius: 22px;
                box-shadow:
                        inset 0 0 12px rgba(255,255,255,0.05),
                        0 8px 22px rgba(0,0,0,0.35);
                -webkit-backdrop-filter: blur(10px);
                backdrop-filter: blur(10px);
                transition: 0.25s;
                min-width: 320px;
        }

        .lunar-ios:hover {
                transform: scale(1.02);
                box-shadow:
                        inset 0 0 16px rgba(255,255,255,0.07),
                        0 12px 30px rgba(0,0,0,0.45);
        }

        .lunar-header {
                display: flex;
                align-items: center;
                gap: 10px;
                margin-bottom: 15px;
        }

        .lunar-icon-title {
                font-family: 'Material Symbols Rounded';
                font-size: 28px;
                color: #C8C9CC;
        }

        .lunar-title {
                font-size: 22px;
                font-weight: 600;
                color: #EEE;
        }

        .lunar-moon-icon {
                display: flex;
                justify-content: center;
                margin-bottom: 10px;
        }

        .moon-emoji {
                font-size: 60px;
                filter: drop-shadow(0 4px 6px rgba(0,0,0,0.5));
        }

        .lunar-phase-name {
                text-align: center;
                font-size: 20px;
                font-weight: 600;
                color: #DCDCDC;
                margin-bottom: 15px;
        }

        .divider {
                width: 100%;
                height: 1px;
                background: rgba(255,255,255,0.10);
                margin: 12px 0 18px 0;
        }

        .lunar-row {
                display: flex;
                justify-content: space-between;
                padding: 4px 0;
        }

        .label-ios {
                font-size: 16px;
                color: #B7B7B7;
        }

        .value-ios {
                font-size: 17px;
                font-weight: 600;
                color: #EEE;
        }

        /* Botones responsive */
        #btnCSV, #btnClear {
                width: clamp(55px, 8vw, 85px);
                height: clamp(55px, 8vw, 85px);
                border-radius: 50%;
        }

        #btnCSV span.material-symbols-rounded,
        #btnClear span.material-symbols-rounded {
                font-size: clamp(22px, 4vw, 34px);
        }

        #btnCSV .btn-label,
        #btnClear .btn-label {
                font-size: clamp(8px, 2vw, 12px);
        }

        .heading-main{
            color:#A05C1F;
            font-size:22px;
        }

        .panel-main{
            position:relative;
            display:flex;
            gap:20px;
            align-items:flex-start;
            padding:20px 25px;
            width:100%;
            max-width:650px;
            margin:20px auto;
        }

        .climate-section{
            flex:1;
            min-width:260px;
            padding-right:10px;
        }

        .lunar-section{
            flex:1;
            min-width:240px;
            padding-left:10px;
        }

        .panel-csv{
            max-width:450px;
            margin:35px auto;
            background:#11131d;
            text-align:center;
        }

        .csv-title{
            text-align:center;
            margin-bottom:15px;
        }

        .csv-row{
            margin-bottom:8px;
        }

        .csv-row-large{
            margin-bottom:15px;
        }

        .csv-actions{
            display:flex;
            justify-content:center;
            gap:25px;
            margin-top:20px;
        }

        .circular-btn{
            width:70px;
            height:70px;
            border-radius:50%;
            cursor:pointer;
            display:flex;
            flex-direction:column;
            align-items:center;
            justify-content:center;
            gap:4px;
            transition:0.25s ease;
            -webkit-backdrop-filter:blur(6px);
            backdrop-filter:blur(6px);
            border:1px solid transparent;
        }

        .circular-btn:hover{
            transform:scale(1.1);
        }

        .btn-clear{
            background:radial-gradient(circle, rgba(60,0,0,0.15), rgba(25,0,0,0.42));
            border-color:rgba(120,0,0,0.55);
            box-shadow:0 4px 14px rgba(0,0,0,0.50), inset 0 0 10px rgba(180,0,0,0.25);
        }

        .btn-csv{
            background:radial-gradient(circle, rgba(0,60,0,0.15), rgba(0,25,0,0.42));
            border-color:rgba(0,100,0,0.55);
            box-shadow:0 4px 14px rgba(0,0,0,0.50), inset 0 0 10px rgba(0,160,0,0.25);
        }

        .btn-icon{
            font-size:33px;
        }

        .btn-label{
            font-size:9px;
        }

        #btnClear .btn-icon{
            color:#7A2626;
        }

        #btnClear .btn-label{
            color:#D45A5A;
        }

        #btnCSV .btn-icon{
            color:#0A3F1E;
        }

        #btnCSV .btn-label{
            color:#47B676;
        }

    </style>
</head>

<body>

<h1 class="heading-main">
    Monitor Ambiental ESP8266/DHT22
</h1>


  <!-- Panel principal: Clima + Fase Lunar en una sola tarjeta horizontal -->
    <div class="panel panel-main">

    <!-- ==================== SECCIÓN CLIMA ==================== -->
    <div class="climate-section">

        <div class="section-title">
            <span class="icon icon-temp">wb_sunny</span>
            Temperatura
        </div>
        <div class="label">Actual: <span id="temp" class="value">--.-</span> °C</div>

        <hr>

        <div class="section-title">
            <span class="icon icon-hum">water_drop</span>
            Humedad
        </div>
        <div class="label">Actual: <span id="hum" class="value">--.-</span> %</div>

        <hr>

        <div class="label">
            Temp Min/Max:
            <span id="tmin" class="value">--.-</span> /
            <span id="tmax" class="value">--.-</span> °C
        </div>

        <div class="label">
            Hum Min/Max:
            <span id="hmin" class="value">--.-</span> /
            <span id="hmax" class="value">--.-</span> %
        </div>

        <hr>

        <div class="section-title">
            <span class="icon icon-time">schedule</span>
            Última actualización
        </div>

        <div class="label">
            <span id="time" class="value">--/--/---- --:--:--</span>
        </div>

    </div>

    <!-- ==================== SECCIÓN LUNAR ==================== -->
    <div class="lunar-section">

        <div class="lunar-header">
            <span class="lunar-icon-title">nightlight</span>
            <span class="lunar-title">Fase Lunar</span>
        </div>

        <div class="lunar-moon-icon">
            <span id="moonIcon" class="moon-emoji">🌑</span>
        </div>

        <div class="lunar-phase-name">
            <span id="moonName">--</span>
        </div>

        <div class="divider"></div>

        <div class="lunar-row">
            <span class="label-ios">Próxima Luna Nueva</span>
            <span id="nextNewMoon" class="value-ios">--/--/----</span>
        </div>

        <div class="lunar-row">
            <span class="label-ios">Próxima Luna Llena</span>
            <span id="nextFullMoon" class="value-ios">--/--/----</span>
        </div>

    </div>

  </div>

  <!-- ===========================
       GRÁFICOS
       =========================== -->
  <div class="charts">
    <div class="chart-block">
      <h3>Temperatura (últimos segundos)</h3>
      <canvas id="tempChart"></canvas>
    </div>
    <div class="chart-block">
      <h3>Humedad (últimos segundos)</h3>
      <canvas id="humChart"></canvas>
    </div>
  </div>

  <div class="charts">
    <div class="chart-block">
      <h3>Temperatura (última hora)</h3>
      <canvas id="tempHourChart"></canvas>
    </div>
    <div class="chart-block">
      <h3>Humedad (última hora)</h3>
      <canvas id="humHourChart"></canvas>
    </div>
  </div>

  <div class="footer">by: Luciano Aromi</div>

  <script>
  let tempChart, humChart;
  let tempHourChart, humHourChart;

  const HOUR_POINTS = 60;
  let bufferTemp = [];
  let bufferHum  = [];

  function smoothEWMA(values, alpha = 0.2) {
      if (values.length < 2) return values[values.length - 1];

      let prev = values[0];
      let smoothed = prev;

      for (let i = 1; i < values.length; i++) {
          smoothed = alpha * values[i] + (1 - alpha) * prev;
          prev = smoothed;
      }

      return smoothed;
  }

function crearGraficos() {

    const tctx = document.getElementById('tempChart').getContext('2d');
    const hctx = document.getElementById('humChart').getContext('2d');

    tempChart = new Chart(tctx, {
        type: 'line',
        data: { labels: [], datasets: [{
            label: 'Temperatura (°C)',
            data: [],
            borderColor: 'rgba(255,99,132,1)',
            backgroundColor: 'rgba(255,99,132,0.25)',
            tension: 0.4,
            pointRadius: 0
        }]},
        options: {
            animation: false,
            scales: {
                x: {
                    display: false,
                    grid: {
                        color: "rgba(255,255,255,0.12)",
                        lineWidth: 1
                    }
                },
                y: {
                    beginAtZero: false,
                    grid: {
                        color: "rgba(255,255,255,0.12)",
                        lineWidth: 1
                    },
                    ticks: {
                        callback: function(v){ return v.toFixed(1); }
                    }
                }
            }
        }
    });

    humChart = new Chart(hctx, {
        type: 'line',
        data: { labels: [], datasets: [{
            label: 'Humedad (%)',
            data: [],
            borderColor: 'rgba(54,162,235,1)',
            backgroundColor: 'rgba(54,162,235,0.25)',
            tension: 0.4,
            pointRadius: 0
        }]},
        options: {
            animation: false,
            scales: {
                x: {
                    display: false,
                    grid: {
                        color: "rgba(255,255,255,0.12)",
                        lineWidth: 1
                    }
                },
                y: {
                    beginAtZero: false,
                    suggestedMax: 100,
                    grid: {
                        color: "rgba(255,255,255,0.12)",
                        lineWidth: 1
                    },
                    ticks: {
                        callback:function(v){ return v.toFixed(1); }
                    }
                }
           }
        }
    });

    const thctx = document.getElementById('tempHourChart').getContext('2d');
    const hhctx = document.getElementById('humHourChart').getContext('2d');

    tempHourChart = new Chart(thctx, {
        type: 'scatter',
        data: { datasets: [{
            label: 'Temp 1h (°C)',
            data: [],
            borderColor:'rgba(255,99,132,1)',
            backgroundColor:'rgba(255,99,132,1)',
            showLine:false,
            pointRadius:3
        }]},
        options:{
            animation:false,
            scales:{
                x:{
                    type:'linear',
                    min:0,
                    max:HOUR_POINTS-1,
                    grid:{ color:"rgba(255,255,255,0.12)", lineWidth:1 }
                },
                y:{
                    beginAtZero:false,
                    grid:{ color:"rgba(255,255,255,0.12)", lineWidth:1 },
                    ticks:{ callback:function(v){ return v.toFixed(1); } }
                }
            }
        }
    });

    humHourChart = new Chart(hhctx, {
        type:'scatter',
        data:{ datasets:[{
            label:'Hum 1h (%)',
            data:[],
            borderColor:'rgba(54,162,235,1)',
            backgroundColor:'rgba(54,162,235,1)',
            showLine:false,
            pointRadius:3
        }]},
        options:{
            animation:false,
            scales:{
                x:{
                    type:'linear',
                    min:0,
                    max:HOUR_POINTS-1,
                    grid:{ color:"rgba(255,255,255,0.12)", lineWidth:1 }
                },
                y:{
                    beginAtZero:false,
                    suggestedMax:100,
                    grid:{ color:"rgba(255,255,255,0.12)", lineWidth:1 },
                    ticks:{ callback:function(v){ return v.toFixed(0); } }
                }
            }
        }
    });
}

function agregarPunto(chart,label,value,maxPts){
    chart.data.labels.push(label);
    chart.data.datasets[0].data.push(value);

    if(chart.data.labels.length > maxPts){
        chart.data.labels.shift();
        chart.data.datasets[0].data.shift();
    }

    chart.update();
}

function actualizarDatos(){

    fetch('/data')
        .then(r=>r.json())
        .then(d=>{

            document.getElementById('temp').textContent = d.temp.toFixed(1);
            document.getElementById('hum').textContent  = d.hum.toFixed(1);
            document.getElementById('tmin').textContent = d.tmin.toFixed(1);
            document.getElementById('tmax').textContent = d.tmax.toFixed(1);
            document.getElementById('hmin').textContent = d.hmin.toFixed(1);
            document.getElementById('hmax').textContent = d.hmax.toFixed(1);
            document.getElementById('time').textContent = d.time;

            document.getElementById("moonIcon").textContent      = d.moonIcon;
            document.getElementById("moonName").textContent      = d.moonName;
            document.getElementById("nextNewMoon").textContent   = d.nextNewMoon;
            document.getElementById("nextFullMoon").textContent  = d.nextFullMoon;

            bufferTemp.push(d.temp);
            bufferHum.push(d.hum);

            if(bufferTemp.length > 50) bufferTemp.shift();
            if(bufferHum.length > 50)  bufferHum.shift();

            const tempSmoothed = smoothEWMA(bufferTemp, 0.2);
            const humSmoothed  = smoothEWMA(bufferHum, 0.2);

            agregarPunto(tempChart, d.time, tempSmoothed, 40);
            agregarPunto(humChart , d.time, humSmoothed, 40);

            tempChart.options.scales.y.min = tempSmoothed - 0.5;
            tempChart.options.scales.y.max = tempSmoothed + 0.5;

            humChart.options.scales.y.min = humSmoothed - 2;
            humChart.options.scales.y.max = humSmoothed + 2;

            tempChart.update();
            humChart.update();

            const hc = d.hcount;

            tempHourChart.data.datasets[0].data = [];
            humHourChart.data.datasets[0].data = [];

            for (let i = 0; i < hc; i++) {
                tempHourChart.data.datasets[0].data.push({ x: i, y: d.histT[i] });
                humHourChart.data.datasets[0].data.push({ x: i, y: d.histH[i] });
            }

            if (hc > 2) {
                const minT = Math.min(...d.histT);
                const maxT = Math.max(...d.histT);
                tempHourChart.options.scales.y.min = minT - 4;
                tempHourChart.options.scales.y.max = maxT + 4;

                const minH = Math.min(...d.histH);
                const maxH = Math.max(...d.histH);
                humHourChart.options.scales.y.min = minH - 20;
                humHourChart.options.scales.y.max = maxH + 20;
            }

            tempHourChart.update();
            humHourChart.update();
        })
        .catch(err => console.error('Error al obtener /data', err));
}


window.addEventListener('load', () => {
    crearGraficos();
    actualizarDatos();
    setInterval(actualizarDatos, 2000);
    cargarInfoCSV();
    setInterval(cargarInfoCSV, 5000);
});


function cargarInfoCSV() {
    fetch("/csvinfo")
        .then(r => r.json())
        .then(d => {
            document.getElementById("csvFirst").textContent = d.first || "--/--/----";
            document.getElementById("csvLast").textContent  = d.last  || "--/--/----";
        })
        .catch(err => console.error('Error al obtener /csvinfo', err));
}


</script>


<!-- ===== TARJETA DE INFORME DEL CSV ===== -->
<div class="panel panel-csv">

    <h2 class="csv-title">Historial CSV</h2>

    <div class="csv-row">
        <strong>Primer registro:</strong>
        <span id="csvFirst">--/--/----</span>
    </div>

    <div class="csv-row csv-row-large">
        <strong>Último registro:</strong>
        <span id="csvLast">--/--/----</span>
    </div>

    <div class="csv-actions">

        <!-- ========== BOTÓN CLEAR (ROJO) ========== -->
        <button id="btnClear" class="circular-btn btn-clear">
            <span class="material-symbols-rounded btn-icon">
                delete
            </span>
            <span class="btn-label">CLEAR</span>
        </button>

        <!-- ========== BOTÓN CSV (VERDE) ========== -->
        <button id="btnCSV" class="circular-btn btn-csv">
            <span class="material-symbols-rounded btn-icon">
                table
            </span>
            <span class="btn-label">.CSV</span>
        </button>

    </div>

</div>


<script>
document.getElementById("btnCSV").addEventListener("click", function(){
    window.location.href = "/csv";
});

document.getElementById("btnClear").addEventListener("click", function(){
    if (confirm("¿Seguro que deseas borrar TODO el historial del CSV?")) {
        fetch("/clearcsv")
            .then(()=> alert("Historial CSV borrado correctamente."))
            .catch(()=> alert("Error al borrar el CSV."));
    }
});
</script>

</body>
</html>



<!-- ============================================================================================= -->

)rawliteral");

  client.stop();
}
