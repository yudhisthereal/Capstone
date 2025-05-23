#include "boxing_model_left.h"

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <TensorFlowLite_ESP32.h>

#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Configuration macros
#define USE_SERIAL 0
#define USE_MQTT 1
#define USE_DUMMY 0

// I2C Pins
#define SDA_PIN 21
#define SCL_PIN 22

// Input Data
#define SEQ_LENGTH 12
#define N_FEATURES 6
#define N_INPUTS (SEQ_LENGTH * N_FEATURES)
#define STRIDE 1 // how often to classify

#define N_OUTPUTS 3
#define TENSOR_ARENA_SIZE 24 * 1024

float inputBuffer[SEQ_LENGTH][N_FEATURES] = {0};
int inputIndex = 0;
int sampleCount = 0;
int strideCounter = 0;
bool sequenceReady = false;

// Declare TFLite globals
tflite::MicroInterpreter *interpreter;
tflite::ErrorReporter *error_reporter;
tflite::AllOpsResolver resolver;
tflite::MicroErrorReporter micro_error_reporter;

const tflite::Model *model = nullptr;
TfLiteTensor *input = nullptr;
TfLiteTensor *output = nullptr;

uint8_t tensor_arena[TENSOR_ARENA_SIZE];

// WiFi credentials
const char *ssid = "Xiaomi Biru";
const char *password = "tH3od0rer8seve!+";

// MQTT Broker
const char *mqtt_broker = "3e065ffaa6084b219bc6553c8659b067.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char *mqtt_username = "CapstoneUser";
const char *mqtt_password = "Mango!River_42Sun";

// MQTT topics
const char *topic_publish_data = "boxing/raw_data_left"; // raw data
const char *topic_publish_punch = "boxing/punch_type";   // classification results
const char *topic_subscribe = "boxing/control";          // Optional: for receiving commands

// Moving Average Filter Configuration
const int WINDOW_SIZE = 5;
float accelXBuffer[WINDOW_SIZE] = {0};
float accelYBuffer[WINDOW_SIZE] = {0};
float accelZBuffer[WINDOW_SIZE] = {0};
float gyroXBuffer[WINDOW_SIZE] = {0};
float gyroYBuffer[WINDOW_SIZE] = {0};
float gyroZBuffer[WINDOW_SIZE] = {0};
int bufferIndex = 0;
bool bufferFilled = false;

// Dummy Punch
int dummyPunchIndex = 0;

// Bias (IMU KIRI)
const float bias_ax = 1.073;
const float bias_ay = -0.041;
const float bias_az = 0.164;
const float bias_gx = -0.107;
const float bias_gy = -0.015;
const float bias_gz = -0.020;

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);
Adafruit_MPU6050 mpu;

// Timing
unsigned long previousMillis = 0;
const long interval = 50;

unsigned long previousPunchMillis = 0;
const long punchInterval = 150;
const char *punchTypes[] = {"HOOK", "JAB", "NO PUNCH"};
// const int numPunchTypes = sizeof(punchTypes) / sizeof(punchTypes[0]);

float applyMovingAverage(float newValue, float buffer[], bool &bufferFilled)
{
  buffer[bufferIndex] = newValue;
  float sum = 0;
  int count = bufferFilled ? WINDOW_SIZE : bufferIndex + 1;
  for (int i = 0; i < count; i++)
    sum += buffer[i];
  bufferIndex = (bufferIndex + 1) % WINDOW_SIZE;
  if (!bufferFilled && bufferIndex == 0)
    bufferFilled = true;
  return sum / count;
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
#if USE_SERIAL
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("]: ");
  for (int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }
  Serial.println();
#endif
}

void setupMQTT()
{
  mqttClient.setServer(mqtt_broker, mqtt_port);
  mqttClient.setCallback(mqttCallback);
}

void reconnect()
{
#if USE_SERIAL
  Serial.println("Connecting to MQTT Broker...");
#endif
  while (!mqttClient.connected())
  {
    String clientId = "ESP32-MPU6050-";
    clientId += String(random(0xffff), HEX);
#if USE_SERIAL
    Serial.print("Attempting connection as: ");
    Serial.println(clientId);
#endif
    if (mqttClient.connect(clientId.c_str(), mqtt_username, mqtt_password))
    {
#if USE_SERIAL
      Serial.println("Connected to MQTT Broker");
#endif
      mqttClient.subscribe(topic_subscribe);
    }
    else
    {
#if USE_SERIAL
      Serial.print("Failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" Retrying in 5 seconds...");
#endif
      delay(5000);
    }
  }
}

void setupModel()
{
  Serial.println("=== [Model Setup Start] ===");

  // Set up logging (optional but useful for debugging)
  error_reporter = &micro_error_reporter;

  // Load model
  model = tflite::GetModel(boxing_model_tflite);
  if (model->version() != TFLITE_SCHEMA_VERSION)
  {
    Serial.println("Model schema version mismatch!");
    return;
  }

  // Set up resolver and interpreter
  interpreter = new tflite::MicroInterpreter(model, resolver, tensor_arena, TENSOR_ARENA_SIZE, error_reporter);

  // Allocate memory for tensors
  TfLiteStatus allocate_status = interpreter->AllocateTensors();
  if (allocate_status != kTfLiteOk)
  {
    Serial.println("AllocateTensors() failed");
    return;
  }

  // Get pointers to input and output tensors
  input = interpreter->input(0);
  output = interpreter->output(0);

  Serial.println("=== [Model Setup Complete] ===");
}

void setup()
{
  Serial.begin(115200);
#if USE_SERIAL
  delay(1000);
  Serial.println("Initializing MPU6050...");
#endif

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!mpu.begin())
  {
    Serial.println("MPU6050 not found!");
    while (1)
      delay(10);
  }

  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

#if USE_MQTT
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  wifiClient.setInsecure();
  setupMQTT();
#endif

#if USE_DUMMY
#else
  setupModel();
#endif
}

