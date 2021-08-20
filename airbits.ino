//SGP30 needs 15 seconds to initialize calibration after power on.
//The screen will display TVOC and CO2

#include <M5Core2.h>
#include <driver/i2s.h>
#include "fft.h"
#include "Adafruit_SGP30.h"
#include "FastLED.h"
#include <WiFiClientSecure.h>
#include <MQTTClient.h>
#include <ArduinoJson.h>
#include "WiFi.h"

// Edit the secrets file
// cp secrets-sample.h secrets.h
#include "secrets.h"

#define VERSION 1
#define PIN_CLK  0
#define PIN_DATA 34
#define MODE_MIC 1

#define AWS_IOT_PUBLISH_TOPIC   "$aws/things/airbit/shadow/update"
#define AWS_IOT_SUBSCRIBE_TOPIC "$aws/things/airbit/shadow/delta"

WiFiClientSecure net = WiFiClientSecure();
MQTTClient client = MQTTClient(256);

Adafruit_SGP30 sgp;
int co2 = 0;
int tvoc = 0;
float sound = 0;
float decibels = 0;
bool sendToAWS = false;
bool darkMode = true;

unsigned long lastMillis = 0;

TFT_eSprite DisFFTbuff =  TFT_eSprite(&M5.Lcd);
static QueueHandle_t fftvalueQueue = nullptr;
static QueueHandle_t i2sstateQueue = nullptr;

#define LEDS_PIN 25
#define LEDS_NUM 10
CRGB ledsBuff[LEDS_NUM];

void header(const char *string)
{
    M5.Lcd.setTextSize(1);
    if(darkMode){
      M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
      M5.Lcd.fillRect(0, 0, 320, 30, TFT_BLACK);  
    } else {
      M5.Lcd.setTextColor(TFT_BLACK, TFT_WHITE);
      M5.Lcd.fillRect(0, 0, 320, 30, TFT_WHITE);  
    }
    
    M5.Lcd.setTextDatum(TC_DATUM);
    M5.Lcd.drawString(string, 160, 3, 4); 
    if(sendToAWS){
      if(darkMode)
        M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
      else
        M5.Lcd.setTextColor(TFT_YELLOW, TFT_WHITE);
      M5.Lcd.drawString("AWS", 295, 3, 4);  
    }
}

void subheader(const char *string, uint16_t color)
{
  M5.Lcd.setTextSize(1);
  if(darkMode){
    M5.Lcd.setTextColor(color, TFT_BLACK);
    M5.Lcd.fillRect(0, 0, 320, 60, TFT_BLACK);
  } else {
    M5.Lcd.setTextColor(color, TFT_WHITE);
    M5.Lcd.fillRect(0, 0, 320, 60, TFT_WHITE);
  }
  M5.Lcd.setTextDatum(TC_DATUM);
  M5.Lcd.drawString(string, 160, 30, 4); 
}

void clearScreen(){
  if(darkMode){
    M5.Lcd.fillRect(0, 100, 320, 0, TFT_BLACK);  
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  } else {
    M5.Lcd.fillRect(0, 100, 320, 0, TFT_WHITE);  
    M5.Lcd.setTextColor(TFT_BLACK, TFT_WHITE);
  }
  M5.Lcd.drawString("      ", 130, 40 , 4);
  M5.Lcd.drawString("      ", 130, 80 , 4);
  M5.Lcd.drawString("      ", 130, 130 , 4);
  FastLED.clear();
}

void connectAWS()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  subheader("Connecting to Wi-Fi...", TFT_YELLOW);
  Serial.println("Connecting to Wi-Fi...");

  while (WiFi.status() != WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }

  // Configure WiFiClientSecure to use the AWS IoT device credentials
  net.setCACert(AWS_CERT_CA);
  net.setCertificate(AWS_CERT_CRT);
  net.setPrivateKey(AWS_CERT_PRIVATE);

  // Connect to the MQTT broker on the AWS endpoint we defined earlier
  client.begin(AWS_IOT_ENDPOINT, 8883, net);

  // Create a message handler
  client.onMessage(messageHandler);

  Serial.print("Connecting to AWS IOT");
  subheader("Connecting to AWS IOT...", TFT_YELLOW);

  while (!client.connect(THINGNAME)) {
    Serial.print(".");
    delay(100);
  }

  if(!client.connected()){
    Serial.println("AWS IoT Timeout!");
    return;
  }

  // Subscribe to a topic
  client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC);

  Serial.println("AWS IoT Connected!");
  subheader("", TFT_WHITE);
}

typedef struct {
    uint8_t state;
    void* audioPtr;
    uint32_t audioSize;
} i2sQueueMsg_t;

