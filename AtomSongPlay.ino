#include <M5Atom.h>
#include <driver/i2s.h>

#include "sall.h"

#define CONFIG_I2S_BCK_PIN      19
#define CONFIG_I2S_LRCK_PIN     33
#define CONFIG_I2S_DATA_PIN     22
#define CONFIG_I2S_DATA_IN_PIN  23

#define SPEAKER_I2S_NUMBER      I2S_NUM_0

#define MODE_MIC                0
#define MODE_SPK                1

#define SNDLEN 16000   // 1秒分のバッファ長（×2バイトデータ）
#define WRITETIME 500  // I2S書込間隔（msec）

//  0  2  4  5  7  9 11 12 14 16 17 19 21 23 24
// ド レ ミ フ ソ ラ シ ド レ ミ フ ソ ラ シ ド

#define TEMPO 120    // 再生テンポ（120）
#define DATAMAX 15   // 演奏データ（音符）数

// テンポ120で500msec間隔の発音
const int melody[DATAMAX][2] = {
	{   0,   0},  // ド
	{   8,   2},  // レ
	{  16,   4},  // ミ
	{  24,   5},  // ファ
	{  32,   7},  // ソ
	{  40,   9},  // ラ
	{  48,  11},  // シ
	{  56,  12},  // ド
	{  64,  14},  // レ
	{  72,  16},  // ミ
	{  80,  17},  // ファ
	{  88,  19},  // ソ
	{  96,  21},  // ラ
	{ 104,  23},  // シ
	{ 112,  24}   // ド
};

int sound = -1; // 0以上で演奏
int tempo = TEMPO; // テンポ（1分あたりの拍数）
double tt = 60000 / tempo; // 1拍の時間（ミリ秒、テンポ120で500ミリ秒）
int rate = 16000; // サンプリングレート（160000がディフォルト）
short SONG[SNDLEN];  // 1秒分のバッファ
char csong[32000];  // 1秒分のバッファ（バイト）

int rpos = 0;   // 出力バッファ書込ポインタ
int wpos = 0;   // 出力バッファ読出ポインタ
unsigned char playbuff[2][SNDLEN];
int tskstop = 0;
int ppos = 0;   // 再生データ変換配列位置ポインタ
int playtime = 0;
int lastplaytime = 0;

