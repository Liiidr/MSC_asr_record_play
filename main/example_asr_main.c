#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"
#include "board.h"
#include "audio_common.h"
#include "audio_pipeline.h"
#include "mp3_decoder.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "filter_resample.h"
#include "esp_sr_iface.h"
#include "esp_sr_models.h"
#include "fatfs_stream.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "amrwb_encoder.h"
#include "amr_decoder.h"
#include "spiffs_stream.h"
#include "periph_spiffs.h"


static const char *TAG = "asr_keywords";
static const char *EVENT_TAG = "asr_event";
static const char* amr_filename[] = {"/spiffs/rec0.amr", "/spiffs/rec1.amr"};

extern void javanotify_simplespeech_event(int, int);

#define SDCARD 0
#define SPIFFS 1
#define RECORDER 1
#define PLAYER 1

typedef enum {
    WAKE_UP = 1,
    OPEN_THE_LIGHT,
    CLOSE_THE_LIGHT,
    VOLUME_INCREASE,
    VOLUME_DOWN,
    PLAY,
    PAUSE,
    MUTE,
    PLAY_LOCAL_MUSIC,
} asr_event_t;


audio_pipeline_handle_t pipeline_vad;
audio_element_handle_t i2s_reader_vad, filter, raw_read;
#if SDCARD
audio_pipeline_handle_t pipeline_rec;
audio_element_handle_t fatfs_stream_writer, i2s_reader_rec, filter_rec,amr_encoder;
audio_pipeline_handle_t pipeline_play;
audio_element_handle_t fatfs_stream_reader, i2s_writer_play, filter_play,amr_decoder;
#endif
#if SPIFFS
audio_pipeline_handle_t pipeline_rec;
audio_element_handle_t spiffs_writer, i2s_reader_rec, filter_rec,amr_encoder;
audio_pipeline_handle_t pipeline_play;
audio_element_handle_t spiffs_reader, i2s_writer_play, filter_play,amr_decoder;
#endif

SemaphoreHandle_t xSemaphore_record = NULL;
SemaphoreHandle_t xSemaphore_play = NULL;
audio_event_iface_handle_t evt = NULL;
audio_event_iface_handle_t evt_play = NULL;

int time_rec;

int esp32_record_voicefile(const       char* filename, int time)
{	
	time_rec = time;

	//printf("Recording filename =  %s, %d seconds\r\n",filename, time);
	//audio_element_set_uri(spiffs_writer,filename);				
	xSemaphoreGive( xSemaphore_record );	
	return 0;
}

int esp32_playback_voice(int index)
{
	//audio_element_set_uri(spiffs_reader, amr_filename[index]);	
	//printf("Playing filename =  %s\r\n", amr_filename[index]);
	xSemaphoreGive( xSemaphore_play );	
	return 0;
}


void vad_task(void * pram)
{
#if CONFIG_SR_MODEL_WN4_QUANT
	const esp_sr_iface_t *model = &esp_sr_wakenet4_quantized;
#else
	const esp_sr_iface_t *model = &esp_sr_wakenet3_quantized;
#endif
	model_iface_data_t *iface = model->create(DET_MODE_90);
	int num = model->get_word_num(iface);
	for (int i = 1; i <= num; i++) {
		char *name = model->get_word_name(iface, i);
		ESP_LOGI(TAG, "keywords: %s (index = %d)", name, i);
	}
	float threshold = model->get_det_threshold_by_mode(iface, DET_MODE_90, 1);
	int sample_rate = model->get_samp_rate(iface);
	int audio_chunksize = model->get_samp_chunksize(iface);
	ESP_LOGI(EVENT_TAG, "keywords_num = %d, threshold = %f, sample_rate = %d, chunksize = %d, sizeof_uint16 = %d", num, threshold, sample_rate, audio_chunksize, sizeof(int16_t));
	int16_t *buff = (int16_t *)malloc(audio_chunksize * sizeof(short));
	if (NULL == buff) {
		ESP_LOGE(EVENT_TAG, "Memory allocation failed!");
		model->destroy(iface);
		model = NULL;
		return;
	}
    raw_stream_cfg_t raw_cfg = {
        .out_rb_size = 8 * 1024,
        .type = AUDIO_STREAM_READER,
    };
    raw_read = raw_stream_init(&raw_cfg);

    audio_pipeline_register(pipeline_vad, i2s_reader_vad, "i2s_vad");
    audio_pipeline_register(pipeline_vad, filter, "filter");
    audio_pipeline_register(pipeline_vad, raw_read, "raw");
    audio_pipeline_link(pipeline_vad, (const char *[]) {"i2s_vad", "filter", "raw"}, 3);

	audio_pipeline_run(pipeline_vad);

	while(1){			
				raw_stream_read(raw_read, (char *)buff, audio_chunksize * sizeof(short));
				int keyword = model->detect(iface, (int16_t *)buff);
				switch (keyword) {
					case WAKE_UP:
						ESP_LOGI(TAG, "Wake up");
				       	javanotify_simplespeech_event(1, 0);
						//esp32_record_voicefile(0,15);
						break;
					case OPEN_THE_LIGHT:
						ESP_LOGI(TAG, "Turn on the light");
						
						//esp32_record_voicefile(0,15);
						break;
					case CLOSE_THE_LIGHT:
						ESP_LOGI(TAG, "Turn off the light");
						break;
					case VOLUME_INCREASE:
						ESP_LOGI(TAG, "volume increase");
						break;
					case VOLUME_DOWN:
						ESP_LOGI(TAG, "volume down");
						break;
					case PLAY:
						ESP_LOGI(TAG, "play");		
						//esp32_playback_voice(0);
						break;
					case PAUSE:
						ESP_LOGI(TAG, "pause");
						break;
					case MUTE:
						ESP_LOGI(TAG, "mute");
						
						break;
					case PLAY_LOCAL_MUSIC:
						ESP_LOGI(TAG, "play local music");
						break;
					default:
						ESP_LOGD(TAG, "Not supported keyword");
						break;
				}
					


	}

    model->destroy(iface);
    model = NULL;
    free(buff);
    buff = NULL;

}

