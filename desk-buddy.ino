#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

// ---- Configuration ----
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// OpenWeatherMap OneCall 3.0 API
const char* OWM_API_KEY = "YOUR_API_KEY";
const float OWM_LAT     = xx.xx;    // Your latitude
const float OWM_LON     = xx.xx;    // Your longitude
const char* OWM_CITY    = "YOUR_CITY_NAME";  // Your city name

// UTC offset in seconds (+5:30 IST = 19800)
const long UTC_OFFSET_SEC = 19800;  // Your UTC offset in seconds

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

#define CALIBRATION_SAMPLES 200
#define AVG_SAMPLES 10
#define GRAVITY_THRESHOLD 6.0

// TTP223 touch sensors & buzzer
#define TOUCH_MODE D6   // cycle modes
#define TOUCH_FACE D5   // trigger face expressions
#define BUZZER_PIN D8

enum AppMode { MODE_CLOCK, MODE_WEATHER, MODE_TIMER, MODE_FACE, MODE_DINO, MODE_INVADERS, MODE_MAZE, MODE_FLAPPY, MODE_PONG };
AppMode appMode = MODE_CLOCK;
bool lastTouchState = false;
unsigned long lastTouchTime = 0;
#define TOUCH_DEBOUNCE_MS 300
bool timerAlarmDone = false;
unsigned long weatherEnteredTime = 0;
#define WEATHER_AUTO_RETURN_MS 15000

// Display MUST be declared before RoboEyes include (library references 'display' globally)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_MPU6050 mpu;

#include <FluxGarage_RoboEyes.h>
roboEyes face;

// Face mode state
bool lastFaceTouchState = false;
unsigned long lastFaceTouchTime = 0;
float prevGyroMag = 0;
unsigned long lastInteractionTime = 0;   // last touch or movement
unsigned long lastTiredCheck = 0;
#define TIRED_CHECK_INTERVAL 15000       // check every 15s if idle
#define IDLE_TIMEOUT 20000               // go tired after 20s no interaction

// Auto-switch to face from clock/weather
unsigned long clockEnteredTime = 0;
#define CLOCK_AUTO_FACE_MS 15000

// NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", UTC_OFFSET_SEC, 60000);

// Weather data
String weatherDesc = "--";
float weatherTemp = 0;
float weatherHumidity = 0;
float weatherFeelsLike = 0;
bool dataReady = false;  // true once we've successfully fetched time + weather

// Background WiFi state machine
enum SyncState { SYNC_IDLE, SYNC_CONNECTING, SYNC_FETCHING, SYNC_DISCONNECTING };
SyncState syncState = SYNC_IDLE;
unsigned long syncStateStart = 0;
unsigned long lastSyncTime = 0;
#define SYNC_INTERVAL 1800000UL      // 30 minutes
#define WIFI_CONNECT_TIMEOUT 10000   // 10 seconds max to connect
bool initialSyncDone = false;

// Day-of-week / month names
const char* daysOfWeek[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Calibration offsets
float accelOffsetX = 0, accelOffsetY = 0, accelOffsetZ = 0;
float gyroOffsetX = 0, gyroOffsetY = 0, gyroOffsetZ = 0;

// Moving average buffers
float accelXBuf[AVG_SAMPLES], accelYBuf[AVG_SAMPLES], accelZBuf[AVG_SAMPLES];
float gyroXBuf[AVG_SAMPLES], gyroYBuf[AVG_SAMPLES], gyroZBuf[AVG_SAMPLES];
int bufIndex = 0;
bool bufFilled = false;

// Orientation & timer state
enum Orientation { ORIENT_NONE, ORIENT_Y_UP, ORIENT_Y_NEG, ORIENT_X_POS, ORIENT_X_NEG, ORIENT_Z_POS, ORIENT_Z_NEG };
Orientation currentOrient = ORIENT_NONE;
unsigned long timerDurationMs = 0;
unsigned long timerStartMs = 0;
bool timerRunning = false;

// ---- Background WiFi ----

void fetchWeather() {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient http;
  String url = "https://api.openweathermap.org/data/3.0/onecall?lat=";
  url += String(OWM_LAT, 2);
  url += "&lon=";
  url += String(OWM_LON, 2);
  url += "&exclude=minutely,hourly,daily&units=metric&appid=";
  url += OWM_API_KEY;

  http.begin(*client, url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      weatherDesc = doc["current"]["weather"][0]["main"].as<String>();
      weatherTemp = doc["current"]["temp"];
      weatherHumidity = doc["current"]["humidity"];
      weatherFeelsLike = doc["current"]["feels_like"];
      Serial.println("Weather updated: " + weatherDesc);
    }
  } else {
    Serial.print("Weather HTTP error: ");
    Serial.println(httpCode);
  }
  http.end();
}

// Non-blocking WiFi state machine — call every loop iteration
void wifiTick() {
  switch (syncState) {

    case SYNC_IDLE:
      // Trigger sync on first boot or every 30 minutes
      if (!initialSyncDone || (millis() - lastSyncTime >= SYNC_INTERVAL)) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        syncState = SYNC_CONNECTING;
        syncStateStart = millis();
        Serial.println("WiFi: connecting...");
      }
      break;

    case SYNC_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi: connected");
        syncState = SYNC_FETCHING;
      } else if (millis() - syncStateStart > WIFI_CONNECT_TIMEOUT) {
        // Timed out — give up, try again next cycle
        Serial.println("WiFi: connect timeout");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        syncState = SYNC_IDLE;
        lastSyncTime = millis();  // don't retry immediately
        if (!initialSyncDone) initialSyncDone = true;
      }
      break;

    case SYNC_FETCHING:
      timeClient.begin();
      timeClient.forceUpdate();
      fetchWeather();
      dataReady = true;
      initialSyncDone = true;
      lastSyncTime = millis();
      Serial.println("WiFi: data fetched, disconnecting");
      syncState = SYNC_DISCONNECTING;
      break;

    case SYNC_DISCONNECTING:
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      syncState = SYNC_IDLE;
      Serial.println("WiFi: off");
      break;
  }
}

// ---- Date helper ----

void epochToDate(unsigned long epoch, int &year, int &month, int &day) {
  unsigned long days = epoch / 86400;
  int y = 1970;
  while (true) {
    unsigned long daysInYear = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    if (days < daysInYear) break;
    days -= daysInYear;
    y++;
  }
  year = y;
  int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) daysInMonth[1] = 29;
  int m = 0;
  while (m < 12 && days >= (unsigned long)daysInMonth[m]) {
    days -= daysInMonth[m];
    m++;
  }
  month = m;
  day = days + 1;
}

// ---- MPU6050 ----

void calibrate() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Calibrating...");
  display.println("Keep sensor flat");
  display.println("and still!");
  display.display();

  Serial.println("Calibrating - keep sensor flat and still...");

  float sumAx = 0, sumAy = 0, sumAz = 0;
  float sumGx = 0, sumGy = 0, sumGz = 0;

  for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    sumAx += a.acceleration.x;
    sumAy += a.acceleration.y;
    sumAz += a.acceleration.z;
    sumGx += g.gyro.x;
    sumGy += g.gyro.y;
    sumGz += g.gyro.z;
    delay(5);
  }

  accelOffsetX = sumAx / CALIBRATION_SAMPLES;
  accelOffsetY = sumAy / CALIBRATION_SAMPLES;
  accelOffsetZ = (sumAz / CALIBRATION_SAMPLES) - 9.81;
  gyroOffsetX = sumGx / CALIBRATION_SAMPLES;
  gyroOffsetY = sumGy / CALIBRATION_SAMPLES;
  gyroOffsetZ = sumGz / CALIBRATION_SAMPLES;

  Serial.print("Accel offsets: ");
  Serial.print(accelOffsetX); Serial.print(", ");
  Serial.print(accelOffsetY); Serial.print(", ");
  Serial.println(accelOffsetZ);
  Serial.print("Gyro offsets: ");
  Serial.print(gyroOffsetX); Serial.print(", ");
  Serial.print(gyroOffsetY); Serial.print(", ");
  Serial.println(gyroOffsetZ);

  for (int i = 0; i < AVG_SAMPLES; i++) {
    accelXBuf[i] = 0; accelYBuf[i] = 0; accelZBuf[i] = 0;
    gyroXBuf[i] = 0;  gyroYBuf[i] = 0;  gyroZBuf[i] = 0;
  }
  bufIndex = 0;
  bufFilled = false;

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Calibration done!");
  display.display();
  delay(500);
}

