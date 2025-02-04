#include <PID_v1.h>
#include <LMotorController.h>
#include "I2Cdev.h"
#include <SoftwareSerial.h>
#include <Encoder.h>

#include "MPU6050_6Axis_MotionApps20.h"

#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
#include "Wire.h"
#endif

#define MOVE_BACK_FORTH 0
#define MIN_ABS_SPEED 80

//SoftwareSerial mySerial(13, 66);

Encoder left(9, 6);
Encoder right(10, 11);
long oldPosition  = -999;
int encCount = 0;
//MPU
unsigned long counter;
MPU6050 mpu;

// MPU control/status vars
bool dmpReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer

// orientation/motion vars
Quaternion q;           // [w, x, y, z]         quaternion container
VectorFloat gravity;    // [x, y, z]            gravity vector
float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector


//PID

double originalSetpoint = 174.29;
double angSetpoint = originalSetpoint;
double movingAngleOffset = 0.5;
double angInput = originalSetpoint, angOutput;
int moveState = 0; //0 = balance; 1 = back; 2 = forth

double posInput, posOutput, posSetpoint = 0;

PID angPID(&angInput, &angOutput, &angSetpoint, 25, 240, 2, DIRECT);
PID posPID(&posInput, &posOutput, &posSetpoint, 4, 20, 2, REVERSE);

//MOTOR CONTROLLER

int motorSpeed;
double motorSpeedFactorLeft = 0.8;
double motorSpeedFactorRight = 1;
//MOTOR CONTROLLER
const int ENA = 3;
const int IN1 = 4;
const int IN2 = 7;
const int IN3 = 8;
const int IN4 = 12;
const int ENB = 5;
LMotorController motorController(ENA, IN1, IN2, ENB, IN3, IN4, motorSpeedFactorLeft, motorSpeedFactorRight);


//timers

long time5Hz = 0;


volatile bool mpuInterrupt = false;     // indicates whether MPU interrupt pin has gone high
void dmpDataReady()
{
  mpuInterrupt = true;
}


void setup()
{
  // join I2C bus (I2Cdev library doesn't do this automatically)
#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
  Wire.begin();
  TWBR = 24; // 400kHz I2C clock (200kHz if CPU is 8MHz)
#elif I2CDEV_IMPLEMENTATION == I2CDEV_BUILTIN_FASTWIRE
  Fastwire::setup(400, true);
#endif


  Serial.begin(115200);

  while (!Serial); // wait for Leonardo enumeration, others continue immediately

  // initialize device
  Serial.println(F("Initializing I2C devices..."));
  mpu.initialize();

  // verify connection
  Serial.println(F("Testing device connections..."));
  Serial.println(mpu.testConnection() ? F("MPU6050 connection successful") : F("MPU6050 connection failed"));

  // load and configure the DMP
  Serial.println(F("Initializing DMP..."));
  devStatus = mpu.dmpInitialize();

  // supply your own gyro offsets here, scaled for min sensitivity
  mpu.setXGyroOffset(220);
  mpu.setYGyroOffset(76);
  mpu.setZGyroOffset(-85);
  mpu.setZAccelOffset(1788); // 1688 factory default for my test chip

  // make sure it worked (returns 0 if so)
  if (devStatus == 0)
  {
    // turn on the DMP, now that it's ready
    Serial.println(F("Enabling DMP..."));
    mpu.setDMPEnabled(true);

    // enable Arduino interrupt detection
    Serial.println(F("Enabling interrupt detection (Arduino external interrupt 0)..."));
    attachInterrupt(0, dmpDataReady, RISING);
    mpuIntStatus = mpu.getIntStatus();

    // set our DMP Ready flag so the main loop() function knows it's okay to use it
    Serial.println(F("DMP ready! Waiting for first interrupt..."));
    dmpReady = true;

    // get expected DMP packet size for later comparison
    packetSize = mpu.dmpGetFIFOPacketSize();

    //setup PID

    angPID.SetMode(AUTOMATIC);
    angPID.SetSampleTime(10);
    angPID.SetOutputLimits(-255, 255);

    posPID.SetMode(AUTOMATIC);
    posPID.SetSampleTime(10);
    posPID.SetOutputLimits(-movingAngleOffset, movingAngleOffset);
  }
  else
  {
    // ERROR!
    // 1 = initial memory load failed
    // 2 = DMP configuration updates failed
    // (if it's going to break, usually the code will be 1)
    Serial.print(F("DMP Initialization failed (code "));
    Serial.print(devStatus);
    Serial.println(F(")"));
  }
}


