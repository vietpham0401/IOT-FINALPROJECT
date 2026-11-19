// Danh sách thẻ NFC/RFID PN532 hợp lệ (đồng bộ với ESP32 & Wokwi)
const authorizedDirectory = {
  "01 02 03 04": { owner: "Nguyễn Văn A", plate: "29A-123.45", isParked: false },
  "11 22 33 44": { owner: "Trần Thị B",   plate: "30F-678.90", isParked: false },
  "55 66 77 88": { owner: "Lê Hoàng C",   plate: "51G-888.88", isParked: false }
};

let parkingState = {
  totalSlots: 2,
  gateOpen: false,
  slots: [false, false]
};

// Phiên chờ xác thực 2 lớp (Sau khi quẹt thẻ PN532 -> Chờ Camera điện thoại quét biển số)
let pendingVerification = null;

let accessLogs = [];
let firebaseBaseUrl = localStorage.getItem("smart_parking_fb_url") || "";
let pollTimer = null;
let ipCamTimer = null;
let usingIpCam = false;

// DOM Elements
const slotsContainer = document.getElementById("slotsContainer");
const totalSlotsVal = document.getElementById("totalSlotsVal");
const freeSlotsVal = document.getElementById("freeSlotsVal");
const occupiedSlotsVal = document.getElementById("occupiedSlotsVal");
const parkingStateText = document.getElementById("parkingStateText");
const gateStatusVal = document.getElementById("gateStatusVal");
const barrierArm = document.getElementById("barrierArm");
const lastScannedUid = document.getElementById("lastScannedUid");
const logTableBody = document.getElementById("logTableBody");
const connBadge = document.getElementById("connBadge");

// DOM Elements cho Camera & Đối chiếu biển số
const cameraVideo = document.getElementById("cameraVideo");
const ipCamImage = document.getElementById("ipCamImage");
const captureCanvas = document.getElementById("captureCanvas");
const ocrStatusText = document.getElementById("ocrStatusText");
const verifyCardUid = document.getElementById("verifyCardUid");
const verifyCardOwner = document.getElementById("verifyCardOwner");
const expectedPlateVal = document.getElementById("expectedPlateVal");
const detectedPlateVal = document.getElementById("detectedPlateVal");
const manualPlateInput = document.getElementById("manualPlateInput");
const matchResultBanner = document.getElementById("matchResultBanner");

// Chuẩn hóa chuỗi biển số để so sánh chính xác (VD: "29A-123.45" -> "29A12345")
function normalizePlate(str) {
  if (!str) return "";
  return str.toUpperCase().replace(/[^A-Z0-9]/g, "");
}

function renderDashboard() {
  const occupiedCount = parkingState.slots.filter(Boolean).length;
  const freeCount = parkingState.totalSlots - occupiedCount;

  totalSlotsVal.textContent = parkingState.totalSlots;
  freeSlotsVal.textContent = freeCount;
  occupiedSlotsVal.textContent = occupiedCount;
  parkingStateText.textContent = freeCount > 0 ? `Còn ${freeCount} chỗ trống` : "BÃI ĐÃ ĐẦY (FULL)";

  if (parkingState.gateOpen) {
    gateStatusVal.textContent = "ĐANG MỞ (90°)";
    gateStatusVal.className = "stat-value gate-open";
    barrierArm.classList.add("open");
  } else {
    gateStatusVal.textContent = "ĐANG ĐÓNG (0°)";
    gateStatusVal.className = "stat-value gate-closed";
    barrierArm.classList.remove("open");
  }

  slotsContainer.innerHTML = "";
  parkingState.slots.forEach((isOccupied, idx) => {
    const slotEl = document.createElement("div");
    slotEl.className = `slot-box ${isOccupied ? "occupied" : "free"}`;
    slotEl.innerHTML = `
      <span class="slot-title">Ô ĐỖ S${idx + 1} (LM393 - GPIO ${[32, 33][idx]})</span>
      <div class="slot-icon">${isOccupied ? "🚗" : "🅿️"}</div>
      <span class="slot-badge">${isOccupied ? "ĐÃ CÓ XE (IR = LOW)" : "CÒN TRỐNG (IR = HIGH)"}</span>
    `;
    slotEl.addEventListener("click", () => {
      parkingState.slots[idx] = !parkingState.slots[idx];
      renderDashboard();
    });
    slotsContainer.appendChild(slotEl);
  });
}

