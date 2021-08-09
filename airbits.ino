//SGP30 needs 15 seconds to initialize calibration after power on.
//The screen will display TVOC and CO2

#include <M5Core2.h>
#include <driver/i2s.h>
#include "fft.h"
#include "Adafruit_SGP30.h"
#include "FastLED.h"

#define PIN_CLK  0
#define PIN_DATA 34

#define MODE_MIC 0

Adafruit_SGP30 sgp;
int i = 15;
long last_millis = 0;
float decibels = 0;

TFT_eSprite DisFFTbuff =  TFT_eSprite(&M5.Lcd);
static QueueHandle_t fftvalueQueue = nullptr;
static QueueHandle_t i2sstateQueue = nullptr;

#define LEDS_PIN 25
#define LEDS_NUM 10
CRGB ledsBuff[LEDS_NUM];

void header(const char *string, uint16_t color)
{
    M5.Lcd.fillScreen(color);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.fillRect(0, 0, 360, 30, TFT_BLACK);
    M5.Lcd.setTextDatum(TC_DATUM);
    M5.Lcd.drawString(string, 160, 3, 4); 
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
            decibels = adc_data / 1.6;
            
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
    DisFFTbuff.fillRect(0,0,320,54,DisFFTbuff.color565(0x00,0x00,0x00));
    uint32_t colorY = DisFFTbuff.color565(0xff,0x9c,0x00);
    uint32_t colorG = DisFFTbuff.color565(0x66,0xff,0x00);
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

void clearScreen(){
  M5.Lcd.fillRect(0, 100, 320, 0, TFT_BLACK);  
  M5.Lcd.drawString("      ", 130, 40 , 4);
  M5.Lcd.drawString("      ", 130, 80 , 4);
  M5.Lcd.drawString("      ", 130, 130 , 4);
  FastLED.clear();
}

void setup() {
  M5.begin(true, false, true, true);
  M5.Lcd.fillScreen(TFT_BLACK);
  header("AirBits",TFT_BLACK);

  if (! sgp.begin()){
    Serial.println("Sensor not found :(");
    while (1);
  }
  
  M5.Lcd.drawString("CO2:", 70, 40, 4);
  M5.Lcd.drawString("TVOC:", 60, 80, 4);
  M5.Lcd.drawString("VOL:", 70, 130, 4);

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
  boolean danger = false;
  clearScreen();
  MicroPhoneFFT();  

  if (!sgp.IAQmeasure()) {
    Serial.println("Measurement failed");
    return;
  }

//  M5.Lcd.setTextPadding(M5.Lcd.width());
    
  if (!sgp.IAQmeasure()) {
    M5.Lcd.drawString("N/A", 140, 40 , 4);
    M5.Lcd.drawString("N/A", 130, 80 , 4);
  } else {
    M5.Lcd.drawNumber(sgp.eCO2, 140, 40, 4);
    M5.Lcd.drawNumber(sgp.TVOC, 130, 80 , 4);
    // Danger Sidebar LED
    danger = (sgp.eCO2 > 400);
  }
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
  Serial.print("dB "); Serial.print(decibels); Serial.println(" dB\t");
 
  delay(1000);
}