void record_task(void * pram)
{
	int cnt_time_rec = 0;
	while(1)
	{
		if(xSemaphoreTake( xSemaphore_record, portMAX_DELAY ) == pdTRUE){	
		
			printf("pipeline_rec  run\r\n");
			
			audio_pipeline_stop(pipeline_vad);
			audio_pipeline_wait_for_stop(pipeline_vad);
		
			audio_pipeline_run(pipeline_rec);
			while(1){
				vTaskDelay(1000/portTICK_PERIOD_MS);
				cnt_time_rec++;
				ESP_LOGI(TAG, "[ * ] Recording ... %d", cnt_time_rec);
				if (cnt_time_rec >= time_rec) {
					ESP_LOGI(TAG, "Finishing amr recording");
					audio_pipeline_terminate(pipeline_rec);
					audio_pipeline_run(pipeline_vad);
					cnt_time_rec = 0;
					javanotify_simplespeech_event(0, 0);
					break;
				}
				continue;
			}


		}

	
	//vTaskDelay(1000/portTICK_PERIOD_MS);


	}


}

void playback_task(void * pram)
{

	while(1)
	{
		if(xSemaphoreTake( xSemaphore_play, portMAX_DELAY ) == pdTRUE){	
			printf("pipeline_play  run\r\n");					
			//audio_pipeline_stop(pipeline_vad);
			//audio_pipeline_wait_for_stop(pipeline_vad);	
			printf("play run \r\n");
			audio_pipeline_run(pipeline_play);
			while(1){	
				audio_event_iface_msg_t msg;
				esp_err_t ret = audio_event_iface_listen(evt_play, &msg, portMAX_DELAY);
				printf("audio_event_iface_listen returns %d\n", ret);
				if (msg.source == (void *) i2s_writer_play
					  && msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
					  audio_element_state_t el_state = audio_element_get_state(i2s_writer_play);
					  if (el_state == AEL_STATE_FINISHED) {
						  printf("play stop \r\n");
						  audio_pipeline_stop(pipeline_play);
						  audio_pipeline_wait_for_stop(pipeline_play);
						  javanotify_simplespeech_event(2, 0);
						  break;
					   } else {
							printf("play unknown event: %d\n", el_state);
					   }				  
					continue;
				 }

			}


		}

	
		printf("play task \r\n");
		vTaskDelay(1000/portTICK_PERIOD_MS);


	}


}

extern void JavaTask();

