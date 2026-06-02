#include <Arduino.h>
#include <Wire.h>
#include <a5s-Gaitgaurd_inferencing.h>

// ===== 硬件配置 =====
#define LSM6DS3_ADDR  0x6A
#define BUZZER_PIN    1
#define ERM_PIN       2

#define REG_CTRL1_XL  0x10
#define REG_CTRL2_G   0x11
#define REG_OUTX_L_G  0x22

const float ACC_SENS = 0.000488f * 9.80665f;
const float GYR_SENS = 0.070f    * 0.0174533f;

// ===== 推理 buffer =====
static float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
static size_t feature_ix = 0;

const unsigned long SAMPLE_INTERVAL_US = 1000000.0 / EI_CLASSIFIER_FREQUENCY;
unsigned long next_sample_us = 0;

// ===== 两层平滑 =====
#define SMOOTH_N 5
float p_faller_history[SMOOTH_N] = {0};
int   p_history_idx = 0;
bool  smooth_filled = false;

#define RISK_WINDOW 6
float risk_history[RISK_WINDOW] = {0};
int   risk_history_idx = 0;
bool  risk_filled = false;

#define N_CONSECUTIVE 2
int consecutive_high = 0;

#define THRESH_LOW    0.55f
#define THRESH_HIGH   0.75f

// 静止检测阈值（加速度均方值，可根据实测调整）
#define STATIC_THRESHOLD 100.0f

