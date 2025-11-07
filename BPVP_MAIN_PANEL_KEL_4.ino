/**
 * Menambah library yang diperlukan
 */
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <AsyncTelegram2.h>
#include <time.h>
#include <BlynkSimpleEsp32.h>

/**
 * Objek
 */
BlynkTimer timer;
HardwareSerial SerialPort(2);
WiFiClientSecure client;
AsyncTelegram2 myBot(client);

/**
 * Variabel Global
 */
float temperature, flowRate, totalVolume, tdsValue, phValue;
int waterLevelPercentage;
const int jumlahPompa = 4;
int pinPompa[jumlahPompa] = {PIN_POMPA_A, PIN_POMPA_B, PIN_POMPA_PH_NAIK, PIN_POMPA_PH_TURUN};
const int jumlahLED = 5;
int pinLED[jumlahLED] = {PIN_LED_INDIKATOR_DATA, PIN_LED_ALARM_ALIRAN_AIR, PIN_LED_ALARM_TDS, PIN_LED_ALARM_KAPASITAS_AIR, PIN_LED_ALARM_PH};
unsigned long lastDataReceivedTime = 0;

// Cooldown variables
bool isKapasitasAirOnCooldown = false;
unsigned long kapasitasAirCooldownStartTime = 0;
bool isAliranAirOnCooldown = false;
unsigned long aliranAirCooldownStartTime = 0;
bool isTDSOnCooldown = false;
unsigned long tdsCooldownStartTime = 0;
bool isPHOnCooldown = false;
unsigned long phCooldownStartTime = 0;
int nutrientPumpState = 0; // 0=idle, 1=A running, 2=B running
unsigned long nutrientPumpTimer = 0;
int phPumpState = 0; // 0=idle, 1=Up running, 2=Down running
unsigned long phPumpTimer = 0;

/**
 * Mode
 * 0:Manual
 * 1:Auto
 */
bool mode = 0;

/**
 * Virtual Pin Handler
 */
BLYNK_WRITE(VPIN_POMPA_A)
{
  int value = param.asInt();
  digitalWrite(PIN_POMPA_A, value);
}

BLYNK_WRITE(VPIN_POMPA_B)
{
  int value = param.asInt();
  digitalWrite(PIN_POMPA_B, value);
}

BLYNK_WRITE(VPIN_POMPA_UP)
{
  int value = param.asInt();
  digitalWrite(PIN_POMPA_PH_NAIK, value);
}

BLYNK_WRITE(VPIN_POMPA_DOWN)
{
  int value = param.asInt();
  digitalWrite(PIN_POMPA_PH_TURUN, value);
}

BLYNK_WRITE(VPIN_MODE)
{
  int value = param.asInt();
  setMode(value);
}

BLYNK_WRITE(VPIN_REFRESH)
{
  int value = param.asInt();
  if (value == 1)
  {
    Serial.println("Blynk VirtualWrite VPIN_KAPASitas_AIR: " + String(waterLevelPercentage));
    Blynk.virtualWrite(VPIN_KAPASITAS_AIR, waterLevelPercentage);
    Serial.println("Blynk VirtualWrite VPIN_SUHU_AIR: " + String(temperature));
    Blynk.virtualWrite(VPIN_SUHU_AIR, temperature);
    Serial.println("Blynk VirtualWrite VPIN_TDS_AIR: " + String(tdsValue));
    Blynk.virtualWrite(VPIN_TDS_AIR, tdsValue);
    Serial.println("Blynk VirtualWrite VPIN_FLOWRATE_AIR: " + String(flowRate));
    Blynk.virtualWrite(VPIN_FLOWRATE_AIR, flowRate);
    Serial.println("Blynk VirtualWrite VPIN_PH_AIR: " + String(phValue));
    Blynk.virtualWrite(VPIN_PH_AIR, phValue);
  }

  // Reset Refresh Button
  Serial.println("Blynk VirtualWrite VPIN_REFRESH: 0");
  Blynk.virtualWrite(VPIN_REFRESH, 0);
}

/**
 * Program untuk persiapan / setting
 */