float average(float *buf, int count) {
  float sum = 0;
  for (int i = 0; i < count; i++) sum += buf[i];
  return sum / count;
}

Orientation detectOrientation(float ax, float ay, float az) {
  if (ay > GRAVITY_THRESHOLD)  return ORIENT_Y_UP;
  if (ay < -GRAVITY_THRESHOLD) return ORIENT_Y_NEG;
  if (ax > GRAVITY_THRESHOLD)  return ORIENT_X_POS;
  if (ax < -GRAVITY_THRESHOLD) return ORIENT_X_NEG;
  if (az > GRAVITY_THRESHOLD)  return ORIENT_Z_POS;
  if (az < -GRAVITY_THRESHOLD) return ORIENT_Z_NEG;
  return ORIENT_NONE;
}

uint8_t getDisplayRotation(Orientation orient) {
  switch (orient) {
    case ORIENT_Z_POS: return 0;
    case ORIENT_Z_NEG: return 2;
    case ORIENT_Y_UP:  return 1;
    case ORIENT_Y_NEG: return 3;
    default:           return 0;
  }
}

unsigned long getTimerDuration(Orientation orient) {
  switch (orient) {
    case ORIENT_Z_POS: return 30UL * 60 * 1000;
    case ORIENT_Z_NEG: return  5UL * 60 * 1000;
    case ORIENT_Y_UP:  return 45UL * 60 * 1000;
    case ORIENT_Y_NEG: return 60UL * 60 * 1000;
    default:           return 0;
  }
}

// ---- Buzzer ----

void beep(int onMs, int offMs) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(onMs);
  digitalWrite(BUZZER_PIN, LOW);
  delay(offMs);
}

void beepModeSwitch() {
  beep(50, 50);
}

void beepTimerStart() {
  beep(30, 30);
  beep(30, 0);
}

void beepTimerDone() {
  for (int i = 0; i < 5; i++) {
    beep(100, 100);
  }
}

// ---- Draw functions ----

void drawWelcome() {
  display.clearDisplay();
  display.setRotation(0);

  display.setTextSize(2);
  display.setCursor(2, 5);
  display.println("Desk Buddy");
//  display.setCursor(28, 25);
//  display.println("");

  display.setTextSize(1);
  display.setCursor(20, 50);
  display.println("Starting up...");

  display.display();
}

void drawIdle() {
  display.setRotation(0);
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 20);
  display.println("Timer");
  display.setCursor(10, 40);
  display.println("Stopped");
  display.display();
}

void drawTimer(unsigned long remainingMs, Orientation orient) {
  display.setRotation(getDisplayRotation(orient));
  display.clearDisplay();

  unsigned long totalSec = remainingMs / 1000;
  int mins = totalSec / 60;
  int secs = totalSec % 60;

  display.setTextSize(1);
  display.setCursor(0, 0);
  switch (orient) {
    case ORIENT_Z_POS: display.println("Timer: 30 min"); break;
    case ORIENT_Z_NEG: display.println("Timer: 5 min");  break;
    case ORIENT_Y_UP:  display.println("Timer: 45 min"); break;
    case ORIENT_Y_NEG: display.println("Timer: 60 min"); break;
    default: break;
  }

  display.setTextSize(3);
  display.setCursor(10, 20);
  if (mins < 10) display.print("0");
  display.print(mins);
  display.print(":");
  if (secs < 10) display.print("0");
  display.print(secs);

  display.display();
}

void drawTimerDone(Orientation orient) {
  display.setRotation(getDisplayRotation(orient));
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 10);
  display.println("TIME'S");
  display.setCursor(10, 35);
  display.println("UP!");
  display.display();
}

void drawClock() {
  display.setRotation(0);
  display.clearDisplay();

  if (!dataReady) {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Clock");
    display.drawLine(0, 10, 127, 10, WHITE);
    display.setTextSize(3);
    display.setCursor(4, 22);
//    display.print("00:00:00");
    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print("Waiting for sync...");
    display.display();
    return;
  }

  unsigned long epoch = timeClient.getEpochTime();
  int hrs = timeClient.getHours();
  int mins = timeClient.getMinutes();
  int secs = timeClient.getSeconds();
  int dow = timeClient.getDay();

  int year, month, day;
  epochToDate(epoch, year, month, day);

  // Date line
  display.setTextSize(1);
  display.setCursor(30, 0);
  display.print(daysOfWeek[dow]);
  display.print(", ");
  display.print(day);
  display.print(" ");
  display.print(months[month]);

  // Time
  display.setTextSize(3);
  display.setCursor(20, 22);
  if (hrs < 10) display.print("0");
  display.print(hrs);
  display.print(":");
  if (mins < 10) display.print("0");
  display.print(mins);

  // City
  display.setTextSize(1);
  display.setCursor(38, 56);
  display.print(OWM_CITY);

  display.display();
}

void drawWeather() {
  display.setRotation(0);
  display.clearDisplay();

  if (!dataReady) {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Weather - ");
    display.println(OWM_CITY);
    display.drawLine(0, 10, 127, 10, WHITE);
    display.setTextSize(2);
    display.setCursor(0, 20);
    display.println("No data");
    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print("Waiting for sync...");
    display.display();
    return;
  }

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Weather - ");
  display.println(OWM_CITY);

  display.drawLine(0, 10, 127, 10, WHITE);

  display.setTextSize(2);
  display.setCursor(0, 14);
  display.println(weatherDesc);

  display.setTextSize(1);
  display.setCursor(0, 36);
  display.print("Temp: ");
  display.print(weatherTemp, 1);
  display.print(" C");

  display.setCursor(0, 46);
  display.print("Feels: ");
  display.print(weatherFeelsLike, 1);
  display.print(" C");

  display.setCursor(0, 56);
  display.print("Humidity: ");
  display.print((int)weatherHumidity);
  display.print("%");

  display.display();
}

// ---- Face mode ----

void beepHappy() {
  beep(30, 30);
  beep(50, 30);
  beep(30, 0);
}

void beepAngry() {
  beep(200, 50);
  beep(200, 0);
}

void beepConfused() {
  beep(40, 60);
  beep(40, 60);
  beep(40, 0);
}

void beepLaugh() {
  for (int i = 0; i < 4; i++) {
    beep(20, 40);
  }
}

void beepSurprise() {
  beep(150, 0);
}

// Enter face mode happy
void faceEnterHappy() {
  display.setRotation(0);
  if (random(2) == 0) {
    face.setMood(HAPPY);
    face.anim_laugh();
    beepLaugh();
  } else {
    face.setMood(HAPPY);
    face.setCuriosity(ON);
    beepHappy();
  }
  lastInteractionTime = millis();
}

// Touch triggers angry or confused
void faceTouchReact() {
  if (random(2) == 0) {
    face.setMood(ANGRY);
    beepAngry();
  } else {
    face.setMood(0);
    face.anim_confused();
    beepConfused();
  }
  lastInteractionTime = millis();
}

// Go tired when idle
void faceGoTired() {
  face.setMood(TIRED);
  face.blink();
  beep(100, 0);
}

// Map IMU tilt to eye gaze direction
void updateEyesFromIMU() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  // +X = out of screen, +Y = left of OLED, +Z = down (right-hand rule)
  float tiltLR = -(a.acceleration.y - accelOffsetY);  // positive = right
  float tiltUD = -(a.acceleration.z - accelOffsetZ);  // positive = up (tilt top down)

  // Gaze direction from tilt
  if (tiltLR > 3.0 && tiltUD > 3.0)       face.setPosition(NE);
  else if (tiltLR > 3.0 && tiltUD < -3.0) face.setPosition(SE);
  else if (tiltLR < -3.0 && tiltUD > 3.0) face.setPosition(NW);
  else if (tiltLR < -3.0 && tiltUD < -3.0) face.setPosition(SW);
  else if (tiltLR > 3.0)                   face.setPosition(E);
  else if (tiltLR < -3.0)                  face.setPosition(W);
  else if (tiltUD > 3.0)                   face.setPosition(N);
  else if (tiltUD < -3.0)                  face.setPosition(S);
  else                                      face.setPosition(0);

  // Track movement as interaction
  float gyroMag = abs(g.gyro.x) + abs(g.gyro.y) + abs(g.gyro.z);
  float accelMag = abs(tiltLR) + abs(tiltUD);
  if (gyroMag > 3.0 || accelMag > 4.0) {
    lastInteractionTime = millis();
  }

  // Detect shake — trigger confused
  if (gyroMag > 8.0 && prevGyroMag <= 8.0) {
    face.anim_confused();
    face.setMood(ANGRY);
    beepSurprise();
    lastInteractionTime = millis();
  } else if (gyroMag < 1.0 && prevGyroMag >= 8.0) {
    face.setMood(HAPPY);
  }
  prevGyroMag = gyroMag;
}

