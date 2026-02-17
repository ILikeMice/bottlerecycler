#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <math.h>

#define THERMISTOR_PIN A0
#define MOTOR_PWM_PIN 4      // D2 pin
#define HEATER_PIN 14        // D5 pni

const char* ssid = "ssid!";
const char* password = "your pwd";

ESP8266WebServer server(80);

// this changes probably if youre using a diff thermistor!!!!
#define SERIES_RESISTOR 100000.0
#define NOMINAL_RESISTANCE 100000.0
#define NOMINAL_TEMP 25.0
#define BETA_COEFFICIENT 3950.0

#define HEATER_WINDOW_MS 1000  
#define MAX_HEATER_DUTY 200   

float Kp = 2.0;
float Ki = 1.5;
float Kd = 0.1;

// this prolly also varies depending on your hardware, but i dont think theres any case in which youll need 260C for this.
#define MAX_TEMP 260.0
#define PID_MIN_OUTPUT 0
#define PID_MAX_OUTPUT 255

float currentTempC = 0.0;
float targetTempC = 200.0;
bool autoMode = true;       
bool heaterEnabled = false;  
bool emergencyStop = false;
bool motorRunning = false;

float pidInput, pidOutput, pidSetpoint;
float lastError = 0;
float integral = 0;
unsigned long lastPidTime = 0;

float heaterDutyCycle = 0;  
unsigned long heaterWindowStart = 0;

float tempAtHeatStart = 0;
unsigned long heatStartTime = 0;
bool isHeating = false;

unsigned long lastDebug = 0;

void readTemperature() {
  const int SAMPLES = 20;
  long sum = 0;
  
  for (int i = 0; i < SAMPLES; i++) {
    sum += analogRead(THERMISTOR_PIN);
    delay(2);
  }
  
  float currentADC = sum / SAMPLES;
  
  float reading = (1023.0 / currentADC) - 1.0;
  reading = SERIES_RESISTOR / reading;
  float thermistorResistance = reading;
  float steinhart = thermistorResistance / NOMINAL_RESISTANCE;
  steinhart = log(steinhart);
  steinhart /= BETA_COEFFICIENT;
  steinhart += 1.0 / (NOMINAL_TEMP + 273.15);
  steinhart = 1.0 / steinhart;
  currentTempC = steinhart - 273.15;
  
  if (millis() - lastDebug >= 2000) {
    Serial.print("ADC:");
    Serial.print((int)currentADC);
    Serial.print(" R:");
    Serial.print((int)thermistorResistance);
    Serial.print(" temp:");
    Serial.print(currentTempC, 1);
    Serial.print("target:");
    Serial.print(targetTempC, 0);
    Serial.print("heater duty:");
    Serial.print((int)heaterDutyCycle);
    Serial.print("/255");
    if (autoMode) Serial.print(" AUTO");
    else Serial.print(" MANUAL");
    if (emergencyStop) Serial.print(" EMERGENCY");
    Serial.println();
    lastDebug = millis();
  }
}

void updatePID() {
  unsigned long now = millis();
  if (now - lastPidTime < 200) return;
  lastPidTime = now;
  
  if (!autoMode || emergencyStop || !heaterEnabled) {
    if (!heaterEnabled && autoMode) {
      heaterDutyCycle = 0;
    }
    return;
  }
  
  pidInput = currentTempC;
  pidSetpoint = targetTempC;
  
  float error = pidSetpoint - pidInput;
  
  if (abs(error) < 50) {
    integral += error * 0.2;  
  }
  integral = constrain(integral, 0, 100);
  
  float derivative = (error - lastError) / 0.2; 
  lastError = error;
  float output = (Kp * error) + (Ki * integral) + (Kd * derivative);
  output = constrain(output, PID_MIN_OUTPUT, MAX_HEATER_DUTY);
  heaterDutyCycle = output;
  
  if (heaterDutyCycle > 50) {
    if (!isHeating) {
      isHeating = true;
      tempAtHeatStart = currentTempC;
      heatStartTime = millis();
    }
  } else {
    isHeating = false;
  }
}

void updateHeater() {
  unsigned long now = millis();
  
  if (now - heaterWindowStart > HEATER_WINDOW_MS) {
    heaterWindowStart = now;
  }
  
  int onTime = (heaterDutyCycle / 255.0) * HEATER_WINDOW_MS;
  
  if (!emergencyStop && (heaterEnabled || (autoMode && heaterDutyCycle > 0))) {
    if (now - heaterWindowStart < onTime) {
      digitalWrite(HEATER_PIN, LOW);
    } else {
      digitalWrite(HEATER_PIN, HIGH); 
    }
  } else {
    digitalWrite(HEATER_PIN, HIGH);    
  }
}

void checkSafety() {
  if (currentTempC > MAX_TEMP) {
    emergencyStop = true;
    heaterEnabled = false;
    heaterDutyCycle = 0;
    digitalWrite(HEATER_PIN, HIGH);
  }
  
  if (isnan(currentTempC) || currentTempC < -10 || currentTempC > 350) {
    emergencyStop = true;
    heaterEnabled = false;
    heaterDutyCycle = 0;
    digitalWrite(HEATER_PIN, HIGH);
  }
  
}