bool InitI2SSpakerOrMic(int mode){

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // is fixed at 12bit, stereo, MSB
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 2,
        .dma_buf_len = 1024,
    };
    if (mode == MODE_MIC)
    {
        i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
    }

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_pin_config_t pin_config;
    pin_config.bck_io_num   = I2S_PIN_NO_CHANGE;
    pin_config.ws_io_num    = PIN_CLK;
    pin_config.data_out_num = I2S_PIN_NO_CHANGE;
    pin_config.data_in_num  = PIN_DATA;

    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_set_clk(I2S_NUM_0, 44100, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

    return true;
}

static void i2sMicroFFTtask(void *arg) {
    uint8_t FFTDataBuff[128];
    uint8_t FFTValueBuff[24];
    uint8_t* microRawData = (uint8_t*)calloc(2048,sizeof(uint8_t));
    size_t bytesread;
    int16_t* buffptr;
    double data = 0;
    float adc_data;
    uint16_t ydata;
    uint32_t subData;

    uint8_t state = MODE_MIC;
    i2sQueueMsg_t QueueMsg;
    while(1)
    {
        if( xQueueReceive(i2sstateQueue,&QueueMsg,(TickType_t)0) == pdTRUE)
        {
            if( QueueMsg.state == MODE_MIC )
            {
                InitI2SSpakerOrMic(MODE_MIC);
                state = MODE_MIC;
            }
        }
        else if( state == MODE_MIC )
        {
            fft_config_t *real_fft_plan = fft_init(1024, FFT_REAL, FFT_FORWARD, NULL, NULL);
            i2s_read(I2S_NUM_0, (char *)microRawData, 2048, &bytesread, (100 / portTICK_RATE_MS));
            buffptr = (int16_t*)microRawData;            

            for ( int count_n = 0; count_n < real_fft_plan->size; count_n++)
            {
                adc_data = (float)map(buffptr[count_n], INT16_MIN, INT16_MAX, -2000, 2000);
                real_fft_plan->input[count_n] = adc_data;                
            }
            
            // https://circuitdigest.com/microcontroller-projects/arduino-sound-level-measurement
            // couldn't find a way to convert PDM MEMS to dB
            // this seems to be pretty close after side by side with a decibel reader
            if(adc_data >= 0) {
              decibels = adc_data / 1.6;
              sound = adc_data;  
            }            
            
            fft_execute(real_fft_plan);

            for ( int count_n = 1; count_n < real_fft_plan->size / 4; count_n++)
            {
                data = sqrt(real_fft_plan->output[2 * count_n] * real_fft_plan->output[2 * count_n] + real_fft_plan->output[2 * count_n + 1] * real_fft_plan->output[2 * count_n + 1]);
                if ((count_n - 1) < 128)
                {
                    data = ( data > 2000 ) ? 2000 : data;
                    ydata = map(data, 0, 2000, 0, 255);
                    FFTDataBuff[128 - count_n] = ydata;
                }
            }

            for( int count = 0; count < 24; count++ )
            {
                subData = 0;
                for( int count_i = 0; count_i < 5; count_i++ )
                {
                    subData += FFTDataBuff[count * 5 + count_i ];
                }
                subData /= 5;
                FFTValueBuff[count] = map(subData,0,255,0,8);
            }
            xQueueSend( fftvalueQueue, (void * )&FFTValueBuff, 0 );            
            fft_destroy(real_fft_plan);             
        }
        else
        {
            delay(10);
        }
    }
}

void microPhoneSetup()
{
    fftvalueQueue = xQueueCreate(5, 24 * sizeof(uint8_t));
    if( fftvalueQueue == 0 )
    {
        return;
    }

    i2sstateQueue = xQueueCreate(5, sizeof(i2sQueueMsg_t));
    if( i2sstateQueue == 0 )
    {
        return;
    }

    InitI2SSpakerOrMic(MODE_MIC);
    xTaskCreatePinnedToCore(i2sMicroFFTtask, "microPhoneTask", 4096, NULL, 3, NULL, 0);

    DisFFTbuff.createSprite(310,160);
}

void MicroPhoneFFT()
{
    uint8_t FFTValueBuff[24];
    xQueueReceive( fftvalueQueue, (void * )&FFTValueBuff, portMAX_DELAY );
    DisFFTbuff.fillRect(0, 0, 320, 54, (darkMode ? TFT_BLACK : TFT_WHITE));
    uint32_t colorY = DisFFTbuff.color565(0xff, 0x9c, (darkMode ? TFT_BLACK : TFT_WHITE));
    uint32_t colorG = DisFFTbuff.color565(0x66, 0xff, (darkMode ? TFT_BLACK : TFT_WHITE));
    uint32_t colorRect;
    for( int x = 0; x < 24; x++ )
    {
        for( int y = 0; y < 9; y++ )
        {
            if( y < FFTValueBuff[23-x] )
            {
                colorRect = colorY;
            }
            else if( y == FFTValueBuff[23-x] )
            {
                colorRect = colorG;
            }
            else
            {
                continue;
            }
            DisFFTbuff.fillRect(x*12,54-y*6 - 5,5,5,colorRect);
        }
    }    
    DisFFTbuff.pushSprite(20, 160);
}