void loop() {


  // if programming failed, don't try to do anything
  if (!dmpReady) {
    return;
  }

  // wait for MPU interrupt or extra packet(s) available
  while (!mpuInterrupt && fifoCount < packetSize)
  {

    //Stop Motors if robot falls
    if (abs(angInput - originalSetpoint) > 20) {
      motorController.move(0);
      break;
    }

    double newPosition = (right.read() + left.read()) / 2;
    if (newPosition != oldPosition && abs(newPosition - oldPosition) > 5) {
      oldPosition = newPosition;
      posInput = newPosition / 100;
      //Serial.println(posInput);
    }

    // Serial.println(posInput);

    posPID.Compute();

    calculateAngleSetpoint();

    angPID.Compute();

    if (angInput > angSetpoint + 0.3 || angInput < angSetpoint - 0.3) {
      motorController.move(angOutput, MIN_ABS_SPEED);
    }
    else {
      motorController.move(0);
    }

    /*
        Serial.print(posInput);
        Serial.print("\t");
        Serial.println(angSetpoint);

           Serial.print("\t");
           Serial.print(angInput);
           Serial.print("\t");
           Serial.println(posInput);
           Serial.print(posOutput);
           Serial.print("\t");
           Serial.print(angSetpoint);
           Serial.print("\t");
    */


    unsigned long currentMillis = millis();
    if (currentMillis - time5Hz >= 5000)
    {
      //moveBackForth();
      time5Hz = currentMillis;
    }


  }

  // reset interrupt flag and get INT_STATUS byte
  mpuInterrupt = false;
  mpuIntStatus = mpu.getIntStatus();

  // get current FIFO count
  fifoCount = mpu.getFIFOCount();

  // check for overflow (this should never happen unless our code is too inefficient)
  if ((mpuIntStatus & 0x10) || fifoCount == 1024)
  {
    // reset so we can continue cleanly
    mpu.resetFIFO();
    Serial.println(F("FIFO overflow!"));

    // otherwise, check for DMP data ready interrupt (this should happen frequently)
  }
  else if (mpuIntStatus & 0x02)
  {
    // wait for correct available data length, should be a VERY short wait
    while (fifoCount < packetSize) fifoCount = mpu.getFIFOCount();

    // read a packet from FIFO
    mpu.getFIFOBytes(fifoBuffer, packetSize);

    // track FIFO count here in case there is > 1 packet available
    // (this lets us immediately read more without waiting for an interrupt)
    fifoCount -= packetSize;

    mpu.dmpGetQuaternion(&q, fifoBuffer);
    mpu.dmpGetGravity(&gravity, &q);
    mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);

    angInput = ypr[1] * 180 / M_PI + 180;
  }
}

void calculateAngleSetpoint() {

  if (Serial.available()) {
    counter = millis();
    char c = Serial.read();

    //Serial.println(c);

    switch (c) {

      case 'w':
        motorController.setMotorConst(1, 1);
        angSetpoint = originalSetpoint - movingAngleOffset;
        break;

      case 's':
        motorController.setMotorConst(1, 1);
        angSetpoint = originalSetpoint + movingAngleOffset;
        break;

      case 'a':
        motorController.setMotorConst(0.5, 1);
        angSetpoint = originalSetpoint + movingAngleOffset;
        break;

      case 'd':
        motorController.setMotorConst(1, 0.5);
        angSetpoint = originalSetpoint + movingAngleOffset;
        break;
    }

    posInput = 0;
    left.write(0);
    right.write(0);
    posPID.Compute();
  }
  else if (millis() - counter > 500) {
    angSetpoint = originalSetpoint + posOutput;
    motorController.setMotorConst(0.75, 1);
  }
}



//move back and forth


void moveBackForth()
{
  moveState++;
  if (moveState > 2) moveState = 0;

  if (moveState == 0)
    angSetpoint = originalSetpoint;
  else if (moveState == 1)
    angSetpoint = originalSetpoint - 1;
  else
    angSetpoint = originalSetpoint + 1;
}