void setup()
{
  Serial.begin(115200);
  SerialPort.begin(9600, SERIAL_8N1, 16, 17); // Initialize UART2 with specified pins: RX=16, TX=17
  Serial.println("ESP32 Receiver is Ready...");

  for (int i = 0; i < jumlahPompa; i++)
  {
    pinMode(pinPompa[i], OUTPUT);
    digitalWrite(pinPompa[i], LOW);
  }

  for (int i = 0; i < jumlahLED; i++)
  {
    pinMode(pinLED[i], OUTPUT);
    digitalWrite(pinLED[i], HIGH);
  }

  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASSWORD, "blynk.cloud", 80);

  configTzTime(TIMEZONE, "time.google.com", "time.windows.com", "pool.ntp.org");
  client.setCACert(telegram_cert);
  myBot.setUpdateTime(1000);
  myBot.setTelegramToken(BOTtoken);
  Serial.print("\nTest Telegram connection... ");
  myBot.begin() ? Serial.println("OK") : Serial.println("NOK");
  Serial.print("Bot name: @");
  Serial.println(myBot.getBotName());
  time_t now = time(nullptr);
  struct tm t = *localtime(&now);
  char welcome_msg[64];
  strftime(welcome_msg, sizeof(welcome_msg), "Bot started at %X", &t);

  for (int i = 0; i < (sizeof(CHAT_IDs) / sizeof(CHAT_IDs[0])); i++)
  {
    myBot.sendTo(CHAT_IDs[i], welcome_msg);
  }

  timer.setInterval(3600000L, sendSensor);
  timer.setInterval(1000L, checkAlarm);
  timer.setInterval(5000L, checkWifi);

  digitalWrite(PIN_LED_INDIKATOR_DATA, LOW);
}

/**
 * Program utama
 */
void loop()
{
  Blynk.run();
  timer.run();

  TBMessage msg;
  if (myBot.getNewMessage(msg))
  {
    Serial.print("User ");
    Serial.print(msg.sender.username);
    Serial.print(" (");
    Serial.print(msg.chatId);
    Serial.print(") send this message: ");
    Serial.println(msg.text);
    handleTelegramMessage(msg.chatId, msg.sender.username, msg.text);
  }

  if (SerialPort.available())
  {
    String receivedMessage = SerialPort.readStringUntil('\n');
    handleUartMessage(receivedMessage);
    lastDataReceivedTime = millis();
  }
}

/**
 * Sensor Send Routine
 */
void sendSensor()
{
  Serial.println("Blynk VirtualWrite VPIN_KAPASITAS_AIR: " + String(waterLevelPercentage));
  Blynk.virtualWrite(VPIN_KAPASITAS_AIR, waterLevelPercentage);
  Serial.println("Blynk VirtualWrite VPIN_SUHU_AIR: " + String(temperature));
  Blynk.virtualWrite(VPIN_SUHU_AIR, temperature);
  Serial.println("Blynk VirtualWrite VPIN_TDS_AIR: " + String(tdsValue));
  Blynk.virtualWrite(VPIN_TDS_AIR, tdsValue);
  Serial.println("Blynk VirtualWrite VPIN_FLOWRATE_AIR: " + String(flowRate));
  Blynk.virtualWrite(VPIN_FLOWRATE_AIR, flowRate);
  Serial.println("Blynk VirtualWrite VPIN_PH_AIR: " + String(phValue));
  Blynk.virtualWrite(VPIN_PH_AIR, phValue);

  String sensorMessage = "Suhu Air: " + String(temperature) + "°C";
  sensorMessage += "\nTDS Air: " + String(tdsValue) + " ppm";
  sensorMessage += "\nFlow Rate: " + String(flowRate) + " L/min";
  sensorMessage += "\nWater Level: " + String(waterLevelPercentage) + " %";
  sensorMessage += "\nPH Air: " + String(phValue);
  sendTelegramMessage(sensorMessage);
}

/**
 * UART
 */
void handleUartMessage(String message)
{
  Serial.print("Received: ");
  Serial.println(message);
  sscanf(message.c_str(), "%f&%d&%f&%f&%f&%f", &temperature, &waterLevelPercentage, &flowRate, &totalVolume, &tdsValue, &phValue);
  Serial.println("Temperature: " + String(temperature));
  Serial.println("Water Level Percentage: " + String(waterLevelPercentage));
  Serial.println("Flow Rate: " + String(flowRate));
  Serial.println("Total Volume: " + String(totalVolume));
  Serial.println("TDS Value: " + String(tdsValue));
  Serial.println("PH Value: " + String(phValue));
}

/**
 * TELEGRAM
 */