// ---- Dino Game ----

#define GROUND_Y 54
#define DINO_WIDTH 10
#define DINO_HEIGHT 12
#define DINO_X 15

// Dino state
float dinoY = GROUND_Y - DINO_HEIGHT;
float dinoVelY = 0;
bool dinoJumping = false;
#define DINO_GRAVITY 1.0
#define DINO_JUMP_VEL -8.0

// Obstacles
#define MAX_OBSTACLES 3
float obstX[MAX_OBSTACLES];
int obstW[MAX_OBSTACLES];
int obstH[MAX_OBSTACLES];

// Game state
unsigned long dinoScore = 0;
unsigned long dinoHighScore = 0;
bool dinoGameOver = false;
unsigned long dinoLastFrame = 0;
float dinoSpeed = 3.0;
bool lastDinoTouchState = false;
unsigned long lastDinoTouchTime = 0;

// Ground scroll
int groundOffset = 0;

void dinoReset() {
  dinoY = GROUND_Y - DINO_HEIGHT;
  dinoVelY = 0;
  dinoJumping = false;
  dinoScore = 0;
  dinoSpeed = 3.0;
  dinoGameOver = false;

  // Spread obstacles across the screen
  for (int i = 0; i < MAX_OBSTACLES; i++) {
    obstX[i] = SCREEN_WIDTH + i * (60 + random(40));
    obstW[i] = 4 + random(6);
    obstH[i] = 6 + random(11);
  }
}

void dinoDrawDino(int x, int y) {
  // Body
  display.fillRect(x + 2, y, 6, 8, WHITE);
  // Head
  display.fillRect(x + 4, y - 4, 6, 5, WHITE);
  // Eye
  display.drawPixel(x + 8, y - 3, BLACK);
  // Legs (alternate for running animation)
  if ((millis() / 150) % 2 == 0) {
    display.fillRect(x + 2, y + 8, 2, 4, WHITE);
    display.fillRect(x + 6, y + 8, 2, 4, WHITE);
  } else {
    display.fillRect(x + 3, y + 8, 2, 4, WHITE);
    display.fillRect(x + 5, y + 8, 2, 4, WHITE);
  }
  // Tail
  display.drawLine(x, y + 2, x - 2, y, WHITE);
}

void dinoDrawCactus(int x, int w, int h) {
  int cy = GROUND_Y - h;
  display.fillRect(x, cy, w, h, WHITE);
  // Arms
  if (w > 4) {
    display.fillRect(x - 2, cy + 3, 2, 5, WHITE);
    display.fillRect(x + w, cy + 5, 2, 4, WHITE);
  }
}

void dinoUpdate() {
  unsigned long now = millis();
  if (now - dinoLastFrame < 30) return;  // ~33 fps
  dinoLastFrame = now;

  if (dinoGameOver) return;

  // Jump physics
  if (dinoJumping) {
    dinoVelY += DINO_GRAVITY;
    dinoY += dinoVelY;
    if (dinoY >= GROUND_Y - DINO_HEIGHT) {
      dinoY = GROUND_Y - DINO_HEIGHT;
      dinoVelY = 0;
      dinoJumping = false;
    }
  }

  // Move obstacles
  for (int i = 0; i < MAX_OBSTACLES; i++) {
    obstX[i] -= dinoSpeed;

    // Recycle obstacle when off-screen
    if (obstX[i] + obstW[i] < 0) {
      // Place after the furthest obstacle
      float maxX = 0;
      for (int j = 0; j < MAX_OBSTACLES; j++) {
        if (obstX[j] > maxX) maxX = obstX[j];
      }
      obstX[i] = maxX + 50 + random(60);
      obstW[i] = 4 + random(6);
      obstH[i] = 6 + random(10);
      dinoScore++;
    }

    // Collision detection
    int obstTop = GROUND_Y - obstH[i];
    if (DINO_X + DINO_WIDTH > obstX[i] &&
        DINO_X < obstX[i] + obstW[i] &&
        (int)dinoY + DINO_HEIGHT > obstTop) {
      dinoGameOver = true;
      if (dinoScore > dinoHighScore) dinoHighScore = dinoScore;
      beep(200, 100);
      beep(300, 0);
    }
  }

  // Speed up gradually
  dinoSpeed = 3.0 + dinoScore * 0.15;
  if (dinoSpeed > 8.0) dinoSpeed = 8.0;

  // Ground scroll
  groundOffset = (groundOffset + (int)dinoSpeed) % 4;
}

void dinoDraw() {
  display.setRotation(0);
  display.clearDisplay();

  if (dinoGameOver) {
    display.setTextSize(2);
    display.setCursor(10, 5);
    display.println("GAME OVER");
    display.setTextSize(1);
    display.setCursor(10, 30);
    display.print("Score: ");
    display.println(dinoScore);
    display.setCursor(10, 42);
    display.print("High:  ");
    display.println(dinoHighScore);
    display.setCursor(10, 56);
    display.print("Touch to restart");
    display.display();
    return;
  }

  // Ground line with dashes
  display.drawLine(0, GROUND_Y, SCREEN_WIDTH - 1, GROUND_Y, WHITE);
  for (int x = -groundOffset; x < SCREEN_WIDTH; x += 4) {
    display.drawPixel(x, GROUND_Y + 2, WHITE);
  }

  // Draw dino
  dinoDrawDino(DINO_X, (int)dinoY);

  // Draw obstacles
  for (int i = 0; i < MAX_OBSTACLES; i++) {
    if (obstX[i] < SCREEN_WIDTH && obstX[i] + obstW[i] > 0) {
      dinoDrawCactus((int)obstX[i], obstW[i], obstH[i]);
    }
  }

  // Score
  display.setTextSize(1);
  display.setCursor(80, 0);
  display.print("HI ");
  display.print(dinoHighScore);
  display.setCursor(80, 10);
  display.print("   ");
  display.print(dinoScore);

  display.display();
}

// ---- Space Invaders ----

#define SI_SHIP_W 9
#define SI_SHIP_H 5
#define SI_SHIP_Y 58

#define SI_ROWS 3
#define SI_COLS 8
#define SI_ALIEN_W 7
#define SI_ALIEN_H 5
#define SI_ALIEN_SPACING_X 14
#define SI_ALIEN_SPACING_Y 10
#define SI_ALIEN_TOP 4

#define SI_MAX_BULLETS 3
#define SI_MAX_ENEMY_BULLETS 4
#define SI_BULLET_SPEED 4
#define SI_ENEMY_BULLET_SPEED 2

// Ship
float siShipX = 60;

// Aliens
bool siAliens[SI_ROWS][SI_COLS];
float siAlienOffsetX = 0;
float siAlienOffsetY = 0;
float siAlienDirX = 1.0;
float siAlienSpeed = 0.5;
int siAliveCount = 0;

// Player bullets
float siBulletX[SI_MAX_BULLETS];
float siBulletY[SI_MAX_BULLETS];
bool siBulletActive[SI_MAX_BULLETS];

// Enemy bullets
float siEBulletX[SI_MAX_ENEMY_BULLETS];
float siEBulletY[SI_MAX_ENEMY_BULLETS];
bool siEBulletActive[SI_MAX_ENEMY_BULLETS];

unsigned long siScore = 0;
unsigned long siHighScore = 0;
bool siGameOver = false;
bool siWin = false;
unsigned long siLastFrame = 0;
unsigned long siLastEnemyShot = 0;
bool lastSiTouchState = false;

void siReset() {
  siShipX = 60;
  siAlienOffsetX = 0;
  siAlienOffsetY = 0;
  siAlienDirX = 1.0;
  siAlienSpeed = 0.5;
  siScore = 0;
  siGameOver = false;
  siWin = false;
  siAliveCount = SI_ROWS * SI_COLS;

  for (int r = 0; r < SI_ROWS; r++)
    for (int c = 0; c < SI_COLS; c++)
      siAliens[r][c] = true;

  for (int i = 0; i < SI_MAX_BULLETS; i++) siBulletActive[i] = false;
  for (int i = 0; i < SI_MAX_ENEMY_BULLETS; i++) siEBulletActive[i] = false;
}