function renderLogs() {
  if (accessLogs.length === 0) {
    logTableBody.innerHTML = `<tr><td colspan="4" style="color:#94a3b8;text-align:center;">Chưa có lượt kiểm soát nào</td></tr>`;
    return;
  }

  logTableBody.innerHTML = accessLogs
    .slice(0, 15)
    .map((item) => {
      let tagClass = "tag-denied";
      let label = "TỪ CHỐI";
      if (item.action === "CHECK_IN") {
        tagClass = "tag-in";
        label = "KHỚP BIỂN → MỞ CỔNG VÀO";
      } else if (item.action === "CHECK_OUT") {
        tagClass = "tag-out";
        label = "KHỚP BIỂN → MỞ CỔNG RA";
      } else if (item.action === "PLATE_MISMATCH") {
        tagClass = "tag-denied";
        label = "SAI BIỂN SỐ (CẢNH BÁO)";
      } else if (item.action === "DENIED_FULL") {
        tagClass = "tag-denied";
        label = "BÃI ĐẦY";
      }
      return `
        <tr>
          <td>${item.time}</td>
          <td><code>${item.uid}</code><br/><small style="color:#94a3b8">${item.owner}</small></td>
          <td>Thẻ: <b>${item.expectedPlate}</b><br/>Cam: <b style="color:#fbbf24">${item.detectedPlate}</b></td>
          <td><span class="tag ${tagClass}">${label}</span></td>
        </tr>
      `;
    })
    .join("");
}

function triggerGateTemporarily() {
  parkingState.gateOpen = true;
  renderDashboard();
  setTimeout(() => {
    parkingState.gateOpen = false;
    renderDashboard();
  }, 4500);
}

// BƯỚC 1: KHI QUẸT THẺ PN532 -> ĐƯA VÀO TRẠNG THÁI CHỜ CAMERA XÁC THỰC BIỂN SỐ
function handlePn532Swipe(uid) {
  const nowStr = new Date().toLocaleTimeString("vi-VN");
  lastScannedUid.textContent = `UID: ${uid}`;
  const card = authorizedDirectory[uid];
  const freeCount = parkingState.slots.filter((s) => !s).length;

  if (!card) {
    pendingVerification = null;
    verifyCardUid.textContent = uid;
    verifyCardOwner.textContent = "Chủ xe: Không xác định (Thẻ lạ)";
    expectedPlateVal.textContent = "KHÔNG HỢP LỆ";
    matchResultBanner.className = "match-banner match-error";
    matchResultBanner.textContent = `❌ Thẻ PN532 (${uid}) không có trong hệ thống! Từ chối ngay từ Bước 1.`;

    accessLogs.unshift({
      time: nowStr,
      uid,
      owner: "Thẻ lạ",
      expectedPlate: "N/A",
      detectedPlate: "---",
      action: "DENIED_INVALID"
    });
    renderLogs();
    return;
  }

  if (!card.isParked && freeCount <= 0) {
    pendingVerification = null;
    matchResultBanner.className = "match-banner match-error";
    matchResultBanner.textContent = `⚠️ Bãi đỗ xe đã hết chỗ trống! Không thể nhận thêm xe ${card.plate}.`;
    accessLogs.unshift({
      time: nowStr,
      uid,
      owner: card.owner,
      expectedPlate: card.plate,
      detectedPlate: "---",
      action: "DENIED_FULL"
    });
    renderLogs();
    return;
  }

  // Tạo phiên chờ Camera điện thoại xác nhận biển số
  pendingVerification = {
    uid,
    owner: card.owner,
    expectedPlate: card.plate,
    mode: card.isParked ? "CHECK_OUT" : "CHECK_IN"
  };

  verifyCardUid.textContent = uid;
  verifyCardOwner.textContent = `Chủ xe: ${card.owner} (${card.isParked ? "Xe đang muốn RA" : "Xe đang muốn VÀO"})`;
  expectedPlateVal.textContent = card.plate;
  detectedPlateVal.textContent = "Đang chờ Camera...";
  manualPlateInput.value = card.plate;

  matchResultBanner.className = "match-banner match-pending";
  matchResultBanner.textContent = `📸 Đã nhận thẻ PN532 (${uid}). Đang chờ Camera điện thoại quét biển số xem có khớp với [${card.plate}] không...`;
}