void app_main()
{
	/*
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set(EVENT_TAG, ESP_LOG_INFO);
	*/


#if SDCARD
	ESP_LOGI(TAG, "[1.0] Mount sdcard");
	// Initialize peripherals management
	esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
	set = esp_periph_set_init(&periph_cfg);

	// Initialize SD Card peripheral
	periph_sdcard_cfg_t sdcard_cfg = {
		.root = "/sdcard",
		.card_detect_pin = get_sdcard_intr_gpio(), //GPIO_NUM_34
	};
	esp_periph_handle_t sdcard_handle = periph_sdcard_init(&sdcard_cfg);
	esp_periph_start(set, sdcard_handle);

	// Wait until sdcard is mounted
	while (!periph_sdcard_is_mounted(sdcard_handle)) {
		vTaskDelay(100 / portTICK_PERIOD_MS);
	}

#endif

//#if SPIFFS
#if 0
	// Initialize peripherals management
		esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
		esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
		
		// Initialize Spiffs peripheral
		periph_spiffs_cfg_t spiffs_cfg = {
			.root = "/spiffs",
			.partition_label = NULL,
			.max_files = 10,
			.format_if_mount_failed = true
		};
		esp_periph_handle_t spiffs_handle = periph_spiffs_init(&spiffs_cfg);
		esp_periph_start(set, spiffs_handle);
		// Wait until spiffs is mounted
		while (!periph_spiffs_is_mounted(spiffs_handle)) {
			vTaskDelay(100 / portTICK_PERIOD_MS);
		}

#endif

    ESP_LOGI(EVENT_TAG, "[ 1 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_vad = audio_pipeline_init(&pipeline_cfg);
	pipeline_rec = audio_pipeline_init(&pipeline_cfg);
	pipeline_play = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline_vad);
	mem_assert(pipeline_rec);
	mem_assert(pipeline_play);

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.i2s_config.sample_rate = 48000;
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_reader_vad = i2s_stream_init(&i2s_cfg);
	i2s_reader_rec = i2s_stream_init(&i2s_cfg);
	i2s_cfg.type = AUDIO_STREAM_WRITER;
	i2s_writer_play = i2s_stream_init(&i2s_cfg);

    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = 48000;
    rsp_cfg.src_ch = 2;
    rsp_cfg.dest_rate = 16000;
    rsp_cfg.dest_ch = 1;
    rsp_cfg.type = AUDIO_CODEC_TYPE_ENCODER;
    filter = rsp_filter_init(&rsp_cfg);
	filter_rec = rsp_filter_init(&rsp_cfg);
    rsp_cfg.src_rate = 16000;
    rsp_cfg.src_ch = 1;
    rsp_cfg.dest_rate = 48000;
    rsp_cfg.dest_ch = 2;
    rsp_cfg.type = AUDIO_CODEC_TYPE_DECODER;
	filter_play = rsp_filter_init(&rsp_cfg);


#if SDCARD
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_WRITER;
    fatfs_stream_writer = fatfs_stream_init(&fatfs_cfg);
	fatfs_cfg.type = AUDIO_STREAM_READER;
	fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);	
#endif
#if SPIFFS
	spiffs_stream_cfg_t spiffs_stream_cfg = SPIFFS_STREAM_CFG_DEFAULT();
	spiffs_stream_cfg.type = AUDIO_STREAM_WRITER;	
	spiffs_writer = spiffs_stream_init(&spiffs_stream_cfg);
	audio_element_info_t writer_info = {0};
	audio_element_getinfo(spiffs_writer, &writer_info);
	writer_info.bits = 16;
	writer_info.channels = 1;
	writer_info.sample_rates = 16000;
	audio_element_setinfo(spiffs_writer, &writer_info);

	
	spiffs_stream_cfg.type = AUDIO_STREAM_READER;
	spiffs_reader = spiffs_stream_init(&spiffs_stream_cfg);

	
	//mem_assert(spiffs_writer);
	
#endif



#if RECORDER
	amrwb_encoder_cfg_t amr_enc_cfg = DEFAULT_AMRWB_ENCODER_CONFIG();
	amr_encoder = amrwb_encoder_init(&amr_enc_cfg);
#endif	
#if PLAYER
	amr_decoder_cfg_t amr_cfg = DEFAULT_AMR_DECODER_CONFIG();
	amr_decoder	= amr_decoder_init(&amr_cfg);
#endif

#if RECORDER	
	audio_pipeline_register(pipeline_rec, i2s_reader_rec, "i2s_rec");
	audio_pipeline_register(pipeline_rec, filter_rec, "filter_rec");
	audio_pipeline_register(pipeline_rec, amr_encoder, "Wamr");	
	#if SDCARD
		audio_pipeline_register(pipeline_rec, fatfs_stream_writer, "file_writer");
	#endif
	#if SPIFFS
		audio_pipeline_register(pipeline_rec, spiffs_writer, "file_writer");
	#endif
	
	ESP_LOGI(TAG, "[3.5] Link it together [codec_chip]-->i2s_stream-->amr_encoder-->fatfs_stream-->[sdcard]");
    audio_pipeline_link(pipeline_rec, (const char *[]) { "i2s_rec","filter_rec", "Wamr", "file_writer"  },4);
	#if SDCARD
		audio_element_set_uri(fatfs_stream_writer, "/sdcard/rec.Wamr");
	#endif
	#if SPIFFS
		audio_element_set_uri(spiffs_writer, "/spiffs/rec0.Wamr");
	#endif	
	//ESP_LOGI(TAG, "[4.0] Set up  event listener");
    //audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    //evt = audio_event_iface_init(&evt_cfg);
    //audio_pipeline_set_listener(pipeline_rec, evt);
    //audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

	xSemaphore_record = xSemaphoreCreateBinary();	
	if( xSemaphore_record != NULL )
	{
		// The semaphore was created successfully.
		// The semaphore can now be used.
	}
