#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "pwm_stream.h"
#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "board.h"
#include "audio_mem.h"
#include "raw_stream.h"
#include "filter_resample.h"
#include "esp_sip.h"
#include "g711_decoder.h"
#include "g711_encoder.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "server.h"
#include "jsmn.h"
#include "esp_event_loop.h"
#include "driver/dac.h"

static const char *TAG = "MEGA_VOIP";

#define I2S_SAMPLE_RATE     8000
#define I2S_CHANNELS        2
#define I2S_BITS            16

#define CODEC_SAMPLE_RATE   8000
#define CODEC_CHANNELS      1

// SIP_1 -> R
// SIP_2 -> L

#define LED_1_GPIO GPIO_NUM_12
#define LED_2_GPIO GPIO_NUM_13
#define BTN_1_GPIO GPIO_NUM_14
#define MUTE_GPIO GPIO_NUM_27
#define RELAY_1_GPIO GPIO_NUM_32
#define RELAY_2_GPIO GPIO_NUM_33

static bool config_mode;

static sip_handle_t sip_1, sip_2;
static audio_element_handle_t raw_write_1, raw_write_2;
static audio_pipeline_handle_t player_1, player_2;

int spk_volume = 0;

void max_write_value_callback(int max)
{
	return;
}

static esp_err_t player_1_pipeline_open()
{
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    player_1 = audio_pipeline_init(&pipeline_cfg);
    AUDIO_NULL_CHECK(TAG, player_1, return ESP_FAIL);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_write_1 = raw_stream_init(&raw_cfg);

    g711_decoder_cfg_t g711_cfg = DEFAULT_G711_DECODER_CONFIG();
    audio_element_handle_t sip_decoder = g711_decoder_init(&g711_cfg);

    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = CODEC_SAMPLE_RATE;
    rsp_cfg.src_ch = CODEC_CHANNELS;
    rsp_cfg.dest_rate = I2S_SAMPLE_RATE;
    rsp_cfg.dest_ch = I2S_CHANNELS;
    rsp_cfg.complexity = 5;
    audio_element_handle_t filter = rsp_filter_init(&rsp_cfg);

		i2s_stream_cfg_t i2s_cfg = I2S_STREAM_INTERNAL_DAC_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
		i2s_cfg.i2s_config.sample_rate = I2S_SAMPLE_RATE;
		i2s_cfg.use_alc = true;
		i2s_cfg.volume = spk_volume;
    audio_element_handle_t i2s_stream_writer = i2s_stream_init(&i2s_cfg);

		dac_output_disable(DAC_CHANNEL_1);

    audio_pipeline_register(player_1, raw_write_1, "raw");
    audio_pipeline_register(player_1, sip_decoder, "sip_dec");
    audio_pipeline_register(player_1, filter, "filter");
		audio_pipeline_register(player_1, i2s_stream_writer, "i2s");
		const char *link_tag[4] = {"raw", "sip_dec", "filter", "i2s"};
    audio_pipeline_link(player_1, &link_tag[0], 4);
    audio_pipeline_run(player_1);
    ESP_LOGI(TAG, "SIP player_1 has been created");
    return ESP_OK;
}

static esp_err_t player_2_pipeline_open()
{
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    player_2 = audio_pipeline_init(&pipeline_cfg);
    AUDIO_NULL_CHECK(TAG, player_2, return ESP_FAIL);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_write_2 = raw_stream_init(&raw_cfg);

    g711_decoder_cfg_t g711_cfg = DEFAULT_G711_DECODER_CONFIG();
    audio_element_handle_t sip_decoder = g711_decoder_init(&g711_cfg);

    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = CODEC_SAMPLE_RATE;
    rsp_cfg.src_ch = CODEC_CHANNELS;
    rsp_cfg.dest_rate = I2S_SAMPLE_RATE;
    rsp_cfg.dest_ch = I2S_CHANNELS;
    rsp_cfg.complexity = 5;
    audio_element_handle_t filter = rsp_filter_init(&rsp_cfg);

		i2s_stream_cfg_t i2s_cfg = I2S_STREAM_INTERNAL_DAC_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
		i2s_cfg.i2s_config.sample_rate = I2S_SAMPLE_RATE;
		i2s_cfg.use_alc = true;
		i2s_cfg.volume = spk_volume;
    audio_element_handle_t i2s_stream_writer = i2s_stream_init(&i2s_cfg);

		dac_output_disable(DAC_CHANNEL_2);

    audio_pipeline_register(player_2, raw_write_2, "raw");
    audio_pipeline_register(player_2, sip_decoder, "sip_dec");
    audio_pipeline_register(player_2, filter, "filter");
		audio_pipeline_register(player_2, i2s_stream_writer, "i2s");
		const char *link_tag[4] = {"raw", "sip_dec", "filter", "i2s"};
    audio_pipeline_link(player_2, &link_tag[0], 4);
    audio_pipeline_run(player_2);
    ESP_LOGI(TAG, "SIP player_2 has been created");
    return ESP_OK;
}