void siShoot() {
  for (int i = 0; i < SI_MAX_BULLETS; i++) {
    if (!siBulletActive[i]) {
      siBulletX[i] = siShipX + SI_SHIP_W / 2;
      siBulletY[i] = SI_SHIP_Y - 2;
      siBulletActive[i] = true;
      beep(10, 0);
      return;
    }
  }
}

void siEnemyShoot() {
  // Pick a random alive alien in the bottom-most row for each column
  int shootCol = random(SI_COLS);
  int shootRow = -1;
  for (int r = SI_ROWS - 1; r >= 0; r--) {
    if (siAliens[r][shootCol]) { shootRow = r; break; }
  }
  if (shootRow < 0) return;

  for (int i = 0; i < SI_MAX_ENEMY_BULLETS; i++) {
    if (!siEBulletActive[i]) {
      siEBulletX[i] = shootCol * SI_ALIEN_SPACING_X + siAlienOffsetX + SI_ALIEN_W / 2 + 8;
      siEBulletY[i] = SI_ALIEN_TOP + shootRow * SI_ALIEN_SPACING_Y + siAlienOffsetY + SI_ALIEN_H + 1;
      siEBulletActive[i] = true;
      return;
    }
  }
}

float siGetTilt() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  // +Y = left, so negate for right-positive
  return -(a.acceleration.y - accelOffsetY);
}

void siUpdate() {
  unsigned long now = millis();
  if (now - siLastFrame < 33) return;  // ~30 fps
  siLastFrame = now;

  if (siGameOver || siWin) return;

  // Ship movement from tilt
  float tilt = siGetTilt();
  if (tilt > 2.0) siShipX += 3.0;
  else if (tilt < -2.0) siShipX -= 3.0;
  if (siShipX < 0) siShipX = 0;
  if (siShipX > SCREEN_WIDTH - SI_SHIP_W) siShipX = SCREEN_WIDTH - SI_SHIP_W;

  // Move aliens
  siAlienOffsetX += siAlienDirX * siAlienSpeed;

  // Check if aliens hit screen edge
  bool hitEdge = false;
  for (int r = 0; r < SI_ROWS; r++) {
    for (int c = 0; c < SI_COLS; c++) {
      if (!siAliens[r][c]) continue;
      float ax = c * SI_ALIEN_SPACING_X + siAlienOffsetX + 8;
      if (ax < 0 || ax + SI_ALIEN_W > SCREEN_WIDTH) hitEdge = true;
    }
  }
  if (hitEdge) {
    siAlienDirX = -siAlienDirX;
    siAlienOffsetY += 3;
  }

  // Check if aliens reached ship level
  for (int r = 0; r < SI_ROWS; r++) {
    for (int c = 0; c < SI_COLS; c++) {
      if (!siAliens[r][c]) continue;
      float ay = SI_ALIEN_TOP + r * SI_ALIEN_SPACING_Y + siAlienOffsetY;
      if (ay + SI_ALIEN_H >= SI_SHIP_Y) {
        siGameOver = true;
        if (siScore > siHighScore) siHighScore = siScore;
        beep(200, 100);
        beep(300, 0);
        return;
      }
    }
  }

  // Move player bullets
  for (int i = 0; i < SI_MAX_BULLETS; i++) {
    if (!siBulletActive[i]) continue;
    siBulletY[i] -= SI_BULLET_SPEED;
    if (siBulletY[i] < 0) { siBulletActive[i] = false; continue; }

    // Check hit on aliens
    for (int r = 0; r < SI_ROWS; r++) {
      for (int c = 0; c < SI_COLS; c++) {
        if (!siAliens[r][c]) continue;
        float ax = c * SI_ALIEN_SPACING_X + siAlienOffsetX + 8;
        float ay = SI_ALIEN_TOP + r * SI_ALIEN_SPACING_Y + siAlienOffsetY;
        if (siBulletX[i] >= ax && siBulletX[i] <= ax + SI_ALIEN_W &&
            siBulletY[i] >= ay && siBulletY[i] <= ay + SI_ALIEN_H) {
          siAliens[r][c] = false;
          siBulletActive[i] = false;
          siAliveCount--;
          siScore += 10;
          beep(15, 0);
          // Speed up as aliens die
          siAlienSpeed = 0.5 + (SI_ROWS * SI_COLS - siAliveCount) * 0.08;
          if (siAliveCount == 0) {
            siWin = true;
            if (siScore > siHighScore) siHighScore = siScore;
            beep(50, 50);
            beep(50, 50);
            beep(100, 0);
          }
        }
      }
    }
  }

  // Move enemy bullets
  for (int i = 0; i < SI_MAX_ENEMY_BULLETS; i++) {
    if (!siEBulletActive[i]) continue;
    siEBulletY[i] += SI_ENEMY_BULLET_SPEED;
    if (siEBulletY[i] > SCREEN_HEIGHT) { siEBulletActive[i] = false; continue; }

    // Hit ship?
    if (siEBulletX[i] >= siShipX && siEBulletX[i] <= siShipX + SI_SHIP_W &&
        siEBulletY[i] >= SI_SHIP_Y && siEBulletY[i] <= SI_SHIP_Y + SI_SHIP_H) {
      siGameOver = true;
      if (siScore > siHighScore) siHighScore = siScore;
      beep(200, 100);
      beep(300, 0);
      return;
    }
  }

  // Enemy shooting
  if (now - siLastEnemyShot > 1500) {
    siLastEnemyShot = now;
    siEnemyShoot();
  }
}

void siDrawShip(int x, int y) {
  // Triangle ship
  display.fillTriangle(x, y + SI_SHIP_H, x + SI_SHIP_W, y + SI_SHIP_H,
                        x + SI_SHIP_W / 2, y, WHITE);
}

void siDrawAlien(int x, int y, int row) {
  // Different shapes per row
  if (row == 0) {
    // Top row: small
    display.fillRect(x + 1, y, 5, 3, WHITE);
    display.drawPixel(x, y + 1, WHITE);
    display.drawPixel(x + 6, y + 1, WHITE);
    display.drawPixel(x + 1, y + 3, WHITE);
    display.drawPixel(x + 5, y + 3, WHITE);
  } else if (row == 1) {
    // Middle row
    display.fillRect(x, y, 7, 4, WHITE);
    display.drawPixel(x + 1, y + 4, WHITE);
    display.drawPixel(x + 5, y + 4, WHITE);
    display.drawPixel(x + 3, y - 1, WHITE);
  } else {
    // Bottom row: wide
    display.fillRect(x, y + 1, 7, 3, WHITE);
    display.drawPixel(x + 1, y, WHITE);
    display.drawPixel(x + 5, y, WHITE);
    display.drawPixel(x, y + 4, WHITE);
    display.drawPixel(x + 6, y + 4, WHITE);
  }
}

void siDraw() {
  display.setRotation(0);
  display.clearDisplay();

  if (siGameOver) {
    display.setTextSize(2);
    display.setCursor(10, 5);
    display.println("GAME OVER");
    display.setTextSize(1);
    display.setCursor(10, 30);
    display.print("Score: ");
    display.println(siScore);
    display.setCursor(10, 42);
    display.print("High:  ");
    display.println(siHighScore);
    display.setCursor(10, 56);
    display.print("Touch to restart");
    display.display();
    return;
  }

  if (siWin) {
    display.setTextSize(2);
    display.setCursor(15, 5);
    display.println("YOU WIN!");
    display.setTextSize(1);
    display.setCursor(10, 30);
    display.print("Score: ");
    display.println(siScore);
    display.setCursor(10, 42);
    display.print("High:  ");
    display.println(siHighScore);
    display.setCursor(10, 56);
    display.print("Touch to play again");
    display.display();
    return;
  }

  // Draw aliens
  for (int r = 0; r < SI_ROWS; r++) {
    for (int c = 0; c < SI_COLS; c++) {
      if (!siAliens[r][c]) continue;
      int ax = (int)(c * SI_ALIEN_SPACING_X + siAlienOffsetX) + 8;
      int ay = SI_ALIEN_TOP + r * SI_ALIEN_SPACING_Y + (int)siAlienOffsetY;
      siDrawAlien(ax, ay, r);
    }
  }

  // Draw ship
  siDrawShip((int)siShipX, SI_SHIP_Y);

  // Draw player bullets
  for (int i = 0; i < SI_MAX_BULLETS; i++) {
    if (!siBulletActive[i]) continue;
    display.drawLine((int)siBulletX[i], (int)siBulletY[i],
                      (int)siBulletX[i], (int)siBulletY[i] - 3, WHITE);
  }

  // Draw enemy bullets
  for (int i = 0; i < SI_MAX_ENEMY_BULLETS; i++) {
    if (!siEBulletActive[i]) continue;
    display.drawLine((int)siEBulletX[i], (int)siEBulletY[i],
                      (int)siEBulletX[i], (int)siEBulletY[i] + 2, WHITE);
  }

  // Score
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(siScore);

  display.display();
}

