#include <Wire.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <math.h>

// ============================================================
// ESP32 + Smart BMS SMBus Battery Monitor
// ============================================================

// ---------------- I2C / SMBus ----------------
#define SDA_PIN   21
#define SCL_PIN   22
#define BMS_ADDR  0x0B

// ---------------- OLED ----------------
#define OLED_ADDR    0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------------- Scaling ----------------
// SMBus Battery voltage = mV
// SMBus Battery current = mA
#define VOLTAGE_SCALE 0.001
#define CURRENT_SCALE 0.001

// ---------------- WiFi AP ----------------
const char* ap_ssid     = "TECHHI";
const char* ap_password = "12341234";

AsyncWebServer server(80);

// ============================================================
// GLOBAL BATTERY DATA
// ============================================================

float g_voltage = 0.0;
float g_current = 0.0;
int   g_soc     = 0;
float g_temp    = 0.0;

uint16_t g_tte = 65535;
uint16_t g_ttf = 65535;

bool g_isCharging    = false;
bool g_isDischarging = false;

bool g_okV = false;
bool g_okI = false;
bool g_okT = false;
bool g_okSOC = false;

// Raw BMS values
uint16_t g_rawSOC = 0;
uint16_t g_rawV   = 0;
uint16_t g_rawI   = 0;
uint16_t g_rawT   = 0;

// ============================================================
// TIMERS
// ============================================================

unsigned long lastSMBusRead  = 0;
unsigned long lastOLEDSwitch = 0;
unsigned long vuAnimTimer    = 0;

const unsigned long SMBUS_INTERVAL = 3000;
const unsigned long SCREEN_SWITCH  = 30000;
const unsigned long VU_ANIM_SPEED  = 80;

bool showDataScreen = false;

// ============================================================
// VU METER
// ============================================================

int vuBars[16];
int vuPeak[16];
int vuPeakTimer[16];

// ============================================================
// SMBus READ WORD
// ============================================================

bool readWordSMBus(uint8_t cmd, uint16_t *value)
{
  Wire.beginTransmission(BMS_ADDR);

  Wire.write(cmd);

  uint8_t err = Wire.endTransmission(false);

  if (err != 0)
    return false;

  uint8_t got = Wire.requestFrom(BMS_ADDR, (uint8_t)2);

  if (got != 2)
    return false;

  uint8_t lsb = Wire.read();
  uint8_t msb = Wire.read();

  *value = ((uint16_t)msb << 8) | lsb;

  return true;
}

// ============================================================
// VOLTAGE FALLBACK
// Used ONLY if BMS SOC is not available.
// ============================================================

int estimateSOCFromVoltage(float v)
{
  if (v >= 12.60) return 100;
  if (v >= 12.40) return 90;
  if (v >= 12.20) return 80;
  if (v >= 12.00) return 70;
  if (v >= 11.80) return 60;
  if (v >= 11.60) return 50;
  if (v >= 11.40) return 40;
  if (v >= 11.20) return 30;
  if (v >= 11.00) return 20;
  if (v >= 10.50) return 10;

  return 0;
}

// ============================================================
// FORMAT TIME
// ============================================================

String formatMinutes(uint16_t mins)
{
  if (mins == 65535 || mins == 0 || mins > 1440)
    return "Calc...";

  int h = mins / 60;
  int m = mins % 60;

  if (h > 0)
    return String(h) + "h " + String(m) + "m";

  return String(m) + " min";
}

// ============================================================
// READ ALL BMS DATA
// ============================================================