// BƯỚC 2: ĐỐI CHIẾU BIỂN SỐ TỪ CAMERA VỚI BIỂN SỐ ĐĂNG KÝ TRÊN THẺ PN532
async function verifyDetectedPlate(rawCameraText) {
  const cleanedDisplay = rawCameraText.trim().toUpperCase();
  detectedPlateVal.textContent = cleanedDisplay || "KHÔNG ĐỌC ĐƯỢC";

  if (!pendingVerification) {
    matchResultBanner.className = "match-banner match-error";
    matchResultBanner.textContent = "⚠️ Vui lòng quẹt thẻ PN532 ở Bước 1 trước khi đối chiếu biển số!";
    return;
  }

  const normExpected = normalizePlate(pendingVerification.expectedPlate);
  const normDetected = normalizePlate(cleanedDisplay);
  const nowStr = new Date().toLocaleTimeString("vi-VN");
  const card = authorizedDirectory[pendingVerification.uid];

  // Kiểm tra trùng khớp (hoặc chuỗi OCR chứa chính xác biển số đã chuẩn hóa)
  const isMatched = normDetected.length >= 5 && (normDetected === normExpected || normDetected.includes(normExpected));

  if (isMatched) {
    matchResultBanner.className = "match-banner match-success";
    matchResultBanner.textContent = `✅ XÁC THỰC 2 LỚP THÀNH CÔNG! Biển số Camera [${cleanedDisplay}] TRÙNG KHỚP với thẻ PN532 [${pendingVerification.expectedPlate}]. Đang mở Barie!`;

    if (card) {
      if (pendingVerification.mode === "CHECK_IN") {
        card.isParked = true;
        const freeIdx = parkingState.slots.findIndex((s) => s === false);
        if (freeIdx !== -1) parkingState.slots[freeIdx] = true;
      } else {
        card.isParked = false;
        const occIdx = parkingState.slots.findIndex((s) => s === true);
        if (occIdx !== -1) parkingState.slots[occIdx] = false;
      }
    }

    triggerGateTemporarily();
    accessLogs.unshift({
      time: nowStr,
      uid: pendingVerification.uid,
      owner: pendingVerification.owner,
      expectedPlate: pendingVerification.expectedPlate,
      detectedPlate: cleanedDisplay,
      action: pendingVerification.mode
    });

    await updateFirebaseCameraStatus("MATCHED", cleanedDisplay);
    pendingVerification = null;
  } else {
    matchResultBanner.className = "match-banner match-error";
    matchResultBanner.textContent = `🚨 CẢNH BÁO AN NINH! Biển số Camera [${cleanedDisplay}] KHÔNG KHỚP với biển số đăng ký trên thẻ PN532 [${pendingVerification.expectedPlate}]! Giữ đóng Barie!`;

    accessLogs.unshift({
      time: nowStr,
      uid: pendingVerification.uid,
      owner: pendingVerification.owner,
      expectedPlate: pendingVerification.expectedPlate,
      detectedPlate: cleanedDisplay,
      action: "PLATE_MISMATCH"
    });

    await updateFirebaseCameraStatus("MISMATCH", cleanedDisplay);
  }

  renderDashboard();
  renderLogs();
}