// ---- Maze Game ----

// Maze is a grid of cells, each cell is 8x8 pixels
// 128/8 = 16 cols, 64/8 = 8 rows
#define MZ_COLS 16
#define MZ_ROWS 8
#define MZ_CELL 8

// Wall bits per cell: bit0=top, bit1=right, bit2=bottom, bit3=left
#define MZ_TOP    0x01
#define MZ_RIGHT  0x02
#define MZ_BOTTOM 0x04
#define MZ_LEFT   0x08

uint8_t maze[MZ_ROWS][MZ_COLS];
bool mzVisited[MZ_ROWS][MZ_COLS];

// Ball position (in pixels, float for smooth movement)
float mzBallX, mzBallY;
float mzBallVX = 0, mzBallVY = 0;
#define MZ_BALL_R 2
#define MZ_BALL_ACCEL 0.3
#define MZ_BALL_FRICTION 0.85
#define MZ_BALL_MAX_SPEED 2.5

// Goal position (cell coords)
int mzGoalR, mzGoalC;
bool mzWin = false;
unsigned long mzWinTime = 0;
unsigned long mzStartTime = 0;
unsigned long mzLastFrame = 0;
bool lastMzTouchState = false;
int mzLevel = 1;

// Stack for maze generation (iterative DFS)
int mzStackR[MZ_ROWS * MZ_COLS];
int mzStackC[MZ_ROWS * MZ_COLS];
int mzStackTop = 0;

void mzGenerate() {
  // Init all walls
  for (int r = 0; r < MZ_ROWS; r++)
    for (int c = 0; c < MZ_COLS; c++) {
      maze[r][c] = MZ_TOP | MZ_RIGHT | MZ_BOTTOM | MZ_LEFT;
      mzVisited[r][c] = false;
    }

  // Iterative DFS from (0,0)
  mzStackTop = 0;
  mzStackR[mzStackTop] = 0;
  mzStackC[mzStackTop] = 0;
  mzVisited[0][0] = true;
  mzStackTop++;

  while (mzStackTop > 0) {
    int cr = mzStackR[mzStackTop - 1];
    int cc = mzStackC[mzStackTop - 1];

    // Find unvisited neighbors
    int neighbors[4][2];
    int nCount = 0;
    if (cr > 0 && !mzVisited[cr - 1][cc])           { neighbors[nCount][0] = cr - 1; neighbors[nCount][1] = cc; nCount++; }
    if (cr < MZ_ROWS - 1 && !mzVisited[cr + 1][cc]) { neighbors[nCount][0] = cr + 1; neighbors[nCount][1] = cc; nCount++; }
    if (cc > 0 && !mzVisited[cr][cc - 1])            { neighbors[nCount][0] = cr; neighbors[nCount][1] = cc - 1; nCount++; }
    if (cc < MZ_COLS - 1 && !mzVisited[cr][cc + 1])  { neighbors[nCount][0] = cr; neighbors[nCount][1] = cc + 1; nCount++; }

    if (nCount > 0) {
      int pick = random(nCount);
      int nr = neighbors[pick][0];
      int nc = neighbors[pick][1];

      // Remove walls between current and neighbor
      if (nr < cr) { maze[cr][cc] &= ~MZ_TOP;    maze[nr][nc] &= ~MZ_BOTTOM; }
      if (nr > cr) { maze[cr][cc] &= ~MZ_BOTTOM; maze[nr][nc] &= ~MZ_TOP; }
      if (nc < cc) { maze[cr][cc] &= ~MZ_LEFT;   maze[nr][nc] &= ~MZ_RIGHT; }
      if (nc > cc) { maze[cr][cc] &= ~MZ_RIGHT;  maze[nr][nc] &= ~MZ_LEFT; }

      mzVisited[nr][nc] = true;
      mzStackR[mzStackTop] = nr;
      mzStackC[mzStackTop] = nc;
      mzStackTop++;
    } else {
      mzStackTop--;
    }
  }
}

void mzReset() {
  mzGenerate();
  mzBallX = MZ_CELL / 2;
  mzBallY = MZ_CELL / 2;
  mzBallVX = 0;
  mzBallVY = 0;
  mzGoalR = MZ_ROWS - 1;
  mzGoalC = MZ_COLS - 1;
  mzWin = false;
  mzStartTime = millis();
}

bool mzWallCollision(float nx, float ny) {
  // Check if ball at (nx, ny) hits any wall
  int cellC = (int)(nx / MZ_CELL);
  int cellR = (int)(ny / MZ_CELL);
  if (cellC < 0 || cellC >= MZ_COLS || cellR < 0 || cellR >= MZ_ROWS) return true;

  float localX = nx - cellC * MZ_CELL;
  float localY = ny - cellR * MZ_CELL;
  uint8_t walls = maze[cellR][cellC];

  if ((walls & MZ_TOP)    && localY < MZ_BALL_R + 1) return true;
  if ((walls & MZ_BOTTOM) && localY > MZ_CELL - MZ_BALL_R - 1) return true;
  if ((walls & MZ_LEFT)   && localX < MZ_BALL_R + 1) return true;
  if ((walls & MZ_RIGHT)  && localX > MZ_CELL - MZ_BALL_R - 1) return true;

  return false;
}

void mzUpdate() {
  unsigned long now = millis();
  if (now - mzLastFrame < 33) return;
  mzLastFrame = now;

  if (mzWin) return;

  // Get tilt (+Y = left, +Z = down)
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float tiltX = -(a.acceleration.y - accelOffsetY);  // negate: positive = right
  float tiltY = -(a.acceleration.z - accelOffsetZ);   // negate: positive = up

  // Apply tilt as acceleration (tiltX moves ball right, -tiltY moves ball down)
  mzBallVX += tiltX * MZ_BALL_ACCEL * 0.1;
  mzBallVY += -tiltY * MZ_BALL_ACCEL * 0.1;

  // Friction
  mzBallVX *= MZ_BALL_FRICTION;
  mzBallVY *= MZ_BALL_FRICTION;

  // Clamp speed
  if (mzBallVX > MZ_BALL_MAX_SPEED) mzBallVX = MZ_BALL_MAX_SPEED;
  if (mzBallVX < -MZ_BALL_MAX_SPEED) mzBallVX = -MZ_BALL_MAX_SPEED;
  if (mzBallVY > MZ_BALL_MAX_SPEED) mzBallVY = MZ_BALL_MAX_SPEED;
  if (mzBallVY < -MZ_BALL_MAX_SPEED) mzBallVY = -MZ_BALL_MAX_SPEED;

  // Move X
  float nx = mzBallX + mzBallVX;
  if (!mzWallCollision(nx, mzBallY)) {
    mzBallX = nx;
  } else {
    mzBallVX = 0;
  }

  // Move Y
  float ny = mzBallY + mzBallVY;
  if (!mzWallCollision(mzBallX, ny)) {
    mzBallY = ny;
  } else {
    mzBallVY = 0;
  }

  // Bounds
  if (mzBallX < MZ_BALL_R) mzBallX = MZ_BALL_R;
  if (mzBallX > MZ_COLS * MZ_CELL - MZ_BALL_R) mzBallX = MZ_COLS * MZ_CELL - MZ_BALL_R;
  if (mzBallY < MZ_BALL_R) mzBallY = MZ_BALL_R;
  if (mzBallY > MZ_ROWS * MZ_CELL - MZ_BALL_R) mzBallY = MZ_ROWS * MZ_CELL - MZ_BALL_R;

  // Check goal
  int ballCellC = (int)(mzBallX / MZ_CELL);
  int ballCellR = (int)(mzBallY / MZ_CELL);
  if (ballCellR == mzGoalR && ballCellC == mzGoalC) {
    mzWin = true;
    mzWinTime = millis() - mzStartTime;
    mzLevel++;
    beep(50, 50);
    beep(50, 50);
    beep(100, 0);
  }
}

