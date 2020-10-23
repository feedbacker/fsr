#include <inttypes.h>

#ifdef CORE_TEENSY
  // Use the Joystick library for Teensy
  void ButtonStart() {
    Joystick.begin();
    Joystick.useManualSend(true);
  }
  void ButtonPress(uint8_t button_num) {
    Joystick.button(button_num, 1);
  }
  void ButtonRelease(uint8_t button_num) {
    Joystick.button(button_num, 0);
  }
#else
  // And the Keyboard library for Arduino
  void ButtonStart() {
    Keyboard.begin();
  }
  void ButtonPress(uint8_t button_num) {
    Keyboard.press('a' + button_num - 1);
  }
  void ButtonRelease(uint8_t button_num) {
    Keyboard.release('a' + button_num - 1);
  }
#endif

// Default threshold value for each of the sensors.
const int16_t kDefaultThreshold = 200;
// Max window size for both of the moving averages classes.
const size_t kWindowSize = 100;
// Baud rate used for Serial communication. Technically ignored by Teensys.
const int32_t kBaudRate = 115200;

/*===========================================================================*/

// Calculates the Weighted Moving Average for a given period size.
// Expected values provided to this class should fall in [−32,768, 32,767]
// otherwise it will overflow. We use a 32-bit integer for the intermediate
// sums which we then restrict back down.
class WeightedMovingAverage {
 public:
  WeightedMovingAverage(size_t size) : size_(min(size, kWindowSize)) {}

  int16_t GetAverage(int16_t value) {
    // Add current value and remove oldest value.
    // e.g. with value = 5 and cur_count_ index = 0
    // [1, 2, 3, 4] -> 10 becomes 10 + 5 - 1 = 14 -> [5, 2, 3, 4]
    int32_t next_sum = cur_sum_ + value - values_[cur_count_];
    // Update weighted sum giving most weight to the newest value.
    // [1*1, 2*2, 3*3, 4*4] -> 30 becomes 30 + 4*5 - 10 = 40
    //     -> [4*5, 1*2, 2*3, 3*4]
    // Subtracting by cur_sum_ is the same as removing 1 from each of the weight
    // coefficients.
    int32_t next_weighted_sum = cur_weighted_sum_ + size_ * value - cur_sum_;
    cur_sum_ = next_sum;
    cur_weighted_sum_ = next_weighted_sum;
    values_[cur_count_] = value;
    cur_count_ = (cur_count_ + 1) % size_;
    // Integer division is fine here since both the numerator and denominator
    // are integers and we need to return an int anyways. Off by one isn't
    // substantial here.
    // Sum of weights = sum of all integers from [1, size_]
    return next_weighted_sum/((size_ * (size_ + 1)) / 2);
  }

  // Delete default constructor. Size MUST be explicitly specified.
  WeightedMovingAverage() = delete;

 private:
  size_t size_;
  int32_t cur_sum_;
  int32_t cur_weighted_sum_;
  // Keep track of all values we have in a circular array.
  int16_t values_[kWindowSize];
  size_t cur_count_;
};

// Calculates the Hull Moving Average. This is one of the better smoothing
// algorithms that will smooth the input values without wildly distorting the
// input values while still being responsive to input changes.
//
// The algorithm is essentially:
//   1. Calculate WMA of input values with a period of n/2 and multiply it by 2.
//   2. Calculate WMA of input values with a period of n and subtract it from
//      step 1.
//   3. Calculate WMA of the values from step 2 with a period of sqrt(2).
//
// HMA = WMA( 2 * WMA(input, n/2) - WMA(input, n), sqrt(n) )
class HullMovingAverage {
 public:
  HullMovingAverage(size_t size) :
      wma1_(size/2), wma2_(size), hull_(sqrt(size)) {}

  int16_t GetAverage(int16_t value) {
    int16_t wma1_value = wma1_.GetAverage(value);
    int16_t wma2_value = wma2_.GetAverage(value);
    int16_t hull_value = hull_.GetAverage(2 * wma1_value - wma2_value);

    return hull_value;
  }

 private:
  WeightedMovingAverage wma1_;
  WeightedMovingAverage wma2_;
  WeightedMovingAverage hull_;
};

/*===========================================================================*/

// Class containing all relevant information per sensor.
class SensorState {
 public:
  SensorState(uint8_t pin_value) :
      pin_value_(pin_value), state_(SensorState::OFF),
      user_threshold_(kDefaultThreshold), moving_average_(kWindowSize),
      offset_(0) {}

  // Fetches the sensor value and maybe triggers the button press/release.
  void EvaluateSensor(uint8_t button_num, unsigned long curMillis) {
    int16_t sensor_value = analogRead(pin_value_);

    // Fetch the updated Weighted Moving Average.
    cur_value_ = moving_average_.GetAverage(sensor_value) - offset_;

    if (cur_value_ >= user_threshold_ + kPaddingWidth &&
        state_ == SensorState::OFF) {
      ButtonPress(button_num);
      state_ = SensorState::ON;
      last_trigger_ms_ = curMillis;
    }
    
    if (cur_value_ < user_threshold_ - kPaddingWidth &&
        state_ == SensorState::ON) {
      ButtonRelease(button_num);
      state_ = SensorState::OFF;
    }

//    if (state_ == SensorState::OFF && curMillis - last_trigger_ms_ >= 3000) {
//      UpdateOffset();
//    }
  }