// ===================== XỬ LÝ CAMERA ĐIỆN THOẠI & OCR TESSERACT.JS ===========
document.getElementById("btnStartWebCam").addEventListener("click", async () => {
  try {
    usingIpCam = false;
    if (ipCamTimer) clearInterval(ipCamTimer);
    ipCamImage.classList.add("hidden");
    cameraVideo.classList.remove("hidden");

    const stream = await navigator.mediaDevices.getUserMedia({
      video: { facingMode: "environment", width: { ideal: 640 }, height: { ideal: 480 } },
      audio: false
    });
    cameraVideo.srcObject = stream;
    ocrStatusText.textContent = "Đã bật Camera trực tiếp";
  } catch (err) {
    alert("Không thể mở Camera trình duyệt. Bạn có thể dùng chế độ 'IP Webcam App' hoặc nhập biển số test bên phải! Lỗi: " + err.message);
  }
});

document.getElementById("btnUseIpCam").addEventListener("click", () => {
  document.getElementById("ipCamConfigBar").classList.toggle("hidden");
});

document.getElementById("btnConnectIpCam").addEventListener("click", () => {
  const url = document.getElementById("ipCamUrlInput").value.trim();
  if (!url) return;
  usingIpCam = true;
  if (cameraVideo.srcObject) {
    cameraVideo.srcObject.getTracks().forEach((t) => t.stop());
  }
  cameraVideo.classList.add("hidden");
  ipCamImage.classList.remove("hidden");

  if (ipCamTimer) clearInterval(ipCamTimer);
  const refreshFrame = () => {
    const sep = url.includes("?") ? "&" : "?";
    ipCamImage.src = `${url}${sep}t=${Date.now()}`;
  };
  refreshFrame();
  ipCamTimer = setInterval(refreshFrame, 600);
  ocrStatusText.textContent = "Đang nhận luồng từ IP Webcam ĐT";
});

// Chụp khung hình từ Camera và nhận diện chữ bằng Tesseract.js OCR
document.getElementById("btnScanOcr").addEventListener("click", async () => {
  const ctx = captureCanvas.getContext("2d");
  captureCanvas.width = 480;
  captureCanvas.height = 280;

  if (usingIpCam && !ipCamImage.classList.contains("hidden")) {
    ctx.drawImage(ipCamImage, 0, 0, captureCanvas.width, captureCanvas.height);
  } else if (cameraVideo.srcObject) {
    ctx.drawImage(cameraVideo, 0, 0, captureCanvas.width, captureCanvas.height);
  } else {
    alert("Bạn chưa bật Camera! Hãy bấm 'Bật Camera Điện Thoại / Webcam' trước, hoặc dùng nút Test Nhanh bên phải.");
    return;
  }

  ocrStatusText.textContent = "⏳ AI đang đọc biển số (OCR)...";
  try {
    const result = await Tesseract.recognize(captureCanvas, "eng");
    const rawText = (result.data.text || "").replace(/\s+/g, " ").trim();
    ocrStatusText.textContent = `OCR hoàn tất: "${rawText || "Không rõ chữ"}"`;
    if (rawText) {
      manualPlateInput.value = rawText;
      verifyDetectedPlate(rawText);
    }
  } catch (err) {
    ocrStatusText.textContent = "Lỗi OCR: " + err.message;
  }
});

// Nút đối chiếu thủ công & Nút test nhanh khi thuyết trình
document.getElementById("btnVerifyPlateNow").addEventListener("click", () => {
  verifyDetectedPlate(manualPlateInput.value);
});

document.getElementById("btnSimMatch").addEventListener("click", () => {
  if (!pendingVerification) {
    // Nếu chưa quẹt thẻ, tự động quẹt thẻ Xe 1 luôn để demo mượt
    handlePn532Swipe("01 02 03 04");
  }
  const exactPlate = pendingVerification.expectedPlate;
  manualPlateInput.value = exactPlate;
  verifyDetectedPlate(exactPlate);
});

document.getElementById("btnSimMismatch").addEventListener("click", () => {
  if (!pendingVerification) {
    handlePn532Swipe("01 02 03 04");
  }
  const fakePlate = "99Z-999.99";
  manualPlateInput.value = fakePlate;
  verifyDetectedPlate(fakePlate);
});

