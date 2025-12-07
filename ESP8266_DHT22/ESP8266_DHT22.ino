// =============================
//  LINEAS IMPORTANTES:
// =============================
// const float UMBRAL_LED = 38.0;  // umbral temp API
// const unsigned long WEATHER_INTERVAL = 30000UL; // (30 segundos) - WEATHER_INTERVAL = 10UL * 60UL * 1000UL // (10 min)

// const unsigned long RAIN_BLINK_INTERVAL = 300UL; // más rápido o 1000UL para más lento

// if (precip > 0.5f) // ------------------------------------- (UMBRAL DE PRECIPITACION) 1/2mm

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <DHT.h>
#include <time.h>
#include <FS.h>
#include <math.h>

// =============================
// Brillo del LED de lluvia (0 = apagado, 1023 = máximo)
// =============================
const uint16_t BRILLO_LLUVIA = 15; 


// =============================
// CONTANTES PARA REGULAR TEMPERATURA Y HUMEDAD
// =============================
const float OFFSET_TEMP    = -0.7;  // en °C, ajustalo para igualar al otro sensor
const float OFFSET_HUM     = -14.0;  // en %, ajustalo para igualar al otro sensor


// =============================
// CONTANTES PARA PROBAR LED DE LLUVIA
// =============================
const bool MODO_PRUEBA_LLUVIA = false;   // <-- true para forzar lluvia


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

// Variables globales (valor crudo promediado)
float TEMPERATURA = 0;
float HUMEDAD     = 0;

// Variables filtradas (suavizadas)
float tempFiltrada = NAN;
float humFiltrada  = NAN;
const float ALPHA_FILTRO = 0.2;   // 0.1 = más suave, 0.5 = más rápido

float tempMax = -100, tempMin = 200;
float humMax  = 0,    humMin  = 100;

unsigned long lastRead = 0;

// =============================
// INTERVALOS DE TIEMPO
// =============================
const unsigned long READ_INTERVAL_MS      = 10000UL;  // 10 segundos
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
// ACUMULADORES ESTILO ESTACIÓN METEO
// =============================

// Para promedio de 1 minuto (gráfico 1h)
float minTempSum = 0;
float minHumSum  = 0;
uint16_t minCount = 0;

// Para promedio de 5 minutos (CSV)
float csvTempSum = 0;
float csvHumSum  = 0;
uint16_t csvCount = 0;

// =============================
// LED + WeatherAPI
// =============================
const uint8_t LED_API  = D6;  // LED temp API
const uint8_t LED_RAIN = D7;  // LED lluvia API

float tempApi   = NAN;   // última temperatura desde la API
bool  lluviaApi = false; // lluvia AHORA (estado actual)
bool  lluviaPronosticoApi = false; // lluvia PRONOSTICADA (próximas horas)

const unsigned long RAIN_BLINK_INTERVAL = 500UL; // 500 ms ON/OFF
unsigned long lastRainBlink = 0;
bool rainLedState = false;

unsigned long lastWeatherCheck = 0;
const unsigned long WEATHER_INTERVAL = 10UL * 60UL * 1000UL; // cada 10 minutos

const char* WEATHER_API_KEY = "9ff16c4a57b4424e947202117251907";
const char* WEATHER_CITY    = "Corrientes,Argentina";


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
const unsigned long MAX_LOG_LINES = 8640UL; // 24 × 60 / 5 * 30
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
    if (('a' <= c && c <= 'z') ||
        ('A' <= c && c <= 'Z') ||
        ('0' <= c && c <= '9')) {
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
  client.setBufferSizes(256, 384);  // más chico para ahorrar RAM

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

  String line = client.readStringUntil('\n');
  if (line.startsWith("HTTP/1.1 200")) {
    Serial.println("✅ Telegram enviado correctamente");
  } else {
    Serial.println("⚠ Respuesta inesperada: " + line);
  }

  client.stop();
}

// =============================
// FILTRO EXPONENCIAL SUAVIZADO
// =============================
void actualizarFiltro() {
  if (isnan(tempFiltrada) || isnan(humFiltrada)) {
    tempFiltrada = TEMPERATURA;
    humFiltrada  = HUMEDAD;
  } else {
    tempFiltrada = ALPHA_FILTRO * TEMPERATURA + (1.0 - ALPHA_FILTRO) * tempFiltrada;
    humFiltrada  = ALPHA_FILTRO * HUMEDAD     + (1.0 - ALPHA_FILTRO) * humFiltrada;
  }
}