void handleTelegramMessage(int64_t chatId, String username, String message)
{
  if (!isInChatId(chatId))
  {
    myBot.sendTo(chatId, "Unauthorized user");
    return;
  }

  String command = "";
  String param = "";

  int spaceIndex = message.indexOf(' ');
  if (spaceIndex != -1) {
    command = message.substring(0, spaceIndex);
    param = message.substring(spaceIndex + 1);
  } else {
    command = message;
  }

  Serial.println("Command: " + command + " Param:" + param);

  if (command == "/start")
  {
    String welcome = "Selamat datang " + String(username) + " (" + String(chatId) + ")! Berikut adalah perintah yang tersedia:";
    welcome += "\n/pompa - Mendapatkan informasi status pompa";
    welcome += "\n/sensors - Mendapatkan semua data sensor";
    welcome += "\n/device - Mendapatkan informasi perangkat";
    welcome += "\n/mode 0|1 - Mengatur mode otomatis/manual";
    myBot.sendTo(chatId, welcome);
  }
  else if (command == "/sensors")
  {
    String sensorMessage = "Suhu Air: " + String(temperature) + "°C";
    sensorMessage += "\nTDS Air: " + String(tdsValue) + " ppm";
    sensorMessage += "\nFlow Rate: " + String(flowRate) + " L/min";
    sensorMessage += "\nWater Level: " + String(waterLevelPercentage) + " %";
    sensorMessage += "\nPH Air: " + String(phValue);
    myBot.sendTo(chatId, sensorMessage);
  }
  else if (command == "/mode")
  {
    setMode(param.toInt());
  }
  else if (command == "/device")
  {
    String message = "SSID: " + String(WIFI_SSID);
    message += "\nIP Address: " + WiFi.localIP().toString();
    message += "\nMode: ";
    message += (mode == 1) ? "Auto" : "Manual";
    myBot.sendTo(chatId, message);
  }
  else if (command == "/pompa")
  {
    String message = "Pompa A: ";
    message += "\nStatus:";
    message += (digitalRead(PIN_POMPA_A) == HIGH) ? "ON" : "OFF";
    message += "\nPompa B: ";
    message += (digitalRead(PIN_POMPA_B) == HIGH) ? "ON" : "OFF";
    message += "\nPompa PH Up: ";
    message += (digitalRead(PIN_POMPA_PH_NAIK) == HIGH) ? "ON" : "OFF";
    message += "\nPompa PH Down: ";
    message += (digitalRead(PIN_POMPA_PH_TURUN) == HIGH) ? "ON" : "OFF";
    message += "\n\nCooldown Status:";
    message += "\nTDS Cooldown: " + String(isTDSOnCooldown ? String((tdsCooldownStartTime + ALARM_TDS_COOLDOWN - millis()) / 1000) + "s remaining" : "Inactive");
    message += "\nPH Cooldown: " + String(isPHOnCooldown ? String((phCooldownStartTime + ALARM_PH_COOLDOWN - millis()) / 1000) + "s remaining" : "Inactive");
    myBot.sendTo(chatId, message);
  }
  else
  {
    myBot.sendTo(chatId, "Perintah tidak dikenal. Gunakan /start untuk melihat perintah yang tersedia.");
  }
}

bool isInChatId(int64_t chatId)
{
  for (int i = 0; i < (sizeof(CHAT_IDs) / sizeof(CHAT_IDs[0])); i++)
  {
    if (CHAT_IDs[i] == chatId)
    {
      return true;
    }
  }
  return false;
}

void sendTelegramMessage(String message)
{
  for (int i = 0; i < (sizeof(CHAT_IDs) / sizeof(CHAT_IDs[0])); i++)
  {
    myBot.sendTo(CHAT_IDs[i], message);
  }
}

/**
 * ALARM
 */
