#include <Arduino_BMI270_BMM150.h>

void setup() {
  Serial.begin(115200);
  while (!Serial);

  // Initialize the onboard BMI270 IMU
  if (!IMU.begin()) {
    Serial.println("Failed to initialize BMI270 IMU!");
    while (1);
  }

  // Print the CSV header for your dataset
  Serial.println("timestamp_ms,accel_x,accel_y,accel_z");
}

void loop() {
  float xAccel, yAccel, zAccel;

  // Check if new acceleration data is available
  if (IMU.accelerationAvailable()) {
    IMU.readAcceleration(xAccel, yAccel, zAccel);

    // Output data in CSV format for easy logging
    Serial.print(millis());
    Serial.print(",");
    Serial.print(xAccel, 3);
    Serial.print(",");
    Serial.print(yAccel, 3);
    Serial.print(",");
    Serial.println(zAccel, 3);
  }

  // Sample at roughly 100 Hz
  delay(10);
}