#endif

#if PLAYER
	audio_pipeline_register(pipeline_play, i2s_writer_play, "i2s_play");
	audio_pipeline_register(pipeline_play, filter_play, "filter_play");
	audio_pipeline_register(pipeline_play, amr_decoder, "Wamr_de");	
	#if SDCARD
		audio_pipeline_register(pipeline_play, fatfs_stream_reader, "file_reader");
	#endif
	#if SPIFFS
		audio_pipeline_register(pipeline_play, spiffs_reader, "file_reader");
	#endif
    audio_pipeline_link(pipeline_play, (const char *[]) { "file_reader","Wamr_de", "filter_play", "i2s_play"  },4);
	#if SDCARD
		audio_element_set_uri(fatfs_stream_reader, "/sdcard/rec.Wamr");
	#endif
	#if	SPIFFS
		audio_element_set_uri(spiffs_reader, "/spiffs/rec0.Wamr");
	#endif
	
	audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
	evt_play = audio_event_iface_init(&evt_cfg);
	audio_pipeline_set_listener(pipeline_play, evt_play);

	xSemaphore_play = xSemaphoreCreateBinary();	
	if( xSemaphore_play != NULL )
	{
		// The semaphore was created successfully.
		// The semaphore can now be used.
	}
	
	
#endif

	
	xTaskCreate(vad_task, "vad_task", 4096, NULL, 2,NULL);

#if RECORDER
	
	//esp32_record_voicefile(0,15);	

	xTaskCreate(record_task, "record_task", 4096, NULL, 3,NULL);
#endif
#if PLAYER
	xTaskCreate(playback_task, "playback_task", 4096, NULL, 4,NULL);
#endif

	

    
    while (1) {

  		JavaTask();

		for (int i = 10; i >= 0; i--) {
	        printf("Restarting in %d seconds...\n", i);
	        vTaskDelay(1000 / portTICK_PERIOD_MS);
	    }
	    printf("Restarting now.\n");
	    fflush(stdout);
	    esp_restart();


#if 0
	   	

		if(testcnt == 10){
			audio_pipeline_stop(pipeline_vad);
			audio_pipeline_wait_for_stop(pipeline_vad);

			
		}
	
		if(testcnt == 20){
			time_rec = 10;
			xSemaphoreGive( xSemaphore_record );
			
		//audio_pipeline_run(pipeline_rec);
		
			
		}
		if(testcnt == 30){
			//audio_pipeline_terminate(pipeline_rec);
		audio_pipeline_run(pipeline_vad);

		}

		if(testcnt == 40){
				audio_pipeline_run(pipeline_play);
			}


		if(testcnt == 50){
			audio_pipeline_terminate(pipeline_play);
		}

		if(testcnt == 60){
				audio_pipeline_run(pipeline_vad);
				testcnt = 0;
			}


		testcnt++;
		printf("testcnt = %d\r\n",testcnt);
		vTaskDelay(1000/portTICK_PERIOD_MS);

#endif

    }
    audio_pipeline_terminate(pipeline_vad);
    audio_pipeline_remove_listener(pipeline_vad);
    audio_pipeline_unregister(pipeline_vad, raw_read);
    audio_pipeline_unregister(pipeline_vad, i2s_reader_vad);
    audio_pipeline_unregister(pipeline_vad, filter);
    audio_pipeline_deinit(pipeline_vad);
    audio_element_deinit(raw_read);
    audio_element_deinit(i2s_reader_vad);
    audio_element_deinit(filter);


#if RECORDER
	audio_pipeline_remove_listener(pipeline_rec);
	//esp_periph_set_stop_all(set);
	//audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);
	//audio_event_iface_destroy(evt);
	audio_pipeline_unregister(pipeline_rec, amr_encoder);
	//audio_pipeline_unregister(pipeline_rec, i2s_reader_rec);
	#if SDCARD
		audio_pipeline_unregister(pipeline_rec, fatfs_stream_writer);
		audio_element_deinit(fatfs_stream_writer);
	#endif
	audio_pipeline_deinit(pipeline_rec);
	//audio_element_deinit(i2s_reader_rec);
	audio_element_deinit(amr_encoder);
	//esp_periph_set_destroy(set);


#endif

	
}