void publishMessage()
{
  StaticJsonDocument<200> doc;
  doc["co2"] = co2;
  doc["tvoc"] = tvoc;
  doc["sound"] = sound;  
  doc["decibels"] = decibels;  
  
  StaticJsonDocument<200> state;
  state["state"]["reported"] = doc;

  char jsonBuffer[512];
  serializeJson(state, jsonBuffer); // print to client
  client.publish(AWS_IOT_PUBLISH_TOPIC, jsonBuffer);
  Serial.println("MQTT Posted");
}

void messageHandler(String &topic, String &payload) {
  Serial.println("incoming: " + topic + " - " + payload);

  StaticJsonDocument<200> doc;
  deserializeJson(doc, payload);
  const char* message = doc["message"];  
}

void drawLabels(){
  if(darkMode)    
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  else
    M5.Lcd.setTextColor(TFT_BLACK, TFT_WHITE);
      
  M5.Lcd.drawString("CO2:", 70, 40, 4);
  M5.Lcd.drawString("TVOC:", 60, 80, 4);
  M5.Lcd.drawString("VOL:", 70, 130, 4);
}

void setup() {
  M5.begin(true, false, true, true);
  M5.Lcd.fillScreen(TFT_BLACK);  
  lastMillis = millis();
  
  connectAWS();  

  header("AirBits");
  
  if (!sgp.begin()){
    Serial.println("Sensor not found :(");
    while (1);
  }

  drawLabels();
  
  // Setup Microphone for decibel levels
  microPhoneSetup();

  // Start Side LEDs White
  FastLED.addLeds<SK6812, LEDS_PIN>(ledsBuff, LEDS_NUM);
  for (int i = 0; i < LEDS_NUM; i++){
    ledsBuff[i].setRGB(255, 255, 255);
  }
  FastLED.show();
}

void loop() {
  M5.update();
  if (M5.BtnA.pressedFor(1000, 200)) {
    sendToAWS = !sendToAWS;
    header("AirBits");
  }
  if (M5.BtnB.pressedFor(1000, 200)) {
    darkMode = !darkMode;
    if(darkMode)
      M5.Lcd.fillScreen(TFT_BLACK);
    else
      M5.Lcd.fillScreen(TFT_WHITE);
    header("AirBits");
    drawLabels();
  }  
  
  boolean danger = false;
  clearScreen();
  MicroPhoneFFT();  

  if (!sgp.IAQmeasure()) {
    Serial.println("Measurement failed");
    return;
  }
  if(sgp.eCO2 >= 0)
    co2 = sgp.eCO2;
  if(sgp.TVOC >= 0)
    tvoc = sgp.TVOC;

  M5.Lcd.drawNumber(co2, 140, 40, 4);
  M5.Lcd.drawNumber(tvoc, 130, 80 , 4);
  // Danger Sidebar LED
  danger = (co2 > 400);
  
  M5.Lcd.drawString("ppb", 210, 80, 4);
  M5.Lcd.drawString("ppm", 210, 40, 4);
  
  M5.Lcd.drawNumber(decibels, 140, 130 , 4);  
  M5.Lcd.drawString("dB", 210, 130, 4);

  if(!danger)
    danger = (decibels > 85);

  if(danger){
    FastLED.addLeds<SK6812, LEDS_PIN>(ledsBuff, LEDS_NUM);
    for (int i = 0; i < LEDS_NUM; i++){
      ledsBuff[i].setRGB(255, 0, 0);
    }
    FastLED.show();
  }
  
  Serial.print("eCO2 "); Serial.print(sgp.eCO2); Serial.print(" ppm\t");
  Serial.print("TVOC "); Serial.print(sgp.TVOC); Serial.print(" ppb\t");
  Serial.print("sound "); Serial.print(sound); Serial.print("\t");
  Serial.print("dB "); Serial.print(decibels); Serial.println(" dB\t");
 
  // publish a message roughly every 5 seconds.
  if (sendToAWS && millis() - lastMillis > 5000) {
    lastMillis = millis();
    publishMessage();
    client.loop();  
  }
  
  delay(1000);
}