void checkAlarm()
{
  unsigned long currentMillis = millis();

  if (mode != 1)
  {
    return;
  }

  /**
   * PUMP MANAGEMENT
   */
  if (nutrientPumpState == 1)
  {
    if (currentMillis - nutrientPumpTimer > DURASI_POMPA_A)
    {
      digitalWrite(PIN_POMPA_A, LOW);
      Blynk.virtualWrite(VPIN_POMPA_A, 0);
      Serial.println("MEMATIKAN POMPA A");
      pompaAMati();
      nutrientPumpState = 2;
      nutrientPumpTimer = currentMillis;
      digitalWrite(PIN_POMPA_B, HIGH);
      Blynk.virtualWrite(VPIN_POMPA_B, 1);
      Serial.println("MENYALAKAN POMPA B");
      pompaBMenyala();
    }
  }
  else if (nutrientPumpState == 2)
  {
    if (currentMillis - nutrientPumpTimer > DURASI_POMPA_B)
    {
      digitalWrite(PIN_POMPA_B, LOW);
      Blynk.virtualWrite(VPIN_POMPA_B, 0);
      Serial.println("MEMATIKAN POMPA B");
      pompaBMati();
      nutrientPumpState = 0;
      isTDSOnCooldown = true;
      tdsCooldownStartTime = currentMillis;
    }
  }

  // if (phPumpState == 1)
  // {
  //   if (currentMillis - phPumpTimer > DURASI_POMPA_PH_NAIK)
  //   {
  //     digitalWrite(PIN_POMPA_PH_NAIK, LOW);
  //     Blynk.virtualWrite(VPIN_POMPA_UP, 0);
  //     Serial.println("MEMATIKAN POMPA PH NAIK");
  //     phPumpState = 0;
  //     isPHOnCooldown = true;
  //     phCooldownStartTime = currentMillis;
  //   }
  // }
  // else if (phPumpState == 2)
  // {
  //   if (currentMillis - phPumpTimer > DURASI_POMPA_PH_TURUN)
  //   {
  //     digitalWrite(PIN_POMPA_PH_TURUN, LOW);
  //     Blynk.virtualWrite(VPIN_POMPA_DOWN, 0);
  //     Serial.println("MEMATIKAN POMPA PH TURUN");
  //     phPumpState = 0;
  //     isPHOnCooldown = true;
  //     phCooldownStartTime = currentMillis;
  //   }
  // }

  /**
   * TDS Check
   */
  if (tdsValue < MIN_TDS)
  {
    digitalWrite(PIN_LED_ALARM_TDS, LOW);
    if (isTDSOnCooldown && (currentMillis - tdsCooldownStartTime > ALARM_TDS_COOLDOWN))
    {
      isTDSOnCooldown = false;
    }

    if (!isTDSOnCooldown && nutrientPumpState == 0)
    {
      alarmTdsKurang();
      nutrientPumpState = 1;
      nutrientPumpTimer = currentMillis;
      digitalWrite(PIN_POMPA_A, HIGH);
      Blynk.virtualWrite(VPIN_POMPA_A, 1);
      Serial.println("MENYALAKAN POMPA A");
      pompaAMenyala();
    }
  }
  else if (tdsValue > MAX_TDS)
  {
    digitalWrite(PIN_LED_ALARM_TDS, LOW);

    if (isTDSOnCooldown && (currentMillis - tdsCooldownStartTime > ALARM_TDS_COOLDOWN))
    {
      isTDSOnCooldown = false;
    }

    if (!isTDSOnCooldown)
    {
      alarmTdsBerlebihan();
      isTDSOnCooldown = true;
      tdsCooldownStartTime = currentMillis;
    }
  }
  else
  {
    digitalWrite(PIN_LED_ALARM_TDS, HIGH);
    if (isTDSOnCooldown)
    {
      isTDSOnCooldown = false;
      tdsCooldownStartTime = currentMillis - ALARM_TDS_COOLDOWN;
    }

    if (nutrientPumpState == 1)
    {
      digitalWrite(PIN_POMPA_A, LOW);
      Blynk.virtualWrite(VPIN_POMPA_A, 0);
      Serial.println("MEMATIKAN POMPA A (TDS NORMAL)");
      pompaAMati();
      nutrientPumpState = 0;
    }
    else if (nutrientPumpState == 2)
    {
      digitalWrite(PIN_POMPA_B, LOW);
      Blynk.virtualWrite(VPIN_POMPA_B, 0);
      Serial.println("MEMATIKAN POMPA B (TDS NORMAL)");
      pompaBMati();
      nutrientPumpState = 0;
    }
  }

  /**
   * pH Check
   */
  // if (phValue < MIN_PH || phValue > MAX_PH)
  // {
  //   digitalWrite(PIN_LED_ALARM_PH, LOW);
  //   if (isPHOnCooldown && (currentMillis - phCooldownStartTime > ALARM_PH_COOLDOWN))
  //   {
  //     isPHOnCooldown = false;
  //   }
  //   if (!isPHOnCooldown && phPumpState == 0)
  //   {
  //     phPumpTimer = currentMillis;
  //     if (phValue < MIN_PH)
  //     {
  //       alarmPhKurang();
  //       phPumpState = 1;
  //       digitalWrite(PIN_POMPA_PH_NAIK, HIGH);
  //       Blynk.virtualWrite(VPIN_POMPA_UP, 1);
  //       Serial.println("MENYALAKAN POMPA PH NAIK");
  //     }
  //     else
  //     {
  //       alarmPhBerlebih();
  //       phPumpState = 2;
  //       digitalWrite(PIN_POMPA_PH_TURUN, HIGH);
  //       Blynk.virtualWrite(VPIN_POMPA_DOWN, 1);
  //       Serial.println("MENYALAKAN POMPA PH TURUN");
  //     }
  //   }
  // }
  // else
  // {
  //   digitalWrite(PIN_LED_ALARM_PH, HIGH);
  //   if (isPHOnCooldown)
  //   {
  //     isPHOnCooldown = false;
  //     phCooldownStartTime = currentMillis - ALARM_PH_COOLDOWN;
  //   }
  //   if (phPumpState != 0)
  //   {
  //     digitalWrite(PIN_POMPA_PH_NAIK, LOW);
  //     Blynk.virtualWrite(VPIN_POMPA_UP, 0);
  //     digitalWrite(PIN_POMPA_PH_TURUN, LOW);
  //     Blynk.virtualWrite(VPIN_POMPA_DOWN, 0);
  //     Serial.println("MEMATIKAN POMPA PH (PH NORMAL)");
  //     phPumpState = 0;
  //   }
  // }

  /**
   * Water Level Check
   */
  if (waterLevelPercentage < MIN_WATER_LEVEL)
  {
    digitalWrite(PIN_LED_ALARM_KAPASITAS_AIR, LOW);

    if (isKapasitasAirOnCooldown && (currentMillis - kapasitasAirCooldownStartTime >= ALARM_KAPASITAS_AIR_COOLDOWN))
    {
      isKapasitasAirOnCooldown = false;
    }
    if (!isKapasitasAirOnCooldown)
    {
      alarmAirKosong();
      isKapasitasAirOnCooldown = true;
      kapasitasAirCooldownStartTime = currentMillis;
    }
  }
  else if (waterLevelPercentage > MAX_WATER_LEVEL)
  {
    digitalWrite(PIN_LED_ALARM_KAPASITAS_AIR, LOW);

    if (isKapasitasAirOnCooldown && (currentMillis - kapasitasAirCooldownStartTime >= ALARM_KAPASITAS_AIR_COOLDOWN))
    {
      isKapasitasAirOnCooldown = false;
    }
    if (!isKapasitasAirOnCooldown)
    {
      alarmAirPenuh();
      isKapasitasAirOnCooldown = true;
      kapasitasAirCooldownStartTime = currentMillis;
    }
  }
  else
  {
    digitalWrite(PIN_LED_ALARM_KAPASITAS_AIR, HIGH);
    if (isKapasitasAirOnCooldown)
    {
      isKapasitasAirOnCooldown = false;
      kapasitasAirCooldownStartTime = currentMillis - ALARM_KAPASITAS_AIR_COOLDOWN;
    }
  }

  /**
   * Water Flow Check
   */
  if (flowRate < MIN_WATERFLOW)
  {
    digitalWrite(PIN_LED_ALARM_ALIRAN_AIR, LOW);
    if (isAliranAirOnCooldown && (currentMillis - aliranAirCooldownStartTime >= ALARM_ALIRAN_AIR_COOLDOWN))
    {
      isAliranAirOnCooldown = false;
    }
    if (!isAliranAirOnCooldown)
    {
      alarmAliranAirMati();
      isAliranAirOnCooldown = true;
      aliranAirCooldownStartTime = currentMillis;
    }
  }
  else
  { // Water Flow OK
    digitalWrite(PIN_LED_ALARM_ALIRAN_AIR, HIGH);
    if (isAliranAirOnCooldown)
    {
      isAliranAirOnCooldown = false;
      aliranAirCooldownStartTime = currentMillis - ALARM_ALIRAN_AIR_COOLDOWN;
    }
  }

  /**
   * UART Check (Sensor Activity)
   */
  if (currentMillis - lastDataReceivedTime >= SENSOR_ACTIVITY_TIMEOUT)
  {
    alarmSensorMati();
    lastDataReceivedTime = currentMillis;
    temperature = 0;
    waterLevelPercentage = 0;
    flowRate = 0;
    totalVolume = 0;
    tdsValue = 0;
    phValue = 0;
  }
}
/**
 * NOTIFICATIONS
 */