void mzDraw() {
  display.setRotation(0);
  display.clearDisplay();

  if (mzWin) {
    display.setTextSize(2);
    display.setCursor(10, 2);
    display.println("SOLVED!");
    display.setTextSize(1);
    display.setCursor(10, 25);
    display.print("Time: ");
    display.print(mzWinTime / 1000);
    display.print(".");
    display.print((mzWinTime % 1000) / 100);
    display.println("s");
    display.setCursor(10, 38);
    display.print("Level: ");
    display.println(mzLevel - 1);
    display.setCursor(10, 52);
    display.print("Touch for next maze");
    display.display();
    return;
  }

  // Draw walls
  for (int r = 0; r < MZ_ROWS; r++) {
    for (int c = 0; c < MZ_COLS; c++) {
      int x = c * MZ_CELL;
      int y = r * MZ_CELL;
      uint8_t w = maze[r][c];
      if (w & MZ_TOP)    display.drawLine(x, y, x + MZ_CELL - 1, y, WHITE);
      if (w & MZ_RIGHT)  display.drawLine(x + MZ_CELL - 1, y, x + MZ_CELL - 1, y + MZ_CELL - 1, WHITE);
      if (w & MZ_BOTTOM) display.drawLine(x, y + MZ_CELL - 1, x + MZ_CELL - 1, y + MZ_CELL - 1, WHITE);
      if (w & MZ_LEFT)   display.drawLine(x, y, x, y + MZ_CELL - 1, WHITE);
    }
  }

  // Draw goal (hollow square)
  int gx = mzGoalC * MZ_CELL + 2;
  int gy = mzGoalR * MZ_CELL + 2;
  display.drawRect(gx, gy, MZ_CELL - 4, MZ_CELL - 4, WHITE);

  // Draw ball (filled circle)
  display.fillCircle((int)mzBallX, (int)mzBallY, MZ_BALL_R, WHITE);

  display.display();
}

// ---- Flappy Bird ----

#define FB_BIRD_X 20
#define FB_BIRD_W 8
#define FB_BIRD_H 6
#define FB_GRAVITY 0.6
#define FB_FLAP_VEL -4.0
#define FB_PIPE_W 10
#define FB_GAP_H 22
#define FB_MAX_PIPES 3
#define FB_PIPE_SPEED 2.0

float fbBirdY = 30;
float fbBirdVY = 0;

float fbPipeX[FB_MAX_PIPES];
int fbPipeGapY[FB_MAX_PIPES];  // top of gap
bool fbPipeScored[FB_MAX_PIPES];

unsigned long fbScore = 0;
unsigned long fbHighScore = 0;
bool fbGameOver = false;
bool fbStarted = false;
unsigned long fbLastFrame = 0;
bool lastFbTouchState = false;

void fbReset() {
  fbBirdY = 30;
  fbBirdVY = 0;
  fbScore = 0;
  fbGameOver = false;
  fbStarted = false;

  for (int i = 0; i < FB_MAX_PIPES; i++) {
    fbPipeX[i] = SCREEN_WIDTH + i * 50;
    fbPipeGapY[i] = 10 + random(SCREEN_HEIGHT - FB_GAP_H - 20);
    fbPipeScored[i] = false;
  }
}

void fbUpdate() {
  unsigned long now = millis();
  if (now - fbLastFrame < 30) return;
  fbLastFrame = now;

  if (fbGameOver || !fbStarted) return;

  // Bird physics
  fbBirdVY += FB_GRAVITY;
  fbBirdY += fbBirdVY;

  // Hit ceiling or floor
  if (fbBirdY < 0) {
    fbBirdY = 0;
    fbBirdVY = 0;
  }
  if (fbBirdY + FB_BIRD_H > SCREEN_HEIGHT) {
    fbGameOver = true;
    if (fbScore > fbHighScore) fbHighScore = fbScore;
    beep(200, 100);
    beep(300, 0);
    return;
  }

  // Move pipes
  for (int i = 0; i < FB_MAX_PIPES; i++) {
    fbPipeX[i] -= FB_PIPE_SPEED;

    // Recycle pipe
    if (fbPipeX[i] + FB_PIPE_W < 0) {
      float maxX = 0;
      for (int j = 0; j < FB_MAX_PIPES; j++) {
        if (fbPipeX[j] > maxX) maxX = fbPipeX[j];
      }
      fbPipeX[i] = maxX + 45 + random(20);
      fbPipeGapY[i] = 10 + random(SCREEN_HEIGHT - FB_GAP_H - 20);
      fbPipeScored[i] = false;
    }

    // Score when bird passes pipe
    if (!fbPipeScored[i] && fbPipeX[i] + FB_PIPE_W < FB_BIRD_X) {
      fbScore++;
      fbPipeScored[i] = true;
      beep(10, 0);
    }

    // Collision: bird vs pipe
    if (FB_BIRD_X + FB_BIRD_W > fbPipeX[i] && FB_BIRD_X < fbPipeX[i] + FB_PIPE_W) {
      // Check if bird is outside the gap
      if (fbBirdY < fbPipeGapY[i] || fbBirdY + FB_BIRD_H > fbPipeGapY[i] + FB_GAP_H) {
        fbGameOver = true;
        if (fbScore > fbHighScore) fbHighScore = fbScore;
        beep(200, 100);
        beep(300, 0);
        return;
      }
    }
  }
}

void fbDrawBird(int x, int y) {
  // Body
  display.fillRoundRect(x, y, FB_BIRD_W, FB_BIRD_H, 2, WHITE);
  // Eye
  display.drawPixel(x + 5, y + 1, BLACK);
  // Wing (flap animation)
  if (fbBirdVY < 0) {
    display.drawLine(x + 1, y + 2, x - 1, y, WHITE);
  } else {
    display.drawLine(x + 1, y + 3, x - 1, y + 5, WHITE);
  }
  // Beak
  display.drawPixel(x + FB_BIRD_W, y + 2, WHITE);
  display.drawPixel(x + FB_BIRD_W, y + 3, WHITE);
  display.drawPixel(x + FB_BIRD_W + 1, y + 3, WHITE);
}

void fbDraw() {
  display.setRotation(0);
  display.clearDisplay();

  if (fbGameOver) {
    display.setTextSize(2);
    display.setCursor(10, 5);
    display.println("GAME OVER");
    display.setTextSize(1);
    display.setCursor(10, 30);
    display.print("Score: ");
    display.println(fbScore);
    display.setCursor(10, 42);
    display.print("High:  ");
    display.println(fbHighScore);
    display.setCursor(10, 56);
    display.print("Touch to restart");
    display.display();
    return;
  }

  if (!fbStarted) {
    // Title screen
    display.setTextSize(2);
    display.setCursor(5, 5);
    display.println("Flappy");
    display.setCursor(30, 25);
    display.println("Bird");
    display.setTextSize(1);
    display.setCursor(15, 52);
    display.print("Touch to start");
    // Draw bird
    fbDrawBird(100, 20);
    display.display();
    return;
  }

  // Draw pipes
  for (int i = 0; i < FB_MAX_PIPES; i++) {
    int px = (int)fbPipeX[i];
    if (px > SCREEN_WIDTH || px + FB_PIPE_W < 0) continue;
    // Top pipe
    display.fillRect(px, 0, FB_PIPE_W, fbPipeGapY[i], WHITE);
    // Pipe lip top
    display.fillRect(px - 2, fbPipeGapY[i] - 3, FB_PIPE_W + 4, 3, WHITE);
    // Bottom pipe
    int bottomY = fbPipeGapY[i] + FB_GAP_H;
    display.fillRect(px, bottomY, FB_PIPE_W, SCREEN_HEIGHT - bottomY, WHITE);
    // Pipe lip bottom
    display.fillRect(px - 2, bottomY, FB_PIPE_W + 4, 3, WHITE);
  }

  // Draw bird
  fbDrawBird(FB_BIRD_X, (int)fbBirdY);

  // Score
  display.setTextSize(1);
  display.setCursor(SCREEN_WIDTH - 30, 2);
  display.print(fbScore);

  display.display();
}