// =============================
// LECTURA DEL DHT22
// =============================
bool leerDHT() {
  float sumaT = 0, sumaH = 0;
  int muestras = 10;
  int validas  = 0;

  for (int i = 0; i < muestras; i++) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      sumaT += t;
      sumaH += h;
      validas++;
    }
    delay(50);
  }

  if (validas == 0) return false;

  TEMPERATURA = sumaT / validas;
  HUMEDAD     = sumaH / validas;

  TEMPERATURA += OFFSET_TEMP;
  HUMEDAD     += OFFSET_HUM;

  if (HUMEDAD < 0)   HUMEDAD = 0;
  if (HUMEDAD > 100) HUMEDAD = 100;

  actualizarFiltro();

  float tUso = isnan(tempFiltrada) ? TEMPERATURA : tempFiltrada;
  float hUso = isnan(humFiltrada)  ? HUMEDAD     : humFiltrada;

  if (tUso > tempMax) tempMax = tUso;
  if (tUso < tempMin) tempMin = tUso;

  if (hUso > humMax) humMax = hUso;
  if (hUso < humMin) humMin = hUso;

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
    if (first) {
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
void actualizarHistorial(float t, float h) {
  unsigned long ahora = millis();

  minTempSum += t;
  minHumSum  += h;
  minCount++;

  if (ahora - lastHistMinute >= HIST_MINUTE_INTERVAL) {
    lastHistMinute = ahora;

    if (minCount > 0) {
      float tMin = minTempSum / minCount;
      float hMin = minHumSum  / minCount;

      histTemp[histIndex] = tMin;
      histHum[histIndex]  = hMin;

      histIndex = (histIndex + 1) % HOUR_POINTS;
      if (histCount < HOUR_POINTS) histCount++;

      csvTempSum += tMin;
      csvHumSum  += hMin;
      csvCount++;

      minTempSum = 0;
      minHumSum  = 0;
      minCount   = 0;
    }
  }

  if (ahora - lastCsvSave >= CSV_SAVE_INTERVAL_MS) {
    lastCsvSave = ahora;

    if (csvCount > 0) {
      float tCsv = csvTempSum / csvCount;
      float hCsv = csvHumSum  / csvCount;

      String fecha = obtenerFechaHora();
      agregarRegistroLog(fecha, tCsv, hCsv);

      csvTempSum = 0;
      csvHumSum  = 0;
      csvCount   = 0;
    }
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

  double dn = ciclo - edad;
  double dl = 14.765 - edad;

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
  switch (f) {
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
  switch (f) {
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
// WeatherAPI → temperatura + lluvia → LEDs
// =============================
void actualizarClimaApi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[API] WiFi no conectado, no se consulta WeatherAPI.");
    return;
  }

  WiFiClient client;
  HTTPClient http;

  String url = "http://api.weatherapi.com/v1/forecast.json?key=";
  url += WEATHER_API_KEY;
  url += "&q=";
  url += WEATHER_CITY;
  url += "&lang=es&aqi=no&days=1";  // 1 día de pronóstico

  Serial.println("[API] Consultando WeatherAPI (forecast + current)...");
  Serial.print("[API] URL: ");
  Serial.println(url);

  // IMPORTANTE: evitar chunked y problemas raros
  http.useHTTP10(true);
  http.setTimeout(10000);  // 10 s

  if (!http.begin(client, url)) {
    Serial.println("[API] Error en http.begin()");
    return;
  }

  int httpCode = http.GET();
  Serial.printf("[API] httpCode = %d\n", httpCode);

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[API] Error HTTP: %d\n", httpCode);
    http.end();
    return;
  }

  // En vez de getString() de TODO, leemos solo una parte (p.ej. 4 KB)
  WiFiClient *stream = http.getStreamPtr();
  String payload = "";
  const size_t MAX_LEN = 4096;   // suficiente para location + current + day
  unsigned long t0 = millis();

  while (stream->connected() && (millis() - t0 < 3000) && payload.length() < MAX_LEN) {
    while (stream->available() && payload.length() < MAX_LEN) {
      char c = stream->read();
      payload += c;
    }
  }

  http.end();

  Serial.printf("[API] payload length = %u\n", payload.length());
  Serial.println("[API] ---- PAYLOAD PARCIAL ----");
  Serial.println(payload);
  Serial.println("[API] --------------------------");

  if (payload.length() == 0) {
    Serial.println("[API] Payload vacío, no se puede parsear.");
    return;
  }

  // --------- TEMP_C (current) ----------
  int idx = payload.indexOf("\"temp_c\":");
  if (idx < 0) {
    Serial.println("[API] No se encontró 'temp_c' en la respuesta.");
    return;
  }
  idx += 9; // salta "temp_c":

  int end = payload.indexOf(',', idx);
  if (end < 0) {
    Serial.println("[API] No se pudo aislar el valor de temp_c.");
    return;
  }

  String tempStr = payload.substring(idx, end);
  tempStr.trim();
  float tExt = tempStr.toFloat();

  if (tExt == 0 && tempStr.indexOf('0') == -1) {
    Serial.println("[API] Conversión de temp_c a float dudosa.");
    return;
  }

  tempApi = tExt;
  Serial.printf("[API] Temp API = %.1f C\n", tempApi);

  // --------- LLUVIA / TORMENTA (ahora) ----------
  bool hayLluvia = false;

  // 1) Texto de la condición actual
  int idxCond = payload.indexOf("\"condition\":");
  if (idxCond >= 0) {
    int idxText = payload.indexOf("\"text\":\"", idxCond);
    if (idxText >= 0) {
      idxText += 8; // salta "text":" 
      int endText = payload.indexOf('"', idxText);
      if (endText > idxText) {
        String txt = payload.substring(idxText, endText);
        String txtLower = txt;
        txtLower.toLowerCase();

        if (txtLower.indexOf("lluvia")    >= 0 ||
            txtLower.indexOf("llovizna")  >= 0 ||
            txtLower.indexOf("chubascos") >= 0 ||
            txtLower.indexOf("tormenta")  >= 0) {
          hayLluvia = true;
        }

        Serial.print("[API] Condición actual: ");
        Serial.println(txt);
      }
    }
  }

  // 2) precip_mm actual
  int idxP = payload.indexOf("\"precip_mm\":");
  if (idxP >= 0) {
    idxP += 12; // salta "precip_mm":
    int endP = payload.indexOf(',', idxP);
    if (endP < 0) endP = payload.indexOf('}', idxP);
    if (endP > idxP) {
      String pStr = payload.substring(idxP, endP);
      pStr.trim();
      float precip = pStr.toFloat();
      Serial.printf("[API] Precip_mm (actual) = %.2f\n", precip);
      if (precip > 1.0f) { // ------------------------------------- (UMBRAL DE PRECIPITACION)
        hayLluvia = true;
      }
    }
  }

  lluviaApi = hayLluvia;
  Serial.printf("[API] lluvia actual = %s\n", lluviaApi ? "SI" : "NO");



  // --------- PRONÓSTICO DE LLUVIA (día completo) ----------
  bool hayLluviaPronostico = false;

  int idxChance = payload.indexOf("\"daily_chance_of_rain\":");
  Serial.printf("[API] idxChance = %d\n", idxChance);

  if (idxChance >= 0) {
    int colon = payload.indexOf(':', idxChance);
    Serial.printf("[API] colon = %d\n", colon);

    if (colon > 0) {
      int start = colon + 1;  // primer carácter después de ':'
      int endChance = payload.indexOf(',', start);
      if (endChance < 0) endChance = payload.indexOf('}', start);

      Serial.printf("[API] start = %d, endChance = %d\n", start, endChance);

      if (endChance > start) {
        String cStr = payload.substring(start, endChance);
        cStr.trim();
        Serial.print("[API] cStr (raw) = '");
        Serial.print(cStr);
        Serial.println("'");

        int chance = cStr.toInt();
        Serial.printf("[API] daily_chance_of_rain = %d%%\n", chance);

        if (chance >= 70) { // umbral de pronóstico
          hayLluviaPronostico = true;
        }
      } else {
        Serial.println("[API] endChance <= start, no se pudo aislar daily_chance_of_rain.");
      }
    } else {
      Serial.println("[API] No se encontró ':' después de daily_chance_of_rain.");
    }
  } else {
    Serial.println("[API] 'daily_chance_of_rain' no encontrado en payload.");
  }

  lluviaPronosticoApi = hayLluviaPronostico;
  Serial.printf("[API] lluvia pronosticada = %s\n", lluviaPronosticoApi ? "SI" : "NO");

//-------------------------------------------------------------------------------------------------

  // --------- LED DE TEMPERATURA ----------
  const float UMBRAL_LED = 38.0;  // umbral temp API

  if (!isnan(tempApi) && tempApi >= UMBRAL_LED) {
    digitalWrite(LED_API, HIGH);
    Serial.println("[API] LED_API ON (temp_api >= umbral)");
  } else {
    digitalWrite(LED_API, LOW);
    Serial.println("[API] LED_API OFF (temp_api < umbral o dato inválido)");
  }
}

// ============================= Fin de actualizarClimaApi() =============================

// =============================
// CSV nombre e info
// =============================
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

    if (esCabecera) {
      esCabecera = false;
      continue;
    }

    if (linea.length() > 5) {
      if (primeraFecha == "") {
        primeraFecha = linea.substring(0, 10);
      }
      ultimaFecha = linea.substring(0, 10);
    }
  }

  f.close();

  if (primeraFecha == "" || ultimaFecha == "")
    return "historial_sin_datos.csv";

  auto normalizar = [](String f) {
    String d = f.substring(0, 2);
    String m = f.substring(3, 5);
    String y = f.substring(6, 10);
    return y + m + d;
  };

  String f1 = normalizar(primeraFecha);
  String f2 = normalizar(ultimaFecha);

  return "historial_" + f1 + "_" + f2 + ".csv";
}

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
      if (primera == "") primera = linea.substring(0, 10);
      ultima = linea.substring(0, 10);
    }
  }
  f.close();
}

