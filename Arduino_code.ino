/*
 * ============================================================================
 * LORA-ENABLED METERING FOR RURAL MICROGRIDS - SENDER NODE
 * ============================================================================
 *
 * DESCRIPTION:
 * This firmware implements a transparent serial-to-LoRa bridge. The Arduino
 * Nano receives telemetry packets from a Python application via USB serial
 * and broadcasts them over a 433 MHz SX1278 LoRa radio.
 *
 * HARDWARE:
 * - Microcontroller: Arduino Nano (ATmega328P @ 16 MHz)
 * - Radio: SX1278 / RA-02 LoRa transceiver
 * - Interface: SPI + GPIO
 *
 * VERSION: 1.0
 * YEAR: 2026
 * ============================================================================
 */

#include <SPI.h>
#include <LoRa.h>

// ============================================================================
// HARDWARE PIN DEFINITIONS
// ============================================================================

#define LORA_NSS_PIN     10
#define LORA_RESET_PIN    9
#define LORA_DIO0_PIN     2

#define LED_PIN          13

// ============================================================================
// COMMUNICATION PARAMETERS
// ============================================================================

#define SERIAL_BAUD_RATE 9600

#define LORA_FREQUENCY           433E6
#define LORA_TX_POWER            20
#define LORA_SPREADING_FACTOR     7
#define LORA_SIGNAL_BANDWIDTH 125E3
#define LORA_CODING_RATE          5

// ============================================================================
// BUFFER CONFIGURATION
// ============================================================================

#define MAX_PACKET_SIZE     255
#define SERIAL_BUFFER_SIZE  256

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

static unsigned long g_packetCounter = 0;
static unsigned long g_lastTransmitTime = 0;

static char g_serialBuffer[SERIAL_BUFFER_SIZE];
static uint16_t g_bufferIndex = 0;

static bool g_loraInitialized = false;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

void initializeSerial();
void initializeLoRa();
void blinkLED(uint8_t count, uint16_t duration_ms);
void transmitLoRaPacket(const char* data, uint16_t length);
void processSerialData();

// ============================================================================
// SETUP
// ============================================================================

void setup()
{
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  initializeSerial();
  initializeLoRa();

  blinkLED(3,200);

  Serial.println(F("\n>>> SYSTEM READY - WAITING FOR DATA <<<"));
  Serial.println(F("======================================\n"));
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void loop()
{
  processSerialData();
}

// ============================================================================
// SERIAL INITIALIZATION
// ============================================================================

void initializeSerial()
{
  Serial.begin(SERIAL_BAUD_RATE);

  while (!Serial) {;}

  while (Serial.available() > 0)
  {
    Serial.read();
  }

  Serial.println(F("\n======================================"));
  Serial.println(F(" ARDUINO NANO - LORA TRANSMITTER NODE "));
  Serial.println(F(" LoRa-Enabled Metering for Microgrids "));
  Serial.println(F("======================================"));
  Serial.print(F("Firmware Version: 1.0\n"));
  Serial.print(F("Serial Baud Rate: "));
  Serial.print(SERIAL_BAUD_RATE);
  Serial.println(F(" bps"));
  Serial.println(F("======================================\n"));
}

// ============================================================================
// LORA INITIALIZATION
// ============================================================================

void initializeLoRa()
{
  Serial.println(F("[INIT] Configuring LoRa module..."));

  LoRa.setPins(LORA_NSS_PIN, LORA_RESET_PIN, LORA_DIO0_PIN);

  Serial.print(F("NSS Pin: "));
  Serial.println(LORA_NSS_PIN);

  Serial.print(F("RESET Pin: "));
  Serial.println(LORA_RESET_PIN);

  Serial.print(F("DIO0 Pin: "));
  Serial.println(LORA_DIO0_PIN);

  Serial.print(F("\n[INIT] Starting LoRa at "));
  Serial.print(LORA_FREQUENCY / 1E6);
  Serial.println(F(" MHz"));

  if (!LoRa.begin(LORA_FREQUENCY))
  {
    Serial.println(F("\nFATAL: LoRa initialization failed"));

    while (1)
    {
      digitalWrite(LED_PIN,HIGH);
      delay(100);
      digitalWrite(LED_PIN,LOW);
      delay(100);
    }
  }

  Serial.println(F("[OK] LoRa module initialized"));

  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.setSpreadingFactor(LORA_SPREADING_FACTOR);
  LoRa.setSignalBandwidth(LORA_SIGNAL_BANDWIDTH);
  LoRa.setCodingRate4(LORA_CODING_RATE);
  LoRa.enableCrc();
  LoRa.setPreambleLength(8);

  // Private network sync word
  LoRa.setSyncWord(0xA5);

  Serial.println(F("[CONFIG] Private Sync Word: 0xA5"));
  Serial.println(F("[READY] LoRa operational\n"));

  g_loraInitialized = true;
}

// ============================================================================
// LED BLINK
// ============================================================================

void blinkLED(uint8_t count, uint16_t duration_ms)
{
  for(uint8_t i=0;i<count;i++)
  {
    digitalWrite(LED_PIN,HIGH);
    delay(duration_ms);

    digitalWrite(LED_PIN,LOW);
    delay(duration_ms);
  }
}

// ============================================================================
// LORA TRANSMISSION (SILENT OPERATION)
// ============================================================================

void transmitLoRaPacket(const char* data, uint16_t length)
{
  if(!g_loraInitialized) return;

  if(data == NULL || length == 0) return;

  if(length > MAX_PACKET_SIZE)
  {
    length = MAX_PACKET_SIZE;
  }

  digitalWrite(LED_PIN,HIGH);

  LoRa.beginPacket();
  LoRa.print(data);
  bool success = LoRa.endPacket();

  digitalWrite(LED_PIN,LOW);

  if(success)
  {
    g_packetCounter++;
    g_lastTransmitTime = millis();
  }
}

// ============================================================================
// SERIAL DATA PROCESSING (SILENT)
// ============================================================================

void processSerialData()
{
  while(Serial.available() > 0)
  {
    char incomingChar = Serial.read();

    if(incomingChar == '\n' || incomingChar == '\r')
    {
      if(g_bufferIndex > 0)
      {
        g_serialBuffer[g_bufferIndex] = '\0';

        char* start = g_serialBuffer;

        while(*start == ' ' || *start == '\t')
        {
          start++;
        }

        char* end = start + strlen(start) - 1;

        while(end > start && (*end==' ' || *end=='\t' || *end=='\r' || *end=='\n'))
        {
          *end = '\0';
          end--;
        }

        uint16_t trimmedLength = strlen(start);

        if(trimmedLength > 0)
        {
          transmitLoRaPacket(start,trimmedLength);
        }

        g_bufferIndex = 0;
      }
    }
    else
    {
      if(g_bufferIndex < SERIAL_BUFFER_SIZE-1)
      {
        g_serialBuffer[g_bufferIndex++] = incomingChar;
      }
      else
      {
        g_bufferIndex = 0;

        while(Serial.available() > 0)
        {
          Serial.read();
        }
      }
    }
  }
}