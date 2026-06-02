#include <Arduino.h>
#include <Wire.h>
#include <a5s-Gaitgaurd_inferencing.h>

// ===== 硬件配置 =====
#define LSM6DS3_ADDR  0x6A
#define BUZZER_PIN    1     // D0
#define ERM_PIN       2     // D1

// LSM6DS3 寄存器
#define REG_CTRL1_XL  0x10
#define REG_CTRL2_G   0x11
#define REG_OUTX_L_G  0x22  // 陀螺仪起始寄存器（之后连读到加速度）

// 量程转换系数（输出单位必须和 GSTRIDE 训练数据一致）
// LSM6DS3 默认: 加速度 ±2g, 陀螺仪 ±250dps
// 训练数据单位: 加速度 m/s², 陀螺仪 rad/s
const float ACC_SENS = 0.000488f * 9.80665f;  // ±16g: 0.488 mg/LSB
const float GYR_SENS = 0.070f    * 0.0174533f; // ±2000dps: 70 mdps/LSB

// ===== 推理 buffer =====
static float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
static size_t feature_ix = 0;

// 采样定时
const unsigned long SAMPLE_INTERVAL_US = 1000000.0 / EI_CLASSIFIER_FREQUENCY;
unsigned long next_sample_us = 0;

// 多窗口平滑（最近 3 个窗口的 P(faller) 平均）
#define SMOOTH_N 3
float p_faller_history[SMOOTH_N] = {0, 0, 0};
int   p_history_idx = 0;

// ===== 工具函数 =====
void writeReg(byte reg, byte val) {
  Wire.beginTransmission(LSM6DS3_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// 读 6 轴：陀螺(gx,gy,gz) + 加速(ax,ay,az)，连续 12 字节
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
// Low: 完全静默,不打扰用户
void feedback_low() {
  // 啥也不做
}

// Medium: 只震动,不响——温和提醒,不引起焦虑
void feedback_medium() {
  digitalWrite(ERM_PIN, HIGH);
  delay(200);
  digitalWrite(ERM_PIN, LOW);
}

// High: 只响,不震动——最大化听觉感知,确保高风险时刻被注意到
void feedback_high() {
  for (int i = 0; i < 3; i++) {
    ledcWriteTone(BUZZER_PIN, 2000);
    delay(300);
    ledcWriteTone(BUZZER_PIN, 0);
    delay(150);
  }
}

// ===== Edge Impulse 回调 =====
int raw_feature_get_data(size_t offset, size_t length, float *out_ptr) {
  memcpy(out_ptr, features + offset, length * sizeof(float));
  return 0;
}

// ===== setup / loop =====
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== GaitGuard Inference ===");

  Wire.begin();
  Wire.setClock(400000);  // 400kHz I2C

  // LSM6DS3 配置:
  // CTRL1_XL = 0x40 -> ODR=104Hz, FS=±2g
  // CTRL2_G  = 0x40 -> ODR=104Hz, FS=±250dps
  writeReg(REG_CTRL1_XL, 0x44);  // ODR=104Hz, FS=±16g - 与 GSTRIDE CSIC 一致
  writeReg(REG_CTRL2_G,  0x4C);  // ODR=104Hz, FS=±2000dps - 与 GSTRIDE CSIC 一致

  pinMode(ERM_PIN, OUTPUT);
  digitalWrite(ERM_PIN, LOW);
  ledcAttach(BUZZER_PIN, 2000, 8);

  Serial.printf("Sample interval: %lu us (%.2f Hz)\n",
                SAMPLE_INTERVAL_US, (float)EI_CLASSIFIER_FREQUENCY);
  Serial.printf("Window: %d samples x %d axes = %d floats\n",
                EI_CLASSIFIER_RAW_SAMPLE_COUNT,
                EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME,
                EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
  Serial.println("Feedback policy:");
  Serial.println("  LOW    -> silent");
  Serial.println("  MEDIUM -> vibration only");
  Serial.println("  HIGH   -> buzzer only");
  Serial.println("Starting sampling...\n");

  next_sample_us = micros();
}

void loop() {
  // 1. 等到下一个采样时刻
  if ((long)(micros() - next_sample_us) < 0) return;
  next_sample_us += SAMPLE_INTERVAL_US;

  // 2. 读 6 轴
  float acc[3], gyr[3];
  if (!readIMU(acc, gyr)) {
    Serial.println("IMU read failed");
    return;
  }

  // === DEBUG: 每 100 次采样打印一次原始 6 轴值（约每 1 秒一次）===
  static int dbg_counter = 0;
  if (++dbg_counter >= 100) {
    dbg_counter = 0;
    Serial.printf("DEBUG: acc=[%+6.2f %+6.2f %+6.2f]  gyr=[%+6.3f %+6.3f %+6.3f]\n",
                  acc[0], acc[1], acc[2], gyr[0], gyr[1], gyr[2]);
  }

  // 3. 填进 features buffer（顺序: accX, accY, accZ, gyrX, gyrY, gyrZ）
  features[feature_ix++] = acc[0];
  features[feature_ix++] = acc[1];
  features[feature_ix++] = acc[2];
  features[feature_ix++] = gyr[0];
  features[feature_ix++] = gyr[1];
  features[feature_ix++] = gyr[2];

  // 4. buffer 满了 → 跑推理
  if (feature_ix >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
    signal_t signal;
    signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
    signal.get_data = &raw_feature_get_data;

    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);

    if (err != EI_IMPULSE_OK) {
      Serial.printf("ERR: run_classifier returned %d\n", err);
      feature_ix = 0;
      return;
    }

    // 5. 找出 faller 类别的概率
    float p_faller = 0;
    for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
      if (strcmp(result.classification[i].label, "faller") == 0) {
        p_faller = result.classification[i].value;
      }
    }

    // 6. 滑动平均
    p_faller_history[p_history_idx] = p_faller;
    p_history_idx = (p_history_idx + 1) % SMOOTH_N;
    float p_smoothed = 0;
    for (int i = 0; i < SMOOTH_N; i++) p_smoothed += p_faller_history[i];
    p_smoothed /= SMOOTH_N;

    // 7. 阈值映射 → 触发反馈
    Serial.printf("P(faller)=%.3f  smoothed=%.3f  ", p_faller, p_smoothed);
    if (p_smoothed < 0.4f) {
      Serial.println("[LOW]    -> silent");
      feedback_low();
    } else if (p_smoothed < 0.7f) {
      Serial.println("[MEDIUM] -> vibration");
      feedback_medium();
    } else {
      Serial.println("[HIGH]   -> buzzer");
      feedback_high();
    }

    feature_ix = 0;  // 准备下一个窗口
  }
}