// =============================
// SETUP
// =============================
void setup() {
  Serial.begin(115200);
  dht.begin();

  pinMode(LED_API, OUTPUT);
  digitalWrite(LED_API, LOW);

  pinMode(LED_RAIN, OUTPUT);   // NUEVO
  analogWrite(LED_RAIN, 0); // NUEVO

  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" conectado!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  for (int i = 0; i < HOUR_POINTS; i++) {
    histTemp[i] = NAN;
    histHum[i]  = NAN;
  }

  if (SPIFFS.begin()) {
    spiffsOk = true;
    Serial.println("SPIFFS montado correctamente.");
    iniciarLog();
  } else {
    spiffsOk = false;
    Serial.println("❌ Error montando SPIFFS.");
  }

  server.begin();

  enviarTelegram("🤖 Sistema iniciado correctamente.\nIP: " + WiFi.localIP().toString());
}

// =============================
// LOOP PRINCIPAL
// =============================
void loop() {
  unsigned long ahora = millis();

  // Lectura DHT + alerta
  if (ahora - lastRead > READ_INTERVAL_MS) {
    lastRead = ahora;

    if (leerDHT()) {
      float tUso = isnan(tempFiltrada) ? TEMPERATURA : tempFiltrada;
      float hUso = isnan(humFiltrada)  ? HUMEDAD     : humFiltrada;

      Serial.printf("Temp (filtrada): %.1f C | Hum (filtrada): %.1f %%\n", tUso, hUso);

      actualizarHistorial(tUso, hUso);

      if (tUso >= ALERTA_TEMP &&
          (lastAlert == 0 || (ahora - lastAlert > ALERT_INTERVAL))) {

        lastAlert = ahora;

        String alerta = "ALERTA: Temperatura alta!\n";
        alerta += "🌡 Temp: " + String(tUso) + " °C\n";
        alerta += "💧 Hum: " + String(hUso) + " %\n";
        alerta += "🕒 " + obtenerFechaHora();

        enviarTelegram(alerta);
      }
    }
  }

  // WeatherAPI cada X minutos
  if (ahora - lastWeatherCheck >= WEATHER_INTERVAL) {
    lastWeatherCheck = ahora;
    actualizarClimaApi();
  }

  // ===== LÓGICA DEL LED_RAIN (con PWM) =====
  if (lluviaApi) {
    // 1) Si ESTÁ lloviendo ahora -> LED fijo encendido (pero tenue)
    analogWrite(LED_RAIN, BRILLO_LLUVIA);
    rainLedState = true; // por si venía parpadeando
  } 
  else if (lluviaPronosticoApi) {
    // 2) No llueve ahora, pero hay PRONÓSTICO -> parpadeo con PWM
    if (ahora - lastRainBlink >= RAIN_BLINK_INTERVAL) {
      lastRainBlink = ahora;
      rainLedState = !rainLedState;
      analogWrite(LED_RAIN, rainLedState ? BRILLO_LLUVIA : 0);
    }
  } 
  else {
    // 3) Sin lluvia ni pronóstico -> LED apagado
    analogWrite(LED_RAIN, 0);
    rainLedState = false;
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
  client.read(); // '\n'

  while (client.available()) {
    String header = client.readStringUntil('\r');
    client.read();
    if (header.length() == 1) break;
  }

  // /data
  if (requestLine.indexOf("GET /data") >= 0) {
    String fechaHora = obtenerFechaHora();

    float tOut = isnan(tempFiltrada) ? TEMPERATURA : tempFiltrada;
    float hOut = isnan(humFiltrada)  ? HUMEDAD     : humFiltrada;

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Connection: close");
    client.println();

    client.print("{\"temp\":");
    client.print(tOut, 1);
    client.print(",\"hum\":");
    client.print(hOut, 1);
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

  // /csv
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

  // /clearcsv
  if (requestLine.startsWith("GET /clearcsv ")) {
    if (spiffsOk && SPIFFS.exists(LOG_FILE)) {
      SPIFFS.remove(LOG_FILE);
    }

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

  // /csvinfo
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

  // Página principal (HTML simple de prueba)
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
        body {
            background: #0d0f1a;
            color: #e0e0e0;
            font-family: Segoe UI, sans-serif;
            text-align: center;
            padding: 25px;
        }

        h1 {
            margin-bottom: 5px;
        }

        h2 {
            margin-top: 0;
            color: #aaa;
        }

        .panel {
            margin: 16px auto;
            display: inline-block;
            text-align: left;
            padding: 12px 20px;
            border-radius: 16px;
            background: #1b1d2b;
            min-width: 300px;
        }

        .header-panel {
            display: flex;
            justify-content: space-between;
            align-items: center;
            gap: 10px;
            padding: 11px 15px;
            width: 100%;
            max-width: 560px;
        }

        .header-info {
            display: flex;
            align-items: center;
            gap: 14px;
        }

        .header-icon {
            font-size: 42px;
            color: #ff8b5d;
        }

        .header-text h1 {
            margin: 0;
            font-size: 22px;
            letter-spacing: -0.01em;
        }

        .header-subtitle {
            margin: 0;
            font-size: 10px;
            text-transform: uppercase;
            letter-spacing: 0.16em;
            color: rgba(224, 224, 224, 0.62);
        }

        .header-update {
            display: flex;
            flex-direction: column;
            align-items: flex-end;
            gap: 2px;
        }

        .update-label {
            font-size: 9px;
            letter-spacing: 0.16em;
            text-transform: uppercase;
            color: rgba(224, 224, 224, 0.52);
        }

        .update-value {
            font-size: 14px;
            font-weight: 600;
        }

        .section-title {
            font-size: 18.7px;
            margin-bottom: 6px;
            display: flex;
            align-items: center;
            gap: 6px;
            font-weight: bold;
        }

        .icon {
            font-family: 'Material Symbols Rounded';
            font-size: 28px;
            vertical-align: middle;
        }

        #temp {
            color: #FFD700;
            font-size: 26px;
            font-weight: 700;
        }

        #hum {
            color: #00B4FF;
            font-size: 26px;
            font-weight: 700;
        }

        .icon-temp {
            color: #FFC107;
        }

        .icon-hum {
            color: #03A9F4;
        }

        .icon-time {
            color: #4CAF50;
        }

        .label {
            margin: 4px 0;
            font-size: 15.4px;
        }

        .value {
            font-size: 17.6px;
            font-weight: bold;
        }

        .charts {
            max-width: 1000px;
            margin: 14px auto;
            display: flex;
            flex-wrap: wrap;
            justify-content: space-around;
            gap: 18px;
        }

        canvas {
            background: #141622;
            border-radius: 10px;
            padding: 8px;
        }

        .footer {
            margin-top: 25px;
            font-size: 16px;
            font-family: Georgia, serif;
            color: #DAA5;
            font-style: italic;
            text-shadow: 0 0 3px rgba(255, 215, 0, 0.25);
        }

        .chart-block {
            flex: 1;
            min-width: 280px;
        }

        /* ======== Estilo iPhone Lunar Card ======== */
        .lunar-ios {
            background: rgba(255, 255, 255, 0.04);
            padding: 28px;
            border-radius: 22px;
            box-shadow:
                inset 0 0 12px rgba(255, 255, 255, 0.05),
                0 8px 22px rgba(0, 0, 0, 0.35);
            -webkit-backdrop-filter: blur(10px);
            backdrop-filter: blur(10px);
            transition: 0.25s;
            min-width: 320px;
        }

        .lunar-ios:hover {
            transform: scale(1.02);
            box-shadow:
                inset 0 0 16px rgba(255, 255, 255, 0.07),
                0 12px 30px rgba(0, 0, 0, 0.45);
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
            filter: drop-shadow(0 4px 6px rgba(0, 0, 0, 0.5));
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
            background: rgba(255, 255, 255, 0.10);
            margin: 12px 0 18px 0;
        }

        .lunar-row {
            display: flex;
            justify-content: space-between;
            padding: 4px 0;
        }

        .label-ios {
            font-size: 14px;
            color: #B7B7B7;
        }

        .value-ios {
            font-size: 15px;
            font-weight: 600;
            color: #EEE;
        }

        /* Botones responsive */
        #btnCSV,
        #btnClear {
            width: clamp(44px, 5.5vw, 60px);
            height: clamp(44px, 5.5vw, 60px);
            border-radius: 50%;
        }

        #btnCSV span.material-symbols-rounded,
        #btnClear span.material-symbols-rounded {
            font-size: clamp(16px, 2.6vw, 24px);
        }

        #btnCSV .btn-label,
        #btnClear .btn-label {
            font-size: clamp(5.5px, 1.4vw, 8px);
        }

        @media (max-width: 480px) {
            #btnCSV,
            #btnClear {
                width: clamp(48px, 6.2vw, 64px);
                height: clamp(48px, 6.2vw, 64px);
            }
        }

        .heading-main {
            color: #A05C1F;
            font-size: 22px;
        }

        .panel-main {
            position: relative;
            display: flex;
            gap: 11px;
            align-items: flex-start;
            padding: 11px 15px;
            width: 100%;
            max-width: 560px;
            margin: 11px auto;
        }

        .climate-section {
            flex: 1;
            min-width: 260px;
            padding-right: 6px;
        }

        .lunar-section {
            flex: 1;
            min-width: 240px;
            padding-left: 6px;
        }

        @media (max-width: 600px) {
            body {
                padding: 18px 12px;
            }

            .panel {
                display: block;
                min-width: 0;
            }

            .panel.header-panel {
                flex-direction: column;
                align-items: center;
                justify-content: center;
                min-width: 0;
                width: clamp(200px, 72vw, 280px);
                margin: 0 auto 14px auto;
                padding: 15px 16px 13px 16px;
                gap: 11px;
                text-align: center;
            }

            .header-info {
                justify-content: center;
            }

            .header-update {
                align-items: center;
            }

            .panel-main {
                flex-direction: column;
                align-items: stretch;
                min-width: 0;
                width: clamp(240px, 88vw, 340px);
                margin: 15px auto;
                padding: 18px;
                gap: 16px;
                box-sizing: border-box;
            }

            .climate-section,
            .lunar-section {
                min-width: 0;
                width: 100%;
                padding: 0;
            }

            .climate-section {
                border-bottom: 1px solid rgba(255, 255, 255, 0.12);
                padding-bottom: 12px;
                margin-bottom: 6px;
            }

            .lunar-section {
                text-align: center;
            }

            .lunar-header {
                justify-content: center;
            }

            .lunar-row {
                justify-content: center;
                gap: 10px;
            }
        }

        .panel-csv {
            max-width: 220px;
            width: clamp(180px, 52%, 220px);
            min-width: 0;
            margin: 14px auto;
            background: #11131d;
            text-align: center;
            padding: 8px 10px;
        }

        .csv-title {
            text-align: center;
            margin-bottom: 8px;
            font-size: 17px;
        }

        .csv-row {
            margin-bottom: 3px;
            font-size: 13px;
            color: #adb1bc;
        }

        .csv-row-large {
            margin-bottom: 6px;
        }

        .csv-row strong {
            font-weight: 400;
            color: #a1a5af;
        }

        .csv-row span {
            color: #9096a4;
        }

        .csv-row span.value-bold {
            font-weight: 600;
            color: #bdc2cc;
        }

        .csv-actions {
            display: flex;
            justify-content: center;
            gap: 12px;
            margin-top: 10px;
        }

        hr {
            border: none;
            height: 1px;
            background: rgba(255, 255, 255, 0.08);
            margin: 10px 0;
        }

        .circular-btn {
            width: 70px;
            height: 70px;
            border-radius: 50%;
            cursor: pointer;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            gap: 4px;
            border: 1px solid rgba(255, 255, 255, 0.06);
            background: #141620;
            transition: transform 0.18s ease, background-color 0.18s ease, border-color 0.18s ease;
        }

        .circular-btn:hover {
            transform: translateY(-2px);
            background: #1c1f2b;
            border-color: rgba(255, 255, 255, 0.12);
        }

        .btn-clear {
            background: #1f1419;
            border-color: rgba(200, 90, 110, 0.26);
        }

        .btn-clear:hover {
            background: #271a21;
            border-color: rgba(200, 90, 110, 0.4);
        }

        .btn-csv {
            background: #111d18;
            border-color: rgba(96, 164, 132, 0.26);
        }

        .btn-csv:hover {
            background: #16251f;
            border-color: rgba(96, 164, 132, 0.4);
        }

        .btn-icon {
            font-size: 24px;
            color: #a3a8b4;
        }

        .btn-label {
            font-size: 8px;
            letter-spacing: 0.36px;
            color: #9398a3;
        }

        #btnClear .btn-icon {
            color: #9a5764;
        }

        #btnClear .btn-label {
            color: #82616a;
        }

        #btnCSV .btn-icon {
            color: #46755d;
        }

        #btnCSV .btn-label {
            color: #5f7f6d;
        }
    </style>