class BoxingModel
{
public:
  void predict(float *input_data, float *output_data)
  {
    for (int i = 0; i < N_INPUTS; i++)
    {
      input->data.f[i] = input_data[i];
    }

    if (interpreter->Invoke() != kTfLiteOk)
    {
      Serial.println("Model invocation failed!");
      return;
    }

    for (int i = 0; i < N_OUTPUTS; i++)
    {
      output_data[i] = output->data.f[i];
    }
  }
};

BoxingModel ml;

void loop()
{
  if (!mqttClient.connected())
    reconnect();
  mqttClient.loop();

  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval)
  {
    previousMillis = currentMillis;

    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    float ax = applyMovingAverage(a.acceleration.x, accelXBuffer, bufferFilled) + bias_ax;
    float ay = applyMovingAverage(a.acceleration.y, accelYBuffer, bufferFilled) + bias_ay;
    float az = applyMovingAverage(a.acceleration.z, accelZBuffer, bufferFilled) + bias_az;
    float gx = applyMovingAverage(g.gyro.x, gyroXBuffer, bufferFilled) + bias_gx;
    float gy = applyMovingAverage(g.gyro.y, gyroYBuffer, bufferFilled) + bias_gy;
    float gz = applyMovingAverage(g.gyro.z, gyroZBuffer, bufferFilled) + bias_gz;

    String sensorData = String(ax, 3) + "," + String(ay, 3) + "," + String(az, 3) + "," +
                        String(gx, 3) + "," + String(gy, 3) + "," + String(gz, 3);

#if USE_SERIAL
    Serial.println(sensorData);
#endif

    mqttClient.publish(topic_publish_data, sensorData.c_str());

    // Shift buffer left by 1 to make room for new sample
    for (int i = 0; i < SEQ_LENGTH - 1; i++)
    {
      for (int j = 0; j < N_FEATURES; j++)
      {
        inputBuffer[i][j] = inputBuffer[i + 1][j];
      }
    }

    inputBuffer[SEQ_LENGTH - 1][0] = ax;
    inputBuffer[SEQ_LENGTH - 1][1] = ay;
    inputBuffer[SEQ_LENGTH - 1][2] = az;
    inputBuffer[SEQ_LENGTH - 1][3] = gx;
    inputBuffer[SEQ_LENGTH - 1][4] = gy;
    inputBuffer[SEQ_LENGTH - 1][5] = gz;

    sampleCount++;
    strideCounter++;

    if (strideCounter >= STRIDE && sampleCount >= SEQ_LENGTH)
    {
      sequenceReady = true;
      strideCounter = 0; // reset after every classification
    }
  }

  if (currentMillis - previousPunchMillis >= punchInterval)
  {
    previousPunchMillis = currentMillis;

#if USE_DUMMY
    inputIndex = (inputIndex + 1) % SEQ_LENGTH;
    // Calculate magnitude of acceleration
    float accelMagnitude = sqrt(
        inputBuffer[(inputIndex == 0 ? SEQ_LENGTH : inputIndex) - 1][0] * inputBuffer[(inputIndex == 0 ? SEQ_LENGTH : inputIndex) - 1][0] +
        inputBuffer[(inputIndex == 0 ? SEQ_LENGTH : inputIndex) - 1][1] * inputBuffer[(inputIndex == 0 ? SEQ_LENGTH : inputIndex) - 1][1] +
        inputBuffer[(inputIndex == 0 ? SEQ_LENGTH : inputIndex) - 1][2] * inputBuffer[(inputIndex == 0 ? SEQ_LENGTH : inputIndex) - 1][2]);

    static bool sentNoPunch = false;

    if (accelMagnitude > 17.0)
    {
      const char *dummyPunch = punchTypes[dummyPunchIndex];
      dummyPunchIndex = (dummyPunchIndex + 1) % (numPunchTypes);
      String result = String(dummyPunch) + ", Right";
      mqttClient.publish(topic_publish_punch, result.c_str());
      sentNoPunch = false;
    }
    else if (!sentNoPunch)
    {
      String result = String("no_punch") + ", Right";
      mqttClient.publish(topic_publish_punch, result.c_str());
      sentNoPunch = true;
    }
#else
    if (sequenceReady)
    {
      float flatInput[N_INPUTS];

      // Fill flatInput from circular buffer
      for (int i = 0; i < SEQ_LENGTH; i++)
      {
        for (int j = 0; j < N_FEATURES; j++)
        {
          flatInput[i * N_FEATURES + j] = inputBuffer[i][j];
        }
      }

      float output[N_OUTPUTS];
      ml.predict(flatInput, output);

      int predictedClass = 0;
      float maxProb = output[predictedClass];

      for (byte i = 1; i < 3; i++)
      {
        if (output[i] > maxProb)
        {
          maxProb = output[i];
          predictedClass = i;
        }
      }

      const char *punchLabel = punchTypes[predictedClass];
      String result = String(punchLabel) + ", Left";

      // Serial.println("PREDICTED: " + result + " (" + String(output[predictedClass]) + ")");
      Serial.println("PUNCH: " + String(punchLabel));
      if (output[predictedClass] > 0.5)
      {
        mqttClient.publish(topic_publish_punch, result.c_str());
      }

      sequenceReady = false;
    }
#endif
  }
}