void alarmAirKosong()
{
  String message = "⚠️ Peringatan Kapasitas Air ⚠️\n";
  message += "Kapasitas air terdeteksi kosong! Harap periksa bak air.\n";
  message += "Kapasitas Air: " + String(waterLevelPercentage) + " % (Minimal: " + String(MIN_WATER_LEVEL) + " %)";
  sendTelegramMessage(message);
}

void alarmAirPenuh()
{
  String message = "⚠️ Peringatan Kapasitas Air ⚠️\n";
  message += "Kapasitas air terdeteksi penuh! Harap periksa pompa atau sensor.\n";
  message += "Kapasitas Air: " + String(waterLevelPercentage) + " % (Maksimal: " + String(MAX_WATER_LEVEL) + " %)";
  sendTelegramMessage(message);
}

void alarmAliranAirMati()
{
  String message = "⚠️ Peringatan Aliran Air ⚠️\n";
  message += "Aliran air terdeteksi mati! Harap periksa pompa atau sensor.\n";
  message += "Flow Rate Terukur: " + String(flowRate) + " L/min (Minimal: " + String(MIN_WATERFLOW) + " L/min)";
  sendTelegramMessage(message);
}

void alarmTdsKurang()
{
  String message = "⚠️ Peringatan TDS ⚠️\n";
  message += "TDS terdeteksi kurang! Menyalakan pompa nutrisi.\n";
  message += "TDS Air Terukur: " + String(tdsValue) + " ppm (Minimal: " + String(MIN_TDS) + " ppm)";
  sendTelegramMessage(message);
}

