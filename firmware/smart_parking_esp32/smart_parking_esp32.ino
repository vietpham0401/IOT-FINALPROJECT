/**
 * ============================================================================
 * DỰ ÁN CUỐI KỲ MÔN INTRODUCTION TO IOT
 * Đề tài: IoT-Based Smart Vehicle Parking System using RFID (PN532) & Phone Camera ALPR
 * Vi điều khiển: ESP32 DevKit V1
 * ============================================================================
 *
 * NÂNG CẤP ĐẶC BIỆT:
 * 1. Sử dụng module NFC/RFID PN532 qua giao tiếp I2C (dùng chung chân SDA=21, SCL=22
 *    với màn hình LCD 1602 I2C vì PN532 có địa chỉ 0x24 còn LCD có địa chỉ 0x27).
 *    -> Lưu ý gạt công tắc (DIP Switch) trên mạch PN532 sang chế độ I2C:
 *       Channel 1 (SEL0) = ON (1) | Channel 2 (SEL1) = OFF (0)
 * 2. Xác thực 2 lớp (2-Factor Authentication):
 *    - Bước 1: Quẹt thẻ trên PN532 -> Lấy UID & Biển số xe đăng ký.
 *    - Bước 2: Camera điện thoại trên Web Dashboard quét biển số thực tế (OCR).
 *    - Bước 3: Nếu Biển số Camera == Biển số Thẻ -> Mở cổng Barie (Servo 90°).
 *
 * Thư viện cần cài đặt trên Arduino IDE (Library Manager):
 * 1. Adafruit PN532 (by Adafruit)
 * 2. ESP32Servo (by Kevin Harrington)
 * 3. LiquidCrystal I2C (by Frank de Brabander)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

// ===================== 1. CẤU HÌNH WI-FI & FIREBASE =========================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// URL Firebase Realtime Database
const String FIREBASE_HOST = "https://YOUR_PROJECT_ID.firebasedatabase.app";
const String FIREBASE_AUTH = "";

// Chế độ bắt buộc xác thực biển số qua Camera điện thoại trên Web Dashboard
// Đặt = true khi demo kết hợp Web Dashboard + Camera điện thoại
// Đặt = false nếu muốn test nhanh quẹt thẻ mở cổng ngay khi chưa bật Web
bool REQUIRE_CAMERA_PLATE_MATCH = true;

// ===================== 2. ĐỊNH NGHĨA CHÂN KẾT NỐI (PINOUT) ==================
// Giao tiếp I2C dùng chung cho cả PN532 (0x24) và LCD 1602 (0x27)
#define I2C_SDA_PIN     21
#define I2C_SCL_PIN     22

// Chân IRQ và RESET cho PN532 I2C
#define PN532_IRQ       16
#define PN532_RESET     17

// Màn hình LCD 1602 I2C
#define LCD_ADDRESS     0x27
#define LCD_COLS        16
#define LCD_ROWS        2

// Động cơ Servo điều khiển thanh chắn Barie
#define SERVO_PIN       13
#define GATE_CLOSED_DEG 0
#define GATE_OPEN_DEG   90

// Cảm biến hồng ngoại (IR Sensors) - LOW = Có xe, HIGH = Trống
#define NUM_SLOTS       3
const int IR_SLOT_PINS[NUM_SLOTS] = {32, 33, 25};
#define IR_GATE_PIN     26

// Còi báo Buzzer
#define BUZZER_PIN      27

// ===================== 3. DANH SÁCH THẺ RFID/NFC PN532 HỢP LỆ ===============
struct AuthorizedCard {
  String uid;
  String ownerName;
  String licensePlate;
  bool isParked;
};

AuthorizedCard validCards[] = {
  {"01 02 03 04", "Nguyen Van A", "29A-123.45", false},
  {"11 22 33 44", "Tran Thi B",   "30F-678.90", false},
  {"55 66 77 88", "Le Hoang C",   "51G-888.88", false}
};
const int NUM_VALID_CARDS = sizeof(validCards) / sizeof(validCards[0]);

// ===================== 4. KHỞI TẠO ĐỐI TƯỢNG & BIẾN TOÀN CỤC ================
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
Servo gateServo;

bool slotOccupied[NUM_SLOTS] = {false, false, false};
bool lastSlotState[NUM_SLOTS] = {false, false, false};
int availableSlots = NUM_SLOTS;

bool isGateOpen = false;
unsigned long gateOpenedTime = 0;
const unsigned long GATE_TIMEOUT_MS = 5000;

// Biến phục vụ chờ Camera điện thoại xác nhận biển số
bool waitingForCameraVerify = false;
int pendingCardIndex = -1;
unsigned long verifyRequestTime = 0;
const unsigned long CAMERA_VERIFY_TIMEOUT_MS = 15000; // Chờ tối đa 15 giây để camera quét biển số

unsigned long lastCloudPoll = 0;
const unsigned long CLOUD_POLL_INTERVAL_MS = 1200;

// ===================== 5. HÀM PHỤ TRỢ ÂM THANH & HIỂN THỊ ===================
void beep(int times, int durationMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < times - 1) delay(100);
  }
}

void updateLcdDefaultScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SMART PARKING ");
  lcd.print(availableSlots);
  lcd.print("F");

  lcd.setCursor(0, 1);
  for (int i = 0; i < NUM_SLOTS; i++) {
    lcd.print("S");
    lcd.print(i + 1);
    lcd.print(":");
    lcd.print(slotOccupied[i] ? "X " : "O ");
  }
}

void showLcdAlert(const String& line1, const String& line2, int beepCount, int beepMs) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  if (beepCount > 0) beep(beepCount, beepMs);
}

// ===================== 6. ĐIỀU KHIỂN THANH CHẮN BARIE =======================
void openGate() {
  gateServo.write(GATE_OPEN_DEG);
  isGateOpen = true;
  gateOpenedTime = millis();
  Serial.println("[GATE] Thanh chan Barie: MO (90 deg)");
}

void closeGate() {
  gateServo.write(GATE_CLOSED_DEG);
  isGateOpen = false;
  Serial.println("[GATE] Thanh chan Barie: DONG (0 deg)");
  updateLcdDefaultScreen();
}

// ===================== 7. ĐỒNG BỘ FIREBASE & NHẬN KẾT QUẢ CAMERA ============
void syncSlotsToFirebase() {
  if (WiFi.status() != WL_CONNECTED || FIREBASE_HOST.indexOf("YOUR_PROJECT_ID") != -1) {
    return;
  }

  HTTPClient http;
  String url = FIREBASE_HOST + "/parking/status.json";
  if (FIREBASE_AUTH.length() > 0) url += "?auth=" + FIREBASE_AUTH;

  String payload = "{";
  payload += "\"totalSlots\":" + String(NUM_SLOTS) + ",";
  payload += "\"availableSlots\":" + String(availableSlots) + ",";
  payload += "\"gateOpen\":" + String(isGateOpen ? "true" : "false") + ",";
  payload += "\"slots\":{";
  for (int i = 0; i < NUM_SLOTS; i++) {
    payload += "\"slot" + String(i + 1) + "\":" + String(slotOccupied[i] ? "true" : "false");
    if (i < NUM_SLOTS - 1) payload += ",";
  }
  payload += "}}";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.PUT(payload);
  http.end();
}

// Gửi yêu cầu lên Firebase để Web Dashboard + Camera điện thoại kiểm tra biển số
void requestCameraVerificationOnFirebase(const AuthorizedCard& card, const String& mode) {
  if (WiFi.status() != WL_CONNECTED || FIREBASE_HOST.indexOf("YOUR_PROJECT_ID") != -1) {
    return;
  }

  HTTPClient http;
  String url = FIREBASE_HOST + "/parking/cameraCheck.json";
  if (FIREBASE_AUTH.length() > 0) url += "?auth=" + FIREBASE_AUTH;

  // status: "PENDING" (Chờ camera), "MATCHED" (Trùng biển số), "MISMATCH" (Sai biển số)
  String payload = "{";
  payload += "\"uid\":\"" + card.uid + "\",";
  payload += "\"owner\":\"" + card.ownerName + "\",";
  payload += "\"expectedPlate\":\"" + card.licensePlate + "\",";
  payload += "\"detectedPlate\":\"\",";
  payload += "\"mode\":\"" + mode + "\",";
  payload += "\"status\":\"PENDING\"";
  payload += "}";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.PUT(payload);
  http.end();
}

// Kiểm tra xem Camera điện thoại trên Web Dashboard đã xác nhận khớp biển số chưa
void pollCameraVerificationResult() {
  if (!waitingForCameraVerify || pendingCardIndex < 0) return;

  // Nếu quá 15 giây không quét được biển số -> Hủy phiên
  if (millis() - verifyRequestTime > CAMERA_VERIFY_TIMEOUT_MS) {
    Serial.println("[CAMERA] Het thoi gian cho quet bien so (Timeout)!");
    showLcdAlert("CAMERA TIMEOUT!", "Swipe card again", 2, 200);
    waitingForCameraVerify = false;
    pendingCardIndex = -1;
    delay(1500);
    updateLcdDefaultScreen();
    return;
  }

  if (WiFi.status() != WL_CONNECTED || FIREBASE_HOST.indexOf("YOUR_PROJECT_ID") != -1) {
    return;
  }

  HTTPClient http;
  String url = FIREBASE_HOST + "/parking/cameraCheck/status.json";
  if (FIREBASE_AUTH.length() > 0) url += "?auth=" + FIREBASE_AUTH;

  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String res = http.getString();
    res.replace("\"", "");
    res.trim();

    if (res == "MATCHED") {
      // Camera xác nhận biển số TRÙNG KHỚP!
      AuthorizedCard& card = validCards[pendingCardIndex];
      bool isCheckingOut = card.isParked;
      card.isParked = !card.isParked;

      Serial.println("[CAMERA ALPR] -> BIEN SO TRUNG KHOP: " + card.licensePlate);
      showLcdAlert(isCheckingOut ? "PLATE MATCH: OUT" : "PLATE MATCH: IN", card.licensePlate, 1, 150);
      openGate();
      syncSlotsToFirebase();

      waitingForCameraVerify = false;
      pendingCardIndex = -1;
    } else if (res == "MISMATCH") {
      // Camera phát hiện biển số KHÔNG KHỚP với thẻ RFID!
      Serial.println("[CAMERA ALPR] -> CANH BAO: BIEN SO KHONG TRUNG KHOP VOI THE!");
      showLcdAlert("PLATE MISMATCH!", "Access Denied!", 4, 200);
      waitingForCameraVerify = false;
      pendingCardIndex = -1;
      delay(2000);
      updateLcdDefaultScreen();
    }
  }
  http.end();
}

// ===================== 8. ĐỌC CẢM BIẾN HỒNG NGOẠI (IR SENSORS) ==============
void readParkingSlotSensors() {
  bool stateChanged = false;
  int freeCount = 0;

  for (int i = 0; i < NUM_SLOTS; i++) {
    bool occupied = (digitalRead(IR_SLOT_PINS[i]) == LOW);
    slotOccupied[i] = occupied;
    if (!occupied) freeCount++;

    if (occupied != lastSlotState[i]) {
      stateChanged = true;
      lastSlotState[i] = occupied;
      Serial.printf("[SENSOR] O do so %d: %s\n", i + 1, occupied ? "CO XE (Occupied)" : "TRONG (Free)");
    }
  }

  availableSlots = freeCount;
  if (stateChanged) {
    if (!isGateOpen && !waitingForCameraVerify) {
      updateLcdDefaultScreen();
    }
    syncSlotsToFirebase();
  }
}

// ===================== 9. ĐỌC & XỬ LÝ THẺ TỪ NFC/RFID PN532 =================
String formatUidBytes(uint8_t* uid, uint8_t uidLength) {
  String uidStr = "";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) uidStr += "0";
    uidStr += String(uid[i], HEX);
    if (i < uidLength - 1) uidStr += " ";
  }
  uidStr.toUpperCase();
  return uidStr;
}

void handlePn532Scan() {
  if (waitingForCameraVerify) return; // Đang chờ camera xác thực thẻ vừa quẹt

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;

  // Đọc thẻ chuẩn ISO14443A (Mifare / NFC) với timeout ngắn 40ms để không làm trễ vòng lặp
  bool success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 40);
  if (!success) return;

  String scannedUid = formatUidBytes(uid, uidLength);
  Serial.println("=========================================");
  Serial.println("[PN532] Phat hien the NFC/RFID UID: " + scannedUid);

  int matchedIndex = -1;
  for (int i = 0; i < NUM_VALID_CARDS; i++) {
    if (validCards[i].uid.equalsIgnoreCase(scannedUid)) {
      matchedIndex = i;
      break;
    }
  }

  // Trường hợp 1: Thẻ lạ
  if (matchedIndex == -1) {
    Serial.println("[PN532] -> TU CHOI: The khong hop le!");
    showLcdAlert("ACCESS DENIED!", "UID:" + scannedUid, 3, 150);
    delay(1500);
    updateLcdDefaultScreen();
    return;
  }

  AuthorizedCard& card = validCards[matchedIndex];

  // Trường hợp 2: Xe muốn vào nhưng bãi đã đầy
  if (!card.isParked && availableSlots <= 0) {
    Serial.println("[PN532] -> TU CHOI: Bai do xe da day!");
    showLcdAlert("PARKING FULL!", "Please wait...", 2, 250);
    delay(1500);
    updateLcdDefaultScreen();
    return;
  }

  // Trường hợp 3: Thẻ hợp lệ -> Yêu cầu Camera điện thoại đối chiếu biển số
  if (REQUIRE_CAMERA_PLATE_MATCH && WiFi.status() == WL_CONNECTED) {
    pendingCardIndex = matchedIndex;
    waitingForCameraVerify = true;
    verifyRequestTime = millis();

    String mode = card.isParked ? "CHECK_OUT" : "CHECK_IN";
    Serial.println("[PN532] The hop le (" + card.licensePlate + "). Dang cho Camera dien thoai xac thuc bien so...");
    showLcdAlert("CHECKING PLATE..", card.licensePlate, 1, 80);
    requestCameraVerificationOnFirebase(card, mode);
  } else {
    // Nếu chạy Offline không bật Camera -> Mở cổng trực tiếp
    card.isParked = !card.isParked;
    showLcdAlert(card.isParked ? "WELCOME!" : "GOODBYE!", card.licensePlate, 1, 120);
    openGate();
    syncSlotsToFirebase();
  }
}

// ===================== 10. SETUP & LOOP CHÍNH ===============================
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- KHOI DONG SMART PARKING (PN532 + CAMERA ALPR) ---");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  for (int i = 0; i < NUM_SLOTS; i++) {
    pinMode(IR_SLOT_PINS[i], INPUT_PULLUP);
  }
  pinMode(IR_GATE_PIN, INPUT_PULLUP);

  // Khởi tạo Servo Barie
  ESP32PWM::allocateTimer(0);
  gateServo.setPeriodHertz(50);
  gateServo.attach(SERVO_PIN, 500, 2400);
  gateServo.write(GATE_CLOSED_DEG);

  // Khởi tạo Bus I2C (Dùng chung cho LCD 1602 và PN532)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("SMART PARKING");
  lcd.setCursor(0, 1);
  lcd.print("Init PN532 NFC..");

  // Khởi tạo module PN532
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("[PN532] CANH BAO: Khong tim thay module PN532! Kiem tra cong tac I2C (SEL0=ON, SEL1=OFF).");
  } else {
    Serial.print("[PN532] Tim thay chip PN5");
    Serial.println((versiondata >> 24) & 0xFF, HEX);
    nfc.SAMConfig();
  }

  // Kết nối Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Dang ket noi Wi-Fi");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 8000) {
    delay(400);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Da ket noi! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] Chay o che do Offline.");
  }

  beep(2, 80);
  readParkingSlotSensors();
  updateLcdDefaultScreen();
}

void loop() {
  // 1. Đọc cảm biến IR tại 3 ô đỗ xe
  readParkingSlotSensors();

  // 2. Đọc thẻ NFC/RFID từ module PN532
  handlePn532Scan();

  // 3. Nếu đang chờ Camera điện thoại kiểm tra biển số, hỏi kết quả từ Cloud
  if (waitingForCameraVerify && millis() - lastCloudPoll >= CLOUD_POLL_INTERVAL_MS) {
    lastCloudPoll = millis();
    pollCameraVerificationResult();
  }

  // 4. Đóng thanh chắn Barie tự động & an toàn
  if (isGateOpen) {
    bool vehicleAtGate = (digitalRead(IR_GATE_PIN) == LOW);
    if (vehicleAtGate) {
      gateOpenedTime = millis();
    } else if (millis() - gateOpenedTime >= GATE_TIMEOUT_MS) {
      closeGate();
      syncSlotsToFirebase();
    }
  }
}