  void UpdateThreshold(int16_t new_threshold) {
    user_threshold_ = new_threshold;
  }

  int16_t UpdateOffset() {
    // Update the offset with the last read value. UpdateOffset should be
    // called with no applied pressure on the panels so that it will be
    // calibrated correctly.
    offset_ = cur_value_;
    return offset_;
  }

  int16_t GetCurValue() {
    return cur_value_;
  }

  int16_t GetThreshold() {
    return user_threshold_;
  }

  // Delete default constructor. Pin number MUST be explicitly specified.
  SensorState() = delete;
 
 private:
  // The pin on the Teensy/Arduino corresponding to this sensor.
  uint8_t pin_value_;
  // The current joystick state of the sensor.
  enum State { OFF, ON };
  State state_;
  // The user defined threshold value to activate/deactivate this sensor at.
  int16_t user_threshold_;
  // One-tailed width size to create a window around user_threshold_ to
  // mitigate fluctuations by noise. 
  // TODO(teejusb): Make this a user controllable variable.
  const int16_t kPaddingWidth = 1;
  
  // The smoothed moving average calculated to reduce some of the noise. 
  HullMovingAverage moving_average_;

  // The latest value obtained for this sensor.
  int16_t cur_value_;
  // How much to shift the value read by during each read.
  int16_t offset_;
  // Timestamp of when the last time this sensor was triggered.
  unsigned long last_trigger_ms_ = 0;
};

/*===========================================================================*/

// Defines the sensor collections and sets the pins for them appropriately.
// NOTE(teejusb): These may need to be changed depending on the pins users
// connect their FSRs to.
SensorState kSensorStates[] = {
  SensorState(A0),
  SensorState(A1),
  SensorState(A2),
  SensorState(A3),
};
const size_t kNumSensors = sizeof(kSensorStates)/sizeof(SensorState);

/*===========================================================================*/

class SerialProcessor {
 public:
   void Init(unsigned int baud_rate) {
    Serial.begin(baud_rate);
  }

  void CheckAndMaybeProcessData() {
    while (Serial.available() > 0) {
      size_t bytes_read = Serial.readBytesUntil(
          '\n', buffer_, kBufferSize - 1);
      buffer_[bytes_read] = '\0';

      if (bytes_read == 0) { return; }
 
      switch(buffer_[0]) {
        case 'o':
        case 'O':
          UpdateOffsets();
          break;
        case 'v':
        case 'V':
          PrintValues();
          break;
        case 't':
        case 'T':
          PrintThresholds();
          break;
        default:
          UpdateAndPrintThreshold(bytes_read);
          break;
      }
    }  
  }

  void UpdateAndPrintThreshold(size_t bytes_read) {
    // Need to specify:
    // Sensor number + Threshold value.
    // {0, 1, 2, 3} + "0"-"1023"
    // e.g. 3180 (fourth FSR, change threshold to 180)
    
    if (bytes_read < 2 || bytes_read > 5) { return; }

    size_t sensor_index = buffer_[0] - '0';
    if (sensor_index < 0 || sensor_index >= kNumSensors) { return; }

    kSensorStates[sensor_index].UpdateThreshold(
        strtoul(buffer_ + 1, nullptr, 10));
    PrintThresholds();
  }

  void UpdateOffsets() {
    for (size_t i = 0; i < kNumSensors; ++i) {
      kSensorStates[i].UpdateOffset();
    }
  }

  void PrintValues() {
    Serial.print("v");
    for (size_t i = 0; i < kNumSensors; ++i) {
      Serial.print(" ");
      Serial.print(kSensorStates[i].GetCurValue());
    }
    Serial.print("\n");
  }

  void PrintThresholds() {
    Serial.print("t");
    for (size_t i = 0; i < kNumSensors; ++i) {
      Serial.print(" ");
      Serial.print(kSensorStates[i].GetThreshold());
    }
    Serial.print("\n");
  }

 private:
   static const size_t kBufferSize = 64;
   char buffer_[kBufferSize];
};

/*===========================================================================*/

SerialProcessor serialProcessor;
// Durations are always "unsigned long" regardless of board type.
// Don't need to explicitly worry about the widths.
unsigned long delayOverhead = 0;

void setup() {
  // See how long the delay function itself takes.
  // We use this to try and get a stable 1KHz polling rate.
  unsigned long startMicros = micros();
  delayMicroseconds(1000);
  delayOverhead = 1000 - (micros() - startMicros);
  if (delayOverhead < 0) {
    delayOverhead = 0;
  }
  
  serialProcessor.Init(kBaudRate);
  ButtonStart();
}

void loop() {
  unsigned long startMicros = micros();

  serialProcessor.CheckAndMaybeProcessData();

  unsigned long curMillis = millis();  
  for (size_t i = 0; i < kNumSensors; ++i) {
    kSensorStates[i].EvaluateSensor(i + 1, curMillis);
  }

  #ifdef CORE_TEENSY
    Joystick.send_now();
  #endif

  // How long did this poll take?
  unsigned long duration = micros() - startMicros;

  // Delay an appropriate amount to try and get consistent polling.
  if (1000 - delayOverhead - duration > 0) {
    delayMicroseconds(1000 - delayOverhead - duration);
  }
}