</head>
<body>
    <div class="panel header-panel">
        <div class="header-info">
            <span class="material-symbols-rounded header-icon">sensors</span>
            <div class="header-text">
                <h1>Monitor Ambiental</h1>
                <p class="header-subtitle">ESP8266 · DHT22</p>
            </div>
        </div>
        <div class="header-update">
            <span class="update-label">Última actualización</span>
            <span id="time" class="update-value">--/--/---- --:--:--</span>
        </div>
    </div>

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

    <script>
        let tempChart, humChart;
        let tempHourChart, humHourChart;

        const HOUR_POINTS = 60;
        let bufferTemp = [];
        let bufferHum = [];

        function smoothEWMA(values, alpha = 0.2) {
            if (values.length < 2) {
                return values[values.length - 1];
            }

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
                data: {
                    labels: [],
                    datasets: [{
                        label: 'Temperatura (°C)',
                        data: [],
                        borderColor: 'rgba(255,99,132,1)',
                        backgroundColor: 'rgba(255,99,132,0.25)',
                        tension: 0.4,
                        pointRadius: 0
                    }]
                },
                options: {
                    animation: false,
                    scales: {
                        x: {
                            display: false,
                            grid: {
                                color: 'rgba(255,255,255,0.12)',
                                lineWidth: 1
                            }
                        },
                        y: {
                            beginAtZero: false,
                            grid: {
                                color: 'rgba(255,255,255,0.12)',
                                lineWidth: 1
                            },
                            ticks: {
                                callback: function (v) {
                                    return v.toFixed(1);
                                }
                            }
                        }
                    }
                }
            });

            humChart = new Chart(hctx, {
                type: 'line',
                data: {
                    labels: [],
                    datasets: [{
                        label: 'Humedad (%)',
                        data: [],
                        borderColor: 'rgba(54,162,235,1)',
                        backgroundColor: 'rgba(54,162,235,0.25)',
                        tension: 0.4,
                        pointRadius: 0
                    }]
                },
                options: {
                    animation: false,
                    scales: {
                        x: {
                            display: false,
                            grid: {
                                color: 'rgba(255,255,255,0.12)',
                                lineWidth: 1
                            }
                        },
                        y: {
                            beginAtZero: false,
                            suggestedMax: 100,
                            grid: {
                                color: 'rgba(255,255,255,0.12)',
                                lineWidth: 1
                            },
                            ticks: {
                                callback: function (v) {
                                    return v.toFixed(1);
                                }
                            }
                        }
                    }
                }
            });

            const thctx = document.getElementById('tempHourChart').getContext('2d');
            const hhctx = document.getElementById('humHourChart').getContext('2d');

            tempHourChart = new Chart(thctx, {
                type: 'scatter',
                data: {
                    datasets: [{
                        label: 'Temp 1h (°C)',
                        data: [],
                        borderColor: 'rgba(255,99,132,1)',
                        backgroundColor: 'rgba(255,99,132,1)',
                        showLine: false,
                        pointRadius: 3
                    }]
                },
                options: {
                    animation: false,
                    scales: {
                        x: {
                            type: 'linear',
                            min: 0,
                            max: HOUR_POINTS - 1,
                            grid: {
                                color: 'rgba(255,255,255,0.12)',
                                lineWidth: 1
                            }
                        },
                        y: {
                            beginAtZero: false,
                            grid: {
                                color: 'rgba(255,255,255,0.12)',
                                lineWidth: 1
                            },
                            ticks: {
                                callback: function (v) {
                                    return v.toFixed(1);
                                }
                            }
                        }
                    }
                }
            });

            humHourChart = new Chart(hhctx, {
                type: 'scatter',
                data: {
                    datasets: [{
                        label: 'Hum 1h (%)',
                        data: [],
                        borderColor: 'rgba(54,162,235,1)',
                        backgroundColor: 'rgba(54,162,235,1)',
                        showLine: false,
                        pointRadius: 3
                    }]
                },
                options: {
                    animation: false,
                    scales: {
                        x: {
                            type: 'linear',
                            min: 0,
                            max: HOUR_POINTS - 1,
                            grid: {
                                color: 'rgba(255,255,255,0.12)',
                                lineWidth: 1
                            }
                        },
                        y: {
                            beginAtZero: false,
                            suggestedMax: 100,
                            grid: {
                                color: 'rgba(255,255,255,0.12)',
                                lineWidth: 1
                            },
                            ticks: {
                                callback: function (v) {
                                    return v.toFixed(0);
                                }
                            }
                        }
                    }
                }
            });
        }

        function agregarPunto(chart, label, value, maxPts) {
            chart.data.labels.push(label);
            chart.data.datasets[0].data.push(value);

            if (chart.data.labels.length > maxPts) {
                chart.data.labels.shift();
                chart.data.datasets[0].data.shift();
            }

            chart.update();
        }

        function actualizarDatos() {
            fetch('/data')
                .then(r => r.json())
                .then(d => {
                    document.getElementById('temp').textContent = d.temp.toFixed(1);
                    document.getElementById('hum').textContent = d.hum.toFixed(1);
                    document.getElementById('tmin').textContent = d.tmin.toFixed(1);
                    document.getElementById('tmax').textContent = d.tmax.toFixed(1);
                    document.getElementById('hmin').textContent = d.hmin.toFixed(1);
                    document.getElementById('hmax').textContent = d.hmax.toFixed(1);
                    document.getElementById('time').textContent = d.time;

                    document.getElementById('moonIcon').textContent = d.moonIcon;
                    document.getElementById('moonName').textContent = d.moonName;
                    document.getElementById('nextNewMoon').textContent = d.nextNewMoon;
                    document.getElementById('nextFullMoon').textContent = d.nextFullMoon;

                    bufferTemp.push(d.temp);
                    bufferHum.push(d.hum);

                    if (bufferTemp.length > 50) {
                        bufferTemp.shift();
                    }

                    if (bufferHum.length > 50) {
                        bufferHum.shift();
                    }

                    const tempSmoothed = smoothEWMA(bufferTemp, 0.2);
                    const humSmoothed = smoothEWMA(bufferHum, 0.2);

                    agregarPunto(tempChart, d.time, tempSmoothed, 40);
                    agregarPunto(humChart, d.time, humSmoothed, 40);

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
            fetch('/csvinfo')
                .then(r => r.json())
                .then(d => {
                    document.getElementById('csvFirst').textContent = d.first || '--/--/----';
                    document.getElementById('csvLast').textContent = d.last || '--/--/----';
                })
                .catch(err => console.error('Error al obtener /csvinfo', err));
        }
    </script>

    <!-- ===== TARJETA DE INFORME DEL CSV ===== -->
    <div class="panel panel-csv">
        <h2 class="csv-title">Historial CSV</h2>

        <div class="csv-row">
            <strong>Primer registro:</strong>
            <span id="csvFirst" class="value-bold">--/--/----</span>
        </div>

        <div class="csv-row csv-row-large">
            <strong>Último registro:</strong>
            <span id="csvLast" class="value-bold">--/--/----</span>
        </div>

        <div class="csv-actions">
            <!-- ========== BOTÓN CLEAR (ROJO) ========== -->
            <button id="btnClear" class="circular-btn btn-clear">
                <span class="material-symbols-rounded btn-icon">delete</span>
                <span class="btn-label">Clear</span>
            </button>

            <!-- ========== BOTÓN CSV (VERDE) ========== -->
            <button id="btnCSV" class="circular-btn btn-csv">
                <span class="material-symbols-rounded btn-icon">table</span>
                <span class="btn-label">.CSV</span>
            </button>
        </div>
    </div>

    <div class="footer">by: Luciano Aromi</div>

    <script>
        document.getElementById('btnCSV').addEventListener('click', function () {
            window.location.href = '/csv';
        });

        document.getElementById('btnClear').addEventListener('click', function () {
            if (confirm('¿Seguro que deseas borrar TODO el historial del CSV?')) {
                fetch('/clearcsv')
                    .then(() => alert('Historial CSV borrado correctamente.'))
                    .catch(() => alert('Error al borrar el CSV.'));
            }
        });
    </script>
</body>
</html>



<!-- ============================================================================================= -->


)rawliteral");

  client.stop();
}