void InitI2SSpeakerOrMic(int mode)
{
	esp_err_t err = ESP_OK;
	
	i2s_driver_uninstall(SPEAKER_I2S_NUMBER);
	i2s_config_t i2s_config = {
		.mode                 = (i2s_mode_t)(I2S_MODE_MASTER),
		.sample_rate          = rate,
		.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
		.channel_format       = I2S_CHANNEL_FMT_ALL_RIGHT,
		.communication_format = I2S_COMM_FORMAT_I2S,
		.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
		.dma_buf_count        = 6,
		.dma_buf_len          = 60,
		.use_apll             = false,
		.tx_desc_auto_clear   = true,
		.fixed_mclk           = 0
	};
	
	if(mode == MODE_MIC){
		i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
	}
	else{
		i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
	}
	
	err += i2s_driver_install(SPEAKER_I2S_NUMBER, &i2s_config, 0, NULL);
	
	i2s_pin_config_t tx_pin_config = {
		.bck_io_num           = CONFIG_I2S_BCK_PIN,
		.ws_io_num            = CONFIG_I2S_LRCK_PIN,
		.data_out_num         = CONFIG_I2S_DATA_PIN,
		.data_in_num          = CONFIG_I2S_DATA_IN_PIN,
	};
	
	err += i2s_set_pin(SPEAKER_I2S_NUMBER, &tx_pin_config);
	
	if(mode != MODE_MIC){
		err += i2s_set_clk(SPEAKER_I2S_NUMBER, rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
	}
	
	i2s_zero_dma_buffer(SPEAKER_I2S_NUMBER);
}


// 再生用タスク
void i2sPlayTask(void* arg) {
	size_t bytes_written;
	bool loop = true;
	vTaskDelay(1);
	
	while(loop){
		if(sound >= 0){
			// Write Speaker
			i2s_write(SPEAKER_I2S_NUMBER, playbuff[rpos], SNDLEN, &bytes_written, portMAX_DELAY);
			
			if(rpos == 0) rpos = 1;
			else          rpos = 0;
		}
		
		if(tskstop != 0){
			// 終了
			loop = false;
			break;
		}
		vTaskDelay(1);
	}
	
	rpos = 0;
	tskstop = 0;
	Serial.println("STOP i2sPlayTask.");
	vTaskDelay(10);
	// タスク削除
	vTaskDelete(NULL);
}


void settempo(int t)
{
	tempo = t;
	tt = 60000.0 / (double)tempo;
	//rate = (16000 * t) / 120;
	//InitI2SSpeakerOrMic(MODE_MIC);
}


// return : 1=継続演奏、0=演奏終了（データエンド）
int makesound()
{
	int i;
	double t64;
	double tx;
	int sspos;
	int dd;
	double gain;
	
	// 64分音符1つ分の時間
	t64 = 60000.0 / (double)(tempo * 16);  // msec
	
	gain = 0.3;
	
	// 音作成
	while(1){
		// データ作成
		// 音符を演奏する時間（msec）
		tx = ((double)melody[ppos][0] * t64);
		// 今演奏をすべき時間内にある音か判断
		if(tx < (double)playtime){
			// 1転送分（500msec）内の発音
			// SONG[]バッファ上の位置
			sspos = (int)(16.0 * (tx - (double)lastplaytime));  // 16000 / 500 * tx;
			if(sspos > SNDLEN/2){
				Serial.println("ERROR : sspos > SNDLEN");
			}
			else{
				// SONG[]に新たな音を加算する
				for(i=0; i<SNDLEN/2; i++){
					dd = (int)SONG[sspos+i] + (int)s[melody[ppos][1]][i];
					// 同時発音数でゲイン調整
					if(gain < 1.0){
						dd = (int)((double)dd * gain);
					}
					// オーバーフローリミッタ
					if(dd > 32767){
						SONG[sspos+i] = 32767;
					}
					else if(dd < -32768){
						SONG[sspos+i] = -32768;
					}
					else{
						SONG[sspos+i] = dd;
					}
				}
			}
			ppos++;
			if(ppos >= DATAMAX){
				return(0);  // 演奏終了
			}
		}
		else{
			// 次の時間（発音分）
			return(1);  // 演奏継続
		}
	}
	
	return(0);
}


void setup() 
{
	// put your setup code here, to run once:
	M5.begin(true, false, true);
	
	for(int i=0; i<5; i++){
		M5.dis.drawpix(0, CRGB(0, 0, 0));
		delay(200);
		M5.dis.drawpix(0, CRGB(255, 0, 0));
		delay(200);
	}
	
	memset((char *)&SONG[0], 0, SNDLEN*2);
	sound = -1;
	tempo = TEMPO;
	settempo(tempo);
	
	M5.dis.drawpix(0, CRGB(0, 0, 255));
	InitI2SSpeakerOrMic(MODE_MIC);
	
	delay(1000);
}


void loop() 
{
	// put your main code here, to run repeatedly:
	int i;
	M5.update();
	
	// ボタン押し
	if(M5.Btn.wasPressed()){
		M5.dis.drawpix(0, CRGB(255, 0, 0));
		InitI2SSpeakerOrMic(MODE_SPK);
		
		Serial.printf("BUTTON PUSH. tt=%f\n", tt);
		
		// SONG[]書込
		wpos = 0;
		// 発音範囲を加算
		ppos = 0;
		
		lastplaytime = 0;
		playtime = WRITETIME;  // I2S書込サイズ（500msec）
		makesound();  // SONG[]に音声500msec分の発音を合成
		lastplaytime = playtime;
		playtime += WRITETIME;
		// 転送バッファにコピー
		memcpy(playbuff[wpos], (char *)&SONG[0], SNDLEN);
		wpos = 1;
		// SONG[]バッファを次のデータに備える
		memcpy((char *)&SONG[0], (char *)&SONG[SNDLEN/2], SNDLEN);
		memset((char *)&SONG[SNDLEN/2], 0, SNDLEN);
		
		Serial.println("START i2sPlayTask.");
		// 再生タスク起動
		xTaskCreatePinnedToCore(i2sPlayTask, "i2sPlayTask", 4096, NULL, 1, NULL, 1);
		delay(10);
		
		sound = 0;
	}
	
	// 演奏
	// 読出バッファが切り替わったら（再生済みのバッファに）書込をおこなう
	if(sound >= 0 && wpos != rpos){
		// Write Speaker（データ転送完了まで待たされる）
		sound++;
		if(sound > DATAMAX){
			// 演奏（タスク）終了
			tskstop = 1;
			sound = -1;  // 終了
			wpos = 0;
			delay(WRITETIME + 100);  // 最後の音が再生が終わるまで待つ(>500msec)
			Serial.println("PLAY END.");
			// Set Mic Mode
			InitI2SSpeakerOrMic(MODE_MIC);
			M5.dis.drawpix(0, CRGB(0, 0, 255));
		}
		else if(sound == DATAMAX){
			// 無音作成（最後に無音を再生するため）
			for(i=0; i<SNDLEN; i++){
				playbuff[wpos][i] = 0;
			}
			if(wpos == 0) wpos = 1;
			else          wpos = 0;
		}
		else{
			// 次の500msec分の音を作成
			i = makesound();  // SONG[]に音声500msec分の発音を合成
			if(i == 0){
				sound = DATAMAX-1;
			}
			lastplaytime = playtime;
			playtime += WRITETIME;
			// 転送バッファにコピー
			memcpy(playbuff[wpos], (char *)&SONG[0], SNDLEN);
			// SONG[]バッファを次のデータに備える
			memcpy((char *)&SONG[0], (char *)&SONG[SNDLEN/2], SNDLEN);
			memset((char *)&SONG[SNDLEN/2], 0, SNDLEN);
			if(wpos == 0) wpos = 1;
			else          wpos = 0;
		}
	}
	delay(tt);
}