// Gắn sự kiện nút mô phỏng quẹt thẻ PN532
document.querySelectorAll("[data-uid]").forEach((btn) => {
  btn.addEventListener("click", () => {
    handlePn532Swipe(btn.getAttribute("data-uid"));
  });
});

document.getElementById("btnToggleGate").addEventListener("click", () => {
  parkingState.gateOpen = !parkingState.gateOpen;
  renderDashboard();
});

document.getElementById("btnClearLogs").addEventListener("click", () => {
  accessLogs = [];
  renderLogs();
});

// ===================== ĐỒNG BỘ FIREBASE REALTIME DATABASE ===================
const firebaseModal = document.getElementById("firebaseModal");
const firebaseUrlInput = document.getElementById("firebaseUrlInput");

document.getElementById("btnConfigFirebase").addEventListener("click", () => {
  firebaseUrlInput.value = firebaseBaseUrl;
  firebaseModal.classList.remove("hidden");
});

document.getElementById("btnCancelModal").addEventListener("click", () => {
  firebaseModal.classList.add("hidden");
});

document.getElementById("btnSaveFirebase").addEventListener("click", () => {
  firebaseBaseUrl = firebaseUrlInput.value.trim().replace(/\/$/, "");
  localStorage.setItem("smart_parking_fb_url", firebaseBaseUrl);
  firebaseModal.classList.add("hidden");
  startFirebaseSync();
});

async function updateFirebaseCameraStatus(statusStr, detectedPlateStr) {
  if (!firebaseBaseUrl) return;
  try {
    await fetch(`${firebaseBaseUrl}/parking/cameraCheck.json`, {
      method: "PATCH",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        status: statusStr,
        detectedPlate: detectedPlateStr
      })
    });
  } catch (e) {
    console.warn("Không thể gửi kết quả Camera lên Firebase:", e);
  }
}

async function fetchFromFirebase() {
  if (!firebaseBaseUrl) return;
  try {
    const res = await fetch(`${firebaseBaseUrl}/parking.json`);
    if (!res.ok) return;
    const data = await res.json();
    if (!data) return;

    if (data.status && data.status.slots) {
      parkingState.gateOpen = Boolean(data.status.gateOpen);
      parkingState.slots = [
        Boolean(data.status.slots.slot1),
        Boolean(data.status.slots.slot2)
      ];
      renderDashboard();
    }

    // Nếu mạch ESP32 thật vừa quẹt thẻ PN532 và gửi trạng thái PENDING lên Firebase
    if (
      data.cameraCheck &&
      data.cameraCheck.status === "PENDING" &&
      (!pendingVerification || pendingVerification.uid !== data.cameraCheck.uid)
    ) {
      pendingVerification = {
        uid: data.cameraCheck.uid,
        owner: data.cameraCheck.owner || "Chủ thẻ",
        expectedPlate: data.cameraCheck.expectedPlate,
        mode: data.cameraCheck.mode || "CHECK_IN"
      };
      verifyCardUid.textContent = pendingVerification.uid;
      verifyCardOwner.textContent = `Chủ xe: ${pendingVerification.owner}`;
      expectedPlateVal.textContent = pendingVerification.expectedPlate;
      matchResultBanner.className = "match-banner match-pending";
      matchResultBanner.textContent = `📸 Mạch ESP32 vừa nhận thẻ PN532 (${pendingVerification.uid}). Hãy bấm 'Chụp & Đọc Biển Số' trên Camera điện thoại!`;
    }
  } catch (err) {
    console.warn("Firebase sync error:", err);
  }
}

function startFirebaseSync() {
  if (pollTimer) clearInterval(pollTimer);
  if (firebaseBaseUrl && firebaseBaseUrl.startsWith("https://")) {
    connBadge.textContent = "Đã kết nối Firebase Cloud";
    connBadge.className = "badge badge-live";
    fetchFromFirebase();
    pollTimer = setInterval(fetchFromFirebase, 1200);
  } else {
    connBadge.textContent = "Chế độ Mô phỏng (Simulator)";
    connBadge.className = "badge badge-sim";
  }
}

// Khởi chạy ban đầu
renderDashboard();
renderLogs();
startFirebaseSync();