void readAllSMBus()
{
  uint16_t rawV   = 0;
  uint16_t rawI   = 0;
  uint16_t rawSOC = 0;
  uint16_t rawT   = 0;
  uint16_t rawTTE = 0;
  uint16_t rawTTF = 0;

  // ----------------------------------------------------------
  // Standard Smart Battery commands
  // ----------------------------------------------------------

  g_okV = readWordSMBus(0x09, &rawV);

  g_okI = readWordSMBus(0x0A, &rawI);

  // RelativeStateOfCharge
  bool okSOC = readWordSMBus(0x0D, &rawSOC);

  // Temperature
  g_okT = readWordSMBus(0x08, &rawT);

  // Run Time To Empty
  bool okTTE = readWordSMBus(0x12, &rawTTE);

  // Run Time To Full
  bool okTTF = readWordSMBus(0x13, &rawTTF);

  // ----------------------------------------------------------
  // Voltage
  // ----------------------------------------------------------

  if (g_okV)
  {
    g_rawV = rawV;
    g_voltage = rawV * VOLTAGE_SCALE;
  }

  // ----------------------------------------------------------
  // Current
  // SMBus current is signed
  // ----------------------------------------------------------

  if (g_okI)
  {
    g_rawI = rawI;

    int16_t signedCurrent = (int16_t)rawI;

    g_current = signedCurrent * CURRENT_SCALE;
  }

  // ----------------------------------------------------------
  // Temperature
  // SMBus temperature = 0.1 Kelvin
  // ----------------------------------------------------------

  if (g_okT)
  {
    g_rawT = rawT;

    g_temp = (rawT / 10.0) - 273.15;
  }

  // ==========================================================
  // IMPORTANT: SOC
  // ==========================================================

  if (okSOC)
  {
    g_rawSOC = rawSOC;

    // BMS RelativeStateOfCharge should normally be 0-100%.
    if (rawSOC <= 100)
    {
      g_soc = (int)rawSOC;
      g_okSOC = true;
    }
    else
    {
      g_okSOC = false;
    }
  }
  else
  {
    g_okSOC = false;
  }

  // ----------------------------------------------------------
  // If BMS SOC is unavailable, use voltage only as fallback
  // ----------------------------------------------------------

  if (!g_okSOC)
  {
    g_soc = estimateSOCFromVoltage(g_voltage);
  }

  // Safety clamp
  g_soc = constrain(g_soc, 0, 100);

  // ----------------------------------------------------------
  // Time remaining
  // ----------------------------------------------------------

  if (okTTE)
    g_tte = rawTTE;

  if (okTTF)
    g_ttf = rawTTF;

  // ----------------------------------------------------------
  // Charge / discharge status
  // ----------------------------------------------------------

  g_isCharging = false;
  g_isDischarging = false;

  if (g_current > 0.01)
  {
    g_isCharging = true;
  }
  else if (g_current < -0.01)
  {
    g_isDischarging = true;
  }

  // ==========================================================
  // SERIAL DEBUG
  // ==========================================================

  Serial.println();
  Serial.println("========== BMS DATA ==========");

  Serial.print("Voltage: ");
  Serial.print(g_voltage, 3);
  Serial.println(" V");

  Serial.print("Current: ");
  Serial.print(g_current, 3);
  Serial.println(" A");

  Serial.print("Raw SOC (0x0D): ");
  Serial.println(g_rawSOC);

  Serial.print("SOC: ");
  Serial.print(g_soc);
  Serial.println(" %");

  Serial.print("SOC source: ");

  if (g_okSOC)
    Serial.println("BMS 0x0D");
  else
    Serial.println("Voltage FALLBACK");

  Serial.print("Temperature: ");
  Serial.print(g_temp, 1);
  Serial.println(" C");

  Serial.print("TTE raw: ");
  Serial.println(g_tte);

  Serial.print("TTF raw: ");
  Serial.println(g_ttf);

  Serial.print("Status: ");

  if (g_isCharging)
    Serial.println("CHARGING");
  else if (g_isDischarging)
    Serial.println("DISCHARGING");
  else
    Serial.println("IDLE");

  Serial.println("==============================");
}

// ============================================================
// VU METER
// ============================================================