// ---- Pong ----

#define PG_PADDLE_W 3
#define PG_PADDLE_H 14
#define PG_BALL_SIZE 3
#define PG_WIN_SCORE 5
#define PG_PLAYER_X 3
#define PG_CPU_X (SCREEN_WIDTH - PG_PADDLE_W - 3)

float pgPlayerY = 25;
float pgCpuY = 25;
float pgBallX, pgBallY;
float pgBallVX, pgBallVY;
int pgPlayerScore = 0;
int pgCpuScore = 0;
bool pgGameOver = false;
bool pgPlayerWon = false;
bool pgServing = true;
unsigned long pgLastFrame = 0;
bool lastPgTouchState = false;

// CPU difficulty: how closely it tracks the ball (0-1, higher = harder)
#define PG_CPU_SPEED 2.5
#define PG_CPU_REACT 0.7

void pgServeBall() {
  pgBallX = SCREEN_WIDTH / 2;
  pgBallY = SCREEN_HEIGHT / 2;
  pgBallVX = (random(2) == 0) ? 2.5 : -2.5;
  pgBallVY = (random(100) - 50) / 25.0;
  pgServing = false;
}

void pgReset() {
  pgPlayerY = (SCREEN_HEIGHT - PG_PADDLE_H) / 2;
  pgCpuY = (SCREEN_HEIGHT - PG_PADDLE_H) / 2;
  pgPlayerScore = 0;
  pgCpuScore = 0;
  pgGameOver = false;
  pgPlayerWon = false;
  pgServing = true;
}

void pgUpdate() {
  unsigned long now = millis();
  if (now - pgLastFrame < 25) return;  // ~40 fps
  pgLastFrame = now;

  if (pgGameOver || pgServing) return;

  // Move ball
  pgBallX += pgBallVX;
  pgBallY += pgBallVY;

  // Bounce off top/bottom
  if (pgBallY <= 0) {
    pgBallY = 0;
    pgBallVY = -pgBallVY;
    beep(5, 0);
  }
  if (pgBallY + PG_BALL_SIZE >= SCREEN_HEIGHT) {
    pgBallY = SCREEN_HEIGHT - PG_BALL_SIZE;
    pgBallVY = -pgBallVY;
    beep(5, 0);
  }

  // Player paddle collision (left side)
  if (pgBallX <= PG_PLAYER_X + PG_PADDLE_W &&
      pgBallX + PG_BALL_SIZE >= PG_PLAYER_X &&
      pgBallY + PG_BALL_SIZE >= pgPlayerY &&
      pgBallY <= pgPlayerY + PG_PADDLE_H &&
      pgBallVX < 0) {
    pgBallVX = -pgBallVX;
    // Add spin based on where ball hits paddle
    float hitPos = (pgBallY + PG_BALL_SIZE / 2 - pgPlayerY) / PG_PADDLE_H;
    pgBallVY = (hitPos - 0.5) * 5.0;
    // Speed up slightly
    pgBallVX *= 1.05;
    if (pgBallVX > 5.0) pgBallVX = 5.0;
    beep(15, 0);
  }

  // CPU paddle collision (right side)
  if (pgBallX + PG_BALL_SIZE >= PG_CPU_X &&
      pgBallX <= PG_CPU_X + PG_PADDLE_W &&
      pgBallY + PG_BALL_SIZE >= pgCpuY &&
      pgBallY <= pgCpuY + PG_PADDLE_H &&
      pgBallVX > 0) {
    pgBallVX = -pgBallVX;
    float hitPos = (pgBallY + PG_BALL_SIZE / 2 - pgCpuY) / PG_PADDLE_H;
    pgBallVY = (hitPos - 0.5) * 5.0;
    pgBallVX *= 1.05;
    if (pgBallVX < -5.0) pgBallVX = -5.0;
    beep(15, 0);
  }

  // Ball goes off left — CPU scores
  if (pgBallX + PG_BALL_SIZE < 0) {
    pgCpuScore++;
    beep(100, 50);
    beep(150, 0);
    if (pgCpuScore >= PG_WIN_SCORE) {
      pgGameOver = true;
      pgPlayerWon = false;
    } else {
      pgServing = true;
    }
  }

  // Ball goes off right — player scores
  if (pgBallX > SCREEN_WIDTH) {
    pgPlayerScore++;
    beep(20, 20);
    beep(20, 0);
    if (pgPlayerScore >= PG_WIN_SCORE) {
      pgGameOver = true;
      pgPlayerWon = true;
      beep(50, 50);
      beep(50, 50);
      beep(100, 0);
    } else {
      pgServing = true;
    }
  }

  // CPU AI: track ball with some imprecision
  if (pgBallVX > 0) {
    // Ball coming toward CPU — track it
    float targetY = pgBallY + PG_BALL_SIZE / 2 - PG_PADDLE_H / 2;
    float diff = targetY - pgCpuY;
    if (abs(diff) > 2) {
      pgCpuY += (diff > 0 ? 1 : -1) * PG_CPU_SPEED * PG_CPU_REACT;
    }
  } else {
    // Ball going away — drift toward center
    float center = (SCREEN_HEIGHT - PG_PADDLE_H) / 2;
    float diff = center - pgCpuY;
    if (abs(diff) > 2) {
      pgCpuY += (diff > 0 ? 1 : -1) * PG_CPU_SPEED * 0.3;
    }
  }

  // Clamp CPU paddle
  if (pgCpuY < 0) pgCpuY = 0;
  if (pgCpuY > SCREEN_HEIGHT - PG_PADDLE_H) pgCpuY = SCREEN_HEIGHT - PG_PADDLE_H;
}

void pgPlayerMove() {
  // Move player paddle using tilt
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float tilt = -(a.acceleration.z - accelOffsetZ);  // up/down tilt

  if (tilt > 2.0) pgPlayerY -= 3.0;
  else if (tilt < -2.0) pgPlayerY += 3.0;

  if (pgPlayerY < 0) pgPlayerY = 0;
  if (pgPlayerY > SCREEN_HEIGHT - PG_PADDLE_H) pgPlayerY = SCREEN_HEIGHT - PG_PADDLE_H;
}

void pgDraw() {
  display.setRotation(0);
  display.clearDisplay();

  if (pgGameOver) {
    display.setTextSize(2);
    display.setCursor(15, 5);
    display.println(pgPlayerWon ? "YOU WIN!" : "YOU LOSE");
    display.setTextSize(1);
    display.setCursor(30, 30);
    display.print(pgPlayerScore);
    display.print(" - ");
    display.print(pgCpuScore);
    display.setCursor(10, 50);
    display.print("Touch to play again");
    display.display();
    return;
  }

  if (pgServing) {
    // Draw everything but show "Touch to serve"
    display.setTextSize(1);
    display.setCursor(20, 28);
    display.print("Touch to serve");
  }

  // Center dashed line
  for (int y = 0; y < SCREEN_HEIGHT; y += 4) {
    display.drawPixel(SCREEN_WIDTH / 2, y, WHITE);
  }

  // Scores
  display.setTextSize(2);
  display.setCursor(SCREEN_WIDTH / 2 - 25, 2);
  display.print(pgPlayerScore);
  display.setCursor(SCREEN_WIDTH / 2 + 10, 2);
  display.print(pgCpuScore);

  // Player paddle (left)
  display.fillRect(PG_PLAYER_X, (int)pgPlayerY, PG_PADDLE_W, PG_PADDLE_H, WHITE);

  // CPU paddle (right)
  display.fillRect(PG_CPU_X, (int)pgCpuY, PG_PADDLE_W, PG_PADDLE_H, WHITE);

  // Ball
  if (!pgServing) {
    display.fillRect((int)pgBallX, (int)pgBallY, PG_BALL_SIZE, PG_BALL_SIZE, WHITE);
  }

  display.display();
}

// ---- Setup ----