void alarmTdsBerlebihan()
{
  String message = "⚠️ Peringatan TDS ⚠️\n";
  message += "TDS terdeteksi berlebih! Harap periksa kadar nutrisi.\n";
  message += "TDS Air Terukur: " + String(tdsValue) + " ppm (Maksimal: " + String(MAX_TDS) + " ppm)";
  sendTelegramMessage(message);
}

void alarmPhKurang()
{
  String message = "⚠️ Peringatan pH ⚠️\n";
  message += "pH terdeteksi terlalu rendah! Menyalakan pompa pH Up.\n";
  message += "pH Air Terukur: " + String(phValue) + " (Minimal: " + String(MIN_PH) + ")";
  sendTelegramMessage(message);
}

void alarmPhBerlebih()
{
  String message = "⚠️ Peringatan pH ⚠️\n";
  message += "pH terdeteksi terlalu tinggi! Menyalakan pompa pH Down.\n";
  message += "pH Air Terukur: " + String(phValue) + " (Maksimal: " + String(MAX_PH) + ")";
  sendTelegramMessage(message);
}

void alarmSensorMati()
{
  String message = "⚠️ Sensor Tidak Aktif ⚠️\n";
  message += "Sensor tidak aktif! Tidak ada data yang diterima dalam " + String(SENSOR_ACTIVITY_TIMEOUT / 60000) + " menit.";
  sendTelegramMessage(message);
}

void pompaAMenyala()
{
  String message = "Pompa A telah dinyalakan.";
  sendTelegramMessage(message);
}

void pompaBMenyala()
{
  String message = "Pompa B telah dinyalakan.";
  sendTelegramMessage(message);
}

void pompaAMati()
{
  String message = "Pompa A telah dimatikan.";
  sendTelegramMessage(message);
}

void pompaBMati()
{
  String message = "Pompa B telah dimatikan.";
  sendTelegramMessage(message);
}

/**
 * Set MODE
 */
void setMode(int newMode)
{
  if (newMode == 1)
  {
    mode = 1;
    Blynk.virtualWrite(VPIN_MODE, newMode);
    sendTelegramMessage("Mode otomatis diaktifkan.");
  }
  else if (newMode == 0)
  {
    mode = 0;
    Blynk.virtualWrite(VPIN_MODE, newMode);
    sendTelegramMessage("Mode manual diaktifkan.");
  }
  else
  {
    sendTelegramMessage("Mode " + String(mode) + " tidak dikenali.");
  }
}

/**
 * Check WiFi Connection
 */
void checkWifi()
{
  int status = WiFi.status();
  if (status == WL_CONNECTED) {
    digitalWrite(PIN_LED_INDIKATOR_DATA, LOW);
  } else if (status == WL_CONNECTION_LOST) {
    Serial.println("WiFi connection lost");
    digitalWrite(PIN_LED_INDIKATOR_DATA, HIGH);
  } else {
    Serial.print("WiFi status: ");
    Serial.println(status);
  }
}