void updateVUBars()
{
  int baseHeight = map(g_soc, 0, 100, 4, 40);

  for (int i = 0; i < 16; i++)
  {
    int target = constrain(
      baseHeight + random(-8, 9),
      2,
      44
    );

    if (target > vuBars[i])
      vuBars[i] = target;
    else
      vuBars[i] = max(2, vuBars[i] - 3);

    if (vuBars[i] >= vuPeak[i])
    {
      vuPeak[i] = vuBars[i];
      vuPeakTimer[i] = 12;
    }
    else
    {
      if (vuPeakTimer[i] > 0)
        vuPeakTimer[i]--;
      else if (vuPeak[i] > 2)
        vuPeak[i]--;
    }
  }
}

// ============================================================
// TITLE BAND
// ============================================================

void drawTitleBand()
{
  display.fillRect(
    0,
    0,
    128,
    10,
    SH110X_WHITE
  );

  display.setTextColor(SH110X_BLACK);
  display.setTextSize(1);
  display.setCursor(38, 1);
  display.print("TECHHI");

  display.setTextColor(SH110X_WHITE);
}

// ============================================================
// OLED VU SCREEN
// ============================================================

void drawVUScreen()
{
  display.clearDisplay();

  drawTitleBand();

  display.setTextSize(1);

  display.setCursor(0, 12);
  display.print(String(g_soc) + "%");

  display.setCursor(30, 12);
  display.print(String(g_voltage, 2) + "V");

  display.setCursor(75, 12);

  if (g_isCharging)
  {
    display.print(
      "CHG " +
      String(fabs(g_current), 2) +
      "A"
    );
  }
  else if (g_isDischarging)
  {
    display.print(
      "DSG " +
      String(fabs(g_current), 2) +
      "A"
    );
  }
  else
  {
    display.print("IDLE");
  }

  int bbottom = 63;
  int bheight = 42;

  for (int i = 0; i < 16; i++)
  {
    int x = i * 8;

    int barH = map(
      vuBars[i],
      0,
      44,
      0,
      bheight
    );

    int barY = bbottom - barH;

    for (int seg = barY; seg < bbottom; seg += 3)
    {
      display.fillRect(
        x,
        seg,
        6,
        min(2, bbottom - seg),
        SH110X_WHITE
      );
    }

    int peakY =
      bbottom -
      map(vuPeak[i], 0, 44, 0, bheight) -
      1;

    if (peakY >= 21 && peakY < bbottom)
    {
      display.fillRect(
        x,
        peakY,
        6,
        1,
        SH110X_WHITE
      );
    }
  }

  display.display();
}

// ============================================================
// OLED DATA SCREEN
// ============================================================

void drawDataScreen()
{
  display.clearDisplay();

  drawTitleBand();

  // SOC
  display.setTextSize(2);
  display.setCursor(0, 12);
  display.print(String(g_soc) + "%");

  // Voltage
  display.setTextSize(1);
  display.setCursor(68, 12);
  display.print(
    "V:" + String(g_voltage, 2) + "V"
  );

  // Temperature
  display.setCursor(68, 21);
  display.print(
    "T:" + String(g_temp, 1) + "C"
  );

  // Battery bar
  display.drawRect(
    0,
    29,
    128,
    7,
    SH110X_WHITE
  );

  display.fillRect(
    2,
    31,
    (g_soc * 124) / 100,
    3,
    SH110X_WHITE
  );

  // Charge time
  display.setTextSize(1);
  display.setCursor(0, 39);

  if (g_isCharging)
  {
    display.print(
      "CHG:" +
      String(fabs(g_current), 2) +
      "A " +
      formatMinutes(g_ttf)
    );
  }
  else
  {
    display.print("CHG: --");
  }

  // Discharge time
  display.setCursor(0, 51);

  if (g_isDischarging)
  {
    display.print(
      "DSG:" +
      String(fabs(g_current), 2) +
      "A " +
      formatMinutes(g_tte)
    );
  }
  else
  {
    display.print("DSG: --");
  }

  display.display();
}

// ============================================================
// JSON DATA
// ============================================================