// ===== 工具函数 =====
void writeReg(byte reg, byte val) {
  Wire.beginTransmission(LSM6DS3_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool readIMU(float *acc, float *gyr) {
  Wire.beginTransmission(LSM6DS3_ADDR);
  Wire.write(REG_OUTX_L_G);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(LSM6DS3_ADDR, 12);
  if (Wire.available() < 12) return false;

  int16_t gx = Wire.read() | (Wire.read() << 8);
  int16_t gy = Wire.read() | (Wire.read() << 8);
  int16_t gz = Wire.read() | (Wire.read() << 8);
  int16_t ax = Wire.read() | (Wire.read() << 8);
  int16_t ay = Wire.read() | (Wire.read() << 8);
  int16_t az = Wire.read() | (Wire.read() << 8);

  acc[0] = ax * ACC_SENS;
  acc[1] = ay * ACC_SENS;
  acc[2] = az * ACC_SENS;
  gyr[0] = gx * GYR_SENS;
  gyr[1] = gy * GYR_SENS;
  gyr[2] = gz * GYR_SENS;
  return true;
}

// ===== 反馈输出 =====
void feedback_low() {}

void feedback_medium() {
  digitalWrite(ERM_PIN, HIGH);
  delay(200);
  digitalWrite(ERM_PIN, LOW);
}

void feedback_high() {
  for (int i = 0; i < 3; i++) {
    ledcWriteTone(BUZZER_PIN, 2000);
    delay(300);
    ledcWriteTone(BUZZER_PIN, 0);
    delay(150);
  }
}

int raw_feature_get_data(size_t offset, size_t length, float *out_ptr) {
  memcpy(out_ptr, features + offset, length * sizeof(float));
  return 0;
}

// ===== setup =====
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== GaitGuard - Fall Risk Assessment ===");

  Wire.begin();
  Wire.setClock(400000);

  writeReg(REG_CTRL1_XL, 0x44);
  writeReg(REG_CTRL2_G,  0x4C);

  pinMode(ERM_PIN, OUTPUT);
  digitalWrite(ERM_PIN, LOW);
  ledcAttach(BUZZER_PIN, 2000, 8);

  Serial.printf("Sample interval: %lu us (%.2f Hz)\n",
                SAMPLE_INTERVAL_US, (float)EI_CLASSIFIER_FREQUENCY);
  Serial.printf("Window: %d samples x %d axes = %d floats\n",
                EI_CLASSIFIER_RAW_SAMPLE_COUNT,
                EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME,
                EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
  Serial.println("Thresholds: LOW<0.35 | MEDIUM<0.65 | HIGH>=0.65");
  Serial.println("Starting...\n");

  next_sample_us = micros();
}

// ===== loop =====
void loop() {
  if ((long)(micros() - next_sample_us) < 0) return;
  next_sample_us += SAMPLE_INTERVAL_US;

  float acc[3], gyr[3];
  if (!readIMU(acc, gyr)) {
    Serial.println("IMU read failed");
    return;
  }

  // DEBUG: 每100次打印一次
  static int dbg_counter = 0;
  if (++dbg_counter >= 100) {
    dbg_counter = 0;
    Serial.printf("DEBUG acc=[%+6.2f %+6.2f %+6.2f] gyr=[%+6.3f %+6.3f %+6.3f]\n",
                  acc[0], acc[1], acc[2], gyr[0], gyr[1], gyr[2]);
  }

  features[feature_ix++] = acc[0];
  features[feature_ix++] = acc[1];
  features[feature_ix++] = acc[2];
  features[feature_ix++] = gyr[0];
  features[feature_ix++] = gyr[1];
  features[feature_ix++] = gyr[2];

  if (feature_ix >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {

    // 静止检测：计算加速度均方值
    float acc_ms = 0;
    for (size_t i = 0; i < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; i += 6) {
      acc_ms += features[i]*features[i] 
              + features[i+1]*features[i+1] 
              + features[i+2]*features[i+2];
    }
    acc_ms /= (EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE / 6);

    if (acc_ms < STATIC_THRESHOLD) {
      Serial.printf("STATIC (acc_ms=%.1f) -> silent\n", acc_ms);
      feature_ix = 0;
      return;
    }

    // 推理
    signal_t signal;
    signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
    signal.get_data = &raw_feature_get_data;

    ei_impulse_result_t result = {0};
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);

    if (err != EI_IMPULSE_OK) {
      Serial.printf("ERR: run_classifier returned %d\n", err);
      feature_ix = 0;
      return;
    }

    // P(faller)
    float p_faller = 0;
    for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
      if (strcmp(result.classification[i].label, "faller") == 0) {
        p_faller = result.classification[i].value;
      }
    }

    // 短期平滑
    p_faller_history[p_history_idx] = p_faller;
    p_history_idx = (p_history_idx + 1) % SMOOTH_N;
    if (p_history_idx == 0) smooth_filled = true;

    float p_smoothed = 0;
    int n_smooth = smooth_filled ? SMOOTH_N : p_history_idx;
    if (n_smooth == 0) n_smooth = 1;
    for (int i = 0; i < n_smooth; i++) p_smoothed += p_faller_history[i];
    p_smoothed /= n_smooth;

    // 长期 risk_score
    risk_history[risk_history_idx] = p_smoothed;
    risk_history_idx = (risk_history_idx + 1) % RISK_WINDOW;
    if (risk_history_idx == 0) risk_filled = true;

    float risk_score = 0;
    int n_risk = risk_filled ? RISK_WINDOW : risk_history_idx;
    if (n_risk == 0) n_risk = 1;
    for (int i = 0; i < n_risk; i++) risk_score += risk_history[i];
    risk_score /= n_risk;

    // 输出
    Serial.printf("acc_ms=%.1f  P(faller)=%.3f  smoothed=%.3f  risk_score=%.3f  ",
                  acc_ms, p_faller, p_smoothed, risk_score);

    if (risk_score >= THRESH_HIGH) {
      consecutive_high++;
      if (consecutive_high >= N_CONSECUTIVE) {
        Serial.println("[HIGH] -> buzzer");
        feedback_high();
      } else {
        Serial.printf("[HIGH pending %d/%d]\n", consecutive_high, N_CONSECUTIVE);
      }
    } else if (risk_score >= THRESH_LOW) {
      consecutive_high = 0;
      Serial.println("[MEDIUM] -> vibration");
      feedback_medium();
    } else {
      consecutive_high = 0;
      Serial.println("[LOW] -> silent");
      feedback_low();
    }

    feature_ix = 0;
  }
}