void setup(void) {
  Serial.begin(115200);
  while (!Serial)
    delay(10);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
    for (;;);
  }

  display.clearDisplay();
  display.setTextColor(WHITE);

  pinMode(TOUCH_MODE, INPUT);
  pinMode(TOUCH_FACE, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Welcome screen
  drawWelcome();
  beep(100, 50);
  beep(50, 0);
  delay(2000);

  // Init MPU6050
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Initializing MPU6050...");
  display.display();

  Serial.println("Adafruit MPU6050 test!");

  if (!mpu.begin()) {
    Serial.println("Failed to find MPU6050 chip");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("MPU6050 not found!");
    display.println("Check wiring.");
    display.display();
    while (1) {
      delay(10);
    }
  }
  Serial.println("MPU6050 Found!");

  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  Serial.println("");
  calibrate();

  // Init RoboEyes
  face.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 60);
  face.setAutoblinker(ON, 3, 2);
  face.setIdleMode(ON, 3, 2);

  // Start clock timer (default mode is clock)
  clockEnteredTime = millis();

  // WiFi starts in IDLE — wifiTick() will trigger first connect immediately
  WiFi.mode(WIFI_OFF);
}

// ---- Loop ----

void loop() {
  // Background WiFi sync — runs every iteration, non-blocking
  wifiTick();

  // Touch sensor: cycle through Timer -> Clock -> Weather -> Timer ...
  bool touchState = digitalRead(TOUCH_MODE) == HIGH;
  if (touchState && !lastTouchState && (millis() - lastTouchTime > TOUCH_DEBOUNCE_MS)) {
    lastTouchTime = millis();
    if (appMode == MODE_CLOCK)       { appMode = MODE_WEATHER; weatherEnteredTime = millis(); }
    else if (appMode == MODE_WEATHER) appMode = MODE_TIMER;
    else if (appMode == MODE_TIMER)   { appMode = MODE_FACE; faceEnterHappy(); }
    else if (appMode == MODE_FACE)    { appMode = MODE_DINO; dinoReset(); }
    else if (appMode == MODE_DINO)    { appMode = MODE_INVADERS; siReset(); }
    else if (appMode == MODE_INVADERS) { appMode = MODE_MAZE; mzReset(); }
    else if (appMode == MODE_MAZE)    { appMode = MODE_FLAPPY; fbReset(); }
    else if (appMode == MODE_FLAPPY)  { appMode = MODE_PONG; pgReset(); }
    else                              { appMode = MODE_CLOCK; clockEnteredTime = millis(); }
    beepModeSwitch();
    const char* modeNames[] = {"CLOCK", "WEATHER", "TIMER", "FACE", "DINO", "INVADERS", "MAZE", "FLAPPY", "PONG"};
    Serial.print("Switched to ");
    Serial.println(modeNames[appMode]);
  }
  lastTouchState = touchState;

  // Clock mode — auto-switch to face after 15 seconds
  if (appMode == MODE_CLOCK) {
    if (millis() - clockEnteredTime >= CLOCK_AUTO_FACE_MS) {
      appMode = MODE_FACE;
      faceEnterHappy();
    } else {
      drawClock();
      delay(50);
      return;
    }
  }

  // Weather mode — auto-switch to face after 15 seconds
  if (appMode == MODE_WEATHER) {
    if (millis() - weatherEnteredTime >= WEATHER_AUTO_RETURN_MS) {
      appMode = MODE_FACE;
      faceEnterHappy();
    } else {
      drawWeather();
      delay(50);
      return;
    }
  }

  // --- Face mode ---
  if (appMode == MODE_FACE) {
    // D5 touch triggers angry/confused
    bool faceTouchState = digitalRead(TOUCH_FACE) == HIGH;
    if (faceTouchState && !lastFaceTouchState && (millis() - lastFaceTouchTime > TOUCH_DEBOUNCE_MS)) {
      lastFaceTouchTime = millis();
      faceTouchReact();
    }
    lastFaceTouchState = faceTouchState;

    // IMU controls gaze + shake detection
    updateEyesFromIMU();

    // Go tired if no interaction for a while
    if (millis() - lastInteractionTime > IDLE_TIMEOUT &&
        millis() - lastTiredCheck > TIRED_CHECK_INTERVAL) {
      lastTiredCheck = millis();
      faceGoTired();
    }

    // Update RoboEyes animation (non-blocking, handles its own frame rate)
    face.update();
    return;
  }

  // --- Dino game mode ---
  if (appMode == MODE_DINO) {
    bool dinoTouchState = digitalRead(TOUCH_FACE) == HIGH;
    if (dinoTouchState && !lastDinoTouchState) {
      if (dinoGameOver) {
        dinoReset();
      } else if (!dinoJumping) {
        dinoJumping = true;
        dinoVelY = DINO_JUMP_VEL;
        beep(15, 0);
      }
    }
    lastDinoTouchState = dinoTouchState;

    dinoUpdate();
    dinoDraw();
    return;
  }

  // --- Space Invaders mode ---
  if (appMode == MODE_INVADERS) {
    bool siTouchState = digitalRead(TOUCH_FACE) == HIGH;
    if (siTouchState && !lastSiTouchState) {
      if (siGameOver || siWin) {
        siReset();
      } else {
        siShoot();
      }
    }
    lastSiTouchState = siTouchState;

    siUpdate();
    siDraw();
    return;
  }

  // --- Maze mode ---
  if (appMode == MODE_MAZE) {
    bool mzTouchState = digitalRead(TOUCH_FACE) == HIGH;
    if (mzTouchState && !lastMzTouchState) {
      mzReset();
      beep(30, 0);
    }
    lastMzTouchState = mzTouchState;

    mzUpdate();
    mzDraw();
    return;
  }

  // --- Flappy Bird mode ---
  if (appMode == MODE_FLAPPY) {
    bool fbTouchState = digitalRead(TOUCH_FACE) == HIGH;
    if (fbTouchState && !lastFbTouchState) {
      if (fbGameOver) {
        fbReset();
      } else if (!fbStarted) {
        fbStarted = true;
        fbBirdVY = FB_FLAP_VEL;
        beep(10, 0);
      } else {
        fbBirdVY = FB_FLAP_VEL;
        beep(10, 0);
      }
    }
    lastFbTouchState = fbTouchState;

    fbUpdate();
    fbDraw();
    return;
  }

  // --- Pong mode ---
  if (appMode == MODE_PONG) {
    bool pgTouchState = digitalRead(TOUCH_FACE) == HIGH;
    if (pgTouchState && !lastPgTouchState) {
      if (pgGameOver) {
        pgReset();
      } else if (pgServing) {
        pgServeBall();
        beep(10, 0);
      }
    }
    lastPgTouchState = pgTouchState;

    pgPlayerMove();
    pgUpdate();
    pgDraw();
    return;
  }

  // --- Timer mode ---
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float ax = a.acceleration.x - accelOffsetX;
  float ay = a.acceleration.y - accelOffsetY;
  float az = a.acceleration.z - accelOffsetZ;

  accelXBuf[bufIndex] = ax;
  accelYBuf[bufIndex] = ay;
  accelZBuf[bufIndex] = az;
  gyroXBuf[bufIndex] = g.gyro.x - gyroOffsetX;
  gyroYBuf[bufIndex] = g.gyro.y - gyroOffsetY;
  gyroZBuf[bufIndex] = g.gyro.z - gyroOffsetZ;
  bufIndex++;
  if (bufIndex >= AVG_SAMPLES) {
    bufIndex = 0;
    bufFilled = true;
  }

  int count = bufFilled ? AVG_SAMPLES : bufIndex;
  float avgAx = average(accelXBuf, count);
  float avgAy = average(accelYBuf, count);
  float avgAz = average(accelZBuf, count);

  Orientation newOrient = detectOrientation(avgAx, avgAy, avgAz);

  if (newOrient != currentOrient && newOrient != ORIENT_NONE) {
    currentOrient = newOrient;
    timerDurationMs = getTimerDuration(currentOrient);
    timerStartMs = millis();
    timerRunning = (timerDurationMs > 0);
    timerAlarmDone = false;
    if (timerRunning) beepTimerStart();
    Serial.print("Orientation changed: ");
    Serial.println(currentOrient);
  }

  if (currentOrient == ORIENT_X_POS || currentOrient == ORIENT_X_NEG) {
    timerRunning = false;
    drawIdle();
  } else if (timerRunning) {
    unsigned long elapsed = millis() - timerStartMs;
    if (elapsed >= timerDurationMs) {
      timerRunning = false;
      if (!timerAlarmDone) {
        beepTimerDone();
        timerAlarmDone = true;
      }
      drawTimerDone(currentOrient);
    } else {
      drawTimer(timerDurationMs - elapsed, currentOrient);
    }
  }

  delay(50);
}