static ip4_addr_t _get_network_ip()
{
    tcpip_adapter_ip_info_t ip;
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip);
    return ip.ip;
}

static int _sip_1_event_handler(sip_event_msg_t *event)
{
    switch ((int)event->type) {
        case SIP_EVENT_REQUEST_NETWORK_STATUS:
            ESP_LOGD(TAG, "SIP_EVENT_REQUEST_NETWORK_STATUS");
						ip4_addr_t ip;
            ip = _get_network_ip();
            if (ip.addr) {
                return true;
            }
            return ESP_OK;
        case SIP_EVENT_REQUEST_NETWORK_IP:
            ESP_LOGD(TAG, "SIP_EVENT_REQUEST_NETWORK_IP");
            ip = _get_network_ip();
            int ip_len = sprintf((char *)event->data, "%s", ip4addr_ntoa(&ip));
            return ip_len;
        case SIP_EVENT_REGISTERED:
            ESP_LOGI(TAG, "SIP_EVENT_REGISTERED");
            break;
        case SIP_EVENT_RINGING:
            ESP_LOGI(TAG, "ringing... RemotePhoneNum %s", (char *)event->data);
            break;
        case SIP_EVENT_INVITING:
            ESP_LOGI(TAG, "SIP_EVENT_INVITING Remote Ring...");
            break;
        case SIP_EVENT_BUSY:
            ESP_LOGI(TAG, "SIP_EVENT_BUSY");
            break;
        case SIP_EVENT_HANGUP:
            ESP_LOGI(TAG, "SIP_EVENT_HANGUP");
            break;
        case SIP_EVENT_AUDIO_SESSION_BEGIN:
            ESP_LOGI(TAG, "SIP_EVENT_AUDIO_SESSION_BEGIN");
            player_1_pipeline_open();
            break;
        case SIP_EVENT_AUDIO_SESSION_END:
            ESP_LOGI(TAG, "SIP_EVENT_AUDIO_SESSION_END");
            audio_pipeline_stop(player_1);
            audio_pipeline_wait_for_stop(player_1);
            audio_pipeline_deinit(player_1);
            break;
        case SIP_EVENT_READ_AUDIO_DATA:
            return 0;
        case SIP_EVENT_WRITE_AUDIO_DATA:
            return raw_stream_write(raw_write_1, (char *)event->data, event->data_len);
        case SIP_EVENT_READ_DTMF:
            ESP_LOGI(TAG, "SIP_EVENT_READ_DTMF ID : %d ", ((char *)event->data)[0]);
            break;
    }
    return 0;
}

