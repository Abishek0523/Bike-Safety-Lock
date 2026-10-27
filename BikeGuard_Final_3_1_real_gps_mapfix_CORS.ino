  #include <WiFi.h>
  #include <WebServer.h>
  #include <TinyGPS++.h>
  #include <HardwareSerial.h>
  #include <Wire.h>
  #include <LiquidCrystal_I2C.h>
  #include <Keypad.h> // Install "Keypad" by Mark Stanley & Alexander Brevig via Library Manager

  LiquidCrystal_I2C lcd(0x27, 16, 2);

  // Shows two lines of status text on the physical LCD, kept in sync with bikeStatus
  void updateLCD(String line1, String line2) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1);
    lcd.setCursor(0, 1);
    lcd.print(line2);
  }

  const char* ssid = "BikeGuard_01";
  const char* password = "bikeguard123"; // at least 8 characters

  // OPTIONAL — real map tiles need internet on the device viewing the dashboard.
  // Put your phone-hotspot / home-WiFi name and password here and the ESP32 will join it
  // (in addition to its own BikeGuard_01 network). Then open the dashboard from a device on
  // that same WiFi using the IP shown in the Serial Monitor. Leave "" for access-point-only.
  const char* STA_SSID = "Sv12_5G";
  const char* STA_PASS = "8374702248";

  WebServer server(80);
  TinyGPSPlus gps;
  HardwareSerial gpsSerial(2);

  const int MPU_ADDR = 0x68;
  const int SDA_PIN = 4;
  const int SCL_PIN = 5;

  // Motion sensitivity: higher = less sensitive, lower = more sensitive.
  // Raw accelerometer values from the MPU6050 sit around 16000-17000 per axis at rest
  // (gravity on one axis). A sudden bump/movement pushes the total change past this.
  const long MOTION_THRESHOLD = 3000;

  const int RELAY_PIN = 23;

  // Buzzer: active-HIGH module (or a plain piezo/transistor-driven buzzer) on this pin.
  // Change the pin if 25 is already used by something else on your wiring.
  const int BUZZER_PIN = 25;

  long baselineX = 0, baselineY = 0, baselineZ = 0;
  bool baselineSet = false;

  String bikeStatus = "secure"; // secure | motion | running | theft

  // Relay logic: this module is active-LOW and powered from 5V.
  //   ignition ALLOWED -> drive the pin LOW  (relay energized, COM connects to NO)
  //   ignition CUT     -> release the pin (INPUT) so the module's own 5V pull-up switches
  //                       the relay fully OFF. (The ESP32's 3.3V HIGH is not high enough to
  //                       switch a 5V active-LOW input off, which left the relay stuck on.)
  void setIgnition(bool allowed) {
    if (allowed) {
      pinMode(RELAY_PIN, OUTPUT);
      digitalWrite(RELAY_PIN, LOW);
    } else {
      pinMode(RELAY_PIN, INPUT);
    }
  }

  // Buzzer control: ON for "motion" (just triggered) and "theft" (confirmed alarm),
  // OFF once the owner confirms, locks, or resets the system.
  // Buzzer control: "off" (silent), "pulse" (beep-beep while motion is being confirmed),
  // or "steady" (continuous alarm once theft is confirmed). Timing is handled non-blockingly
  // in updateBuzzer(), called every loop() cycle, so it never freezes the website/GPS/keypad.
  String buzzerMode = "off";
  unsigned long buzzerLastToggle = 0;
  bool buzzerPinState = false;
  int buzzerPulseStep = 0; // 0=beep, 1=gap, 2=beep, 3=pause

  void setBuzzerMode(String mode) {
    buzzerMode = mode;
    buzzerLastToggle = millis();
    if (mode == "pulse") {
      buzzerPulseStep = 0;            // start the pattern from the first beep...
      digitalWrite(BUZZER_PIN, HIGH); // ...and sound it immediately
      buzzerPinState = true;
    } else if (mode == "steady") {
      digitalWrite(BUZZER_PIN, HIGH);
      buzzerPinState = true;
    } else if (mode == "off") {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerPinState = false;
    }
    // "pulse" mode is handled by updateBuzzer() below
  }

  void updateBuzzer() {
    if (buzzerMode != "pulse") return;
    // Beep-beep pattern: 150ms on, 150ms off, 150ms on, then 500ms pause, repeating
    unsigned long elapsed = millis() - buzzerLastToggle;
    const int stepDurations[4] = {150, 150, 150, 500}; // on, off, on, pause

    if (elapsed >= (unsigned long)stepDurations[buzzerPulseStep]) {
      buzzerPulseStep = (buzzerPulseStep + 1) % 4;
      buzzerLastToggle = millis();
      buzzerPinState = (buzzerPulseStep == 0 || buzzerPulseStep == 2); // ON during steps 0 and 2
      digitalWrite(BUZZER_PIN, buzzerPinState ? HIGH : LOW);
    }
  }

  // ---- 4x4 matrix keypad: enter the unlock PIN on-site when the bike is triggered ----
  const byte KEYPAD_ROWS = 4;
  const byte KEYPAD_COLS = 4;
  char keypadKeys[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
  };
  // Wiring: pick 8 free GPIOs on your DevKit. These don't clash with the relay,
  // buzzer, I2C bus, or GPS UART used elsewhere in this sketch — rewire here if needed.
  byte keypadRowPins[KEYPAD_ROWS] = {27, 14, 13, 19}; // R1..R4
  byte keypadColPins[KEYPAD_COLS] = {26, 33, 32, 15}; // C1..C4
  Keypad keypad = Keypad(makeKeymap(keypadKeys), keypadRowPins, keypadColPins, KEYPAD_ROWS, KEYPAD_COLS);

  String LOCK_PIN = "1234"; // change this to your own 4-digit unlock code, or update it from Settings
  String enteredPin = "";
  int pinAttempts = 0;
  const int MAX_PIN_ATTEMPTS = 3; // too many wrong tries = treated as theft

  // ---- SIM800L GSM module: sends SMS alerts ----
  // Free pins that don't clash with GPS (16/17), I2C (4/5), relay (23), buzzer (25), or the keypad.
  HardwareSerial sim800(1);
  const int SIM800_RX = 18; // ESP32 receives here <- SIM800L TX
  const int SIM800_TX = 21; // ESP32 sends here    -> SIM800L RX
  const String OWNER_PHONE = "+916301951597"; // <-- put your real number here, with country code

  unsigned long lastTheftSmsTime = 0;
  const unsigned long THEFT_SMS_INTERVAL = 60000; // repeat location updates every 60s while theft is active

  const char htmlPage[] PROGMEM = R"rawliteral(
  <!DOCTYPE html>
  <html lang="en">
  <head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Sentry — Bike Security Console</title>
  <!-- Google Fonts removed: ESP32's local WiFi has no internet, falls back to system fonts -->
  <style>
    :root{
      --bg:#0E1116;
      --panel:#161B22;
      --panel-raised:#1C222B;
      --line:#262E38;
      --text:#E9ECF1;
      --text-dim:#8B95A5;
      --text-faint:#5A6472;
      --safe:#3ED598;
      --safe-dim:#1E3A30;
      --alert:#FF5A5F;
      --alert-dim:#3A2020;
      --route:#4FA3FF;
      --warn:#F5B94D;
      --radius:10px;
    }
    *{box-sizing:border-box; margin:0; padding:0;}
    html,body{height:100%;}
    body{
      background:var(--bg);
      color:var(--text);
      font-family:'Space Grotesk', sans-serif;
      -webkit-font-smoothing:antialiased;
      display:flex;
      min-height:100vh;
      overflow-x:hidden;
    }
    .mono{ font-family:'JetBrains Mono', monospace; }

    /* ---------- Sidebar ---------- */
    .sidebar{
      width:220px;
      flex-shrink:0;
      background:var(--panel);
      border-right:1px solid var(--line);
      display:flex;
      flex-direction:column;
      padding:20px 12px;
    }
    .brand{
      display:flex;
      align-items:center;
      gap:10px;
      padding:6px 10px 22px 10px;
    }
    .brand-mark{
      width:30px; height:30px;
      border-radius:8px;
      background:radial-gradient(circle at 30% 30%, var(--safe), #14382a);
      position:relative;
      flex-shrink:0;
    }
    .brand-mark::after{
      content:"";
      position:absolute; inset:9px;
      border-radius:50%;
      border:2px solid var(--bg);
    }
    .brand-name{ font-weight:700; font-size:16px; letter-spacing:-0.01em; }
    .brand-sub{ font-size:11px; color:var(--text-faint); margin-top:-2px; }

    .bike-switch{
      background:var(--panel-raised);
      border:1px solid var(--line);
      border-radius:var(--radius);
      padding:10px 12px;
      margin-bottom:18px;
      cursor:pointer;
    }
    .bike-switch .label{ font-size:10px; color:var(--text-faint); text-transform:uppercase; letter-spacing:.06em;}
    .bike-switch .name{ font-size:13px; font-weight:600; margin-top:2px; display:flex; align-items:center; justify-content:space-between;}
    .bike-switch select{
      width:100%; background:transparent; border:none; color:var(--text);
      font-family:inherit; font-size:13px; font-weight:600; margin-top:2px; cursor:pointer;
    }
    .bike-switch select:focus{ outline:none; }
    .bike-switch option{ background:var(--panel-raised); }

    nav{ display:flex; flex-direction:column; gap:2px; flex:1; }
    .nav-item{
      display:flex; align-items:center; gap:10px;
      padding:9px 10px;
      border-radius:8px;
      color:var(--text-dim);
      font-size:13.5px;
      cursor:pointer;
      border:1px solid transparent;
      transition:background .12s ease, color .12s ease;
      position:relative;
    }
    .nav-item:hover{ background:var(--panel-raised); color:var(--text); }
    .nav-item.active{ background:var(--panel-raised); color:var(--text); border-color:var(--line); }
    .nav-item.active::before{
      content:"";
      position:absolute; left:-12px; top:50%; transform:translateY(-50%);
      width:3px; height:16px; background:var(--safe); border-radius:2px;
    }
    .nav-icon{ width:16px; height:16px; flex-shrink:0; opacity:.9; }
    .nav-badge{
      margin-left:auto;
      background:var(--alert);
      color:#1a0a0a;
      font-size:10px;
      font-weight:700;
      padding:1px 6px;
      border-radius:20px;
    }

    .sidebar-foot{
      border-top:1px solid var(--line);
      padding-top:12px;
      margin-top:8px;
      display:flex;
      align-items:center;
      gap:10px;
      font-size:12.5px;
      color:var(--text-dim);
    }
    .avatar{
      width:28px; height:28px; border-radius:50%;
      background:linear-gradient(135deg,#4FA3FF,#3ED598);
      display:flex; align-items:center; justify-content:center;
      font-size:11px; font-weight:700; color:#0b0e12;
      flex-shrink:0;
    }

    /* ---------- Main ---------- */
    main{ flex:1; min-width:0; padding:28px 36px 60px; max-width:1180px; }
    .topline{
      display:flex; align-items:baseline; justify-content:space-between;
      margin-bottom:24px; flex-wrap:wrap; gap:10px;
    }
    h1{ font-size:22px; font-weight:600; letter-spacing:-0.01em; }
    .topline .meta{ font-size:12.5px; color:var(--text-faint); }
    .view{ display:none; animation:fade .25s ease; }
    .view.active{ display:block; }
    @keyframes fade{ from{opacity:0; transform:translateY(4px);} to{opacity:1; transform:none;} }

    /* ---------- Cards ---------- */
    .grid{ display:grid; gap:16px; }
    .grid-4{ grid-template-columns:repeat(4,1fr); }
    .grid-2{ grid-template-columns:1.3fr 1fr; }
    @media (max-width:900px){ .grid-4{ grid-template-columns:repeat(2,1fr);} .grid-2{ grid-template-columns:1fr;} }

    .card{
      background:var(--panel);
      border:1px solid var(--line);
      border-radius:var(--radius);
      padding:18px;
    }
    .stat-label{ font-size:12px; color:var(--text-faint); display:flex; align-items:center; gap:6px; }
    .stat-value{ font-size:26px; font-weight:600; margin-top:8px; font-family:'JetBrains Mono', monospace; }
    .stat-value.small{ font-size:16px; }
    .pill{
      display:inline-flex; align-items:center; gap:6px;
      font-size:12px; font-weight:600; padding:4px 10px; border-radius:20px;
    }
    .pill.safe{ background:var(--safe-dim); color:var(--safe); }
    .pill.alert{ background:var(--alert-dim); color:var(--alert); }
    .dot{ width:6px; height:6px; border-radius:50%; background:currentColor; }
    .dot.pulse{ animation:pulse 1.6s infinite; }
    @keyframes pulse{ 0%,100%{opacity:1;} 50%{opacity:.35;} }

    /* ---------- Radar / Map ---------- */
    .radar-wrap{
      position:relative;
      border-radius:var(--radius);
      overflow:hidden;
      border:1px solid var(--line);
      background:
        radial-gradient(circle at 50% 50%, #16202a 0%, #0c1015 75%);
      height:340px;
    }
    .radar-grid{ position:absolute; inset:0; }
    .radar-sweep{
      position:absolute; left:50%; top:50%;
      width:82%; height:82%;
      transform:translate(-50%,-50%);
      border-radius:50%;
      overflow:hidden;
      pointer-events:none;
    }
    .radar-sweep::before{
      content:"";
      position:absolute; inset:-50%;
      background:conic-gradient(from 0deg, rgba(62,213,152,.5) 0deg, rgba(62,213,152,.14) 45deg, transparent 100deg, transparent 360deg);
      animation:sweep 4s linear infinite;
    }
    @keyframes sweep{ to{ transform:rotate(360deg); } }
    .bike-pin{
      position:absolute; left:50%; top:50%;
      width:18px; height:18px; border-radius:50%;
      background:var(--safe);
      box-shadow:0 0 0 6px rgba(62,213,152,.18), 0 0 0 12px rgba(62,213,152,.08);
      transform:translate(-50%,-50%);
      z-index:2;
    }
    .radar-blip{
      position:absolute; width:16px; height:16px; margin:-8px 0 0 -8px;
      border:1px solid rgba(62,213,152,.5); border-radius:50%;
    }
    .radar-blip::after{
      content:""; position:absolute; left:50%; top:50%; width:4px; height:4px;
      background:rgba(62,213,152,.85); border-radius:50%; transform:translate(-50%,-50%);
    }
    .radar-blip.small{ width:10px; height:10px; margin:-5px 0 0 -5px; }
    .route-line{ position:absolute; inset:0; }
    /* real map (Leaflet) sits behind the radar until its tiles actually load */
    .live-map{ position:absolute; inset:0; z-index:1; visibility:hidden; }
    .radar-wrap.has-map .live-map{ visibility:visible; }
    .radar-wrap.has-map .radar-grid, .radar-wrap.has-map .route-line, .radar-wrap.has-map .radar-sweep,
    .radar-wrap.has-map .bike-pin, .radar-wrap.has-map .radar-blip{ display:none; }
    .leaflet-container{ background:#0c1015; font-family:inherit; }
    .map-note{ font-size:12px; color:var(--text-faint); margin:10px 2px 0; line-height:1.5; }
    .map-caption{
      z-index:1000;
      position:absolute; bottom:12px; left:14px; font-size:11px; color:var(--text-dim);
      background:rgba(14,17,22,.7); padding:5px 10px; border-radius:6px; backdrop-filter:blur(4px);
    }
    .map-coords{
      z-index:1000;
      position:absolute; top:12px; right:14px; font-size:11px; color:var(--text-dim);
      background:rgba(14,17,22,.7); padding:5px 10px; border-radius:6px; font-family:'JetBrains Mono',monospace;
    }

    /* ---------- Lock control ---------- */
    .lock-stage{
      display:flex; flex-direction:column; align-items:center; justify-content:center;
      padding:40px 20px; text-align:center;
    }
    .lock-ring{
      width:150px; height:150px; border-radius:50%;
      border:1px solid var(--line);
      display:flex; align-items:center; justify-content:center;
      background:var(--panel-raised);
      cursor:pointer;
      transition:box-shadow .2s ease;
      position:relative;
    }
    .lock-ring.locked{ box-shadow:0 0 0 8px var(--safe-dim); }
    .lock-ring.unlocked{ box-shadow:0 0 0 8px var(--alert-dim); }
    .lock-ring svg{ width:52px; height:52px; }
    .lock-status-text{ margin-top:18px; font-size:15px; font-weight:600; }
    .lock-status-sub{ font-size:12.5px; color:var(--text-faint); margin-top:4px; }
    .lock-actions{ display:flex; gap:10px; margin-top:26px; }

    button{ font-family:inherit; cursor:pointer; }
    .btn{
      border:1px solid var(--line);
      background:var(--panel-raised);
      color:var(--text);
      padding:10px 16px;
      border-radius:8px;
      font-size:13px;
      font-weight:600;
      display:inline-flex; align-items:center; gap:8px;
      transition:border-color .15s ease, background .15s ease;
    }
    .btn:hover{ border-color:#3a4453; }
    .btn.primary{ background:var(--safe); color:#0b1a14; border-color:var(--safe); }
    .btn.primary:hover{ filter:brightness(1.06); }
    .btn.danger{ background:transparent; color:var(--alert); border-color:#4a2626; }
    .btn.danger:hover{ background:var(--alert-dim); }
    .btn.ghost{ background:transparent; }
    .btn:disabled{ opacity:.45; cursor:not-allowed; }

    .emergency-box{
      margin-top:28px; padding:16px; border:1px dashed #4a2626; border-radius:var(--radius);
      background:rgba(255,90,95,.04); text-align:left; display:flex; align-items:center; justify-content:space-between; gap:16px;
    }
    .emergency-box p{ font-size:12.5px; color:var(--text-dim); margin-top:4px; }

    /* ---------- Alerts ---------- */
    .alert-row{
      display:flex; gap:14px; padding:14px 4px; border-bottom:1px solid var(--line);
    }
    .alert-row:last-child{ border-bottom:none; }
    .alert-icon{
      width:34px; height:34px; border-radius:8px; flex-shrink:0;
      display:flex; align-items:center; justify-content:center;
      background:var(--alert-dim); color:var(--alert);
    }
    .alert-icon.info{ background:#1e2b3a; color:var(--route); }
    .alert-body{ flex:1; min-width:0; }
    .alert-title{ font-size:13.5px; font-weight:600; }
    .alert-meta{ font-size:12px; color:var(--text-faint); margin-top:3px; }
    .alert-tags{ display:flex; gap:6px; margin-top:8px; flex-wrap:wrap; }
    .tag{ font-size:10.5px; padding:2px 7px; border-radius:5px; background:var(--panel-raised); color:var(--text-dim); border:1px solid var(--line); }

    /* ---------- Ride history ---------- */
    table{ width:100%; border-collapse:collapse; }
    th{ text-align:left; font-size:11px; color:var(--text-faint); text-transform:uppercase; letter-spacing:.05em; font-weight:600; padding:0 10px 10px; }
    td{ font-size:13px; padding:12px 10px; border-top:1px solid var(--line); }
    tr td:first-child, th:first-child{ padding-left:4px; }
    .ride-dist{ font-family:'JetBrains Mono',monospace; font-weight:600; }

    /* ---------- Access management ---------- */
    .user-row{
      display:flex; align-items:center; gap:12px; padding:13px 4px; border-bottom:1px solid var(--line);
    }
    .user-row:last-child{ border-bottom:none; }
    .u-avatar{
      width:32px; height:32px; border-radius:50%; flex-shrink:0;
      display:flex; align-items:center; justify-content:center; font-size:12px; font-weight:700; color:#0b0e12;
    }
    .u-name{ font-size:13.5px; font-weight:600; }
    .u-role{ font-size:11.5px; color:var(--text-faint); }
    .role-badge{ font-size:11px; padding:3px 9px; border-radius:20px; border:1px solid var(--line); color:var(--text-dim); }
    .role-badge.owner{ color:var(--safe); border-color:#1e3a30; }
    .req-row{ display:flex; align-items:center; gap:12px; padding:12px 14px; background:var(--panel-raised); border-radius:8px; border:1px solid var(--line); margin-top:10px; }
    .req-actions{ margin-left:auto; display:flex; gap:8px; }
    .field{ margin-bottom:14px; }
    .field label{ font-size:12px; color:var(--text-dim); display:block; margin-bottom:6px; }
    .field input, .field select{
      width:100%; background:var(--panel-raised); border:1px solid var(--line); color:var(--text);
      padding:9px 11px; border-radius:8px; font-family:inherit; font-size:13px;
    }
    .field input:focus, .field select:focus{ outline:none; border-color:#3a4453; }

    /* ---------- Settings ---------- */
    .pin-input{
      background:var(--panel-raised);
      border:1px solid var(--line);
      color:var(--text);
      border-radius:var(--radius);
      padding:10px 12px;
      font-family:'JetBrains Mono', monospace;
      font-size:15px;
      width:140px;
      letter-spacing:2px;
    }
    .pin-input:focus{ outline:none; border-color:var(--route); }
    .setting-row{
      display:flex; align-items:center; justify-content:space-between; padding:16px 4px; border-bottom:1px solid var(--line); gap:20px;
    }
    .setting-row:last-child{ border-bottom:none; }
    .setting-title{ font-size:13.5px; font-weight:600; }
    .setting-sub{ font-size:12px; color:var(--text-faint); margin-top:3px; max-width:420px; }
    .switch{ position:relative; width:40px; height:22px; flex-shrink:0; }
    .switch input{ opacity:0; width:0; height:0; }
    .slider-toggle{
      position:absolute; cursor:pointer; inset:0; background:#2a323d; border-radius:20px; transition:.2s;
    }
    .slider-toggle::before{
      content:""; position:absolute; height:16px; width:16px; left:3px; top:3px; background:#c9cfd8; border-radius:50%; transition:.2s;
    }
    .switch input:checked + .slider-toggle{ background:var(--safe-dim); }
    .switch input:checked + .slider-toggle::before{ transform:translateX(18px); background:var(--safe); }
    .range-row{ display:flex; align-items:center; gap:14px; width:220px; flex-shrink:0; }
    input[type=range]{ flex:1; accent-color:var(--safe); }

    .section-title{ font-size:12px; text-transform:uppercase; letter-spacing:.06em; color:var(--text-faint); margin:26px 0 4px; }
    .section-title:first-child{ margin-top:0; }

    ::-webkit-scrollbar{ width:8px; height:8px; }
    ::-webkit-scrollbar-thumb{ background:#2a323d; border-radius:8px; }
  </style>
  </head>
  <body>

  <div class="sidebar">
    <div class="brand">
      <div class="brand-mark"></div>
      <div>
        <div class="brand-name">Sentry</div>
        <div class="brand-sub">bike security console</div>
      </div>
    </div>

    <div class="bike-switch">
      <div class="label">Active bike</div>
      <select id="bikeSelect">
        <option>Trek FX2 — "The Commuter"</option>
        <option>Canyon Grail — "Weekend Rig"</option>
      </select>
    </div>

    <nav id="nav">
      <div class="nav-item active" data-view="dashboard">
        <svg class="nav-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="3" y="3" width="8" height="8" rx="1.5"/><rect x="13" y="3" width="8" height="8" rx="1.5"/><rect x="3" y="13" width="8" height="8" rx="1.5"/><rect x="13" y="13" width="8" height="8" rx="1.5"/></svg>
        Dashboard
      </div>
      <div class="nav-item" data-view="tracking">
        <svg class="nav-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M12 21s-7-6.1-7-11a7 7 0 0 1 14 0c0 4.9-7 11-7 11z"/><circle cx="12" cy="10" r="2.4"/></svg>
        Live tracking
      </div>
      <div class="nav-item" data-view="lock">
        <svg class="nav-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="4" y="10" width="16" height="10" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/></svg>
        Lock control
      </div>
      <div class="nav-item" data-view="alerts">
        <svg class="nav-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M12 3 2 20h20L12 3z"/><path d="M12 10v4"/><circle cx="12" cy="17" r=".6" fill="currentColor"/></svg>
        Theft alerts
        <span class="nav-badge">2</span>
      </div>
      <div class="nav-item" data-view="history">
        <svg class="nav-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M3 12a9 9 0 1 0 3-6.7"/><path d="M3 4v5h5"/><path d="M12 7v5l3 3"/></svg>
        Ride history
      </div>
      <div class="nav-item" data-view="access">
        <svg class="nav-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="9" cy="8" r="3.2"/><path d="M2.5 20c0-3.6 2.9-6 6.5-6s6.5 2.4 6.5 6"/><path d="M17 8.5a3 3 0 1 1 3.6 2.94"/><path d="M16 20c.2-2.6 1.7-4.4 3.4-5.2"/></svg>
        Manage access
      </div>
      <div class="nav-item" data-view="settings">
        <svg class="nav-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><circle cx="12" cy="12" r="3"/><path d="M19.4 13a1.7 1.7 0 0 0 .3 1.9l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.9-.3 1.7 1.7 0 0 0-1 1.6V19a2 2 0 1 1-4 0v-.2a1.7 1.7 0 0 0-1-1.6 1.7 1.7 0 0 0-1.9.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.9 1.7 1.7 0 0 0-1.6-1H3a2 2 0 1 1 0-4h.2a1.7 1.7 0 0 0 1.6-1 1.7 1.7 0 0 0-.3-1.9l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.9.3H9a1.7 1.7 0 0 0 1-1.6V3a2 2 0 1 1 4 0v.2a1.7 1.7 0 0 0 1 1.6 1.7 1.7 0 0 0 1.9-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.9V9a1.7 1.7 0 0 0 1.6 1H21a2 2 0 1 1 0 4h-.2a1.7 1.7 0 0 0-1.6 1z"/></svg>
        Settings
      </div>
    </nav>

    <div class="sidebar-foot">
      <div class="avatar">MR</div>
      <div>
        <div style="font-weight:600; color:var(--text);">Maya R.</div>
        <div style="font-size:11px;">Owner</div>
      </div>
    </div>
  </div>

  <main>

    <!-- DASHBOARD -->
    <section class="view active" id="view-dashboard">
      <div class="topline">
        <h1>Dashboard</h1>
        <div class="meta mono" id="clock"></div>
      </div>

      <div class="grid grid-4" style="margin-bottom:16px;">
        <div class="card">
          <div class="stat-label">Bike status</div>
          <div style="margin-top:10px;"><span class="pill safe" id="statusPill"><span class="dot"></span> Locked</span></div>
        </div>
        <div class="card">
          <div class="stat-label">Satellites</div>
          <div class="stat-value" id="satCount">--</div>
        </div>
        <div class="card">
          <div class="stat-label">Last updated</div>
          <div class="stat-value small mono" id="lastUpdated">--</div>
        </div>
        <div class="card">
          <div class="stat-label">GPS signal</div>
          <div style="margin-top:10px;"><span class="pill safe" id="gpsPill"><span class="dot pulse"></span> Searching</span></div>
        </div>
      </div>

      <div class="grid grid-2">
        <div class="card" style="padding:0;">
          <div style="padding:16px 16px 0;">
            <div class="stat-label">Current location</div>
          </div>
          <div class="radar-wrap" style="height:280px; margin:12px;">
            <svg class="radar-grid" viewBox="0 0 300 280" width="100%" height="100%">
              <circle cx="150" cy="140" r="40" fill="none" stroke="#26303c" stroke-width="1"/>
              <circle cx="150" cy="140" r="80" fill="none" stroke="#1c242e" stroke-width="1"/>
              <circle cx="150" cy="140" r="120" fill="none" stroke="#171e26" stroke-width="1"/>
            </svg>
            <div class="radar-sweep"></div>
            <div class="radar-blip" style="left:74%; top:24%;"></div>
            <div class="radar-blip small" style="left:20%; top:66%;"></div>
            <div class="bike-pin"></div>
            <div class="live-map" id="dashMap"></div>
            <div class="map-caption" id="dashCaption">Live position</div>
            <div class="map-coords" id="dashCoords">Waiting for GPS fix...</div>
          </div>
        </div>

        <div class="card">
          <div class="stat-label" style="margin-bottom:12px;">Quick actions</div>
          <div style="display:flex; flex-direction:column; gap:10px;">
            <button class="btn primary" onclick="switchView('lock')">Lock / unlock bike</button>
            <button class="btn" onclick="triggerMotion()">⚡ Simulate motion (demo)</button>
            <button class="btn" onclick="switchView('tracking')">Open live tracking</button>
            <button class="btn" onclick="switchView('history')">View ride history</button>
          </div>
          <div class="section-title">Bike details</div>
          <div style="font-size:13px; color:var(--text-dim); line-height:1.9;">
            Trek FX2 · 2023 · Serial #TF2-88213<br>
            Paired since Jan 14, 2026
          </div>
        </div>
      </div>
    </section>

    <!-- LIVE TRACKING -->
    <section class="view" id="view-tracking">
      <div class="topline"><h1>Live tracking</h1><div class="meta">Updates every 8s while unlocked, every 5min while locked</div></div>
      <div class="card" style="padding:0; margin-bottom:16px;">
        <div class="radar-wrap" style="height:400px;">
          <svg class="route-line" viewBox="0 0 600 340" width="100%" height="100%">
            <path d="M60 280 C 140 260, 160 200, 220 190 S 320 130, 360 120 S 460 80, 520 60" fill="none" stroke="#4FA3FF" stroke-width="2.5" stroke-dasharray="1 0" opacity="0.85"/>
            <circle cx="60" cy="280" r="4" fill="#5A6472"/>
            <circle cx="520" cy="60" r="6" fill="#3ED598"/>
          </svg>
          <div class="radar-sweep"></div>
          <div class="live-map" id="trackMap"></div>
          <div class="map-caption">Live GPS feed from bike</div>
          <div class="map-coords" id="liveCoords">Waiting for GPS fix...</div>
        </div>
        <div style="padding:0 16px 16px;">
          <a class="btn primary" id="gmapsLink" href="#" target="_blank" rel="noopener" style="display:inline-block; text-decoration:none; margin-top:12px;">Open in Google Maps</a>
          <div class="map-note" id="mapNote">Showing the radar view. Live street maps need internet on the device you are viewing this on.</div>
        </div>
      </div>
      <div class="grid grid-4">
        <div class="card"><div class="stat-label">Speed</div><div class="stat-value" id="speedVal">0.0 <span style="font-size:13px; color:var(--text-faint);">km/h</span></div></div>
        <div class="card"><div class="stat-label">Heading</div><div class="stat-value" id="headingVal">--</div></div>
        <div class="card"><div class="stat-label">Status</div><div class="stat-value small" id="trackStatus">--</div></div>
        <div class="card"><div class="stat-label">Satellites</div><div class="stat-value" id="satCount2">--</div></div>
      </div>
    </section>

    <!-- LOCK CONTROL -->
    <section class="view" id="view-lock">
      <div class="topline"><h1>Lock control</h1></div>
      <div class="card">
        <div class="lock-stage">
          <div class="lock-ring locked" id="lockRing" onclick="handleRingClick()">
            <svg viewBox="0 0 24 24" fill="none" stroke="#3ED598" stroke-width="1.6" id="lockIcon">
              <rect x="4" y="10" width="16" height="10" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/>
            </svg>
          </div>
          <div class="lock-status-text" id="lockStatusText">Locked</div>
          <div class="lock-status-sub" id="lockStatusSub">Bike is secure and monitoring for motion</div>
          <div id="lockActions" class="lock-actions"></div>
          <div id="pinPad" class="card" style="display:none; margin-top:10px;">
            <div class="setting-sub" style="margin-bottom:8px;">Or unlock with your PIN</div>
            <input type="password" id="webPinInput" maxlength="4" placeholder="Enter PIN" class="pin-input">
            <button class="btn primary" onclick="submitWebPin()" style="margin-left:8px;">Unlock</button>
            <div id="webPinMsg" class="setting-sub" style="margin-top:8px;"></div>
          </div>

          <div class="emergency-box">
            <div>
              <div class="setting-title" style="color:var(--alert);">Panic / force alarm</div>
              <p>Immediately sounds the alarm and cuts ignition, as if unauthorized motion was detected.</p>
            </div>
            <button class="btn danger" onclick="rejectOwner()">Trigger</button>
          </div>
        </div>
      </div>
    </section>

    <!-- THEFT ALERTS -->
    <section class="view" id="view-alerts">
      <div class="topline"><h1>Theft alerts</h1><div class="meta">2 unresolved</div></div>
      <div class="card">
        <div class="alert-row">
          <div class="alert-icon">
            <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M12 3 2 20h20L12 3z"/><path d="M12 10v4"/></svg>
          </div>
          <div class="alert-body">
            <div class="alert-title">Motion detected while locked</div>
            <div class="alert-meta">Today, 2:47 PM · 9th &amp; Alder St</div>
            <div class="alert-tags"><span class="tag">SMS sent</span><span class="tag">Email sent</span><span class="tag" style="color:var(--alert); border-color:#4a2626;">Unresolved</span></div>
          </div>
          <button class="btn ghost" style="align-self:center;">Dismiss</button>
        </div>
        <div class="alert-row">
          <div class="alert-icon">
            <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="4" y="10" width="16" height="10" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/><path d="M9 15l1.5 1.5L15 12"/></svg>
          </div>
          <div class="alert-body">
            <div class="alert-title">Lock tampering detected</div>
            <div class="alert-meta">Today, 2:48 PM · 9th &amp; Alder St</div>
            <div class="alert-tags"><span class="tag">SMS sent</span><span class="tag" style="color:var(--alert); border-color:#4a2626;">Unresolved</span></div>
          </div>
          <button class="btn ghost" style="align-self:center;">Dismiss</button>
        </div>
        <div class="alert-row">
          <div class="alert-icon info">
            <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M12 8v5"/><circle cx="12" cy="16" r=".6" fill="currentColor"/><circle cx="12" cy="12" r="9"/></svg>
          </div>
          <div class="alert-body">
            <div class="alert-title">Left the geofence — "Home"</div>
            <div class="alert-meta">Yesterday, 8:12 AM</div>
            <div class="alert-tags"><span class="tag">Email sent</span><span class="tag">Resolved</span></div>
          </div>
          <button class="btn ghost" style="align-self:center;">Dismiss</button>
        </div>
      </div>
    </section>

    <!-- RIDE HISTORY -->
    <section class="view" id="view-history">
      <div class="topline"><h1>Ride history</h1></div>
      <div class="card">
        <table>
          <thead><tr><th>Date</th><th>Route</th><th>Distance</th><th>Duration</th></tr></thead>
          <tbody>
            <tr><td>Sep 3, 2026 — 6:14 PM</td><td>Home → Riverside Café</td><td class="ride-dist">2.1 mi</td><td class="mono">14 min</td></tr>
            <tr><td>Sep 2, 2026 — 8:02 AM</td><td>Home → Office</td><td class="ride-dist">4.6 mi</td><td class="mono">22 min</td></tr>
            <tr><td>Aug 31, 2026 — 5:40 PM</td><td>Office → Grocery → Home</td><td class="ride-dist">5.9 mi</td><td class="mono">31 min</td></tr>
            <tr><td>Aug 30, 2026 — 7:55 AM</td><td>Home → Office</td><td class="ride-dist">4.6 mi</td><td class="mono">19 min</td></tr>
            <tr><td>Aug 28, 2026 — 11:10 AM</td><td>Weekend loop — Wynn Park trail</td><td class="ride-dist">11.3 mi</td><td class="mono">58 min</td></tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- MANAGE ACCESS -->
    <section class="view" id="view-access">
      <div class="topline"><h1>Manage access</h1><div class="meta">Trek FX2 — "The Commuter"</div></div>

      <div class="grid grid-2">
        <div class="card">
          <div class="stat-label" style="margin-bottom:6px;">People with access</div>
          <div class="user-row">
            <div class="u-avatar" style="background:linear-gradient(135deg,#4FA3FF,#3ED598);">MR</div>
            <div><div class="u-name">Maya R. (you)</div><div class="u-role">maya@sentry.app</div></div>
            <span class="role-badge owner" style="margin-left:auto;">Owner</span>
          </div>
          <div class="user-row">
            <div class="u-avatar" style="background:linear-gradient(135deg,#F5B94D,#FF5A5F);">DK</div>
            <div><div class="u-name">Daniel K.</div><div class="u-role">Approved Jun 2, 2026</div></div>
            <span class="role-badge" style="margin-left:auto;">Rider</span>
            <button class="btn ghost" style="padding:6px 10px;">Revoke</button>
          </div>

          <div class="section-title">Pending requests</div>
          <div class="req-row">
            <div class="u-avatar" style="background:linear-gradient(135deg,#8B95A5,#5A6472); font-size:11px;">JT</div>
            <div><div class="u-name">Jordan T.</div><div class="u-role">Requested access · 3h ago</div></div>
            <div class="req-actions">
              <button class="btn ghost" style="padding:6px 12px;">Deny</button>
              <button class="btn primary" style="padding:6px 12px;">Approve</button>
            </div>
          </div>
        </div>

        <div class="card">
          <div class="stat-label" style="margin-bottom:14px;">Invite a rider</div>
          <div class="field">
            <label>Email address</label>
            <input type="email" placeholder="name@example.com">
          </div>
          <div class="field">
            <label>Role</label>
            <select><option>Rider — can lock/unlock &amp; view location</option><option>Viewer — can view only</option></select>
          </div>
          <button class="btn primary" style="width:100%; justify-content:center;">Send invite</button>
          <div class="section-title">Access requests</div>
          <div class="setting-row" style="padding:12px 0;">
            <div>
              <div class="setting-title">Require owner approval</div>
              <div class="setting-sub">New riders must be approved before they can access this bike.</div>
            </div>
            <label class="switch"><input type="checkbox" checked><span class="slider-toggle"></span></label>
          </div>
        </div>
      </div>
    </section>

    <!-- SETTINGS -->
    <section class="view" id="view-settings">
      <div class="topline"><h1>Settings</h1></div>

      <div class="section-title">Unlock PIN</div>
      <div class="card">
        <div class="setting-row">
          <div><div class="setting-title">Change unlock PIN</div><div class="setting-sub">4-digit code used on the website and physical keypad to unlock the bike.</div></div>
        </div>
        <div style="display:flex; gap:8px; margin-top:10px; flex-wrap:wrap;">
          <input type="password" id="oldPinInput" maxlength="4" placeholder="Current PIN" class="pin-input">
          <input type="password" id="newPinInput" maxlength="4" placeholder="New 4-digit PIN" class="pin-input">
          <button class="btn primary" onclick="changePin()">Update PIN</button>
        </div>
        <div id="pinChangeMsg" class="setting-sub" style="margin-top:8px;"></div>
      </div>

      <div class="section-title">Notifications</div>
      <div class="card">
        <div class="setting-row">
          <div><div class="setting-title">Motion alerts</div><div class="setting-sub">Notify me when motion is detected while the bike is locked.</div></div>
          <label class="switch"><input type="checkbox" checked><span class="slider-toggle"></span></label>
        </div>
        <div class="setting-row">
          <div><div class="setting-title">SMS notifications</div><div class="setting-sub">Send a text message for theft alerts.</div></div>
          <label class="switch"><input type="checkbox" checked><span class="slider-toggle"></span></label>
        </div>
        <div class="setting-row">
          <div><div class="setting-title">Email notifications</div><div class="setting-sub">Send an email summary for all alerts.</div></div>
          <label class="switch"><input type="checkbox" checked><span class="slider-toggle"></span></label>
        </div>
        <div class="setting-row">
          <div><div class="setting-title">Ride completion summary</div><div class="setting-sub">Email a summary after each ride ends.</div></div>
          <label class="switch"><input type="checkbox"><span class="slider-toggle"></span></label>
        </div>
      </div>

      <div class="section-title">Tracking</div>
      <div class="card">
        <div class="setting-row">
          <div><div class="setting-title">GPS update interval</div><div class="setting-sub">More frequent updates use more battery.</div></div>
          <div class="range-row">
            <input type="range" min="5" max="300" value="30" id="gpsInterval" oninput="document.getElementById('gpsIntervalVal').textContent=this.value">
            <span class="mono" id="gpsIntervalVal" style="width:46px; text-align:right; font-size:12.5px; color:var(--text-dim);">30s</span>
          </div>
        </div>
      </div>

      <div class="section-title">Account</div>
      <div class="card">
        <div class="field"><label>Current password</label><input type="password" placeholder="••••••••"></div>
        <div class="field"><label>New password</label><input type="password" placeholder="••••••••"></div>
        <button class="btn primary">Update password</button>
      </div>
    </section>

  </main>

  <script>
    function switchView(name){
      document.querySelectorAll('.nav-item').forEach(el => el.classList.toggle('active', el.dataset.view === name));
      document.querySelectorAll('.view').forEach(el => el.classList.toggle('active', el.id === 'view-' + name));
      // maps created while hidden need to re-measure their container once shown
      setTimeout(() => { for (const k in maps) maps[k].map.invalidateSize(); }, 60);
    }

    // ---- Real street map (Leaflet + CARTO dark tiles) ----
    // Loaded AFTER the page so an offline network never blocks the dashboard.
    // If the libraries/tiles can't load, the radar view simply stays in place.
    const maps = {};
    let trail = [];
    function loadLeaflet(){
      const css = document.createElement('link');
      css.rel = 'stylesheet'; css.href = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.css';
      document.head.appendChild(css);
      const js = document.createElement('script');
      js.src = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.js';
      js.onload = initMaps;
      document.head.appendChild(js);
    }
    function makeMap(elId){
      const el = document.getElementById(elId);
      const map = L.map(el, {zoomControl:true}).setView([20.5937, 78.9629], 4);
      // Public OpenStreetMap tiles: no CARTO API key required.
      const tiles = L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
        maxZoom: 19,
        attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap contributors</a>'
      }).addTo(map);

      // Show the Leaflet map only after its first tile arrives. If tiles fail,
      // keep the radar fallback visible instead of showing an empty rectangle.
      const radarWrap = el.closest('.radar-wrap');
      tiles.once('load', () => {
        if (radarWrap) radarWrap.classList.add('has-map');
        setTimeout(() => map.invalidateSize(), 100);
      });
      tiles.on('tileerror', (event) => {
        console.warn('Map tile failed to load:', event.tile && event.tile.src);
      });

      const marker = L.circleMarker([0,0], {radius:9, weight:3, color:'#3ED598', fillColor:'#3ED598', fillOpacity:.9});
      const line = L.polyline([], {color:'#4FA3FF', weight:3, opacity:.85}).addTo(map);
      const m = {map, marker, line, added:false, userMoved:false};
      map.on('dragstart', () => { m.userMoved = true; });
      return m;
    }
    function initMaps(){
      maps.dash = makeMap('dashMap');
      maps.track = makeMap('trackMap');
      setTimeout(() => {
        Object.values(maps).forEach(m => m.map.invalidateSize());
      }, 250);
    }
    function updateMaps(data, hasFix){
      const note = document.getElementById('mapNote');
      const link = document.getElementById('gmapsLink');
      if (hasFix) link.href = 'https://www.google.com/maps?q=' + data.lat + ',' + data.lng;
      if (note) {
        const live = document.querySelector('#trackMap')?.parentElement.classList.contains('has-map');
        note.textContent = live
          ? 'Live street map. Blue line = path travelled since this page opened.'
          : 'Showing the radar view. Live street maps need internet on the device you are viewing this on' +
            (data.ip ? ' (you can also open http://' + data.ip + ').' : '.');
      }
      if (!hasFix || !maps.dash) return;
      const ll = [data.lat, data.lng];
      const last = trail[trail.length - 1];
      if (!last || last[0] !== ll[0] || last[1] !== ll[1]) { trail.push(ll); if (trail.length > 300) trail.shift(); }
      const col = data.status === 'theft' ? '#FF5A5F' : data.status === 'motion' ? '#F5B94D' : '#3ED598';
      for (const k in maps) {
        const m = maps[k];
        m.marker.setStyle({color:col, fillColor:col}).setLatLng(ll);
        if (!m.added) { m.marker.addTo(m.map); m.added = true; m.map.setView(ll, 17); }
        m.line.setLatLngs(trail);
        if (data.status === 'theft' || !m.userMoved) m.map.panTo(ll); // follow the bike (always while stolen)
      }
    }
    window.addEventListener('load', loadLeaflet);
    document.querySelectorAll('.nav-item').forEach(el => {
      el.addEventListener('click', () => switchView(el.dataset.view));
    });

    function tickClock(){
      const el = document.getElementById('clock');
      if(!el) return;
      const now = new Date();
      el.textContent = now.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second:'2-digit'});
    }
    setInterval(tickClock, 1000); tickClock();

    // ---- Real backend wiring ----
    const UNLOCKED_ICON = '<rect x="4" y="10" width="16" height="10" rx="2"/><path d="M8 10V7a4 4 0 0 1 5.6-3.9"/>';
    const LOCKED_ICON = '<rect x="4" y="10" width="16" height="10" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/>';
    let lastFetchTime = null;

    function triggerMotion(){ fetch('/motion').then(refreshStatus); }
    function confirmOwner(){ fetch('/confirm').then(refreshStatus); }
    function rejectOwner(){ fetch('/reject').then(refreshStatus); }
    function lockBike(){ fetch('/lock').then(refreshStatus); }
    function resetSystem(){ fetch('/reset').then(refreshStatus); }

    async function submitWebPin(){
      const pin = document.getElementById('webPinInput').value;
      const msg = document.getElementById('webPinMsg');
      if(pin.length !== 4){ msg.textContent = 'Enter a 4-digit PIN.'; msg.style.color = 'var(--alert)'; return; }
      const res = await fetch('/unlock_pin?pin=' + encodeURIComponent(pin));
      const text = await res.text();
      if(text === 'ok'){
        msg.textContent = ''; document.getElementById('webPinInput').value = '';
        refreshStatus();
      } else {
        msg.textContent = 'Incorrect PIN — try again.'; msg.style.color = 'var(--alert)';
        document.getElementById('webPinInput').value = '';
      }
    }

    async function changePin(){
      const oldPin = document.getElementById('oldPinInput').value;
      const newPin = document.getElementById('newPinInput').value;
      const msg = document.getElementById('pinChangeMsg');
      if(newPin.length !== 4){ msg.textContent = 'New PIN must be exactly 4 digits.'; msg.style.color = 'var(--alert)'; return; }
      const res = await fetch('/setpin?oldpin=' + encodeURIComponent(oldPin) + '&newpin=' + encodeURIComponent(newPin));
      const text = await res.text();
      if(text === 'ok'){
        msg.textContent = 'PIN updated successfully.'; msg.style.color = 'var(--safe)';
        document.getElementById('oldPinInput').value = ''; document.getElementById('newPinInput').value = '';
      } else {
        msg.textContent = 'Current PIN was incorrect.'; msg.style.color = 'var(--alert)';
      }
    }

    function handleRingClick(){
      fetch('/status').then(r => r.json()).then(data => {
        if(data.status === 'secure') triggerMotion();
        else if(data.status === 'running') lockBike();
        else if(data.status === 'theft') resetSystem();
        // during "motion" status, ring click does nothing — use the Yes/No buttons instead
      });
    }

    function headingToCompass(deg){
      const dirs = ['N','NE','E','SE','S','SW','W','NW'];
      return dirs[Math.round(deg / 45) % 8];
    }

    async function refreshStatus(){
      let data;
      try {
        const res = await fetch('/status');
        data = await res.json();
      } catch(e) { return; }

      lastFetchTime = new Date();

      const ring = document.getElementById('lockRing');
      const icon = document.getElementById('lockIcon');
      const text = document.getElementById('lockStatusText');
      const sub = document.getElementById('lockStatusSub');
      const pill = document.getElementById('statusPill');
      const actions = document.getElementById('lockActions');

      ring.classList.remove('locked', 'unlocked');
      actions.innerHTML = '';
      document.getElementById('pinPad').style.display = (data.status === 'motion') ? 'block' : 'none';

      if(data.status === 'secure'){
        ring.classList.add('locked');
        icon.setAttribute('stroke', '#3ED598'); icon.innerHTML = LOCKED_ICON;
        text.textContent = 'Locked'; sub.textContent = 'Bike is secure — tap the icon to simulate motion';
        pill.className = 'pill safe'; pill.innerHTML = '<span class="dot"></span> Locked';
      } else if(data.status === 'motion'){
        ring.classList.add('locked');
        icon.setAttribute('stroke', '#F5B94D'); icon.innerHTML = LOCKED_ICON;
        text.textContent = 'Motion detected'; sub.textContent = 'Is this you riding the bike?';
        pill.className = 'pill alert'; pill.innerHTML = '<span class="dot pulse"></span> Motion detected';
        actions.innerHTML = '<button class="btn primary" onclick="confirmOwner()">Yes, it\'s me</button><button class="btn danger" onclick="rejectOwner()">Not me</button>';
      } else if(data.status === 'running'){
        ring.classList.add('unlocked');
        icon.setAttribute('stroke', '#3ED598'); icon.innerHTML = UNLOCKED_ICON;
        text.textContent = 'Unlocked'; sub.textContent = 'Ignition running — tap the button to lock';
        pill.className = 'pill safe'; pill.innerHTML = '<span class="dot pulse"></span> Unlocked';
        actions.innerHTML = '<button class="btn primary" onclick="lockBike()">Lock bike</button>';
      } else if(data.status === 'theft'){
        ring.classList.add('unlocked');
        icon.setAttribute('stroke', '#FF5A5F'); icon.innerHTML = UNLOCKED_ICON;
        text.textContent = 'ALARM ACTIVE'; sub.textContent = 'Unauthorized access — tap the button to reset';
        pill.className = 'pill alert'; pill.innerHTML = '<span class="dot pulse"></span> Alarm triggered';
        actions.innerHTML = '<button class="btn danger" onclick="resetSystem()">Reset system</button>';
      }

      const hasFix = data.lat != 0 || data.lng != 0;
      const coordText = hasFix ? data.lat.toFixed(5) + '° N, ' + data.lng.toFixed(5) + '° E' : 'Waiting for GPS fix...';
      document.getElementById('dashCoords').textContent = coordText;
      document.getElementById('liveCoords').textContent = coordText;
      updateMaps(data, hasFix);

      const gpsPill = document.getElementById('gpsPill');
      if(data.sats >= 4){
        gpsPill.className = 'pill safe'; gpsPill.innerHTML = '<span class="dot pulse"></span> Fix acquired';
      } else {
        gpsPill.className = 'pill alert'; gpsPill.innerHTML = '<span class="dot"></span> Searching (' + data.sats + ')';
      }

      document.getElementById('satCount').textContent = data.sats;
      document.getElementById('satCount2').textContent = data.sats;
      document.getElementById('speedVal').innerHTML = data.speed.toFixed(1) + ' <span style="font-size:13px; color:var(--text-faint);">km/h</span>';
      document.getElementById('headingVal').textContent = hasFix ? headingToCompass(data.course) : '--';
      document.getElementById('trackStatus').textContent = data.status.charAt(0).toUpperCase() + data.status.slice(1);
    }

    function tickLastUpdated(){
      const el = document.getElementById('lastUpdated');
      if(!el || !lastFetchTime) return;
      const secs = Math.round((new Date() - lastFetchTime) / 1000);
      el.textContent = secs + 's ago';
    }

    setInterval(refreshStatus, 1500);
    setInterval(tickLastUpdated, 1000);
    refreshStatus();
  </script>
  </body>
  </html>
  )rawliteral";

  // Allow the separately hosted GitHub Pages dashboard to read ESP32 responses.
  // This dashboard controls a physical bike: use it only on a trusted network.
  void addCorsHeaders() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  }

  void handleRoot() {
    addCorsHeaders(); server.send_P(200, "text/html", htmlPage);
  }

  void handleStatus() {
    String json = "{";
    json += "\"status\":\"" + bikeStatus + "\",";
    json += "\"lat\":" + String(gps.location.isValid() ? gps.location.lat() : 0, 6) + ",";
    json += "\"lng\":" + String(gps.location.isValid() ? gps.location.lng() : 0, 6) + ",";
    json += "\"sats\":" + String(gps.satellites.isValid() ? gps.satellites.value() : 0) + ",";
    json += "\"speed\":" + String(gps.speed.isValid() ? gps.speed.kmph() : 0, 1) + ",";
    json += "\"course\":" + String(gps.course.isValid() ? gps.course.deg() : 0, 1) + ",";
    json += "\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("")) + "\"";
    json += "}";
    addCorsHeaders(); server.send(200, "application/json", json);
  }

  // ---- SIM800L SMS sending ----
  // Sends one SMS via AT commands. Blocking for ~4-5 seconds while it sends —
  // acceptable here since it only fires on motion/theft events, not every loop.
  // Like delay(), but keeps the beep-beep pattern running while waiting.
  void smartDelay(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
      updateBuzzer();
      delay(5);
    }
  }

  void sendSMS(String message) {
    sim800.println("AT+CMGF=1"); // text mode
    smartDelay(300);
    sim800.print("AT+CMGS=\"");
    sim800.print(OWNER_PHONE);
    sim800.println("\"");
    smartDelay(300);
    sim800.print(message);
    smartDelay(200);
    sim800.write(26); // Ctrl+Z sends the message
    smartDelay(4000);
  }

  // Builds a message with live GPS location, speed, and direction of travel.
  String buildLocationMessage(String prefix) {
    String msg = prefix + "\n";
    if (gps.location.isValid()) {
      msg += "Location: " + String(gps.location.lat(), 6) + ", " + String(gps.location.lng(), 6) + "\n";
      msg += "Maps: https://maps.google.com/?q=" + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6) + "\n";
    } else {
      msg += "Location: GPS fix not yet available\n";
    }
    if (gps.speed.isValid()) {
      msg += "Speed: " + String(gps.speed.kmph(), 1) + " km/h\n";
    }
    if (gps.course.isValid()) {
      const char* dirs[8] = {"N","NE","E","SE","S","SW","W","NW"};
      int idx = (int)((gps.course.deg() + 22.5) / 45.0) % 8;
      msg += "Direction: " + String(dirs[idx]);
    }
    return msg;
  }

  // Raw lat,lng formatted to fit the 16-character LCD line.
  String rawLocationForLCD() {
    if (!gps.location.isValid()) return "No GPS fix yet";
    return String(gps.location.lat(), 4) + "," + String(gps.location.lng(), 4);
  }

  // ---- Internal state transitions (no HTTP dependency — safe to call from loop()/keypad too) ----

  // Motion detected (manual demo button, automatic MPU6050 trigger, or "going to start"):
  // ignition stays off, buzzer beep-beeps while waiting for the PIN, and an SMS goes out immediately.
  void doMotionTrigger() {
    bikeStatus = "motion";
    setIgnition(false);
    setBuzzerMode("pulse");
    enteredPin = "";
    pinAttempts = 0;
    updateLCD("Bike Triggered!", "Enter PIN:");
    sendSMS(buildLocationMessage("ALERT: Motion detected on your bike!"));
  }

  // Correct PIN entered (or owner confirms "Yes, it's me" remotely): allow the bike to start.
  void doConfirm() {
    bikeStatus = "running";
    setIgnition(true);
    setBuzzerMode("off");
    updateLCD("Status: Unlocked", "Ignition ON");
  }

  // Wrong PIN too many times (or owner says "Not me" remotely, or timeout): alarm stays on
  // continuously (not pulsing) and the LCD shows the raw coordinates just sent in the alert SMS.
  void doReject() {
    bikeStatus = "theft";
    setIgnition(false);
    setBuzzerMode("steady");
    updateLCD("ALARM TRIGGERED!", rawLocationForLCD());
    sendSMS(buildLocationMessage("THEFT CONFIRMED! Your bike is being stolen."));
    lastTheftSmsTime = millis(); // starts the repeating-update timer in loop()
  }

  // "Lock bike" button: cut ignition, silence buzzer, back to secure/armed.
  void doLock() {
    bikeStatus = "secure";
    setIgnition(false);
    setBuzzerMode("off");
    enteredPin = "";
    pinAttempts = 0;
    updateLCD("BIKE LOCKED", "Ignition OFF");
  }

  // Reset after a theft alarm: silence buzzer, back to secure/armed, ignition off.
  void doReset() {
    bikeStatus = "secure";
    setIgnition(false);
    setBuzzerMode("off");
    enteredPin = "";
    pinAttempts = 0;
    updateLCD("BIKE LOCKED", "Ignition OFF");
  }

  // ---- HTTP handlers: just call the internal transition, then reply to the browser ----
  void handleMotion()  { doMotionTrigger(); addCorsHeaders(); server.send(200, "text/plain", "ok"); }
  void handleConfirm() { doConfirm();       addCorsHeaders(); server.send(200, "text/plain", "ok"); }
  void handleReject()  { doReject();        addCorsHeaders(); server.send(200, "text/plain", "ok"); }
  void handleLock()    { doLock();          addCorsHeaders(); server.send(200, "text/plain", "ok"); }
  void handleReset()   { doReset();         addCorsHeaders(); server.send(200, "text/plain", "ok"); }

  // Web-based PIN unlock: same rules as the physical keypad (3 wrong tries = theft alarm).
  void handleUnlockPin() {
    String pin = server.arg("pin");
    if (pin == LOCK_PIN) {
      pinAttempts = 0;
      doConfirm();
      addCorsHeaders(); server.send(200, "text/plain", "ok");
    } else {
      pinAttempts++;
      if (pinAttempts >= MAX_PIN_ATTEMPTS) {
        pinAttempts = 0;
        doReject();
      }
      addCorsHeaders(); server.send(200, "text/plain", "fail");
    }
  }

  // Change the unlock PIN from the Settings page — requires the current PIN to confirm.
  void handleSetPin() {
    String oldPin = server.arg("oldpin");
    String newPin = server.arg("newpin");
    if (oldPin == LOCK_PIN && newPin.length() == 4) {
      LOCK_PIN = newPin;
      addCorsHeaders(); server.send(200, "text/plain", "ok");
    } else {
      addCorsHeaders(); server.send(200, "text/plain", "fail");
    }
  }

  // Reads the 4x4 keypad and, while the bike is in "motion" (triggered) state, collects a
  // 4-digit PIN. Correct PIN = same as tapping "Yes, it's me" on the website. Wrong PIN
  // three times = same as "Not me" (theft alarm). Ignored entirely in any other state.
  void handleKeypad() {
    char key = keypad.getKey();
    if (!key) return;
    if (bikeStatus != "motion") return; // no PIN prompt outside the triggered state

    if (key == '*') { // clear current entry
      enteredPin = "";
      updateLCD("Bike Triggered!", "Enter PIN:");
      return;
    }

    if (key < '0' || key > '9') return; // ignore A/B/C/D/# — digits only

    enteredPin += key;
    String masked = "";
    for (unsigned int i = 0; i < enteredPin.length(); i++) masked += '*';
    updateLCD("Bike Triggered!", masked);

    if (enteredPin.length() < 4) return;

    if (enteredPin == LOCK_PIN) {
      enteredPin = "";
      pinAttempts = 0;
      doConfirm();
    } else {
      enteredPin = "";
      pinAttempts++;
      if (pinAttempts >= MAX_PIN_ATTEMPTS) {
        pinAttempts = 0;
        doReject();
      } else {
        updateLCD("Wrong PIN!", "Try again");
        smartDelay(800);
        updateLCD("Bike Triggered!", "Enter PIN:");
      }
    }
  }

  // ---- MPU6050 direct register access (no external library) ----
  void mpuWake() {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x6B); // PWR_MGMT_1 register
    Wire.write(0);    // wake the sensor up
    Wire.endTransmission(true);
  }

  bool mpuReadAccel(long &ax, long &ay, long &az) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom(MPU_ADDR, 6, true);

    if (Wire.available() < 6) return false;

    ax = (int16_t)(Wire.read() << 8 | Wire.read());
    ay = (int16_t)(Wire.read() << 8 | Wire.read());
    az = (int16_t)(Wire.read() << 8 | Wire.read());
    return true;
  }

  void checkMotion() {
    long ax, ay, az;
    if (!mpuReadAccel(ax, ay, az)) return; // skip this cycle if the read glitched

    if (!baselineSet) {
      // First good reading becomes the "at rest" reference point
      baselineX = ax; baselineY = ay; baselineZ = az;
      baselineSet = true;
      return;
    }

    long change = abs(ax - baselineX) + abs(ay - baselineY) + abs(az - baselineZ);

    if (change > MOTION_THRESHOLD && bikeStatus == "secure") {
      doMotionTrigger();
    }

    // Slowly drift the baseline toward the current reading so gradual tilts
    // (parking on a slope, wind) don't get mistaken for a sudden bump forever
    baselineX = (baselineX * 9 + ax) / 10;
    baselineY = (baselineY * 9 + ay) / 10;
    baselineZ = (baselineZ * 9 + az) / 10;
  }

  void setup() {
    Serial.begin(115200);

    setIgnition(false); // start locked/secure - ignition OFF until authorized

    pinMode(BUZZER_PIN, OUTPUT);
    setBuzzerMode("off"); // buzzer off at boot

    // GPS
    gpsSerial.begin(9600, SERIAL_8N1, 16, 17);

    // SIM800L GSM module
    sim800.begin(9600, SERIAL_8N1, SIM800_RX, SIM800_TX);

    // MPU6050
    Wire.begin(SDA_PIN, SCL_PIN);
    mpuWake();

    // LCD (shares the same I2C bus as MPU6050)
    lcd.init();
    lcd.backlight();
    updateLCD("BIKE LOCKED", "Ignition OFF");

    // WiFi + website
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ssid, password);
    Serial.print("Access Point started. IP address: ");
    Serial.println(WiFi.softAPIP());
    if (STA_SSID[0] != '\0') {
      WiFi.begin(STA_SSID, STA_PASS); // connects in the background, no blocking
      Serial.println("Joining WiFi for live maps...");
    }

    server.on("/", handleRoot);
    server.on("/status", handleStatus);
    server.on("/motion", handleMotion);
    server.on("/confirm", handleConfirm);
    server.on("/reject", handleReject);
    server.on("/lock", handleLock);
    server.on("/reset", handleReset);
    server.on("/unlock_pin", handleUnlockPin);
    server.on("/setpin", handleSetPin);
    server.begin();
  }

  unsigned long lastMotionCheck = 0;

  void loop() {
    while (gpsSerial.available() > 0) {
      gps.encode(gpsSerial.read());
    }

    // Check motion a few times a second, not every loop cycle
    if (millis() - lastMotionCheck > 200) {
      checkMotion();
      lastMotionCheck = millis();
    }

    handleKeypad();
    updateBuzzer(); // non-blocking beep-beep pattern while in "motion" state

    static bool staAnnounced = false;
    if (!staAnnounced && WiFi.status() == WL_CONNECTED) {
      staAnnounced = true;
      Serial.print("Joined WiFi. Open the dashboard from that network at http://");
      Serial.println(WiFi.localIP());
    }

    // While the bike is stolen, keep sending fresh location updates until it's rescued
    // (owner presses Lock/Reset on the website, which returns bikeStatus to "secure").
    if (bikeStatus == "theft" && millis() - lastTheftSmsTime > THEFT_SMS_INTERVAL) {
      sendSMS(buildLocationMessage("Bike still stolen — location update:"));
      updateLCD("ALARM TRIGGERED!", rawLocationForLCD());
      lastTheftSmsTime = millis();
    }

    server.handleClient();
  }