String buildJSON()
{
  String status =
    g_isCharging
      ? "CHG"
      : (g_isDischarging ? "DSG" : "IDLE");

  String json = "{";

  json += "\"voltage\":";
  json += String(g_voltage, 2);
  json += ",";

  json += "\"current\":";
  json += String(fabs(g_current), 2);
  json += ",";

  json += "\"soc\":";
  json += String(g_soc);
  json += ",";

  json += "\"rawSOC\":";
  json += String(g_rawSOC);
  json += ",";

  json += "\"socValid\":";
  json += String(g_okSOC ? "true" : "false");
  json += ",";

  json += "\"temp\":";
  json += String(g_temp, 1);
  json += ",";

  json += "\"tte\":\"";
  json += formatMinutes(g_tte);
  json += "\",";

  json += "\"ttf\":\"";
  json += formatMinutes(g_ttf);
  json += "\",";

  json += "\"charging\":";
  json += String(g_isCharging ? "true" : "false");
  json += ",";

  json += "\"discharging\":";
  json += String(g_isDischarging ? "true" : "false");
  json += ",";

  json += "\"status\":\"";
  json += status;
  json += "\"";

  json += "}";

  return json;
}

// ============================================================
// WEB DASHBOARD
// ============================================================

String buildPage()
{
  String sc =
    g_soc > 60
      ? "#22c55e"
      : (g_soc > 25 ? "#f59e0b" : "#ef4444");

  String cL =
    g_isCharging
      ? "Charging"
      : (g_isDischarging ? "Discharging" : "Idle");

  String cB =
    g_isCharging
      ? "chg"
      : (g_isDischarging ? "dsg" : "idle");

  String p =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"

    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"

    "body{font-family:system-ui,sans-serif;"
    "background:#0f172a;color:#e2e8f0;padding:20px 16px}"

    ".c{max-width:420px;margin:0 auto}"

    ".h{text-align:center;margin-bottom:20px}"

    ".h h1{font-size:20px;font-weight:700;color:#f8fafc}"

    ".h p{font-size:12px;color:#64748b;margin-top:3px}"

    ".dot{display:inline-block;width:8px;height:8px;"
    "border-radius:50%;background:#22c55e;margin-right:6px;"
    "animation:pulse 2s infinite}"

    "@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}"

    ".gw{display:flex;justify-content:center;margin-bottom:18px}"

    ".g{position:relative;width:200px;height:200px;"
    "border-radius:50%;background:#1e293b;"
    "border:3px solid #334155;overflow:hidden;"
    "box-shadow:0 0 0 6px #0f172a,"
    "inset 0 0 30px rgba(0,0,0,.4)}"

    ".lw{position:absolute;bottom:0;left:0;width:100%;"
    "transition:height .6s}"

    ".lf{width:100%;height:200px}"

    "@keyframes wave{from{transform:translateX(0)}"
    "to{transform:translateX(50%)}}"

    ".wa{animation:wave 3s linear infinite}"

    ".gt{position:absolute;top:0;left:0;width:100%;"
    "height:100%;display:flex;flex-direction:column;"
    "align-items:center;justify-content:center;z-index:3}"

    ".gp{font-size:46px;font-weight:800;color:#f8fafc;"
    "text-shadow:0 2px 8px rgba(0,0,0,.6)}"

    ".gs{font-size:11px;color:#cbd5e1;"
    "text-transform:uppercase;letter-spacing:1.5px}"

    ".gv{font-size:15px;font-weight:600;color:#f8fafc;"
    "margin-top:3px}"

    ".tr{display:grid;grid-template-columns:1fr 1fr;"
    "gap:10px;margin-bottom:14px}"

    ".tc{background:#1e293b;border:1px solid #334155;"
    "border-radius:16px;padding:14px;text-align:center}"

    ".tc.ch{border-color:#22c55e44}"

    ".tc.ds{border-color:#3b82f644}"

    ".tv-c{font-size:18px;font-weight:800;color:#22c55e}"

    ".tv-d{font-size:18px;font-weight:800;color:#3b82f6}"

    ".tl{font-size:10px;color:#94a3b8;"
    "text-transform:uppercase;letter-spacing:1px;margin-top:4px}"

    ".gr{display:grid;grid-template-columns:1fr 1fr;gap:10px}"

    ".cd{background:#1e293b;border:1px solid #334155;"
    "border-radius:14px;padding:14px}"

    ".cl{font-size:10px;color:#64748b;"
    "text-transform:uppercase;letter-spacing:.5px;margin-bottom:5px}"

    ".cv{font-size:22px;font-weight:700;color:#f8fafc}"

    ".cu{font-size:12px;color:#94a3b8;font-weight:400}"

    ".b{display:inline-block;padding:2px 8px;"
    "border-radius:20px;font-size:10px;font-weight:600;"
    "margin-top:4px}"

    ".chg{background:#22c55e22;color:#22c55e}"

    ".dsg{background:#3b82f622;color:#3b82f6}"

    ".idle{background:#94a3b822;color:#94a3b8}"

    ".err{background:#ef444422;color:#ef4444}"

    "</style></head><body><div class='c'>";

  // Header
  p +=
    "<div class='h'>"
    "<h1><span class='dot'></span>TECHHI Battery</h1>"
    "<p>A2170 SMBus &middot; live update</p>"
    "</div>";

  // Gauge
  p +=
    "<div class='gw'>"
    "<div class='g'>"

    "<div class='lw' id='lw' "
    "style='height:" + String(g_soc) + "%;'>"

    "<svg class='wa' style='width:100%;display:block;' "
    "viewBox='0 0 800 80' preserveAspectRatio='none'>"

    "<path id='wp' "
    "d='M0,40 C100,0 200,80 300,40 "
    "C400,0 500,80 600,40 "
    "C700,0 800,80 900,40 "
    "L900,80 L0,80 Z' "
    "fill='" + sc + "'/>"

    "</svg>"

    "<div class='lf' id='lf' "
    "style='background:" + sc + ";'></div>"

    "</div>"

    "<div class='gt'>"

    "<div class='gp' id='gp'>"
    + String(g_soc) +
    "%</div>"

    "<div class='gs'>Battery</div>"

    "<div class='gv' id='gv'>"
    + String(g_voltage, 2) +
    " V</div>"

    "</div>"
    "</div>"
    "</div>";

  // Charge / discharge
  p +=
    "<div class='tr'>"

    "<div class='tc ch'>"

    "<div class='tv-c' id='ttf'>"
    + String(g_isCharging ? "&#9679; " : "&#9675; ")
    + formatMinutes(g_ttf) +
    "</div>"

    "<div class='tl'>&#128267; To Full Charge</div>";

  if (g_isCharging)
  {
    p +=
      "<span class='b chg' id='cb'>CHG "
      + String(fabs(g_current), 2)
      + "A</span>";
  }
  else
  {
    p +=
      "<span class='b idle' id='cb'>"
      "Not Charging</span>";
  }

  p += "</div>";

  p +=
    "<div class='tc ds'>"

    "<div class='tv-d' id='tte'>"
    + String(g_isDischarging ? "&#9679; " : "&#9675; ")
    + formatMinutes(g_tte) +
    "</div>"

    "<div class='tl'>&#128267; Backup Remaining</div>";

  if (g_isDischarging)
  {
    p +=
      "<span class='b dsg' id='db'>DSG "
      + String(fabs(g_current), 2)
      + "A</span>";
  }
  else
  {
    p +=
      "<span class='b idle' id='db'>"
      "No Load</span>";
  }

  p += "</div></div>";

  // Data cards
  p +=
    "<div class='gr'>"

    "<div class='cd'>"
    "<div class='cl'>Pack Voltage</div>"
    "<div class='cv' id='volt'>"
    + String(g_voltage, 2) +
    "<span class='cu'> V</span>"
    "</div></div>"

    "<div class='cd'>"
    "<div class='cl'>Pack Current</div>"
    "<div class='cv' id='curr'>"
    + String(fabs(g_current), 2) +
    "<span class='cu'> A</span>"
    "</div>"
    "<span class='b " + cB + "' id='cs'>"
    + cL +
    "</span></div>"

    "<div class='cd'>"
    "<div class='cl'>Battery %</div>"
    "<div class='cv' id='soc'>"
    + String(g_soc) +
    "<span class='cu'> %</span>"
    "</div></div>"

    "<div class='cd'>"
    "<div class='cl'>Temperature</div>"
    "<div class='cv' id='temp'>"
    + String(g_temp, 1) +
    "<span class='cu'> &deg;C</span>"
    "</div></div>"

    "</div>";

  // ==========================================================
  // JAVASCRIPT
  // ==========================================================

  p +=
    "<script>"

    "const C={g:'#22c55e',a:'#f59e0b',r:'#ef4444'};"

    "function sc(s){"
    "return s>60?C.g:s>25?C.a:C.r;"
    "}"

    "async function upd(){"

    "try{"

    "const r=await fetch('/data');"
    "const d=await r.json();"

    "const c=sc(d.soc);"

    "document.getElementById('lw').style.height="
    "d.soc+'%';"

    "document.getElementById('lf').style.background=c;"

    "document.getElementById('wp').setAttribute('fill',c);"

    "document.getElementById('gp').textContent="
    "d.soc+'%';"

    "document.getElementById('gv').textContent="
    "d.voltage+' V';"

    "document.getElementById('volt').innerHTML="
    "d.voltage+'<span class=\"cu\"> V</span>';"

    "document.getElementById('curr').innerHTML="
    "d.current+'<span class=\"cu\"> A</span>';"

    "document.getElementById('soc').innerHTML="
    "d.soc+'<span class=\"cu\"> %</span>';"

    "document.getElementById('temp').innerHTML="
    "d.temp+'<span class=\"cu\"> &deg;C</span>';"

    "document.getElementById('ttf').innerHTML="
    "(d.charging?'&#9679; ':'&#9675; ')+d.ttf;"

    "document.getElementById('tte').innerHTML="
    "(d.discharging?'&#9679; ':'&#9675; ')+d.tte;"

    "const cb=document.getElementById('cb');"
    "const db=document.getElementById('db');"
    "const cs=document.getElementById('cs');"

    "if(d.charging){"

    "cb.textContent='CHG '+d.current+'A';"
    "cb.className='b chg';"

    "cs.textContent='Charging';"
    "cs.className='b chg';"

    "}"

    "else{"

    "cb.textContent='Not Charging';"
    "cb.className='b idle';"

    "cs.textContent=d.discharging?"
    "'Discharging':'Idle';"

    "cs.className=d.discharging?"
    "'b dsg':'b idle';"

    "}"

    "if(d.discharging){"

    "db.textContent='DSG '+d.current+'A';"
    "db.className='b dsg';"

    "}"

    "else{"

    "db.textContent='No Load';"
    "db.className='b idle';"

    "}"

    "}catch(e){"

    "console.log(e);"

    "}"

    "}"

    "setInterval(upd,3000);"

    "</script>";

  p +=
    "</div></body></html>";

  return p;
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);

  delay(500);

  // I2C
  Wire.begin(
    SDA_PIN,
    SCL_PIN
  );

  Wire.setClock(100000);

  // ----------------------------------------------------------
  // Initialize VU meter
  // ----------------------------------------------------------

  for (int i = 0; i < 16; i++)
  {
    vuBars[i] =
      random(4, 20);

    vuPeak[i] =
      vuBars[i];

    vuPeakTimer[i] = 0;
  }

  // ----------------------------------------------------------
  // OLED
  // ----------------------------------------------------------

  if (!display.begin(OLED_ADDR, true))
  {
    Serial.println("OLED not found");
  }
  else
  {
    // Startup animation
    for (int frame = 0; frame < 4; frame++)
    {
      display.clearDisplay();

      if (frame % 2 == 0)
      {
        display.drawRect(
          0,
          0,
          128,
          64,
          SH110X_WHITE
        );

        display.drawRect(
          2,
          2,
          124,
          60,
          SH110X_WHITE
        );
      }

      display.setTextColor(SH110X_WHITE);

      display.setTextSize(2);

      display.setCursor(2, 22);

      display.print("TECHHI1");

      int dotOffset =
        (frame * 10) % 20;

      for (
        int x = dotOffset;
        x < 130;
        x += 18
      )
      {
        display.fillCircle(
          x,
          56,
          3,
          SH110X_WHITE
        );
      }

      int dotOffset2 =
        10 - (frame * 10) % 20;

      for (
        int x = dotOffset2;
        x < 130;
        x += 18
      )
      {
        display.fillCircle(
          x,
          8,
          3,
          SH110X_WHITE
        );
      }

      display.display();

      delay(350);
    }

    display.clearDisplay();
    display.display();

    delay(100);

    // Final TECHHI1
    display.clearDisplay();

    display.drawRect(
      0,
      0,
      128,
      64,
      SH110X_WHITE
    );

    display.drawRect(
      3,
      3,
      122,
      58,
      SH110X_WHITE
    );

    display.fillCircle(
      6,
      6,
      3,
      SH110X_WHITE
    );

    display.fillCircle(
      121,
      6,
      3,
      SH110X_WHITE
    );

    display.fillCircle(
      6,
      57,
      3,
      SH110X_WHITE
    );

    display.fillCircle(
      121,
      57,
      3,
      SH110X_WHITE
    );

    display.setTextColor(SH110X_WHITE);

    display.setTextSize(2);

    display.setCursor(2, 22);

    display.print("TECHHI1");

    display.display();

    delay(2000);
  }

  // ----------------------------------------------------------
  // WiFi Access Point
  // ----------------------------------------------------------

  WiFi.softAP(
    ap_ssid,
    ap_password
  );

  Serial.println();
  Serial.print("AP IP: http://");
  Serial.println(
    WiFi.softAPIP()
  );

  // ----------------------------------------------------------
  // Web server
  // ----------------------------------------------------------

  server.on(
    "/",
    HTTP_GET,
    [](AsyncWebServerRequest *req)
    {
      req->send(
        200,
        "text/html",
        buildPage()
      );
    }
  );

  server.on(
    "/data",
    HTTP_GET,
    [](AsyncWebServerRequest *req)
    {
      req->send(
        200,
        "application/json",
        buildJSON()
      );
    }
  );

  server.begin();

  Serial.println(
    "Async server started."
  );

  Serial.println();
  Serial.println(
    "Waiting for BMS SMBus data..."
  );
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  unsigned long now = millis();

  // ----------------------------------------------------------
  // Read BMS every 3 seconds
  // ----------------------------------------------------------

  if (
    now - lastSMBusRead >=
    SMBUS_INTERVAL
  )
  {
    lastSMBusRead = now;

    readAllSMBus();
  }

  // ----------------------------------------------------------
  // Change OLED screen every 30 seconds
  // ----------------------------------------------------------

  if (
    now - lastOLEDSwitch >=
    SCREEN_SWITCH
  )
  {
    lastOLEDSwitch = now;

    showDataScreen =
      !showDataScreen;
  }

  // ----------------------------------------------------------
  // OLED refresh
  // ----------------------------------------------------------

  if (
    now - vuAnimTimer >=
    VU_ANIM_SPEED
  )
  {
    vuAnimTimer = now;

    if (!showDataScreen)
    {
      updateVUBars();
      drawVUScreen();
    }
    else
    {
      drawDataScreen();
    }
  }

  // ESPAsyncWebServer works automatically.
}