static int _sip_2_event_handler(sip_event_msg_t *event)
{
    ip4_addr_t ip;
    switch ((int)event->type) {
        case SIP_EVENT_REQUEST_NETWORK_STATUS:
            ESP_LOGD(TAG, "SIP_EVENT_REQUEST_NETWORK_STATUS");
            ip = _get_network_ip();
            if (ip.addr) {
                return true;
            }
            return ESP_OK;
        case SIP_EVENT_REQUEST_NETWORK_IP:
            ESP_LOGD(TAG, "SIP_EVENT_REQUEST_NETWORK_IP");
            ip = _get_network_ip();
            int ip_len = sprintf((char *)event->data, "%s", ip4addr_ntoa(&ip));
            return ip_len;
        case SIP_EVENT_REGISTERED:
            ESP_LOGI(TAG, "SIP_EVENT_REGISTERED");
            break;
        case SIP_EVENT_RINGING:
            ESP_LOGI(TAG, "ringing... RemotePhoneNum %s", (char *)event->data);
            break;
        case SIP_EVENT_INVITING:
            ESP_LOGI(TAG, "SIP_EVENT_INVITING Remote Ring...");
            break;
        case SIP_EVENT_BUSY:
            ESP_LOGI(TAG, "SIP_EVENT_BUSY");
            break;
        case SIP_EVENT_HANGUP:
            ESP_LOGI(TAG, "SIP_EVENT_HANGUP");
            break;
        case SIP_EVENT_AUDIO_SESSION_BEGIN:
            ESP_LOGI(TAG, "SIP_EVENT_AUDIO_SESSION_BEGIN");
            player_2_pipeline_open();
            break;
        case SIP_EVENT_AUDIO_SESSION_END:
            ESP_LOGI(TAG, "SIP_EVENT_AUDIO_SESSION_END");
            audio_pipeline_stop(player_2);
            audio_pipeline_wait_for_stop(player_2);
            audio_pipeline_deinit(player_2);
            break;
        case SIP_EVENT_READ_AUDIO_DATA:
            return 0;
        case SIP_EVENT_WRITE_AUDIO_DATA:
            return raw_stream_write(raw_write_2, (char *)event->data, event->data_len);
        case SIP_EVENT_READ_DTMF:
            ESP_LOGI(TAG, "SIP_EVENT_READ_DTMF ID : %d ", ((char *)event->data)[0]);
            break;
    }
    return 0;
}

unsigned long IRAM_ATTR millis()
{
    return (unsigned long) (esp_timer_get_time() / 1000ULL);
}

#define led_period 200

static void main_loop_task(void *arg)
{
	ESP_LOGI(TAG, "GPIO config");

	/* Configure GPIO */
	gpio_pad_select_gpio(LED_1_GPIO);
	gpio_pad_select_gpio(LED_2_GPIO);
	gpio_pad_select_gpio(MUTE_GPIO);
	gpio_pad_select_gpio(RELAY_1_GPIO);
	gpio_pad_select_gpio(RELAY_2_GPIO);

  /* Set the GPIO as a push/pull output */
  gpio_set_direction(LED_1_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(LED_2_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(MUTE_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(RELAY_1_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(RELAY_2_GPIO, GPIO_MODE_OUTPUT);

	/* Turn off LEDs and relays */
  gpio_set_level(LED_1_GPIO, 0);
	gpio_set_level(LED_2_GPIO, 0);
	gpio_set_level(RELAY_1_GPIO, 0);
	gpio_set_level(RELAY_2_GPIO, 0);

	/* Mute */
	gpio_set_level(MUTE_GPIO, 1);

	sip_state_t sip_1_state, sip_1_state_old, sip_2_state, sip_2_state_old;
	sip_1_state_old = SIP_STATE_NONE;
	sip_1_state = SIP_STATE_NONE;
	sip_2_state_old = SIP_STATE_NONE;
	sip_2_state = SIP_STATE_NONE;

	bool sip_1_on, sip_2_on;
	sip_1_on = false;
	sip_2_on = false;

	unsigned long last_led_update = millis();
	bool blink = false;

	if (config_mode) {
		while (1) {
			if(millis() - last_led_update > led_period / 2){
				last_led_update = millis();
				blink = !blink;
				gpio_set_level(LED_1_GPIO, blink);
				gpio_set_level(LED_2_GPIO, !blink);
			}

			vTaskDelay(100 / portTICK_PERIOD_MS);
		}
	}

	while(1)
	{
		sip_1_state = esp_sip_get_state(sip_1);

		if (sip_1_state != sip_1_state_old) {
			if (sip_1_state < SIP_STATE_REGISTERED) {
				gpio_set_level(LED_1_GPIO, 0);
			} else {
				gpio_set_level(LED_1_GPIO, 1);
			}
			if (sip_1_state & SIP_STATE_REGISTERED) {
				sip_1_on = false;
				if(sip_2_state & SIP_STATE_RINGING) {
					sip_2_state_old = SIP_STATE_NONE;
				}
			}
			if (sip_1_state & SIP_STATE_RINGING) {
				if (!sip_2_on) {
					sip_1_on = true;
					ESP_LOGI(TAG, "SIP 1 Automatic answer");
					esp_sip_uas_answer(sip_1, true);
				} else {
					ESP_LOGW(TAG, "SIP 2 Already on call");
				}
			}
			if (sip_1_state & SIP_STATE_ON_CALL) {
				gpio_set_level(RELAY_1_GPIO, 1);
			} else {
				gpio_set_level(RELAY_1_GPIO, 0);
			}
		}

		sip_1_state_old = sip_1_state;

		sip_2_state = esp_sip_get_state(sip_2);

		if (sip_2_state != sip_2_state_old) {
			if (sip_2_state < SIP_STATE_REGISTERED) {
				gpio_set_level(LED_2_GPIO, 0);
			} else {
				gpio_set_level(LED_2_GPIO, 1);
			}
			if (sip_2_state & SIP_STATE_REGISTERED) {
				sip_2_on = false;
				if(sip_1_state & SIP_STATE_RINGING) {
					sip_1_state_old = SIP_STATE_NONE;
				}
			}
			if (sip_2_state & SIP_STATE_RINGING) {
				if (!sip_1_on) {
					sip_2_on = true;
					ESP_LOGI(TAG, "SIP 2 Automatic answer");
					esp_sip_uas_answer(sip_2, true);
				} else {
					ESP_LOGW(TAG, "SIP 1 Already on call");
				}
			}
			if (sip_2_state & SIP_STATE_ON_CALL) {
				gpio_set_level(RELAY_2_GPIO, 1);
			} else {
				gpio_set_level(RELAY_2_GPIO, 0);
			}
		}

		sip_2_state_old = sip_2_state;

		if(millis() - last_led_update > led_period){
			last_led_update = millis();
			blink = !blink;
			if (sip_1_state > SIP_STATE_REGISTERED) {
				gpio_set_level(LED_1_GPIO, blink);
			}
			if (sip_2_state > SIP_STATE_REGISTERED) {
				gpio_set_level(LED_2_GPIO, blink);
			}
		}

		if(sip_1_state > SIP_STATE_REGISTERED || sip_2_state > SIP_STATE_REGISTERED){
			gpio_set_level(MUTE_GPIO, 0);
		} else {
			gpio_set_level(MUTE_GPIO, 1);
		}

		vTaskDelay(100 / portTICK_PERIOD_MS);
	}
}

static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
  if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
      strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
    return 0;
  }
  return -1;
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
			case SYSTEM_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "station:"MACSTR" join, AID=%d",
                 MAC2STR(event->event_info.sta_connected.mac),
                 event->event_info.sta_connected.aid);
        break;
    	case SYSTEM_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "station:"MACSTR"leave, AID=%d",
                 MAC2STR(event->event_info.sta_disconnected.mac),
                 event->event_info.sta_disconnected.aid);
        break;
    	default:
        break;
    }

    return ESP_OK;
}