void setMotor(bool state) {
  motorRunning = state;
  digitalWrite(MOTOR_PWM_PIN, state ? HIGH : LOW);
}

void setHeaterManual(bool state) {
  if (emergencyStop) return;
  autoMode = false;
  heaterEnabled = state;
  heaterDutyCycle = state ? MAX_HEATER_DUTY : 0;
}

void setHeaterAuto(bool enable) {
  if (emergencyStop) return;
  autoMode = true;
  heaterEnabled = enable;
  if (!enable) {
    heaterDutyCycle = 0;
    integral = 0;
    lastError = 0;
  }
}

void setTargetTemp(float temp) {
  targetTempC = constrain(temp, 0, 250);
  integral = 0;  
  lastError = 0;
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Extruder Control</title>
  <style>
    body { font-family: Arial; background: #1a1a1a; color: #fff; text-align: center; padding: 20px; }
    .emergency { background: #d32f2f; padding: 20px; font-size: 24px; cursor: pointer; 
                 margin-bottom: 20px; border-radius: 8px; font-weight: bold; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; max-width: 700px; margin: 0 auto; }
    .card { background: #333; padding: 20px; border-radius: 12px; }
    .temp { font-size: 48px; color: #ff9800; }
    .target { font-size: 36px; color: #4CAF50; }
    .status { font-size: 20px; margin: 10px 0; }
    .on { color: #4CAF50; }
    .off { color: #666; }
    .heating { color: #ff9800; }
    button { padding: 15px 30px; font-size: 16px; margin: 5px; border: none; 
             border-radius: 8px; cursor: pointer; }
    .heat-auto { background: #ff9800; color: #000; }
    .heat-man { background: #9c27b0; color: #fff; }
    .heat-off { background: #555; color: #fff; }
    .motor-on { background: #2196F3; color: #fff; }
    .motor-off { background: #555; color: #fff; }
    .pid-box { background: #222; padding: 10px; border-radius: 8px; margin-top: 10px; font-size: 14px; }
    input[type=number] { width: 60px; font-size: 16px; }
    .warning { color: #ff6b6b; font-weight: bold; margin-top: 10px; }
    .mode-auto { border: 2px solid #ff9800; }
    .mode-man { border: 2px solid #9c27b0; }
  </style>
</head>
<body>
  <h1>Extruder Control</h1>
  
  <div class="emergency" onclick="fetch('/stop').then(update)">EMERGENCY STOP</div>
  
  <div class="grid">
    <div class="card">
      <div>Current Temperature</div>
      <div class="temp" id="temp">--°C</div>
      <div class="status" id="pidStatus">PID: --</div>
    </div>
    
    <div class="card">
      <div>Target Temperature</div>
      <div class="target" id="target">--°C</div>
      <input type="number" id="targetInput" value="200" min="0" max="250" 
             onchange="setTarget(this.value)" style="margin-top: 10px;">
      <button onclick="setTarget(document.getElementById('targetInput').value)" 
              style="padding: 10px 20px;">Set Target</button>
    </div>
    
    <div class="card" id="heaterCard">
      <div>Heater Control</div>
      <div class="status" id="heaterStatus">Mode: --</div>
      <div class="status" id="dutyStatus">Duty: --%</div>
      <button onclick="setHeater('auto')" class="heat-auto">Auto PID</button>
      <button onclick="setHeater('manual')" class="heat-man">Manual ON</button>
      <button onclick="setHeater('off')" class="heat-off">OFF</button>
      <div class="pid-box">
        Kp: <input type="number" id="kp" value="12" step="0.5" onchange="tunePID()">
        Ki: <input type="number" id="ki" value="0.4" step="0.1" onchange="tunePID()">
        Kd: <input type="number" id="kd" value="80" step="5" onchange="tunePID()">
      </div>
    </div>
    
    <div class="card">
      <div>Motor Control</div>
      <div class="status" id="motorStatus">Motor: --</div>
      <button id="motorBtn" onclick="toggleMotor()" class="motor-off">Toggle Motor</button>
      <div class="warning" id="emergency" style="display:none">EMERGENCY STOP ACTIVE</div>
    </div>
  </div>

  <script>
    function update() {
      fetch('/status')
        .then(r => r.json())
        .then(d => {
          document.getElementById('temp').innerText = d.temp.toFixed(1) + '°C';
          document.getElementById('target').innerText = d.target.toFixed(0) + '°C';
          document.getElementById('targetInput').value = d.target.toFixed(0);
          
          let hStatus = document.getElementById('heaterStatus');
          let dStatus = document.getElementById('dutyStatus');
          let hCard = document.getElementById('heaterCard');
          
          if (d.auto) {
            hStatus.innerText = 'Mode: AUTO PID';
            hStatus.className = 'status heating';
            hCard.className = 'card mode-auto';
          } else if (d.heater) {
            hStatus.innerText = 'Mode: MANUAL';
            hStatus.className = 'status on';
            hCard.className = 'card mode-man';
          } else {
            hStatus.innerText = 'Mode: OFF';
            hStatus.className = 'status off';
            hCard.className = 'card';
          }
          
          dStatus.innerText = 'Duty: ' + (d.duty / 2.55).toFixed(0) + '%';
          document.getElementById('pidStatus').innerText = 'PID out: ' + d.duty.toFixed(0) + '/255';
          
          let mStatus = document.getElementById('motorStatus');
          mStatus.innerText = 'Motor: ' + (d.motor ? 'ON' : 'OFF');
          mStatus.className = 'status ' + (d.motor ? 'on' : 'off');
          
          document.getElementById('motorBtn').className = d.motor ? 'motor-on' : 'motor-off';
          
          document.getElementById('emergency').style.display = d.emergency ? 'block' : 'none';
          if (d.emergency) document.body.style.background = '#4a0000';
          
          // Update PID inputs if different
          if (Math.abs(document.getElementById('kp').value - d.kp) > 0.1)
            document.getElementById('kp').value = d.kp;
          if (Math.abs(document.getElementById('ki').value - d.ki) > 0.01)
            document.getElementById('ki').value = d.ki;
          if (Math.abs(document.getElementById('kd').value - d.kd) > 1)
            document.getElementById('kd').value = d.kd;
        });
    }
    
    function setTarget(val) {
      fetch('/target?val=' + val);
    }
    
    function setHeater(mode) {
      fetch('/heater/' + mode).then(update);
    }
    
    function toggleMotor() {
      fetch('/motor/toggle').then(update);
    }
    
    function tunePID() {
      let kp = document.getElementById('kp').value;
      let ki = document.getElementById('ki').value;
      let kd = document.getElementById('kd').value;
      fetch('/tune?kp=' + kp + '&ki=' + ki + '&kd=' + kd);
    }
    
    setInterval(update, 500);
    update();
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleStatus() {
  String json = "{\"temp\":" + String(currentTempC) + 
                ",\"target\":" + String(targetTempC) +
                ",\"duty\":" + String(heaterDutyCycle) +
                ",\"heater\":" + (heaterEnabled ? "true" : "false") +
                ",\"auto\":" + (autoMode ? "true" : "false") +
                ",\"motor\":" + (motorRunning ? "true" : "false") +
                ",\"emergency\":" + (emergencyStop ? "true" : "false") +
                ",\"kp\":" + String(Kp) +
                ",\"ki\":" + String(Ki) +
                ",\"kd\":" + String(Kd) + "}";
  server.send(200, "application/json", json);
}

void handleTarget() {
  if (server.hasArg("val")) {
    setTargetTemp(server.arg("val").toFloat());
  }
  server.send(200, "text/plain", String(targetTempC));
}

void handleHeaterAuto() {
  setHeaterAuto(true);
  server.send(200, "text/plain", "AUTO");
}

void handleHeaterManual() {
  setHeaterManual(true);
  server.send(200, "text/plain", "MANUAL");
}

void handleHeaterOff() {
  heaterEnabled = false;
  heaterDutyCycle = 0;
  integral = 0;
  server.send(200, "text/plain", "OFF");
}

void handleMotorToggle() {
  setMotor(!motorRunning);
  server.send(200, "text/plain", motorRunning ? "ON" : "OFF");
}

void handleTune() {
  if (server.hasArg("kp")) Kp = server.arg("kp").toFloat();
  if (server.hasArg("ki")) Ki = server.arg("ki").toFloat();
  if (server.hasArg("kd")) {Kd = server.arg("kd").toFloat();}
  integral = 0;
  server.send(200, "text/plain", "OK");
}

void handleStop() {
  emergencyStop = true;
  heaterEnabled = false;
  heaterDutyCycle = 0;
  motorRunning = false;
  digitalWrite(HEATER_PIN, HIGH);
  digitalWrite(MOTOR_PWM_PIN, LOW);
  server.send(200, "text/plain", "STOP");
}

void handleReset() {
  emergencyStop = false;
  integral = 0;
  lastError = 0;
  server.send(200, "text/plain", "RESET");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(MOTOR_PWM_PIN, OUTPUT);
  pinMode(HEATER_PIN, OUTPUT);
  
  digitalWrite(MOTOR_PWM_PIN, LOW);
  digitalWrite(HEATER_PIN, HIGH);  // HIGH = MOSFET OFF
  
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/target", handleTarget);
  server.on("/heater/auto", handleHeaterAuto);
  server.on("/heater/manual", handleHeaterManual);
  server.on("/heater/off", handleHeaterOff);
  server.on("/motor/toggle", handleMotorToggle);
  server.on("/tune", handleTune);
  server.on("/stop", handleStop);
  server.on("/reset", handleReset);
  
  server.begin();
  heaterWindowStart = millis();
  lastDebug = millis();
  Serial.println("Extruder PID Controller Started");
  Serial.println("Default target: " + String(targetTempC) + "C");
}

void loop() {
  server.handleClient();
  readTemperature();
  checkSafety();
  updatePID();
  updateHeater();
  delay(5);
}