char ip_aux[16], gw_aux[16], mask_aux[16];
char sip1_uri[64], sip2_uri[64];

void app_main()
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("SIP", ESP_LOG_WARN);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
      // NVS partition was truncated and needs to be erased
      // Retry nvs_flash_init
      ESP_ERROR_CHECK(nvs_flash_erase());
      err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

		esp_chip_info_t chip_info;
		esp_chip_info(&chip_info);
		ESP_LOGI(TAG,"%dMB %s flash", spi_flash_get_chip_size()/(1024*1024), (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embeded" : "external");

		const esp_partition_t *configured = esp_ota_get_boot_partition();
		const esp_partition_t *running = esp_ota_get_running_partition();

	  if (configured != running) {
			ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
	                 configured->address, running->address);
	    ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
	  }
	  ESP_LOGI(TAG, "Running partition %s type 0x%02X subtype 0x%02X (offset 0x%08X)",
	             running->label, running->type, running->subtype, running->address);

		ESP_LOGI(TAG, "Initializing SPIFFS");

	  esp_vfs_spiffs_conf_t conf = {
	    .base_path = "/spiffs",
	    .partition_label = NULL,
	    .max_files = 5,
	    .format_if_mount_failed = true
	  };

	  // Use settings defined above to initialize and mount SPIFFS filesystem.
	  // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
	  esp_err_t ret = esp_vfs_spiffs_register(&conf);

	  if (ret != ESP_OK) {
	      if (ret == ESP_FAIL) {
	          ESP_LOGE(TAG, "Failed to mount or format filesystem");
	      } else if (ret == ESP_ERR_NOT_FOUND) {
	          ESP_LOGE(TAG, "Failed to find SPIFFS partition");
	      } else {
	          ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
	      }
	      //return;
	  }

	  size_t total = 0, used = 0;
	  ret = esp_spiffs_info(NULL, &total, &used);
	  if (ret != ESP_OK) {
	      ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
	  } else {
	      ESP_LOGI(TAG, "SPIFFS Partition size: total: %dkB, used: %dB", total/(1024), used);
	  }

		bool use_dhcp = true;

		tcpip_adapter_ip_info_t info;

		memset(&info, 0, sizeof(info));

		memset(ip_aux, 0, sizeof(ip_aux));
		memset(gw_aux, 0, sizeof(gw_aux));
		memset(mask_aux, 0, sizeof(mask_aux));

		memset(sip1_uri, 0, sizeof(sip1_uri));
		memset(sip2_uri, 0, sizeof(sip2_uri));

		// Check if config file exists
	  struct stat st;

	  if (stat("/spiffs/config.txt", &st) == 0) {
			ESP_LOGI(TAG, "Config file found");
			FILE* f = fopen("/spiffs/config.txt", "r");
			if (f != NULL) {
				char JSON_STRING[256];
				memset(JSON_STRING, 0, sizeof(JSON_STRING));
				fgets(JSON_STRING, sizeof(JSON_STRING), f);
				fclose(f);
				ESP_LOGW(TAG, "config.txt\n%s", JSON_STRING);

				int i;
				int r;
				jsmn_parser p;
				jsmntok_t t[32]; /* We expect no more than 32 tokens */

				jsmn_init(&p);
				r = jsmn_parse(&p, JSON_STRING, strlen(JSON_STRING), t, sizeof(t) / sizeof(t[0]));

				if (r < 0) {
					ESP_LOGE(TAG, "Failed to parse JSON: %d\n", r);
				} else {
					/* Assume the top-level element is an object */
					if (r < 1 || t[0].type != JSMN_OBJECT) {
						ESP_LOGE(TAG, "Object expected\n");
					} else {
						/* Loop over all keys of the root object */
						for (i = 1; i < r; i++) {
							if (jsoneq(JSON_STRING, &t[i], "dhcp") == 0) {
								printf("- DHCP: %.*s\n", t[i + 1].end - t[i + 1].start,
									JSON_STRING + t[i + 1].start);
								if (strncmp(JSON_STRING + t[i + 1].start, "false", t[i + 1].end - t[i + 1].start) == 0) {
									use_dhcp = false;
								}
								i++;
							} else if (jsoneq(JSON_STRING, &t[i], "ip") == 0) {
								printf("- IP addr: %.*s\n", t[i + 1].end - t[i + 1].start,
									JSON_STRING + t[i + 1].start);
								strncpy(ip_aux, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
								ip4addr_aton(ip_aux, &info.ip);
								i++;
							} else if (jsoneq(JSON_STRING, &t[i], "gw") == 0) {
								printf("- IP Gateway: %.*s\n", t[i + 1].end - t[i + 1].start,
									JSON_STRING + t[i + 1].start);
								strncpy(gw_aux, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
								ip4addr_aton(gw_aux, &info.gw);
								i++;
							} else if (jsoneq(JSON_STRING, &t[i], "mask") == 0) {
								printf("- IP Mask: %.*s\n", t[i + 1].end - t[i + 1].start,
									JSON_STRING + t[i + 1].start);
								strncpy(mask_aux, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
								ip4addr_aton(mask_aux, &info.netmask);
								i++;
							} else 	if (jsoneq(JSON_STRING, &t[i], "sip1") == 0) {
								printf("- SIP 1 uri: %.*s\n", t[i + 1].end - t[i + 1].start,
									JSON_STRING + t[i + 1].start);
								strncpy(sip1_uri, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
								i++;
							} else if (jsoneq(JSON_STRING, &t[i], "sip2") == 0) {
								printf("- SIP 2 uri: %.*s\n", t[i + 1].end - t[i + 1].start,
									JSON_STRING + t[i + 1].start);
								strncpy(sip2_uri, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
								i++;
							} else if (jsoneq(JSON_STRING, &t[i], "spk") == 0) {
								printf("- VOL speaker: %.*s\n", t[i + 1].end - t[i + 1].start,
									JSON_STRING + t[i + 1].start);
								char s[8];
								strncpy(s, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
								spk_volume = atoi(s);
								i++;
							} else {
								printf("Unexpected key: %.*s\n", t[i].end - t[i].start,
									JSON_STRING + t[i].start);
							}
						}
					}
				}
				// Delete
				//unlink("/spiffs/config.txt");
			} else {
					ESP_LOGE(TAG, "Failed to open file for reading");
			}
		} else {
			ESP_LOGW(TAG, "Config file not found");
			FILE* f = fopen("/spiffs/config.txt", "w");
			if (f != NULL) {
	#ifdef CONFIG_DHCP
				fprintf(f, "{\"dhcp\":true,");
	#else
				fprintf(f, "{\"dhcp\":false,");
	#endif
				fprintf(f, "\"ip\":\"" CONFIG_IP "\",");
				fprintf(f, "\"gw\":\"" CONFIG_GW "\",");
				fprintf(f, "\"mask\":\"" CONFIG_MASK "\",");
				fprintf(f, "\"sip1\":\"" CONFIG_SIP_1_URI "\",");
				fprintf(f, "\"sip2\":\"" CONFIG_SIP_2_URI "\",");
				fprintf(f, "\"spk\":0}");
				fclose(f);
				ESP_LOGW(TAG, "Default config file written");
			} else {
				ESP_LOGE(TAG, "Failed to open file for writing");
			}
			esp_restart();
		}

		// Config mode?

		/* Set the GPIO as a input */
		gpio_pad_select_gpio(BTN_1_GPIO);
		gpio_set_direction(BTN_1_GPIO, GPIO_MODE_INPUT);

		config_mode = false;
		if (!gpio_get_level(BTN_1_GPIO)) {
			vTaskDelay(100 / portTICK_PERIOD_MS);
			if (!gpio_get_level(BTN_1_GPIO)) {
				ESP_LOGW(TAG, "Enter config softAP mode");
				config_mode = true;
			}
		}

		if (config_mode) {
			ESP_LOGI(TAG, "Start networking WiFi softAP");

			tcpip_adapter_init();

		  ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

			wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

		  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

		  wifi_config_t wifi_config = {
		      .ap = {
		          .ssid = CONFIG_AP_SSID,
		          .ssid_len = strlen(CONFIG_AP_SSID),
		          .password = CONFIG_AP_PASS,
		          .max_connection = 1,
		          .authmode = WIFI_AUTH_WPA_WPA2_PSK
		      },
		  };

		  if (strlen(CONFIG_AP_PASS) == 0) {
		      wifi_config.ap.authmode = WIFI_AUTH_OPEN;
		  }

		  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
		  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
			ESP_ERROR_CHECK(esp_wifi_start());
		} else {
			ESP_LOGI(TAG, "Start networking WiFi STA");

			tcpip_adapter_init();

			if (use_dhcp) {
				ESP_LOGI(TAG, "Using DHCP");
			} else {
				ESP_LOGI(TAG, "Using static IP");
				ESP_ERROR_CHECK(tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA));
				ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &info));
			}

	    ESP_LOGI(TAG, "Initialize peripherals management");

	    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
	    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

	    periph_wifi_cfg_t wifi_cfg = {
	        .ssid = CONFIG_STA_SSID,
	        .password = CONFIG_STA_PASS,
	    };

	    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);

	    esp_periph_start(set, wifi_handle);

	    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);

			ESP_LOGI(TAG, "Create SIP_1 Service");
			
			sip_config_t sip_1_cfg = {
					.uri = sip1_uri,
					.event_handler = _sip_1_event_handler,
					.send_options = true,
		#ifdef CONFIG_SIP_CODEC_G711A
					.acodec_type = SIP_ACODEC_G711A,
		#else
					.acodec_type = SIP_ACODEC_G711U,
		#endif
			};
			sip_1 = esp_sip_init(&sip_1_cfg);
			esp_sip_start(sip_1);

			ESP_LOGI(TAG, "Create SIP_2 Service");

			sip_config_t sip_2_cfg = {
					.uri = sip2_uri,
					.event_handler = _sip_2_event_handler,
					.send_options = true,
		#ifdef CONFIG_SIP_CODEC_G711A
					.acodec_type = SIP_ACODEC_G711A,
		#else
					.acodec_type = SIP_ACODEC_G711U,
		#endif
			};
			sip_2 = esp_sip_init(&sip_2_cfg);
			esp_sip_start(sip_2);
		}

		ESP_LOGI(TAG, "Start server");
		ESP_ERROR_CHECK(start_server());

		ESP_LOGI(TAG, "Start main loop");
		xTaskCreate(main_loop_task, "main_loop_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
}
