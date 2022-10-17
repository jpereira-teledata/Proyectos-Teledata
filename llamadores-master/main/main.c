#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"

#include "board.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "esp_peripherals.h"
#include "audio_mem.h"
#include "raw_stream.h"
#include "filter_resample.h"
#include "esp_sip.h"
#include "g711_decoder.h"
#include "g711_encoder.h"
#include "spiffs_stream.h"
#include "wav_decoder.h"
#include "algorithm_stream.h"

#include "caller.h"
#include "server.h"
#include "client.h"

#include "jsmn.h"

#define FW_VERSION 9

static const char *TAG = "MAIN";

// AUDIO ######################################################################

#define I2S_SAMPLE_RATE     8000
#define I2S_CHANNELS        2
#define I2S_BITS            16

#define ADC_SAMPLE_RATE     48000
#define ADC_CHANNELS        1
#define ADC_BITS            16

#define CODEC_SAMPLE_RATE   8000
#define CODEC_CHANNELS      1
#define CODEC_BITS      		16

sip_handle_t sip;
audio_element_handle_t raw_read, raw_write;
audio_element_handle_t i2s_stream_reader, i2s_stream_writer;
audio_pipeline_handle_t recorder, player, tone_player;
bool tone_init = false;

// ECHO CANCELATION ###########################################################

int spk_volume = 0;
int mic_volume = 0;

#define ALPHA_IN	0.70
#define ALPHA_OUT	0.55
#define THRESHOLD 500

int16_t max_filter = 0;

void max_write_value_callback(int16_t max)
{
	if (max > max_filter)
	{
		max_filter = max * ALPHA_IN + max_filter * (1 - ALPHA_IN);
	} else {
		max_filter = max * ALPHA_OUT + max_filter * (1 - ALPHA_OUT);
	}

	int vol = mic_volume - max_filter / THRESHOLD;
	if (vol > mic_volume) vol = mic_volume;

	//ESP_LOGI(TAG, "max %d, max_filter %d, mic_volume %d, vol %d", max, max_filter, mic_volume, vol);

	if (i2s_stream_reader != NULL) i2s_alc_volume_set(i2s_stream_reader, vol);
}

void max_read_value_callback(int16_t max)
{
	//ESP_LOGI(TAG, "read %d", max);
}

// TONE GENERATOR #############################################################

int tone_volume = -10;

int tomerId = 1;
TimerHandle_t tmr;
uint32_t interval = 4 * 1000;

extern const uint8_t wav_start[] asm("_binary_ringback_wav_start");
extern const uint8_t wav_end[]   asm("_binary_ringback_wav_end");

static struct marker {
	int pos;
	const uint8_t *start;
	const uint8_t *end;
} file_marker;

int wav_music_read_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx)
{
	int read_size = file_marker.end - file_marker.start - file_marker.pos;
	if (read_size == 0) {
		return AEL_IO_DONE;
	} else if (len < read_size) {
		read_size = len;
	}
	memcpy(buf, file_marker.start + file_marker.pos, read_size);
	file_marker.pos += read_size;
	return read_size;
}

static esp_err_t tone_pipeline_open(void)
{
	tone_init = true;

	audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
	tone_player = audio_pipeline_init(&pipeline_cfg);
	AUDIO_NULL_CHECK(TAG, tone_player, return ESP_FAIL);

	wav_decoder_cfg_t  wav_dec_cfg  = DEFAULT_WAV_DECODER_CONFIG();
	audio_element_handle_t music_decoder = wav_decoder_init(&wav_dec_cfg);

	file_marker.start = wav_start;
	file_marker.end   = wav_end;
	file_marker.pos 	= 0;

	audio_element_set_read_cb(music_decoder, wav_music_read_cb, NULL);

	rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
	rsp_cfg.src_rate = CODEC_SAMPLE_RATE;
	rsp_cfg.src_ch = CODEC_CHANNELS;
	rsp_cfg.dest_rate = I2S_SAMPLE_RATE;
	rsp_cfg.dest_ch = I2S_CHANNELS;
	audio_element_handle_t music_filter = rsp_filter_init(&rsp_cfg);

	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG();
	i2s_cfg.i2s_config.sample_rate = I2S_SAMPLE_RATE;
	i2s_cfg.use_alc = true;
	i2s_cfg.volume = tone_volume;

	audio_element_handle_t music_i2s_stream_writer = i2s_stream_init(&i2s_cfg);
	audio_element_info_t i2s_info = {0};
	audio_element_getinfo(music_i2s_stream_writer, &i2s_info);
	i2s_info.bits = I2S_BITS;
	i2s_info.channels = I2S_CHANNELS;
	i2s_info.sample_rates = I2S_SAMPLE_RATE;
	audio_element_setinfo(music_i2s_stream_writer, &i2s_info);

	audio_pipeline_register(tone_player, music_decoder, "music_decoder");
	audio_pipeline_register(tone_player, music_filter, "music_filter");
	audio_pipeline_register(tone_player, music_i2s_stream_writer, "music_i2s");

	const char *link_tag[3] = {"music_decoder", "music_filter", "music_i2s"};
	audio_pipeline_link(tone_player, &link_tag[0], 3);

	tone_init = false;

	ESP_LOGI(TAG, "Tone has been created");
	return ESP_OK;
}

void play_tone(TimerHandle_t xTimer)
{
	if (tone_player != NULL) {
		audio_pipeline_stop(tone_player);
		audio_pipeline_wait_for_stop(tone_player);
		audio_pipeline_deinit(tone_player);
		tone_player = NULL;
	}
	tone_pipeline_open();
	audio_pipeline_run(tone_player);
}

// SIP ########################################################################

static esp_err_t player_pipeline_open(void)
{
	audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
	player = audio_pipeline_init(&pipeline_cfg);
	AUDIO_NULL_CHECK(TAG, player, return ESP_FAIL);

	raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
	raw_cfg.type = AUDIO_STREAM_WRITER;
	raw_write = raw_stream_init(&raw_cfg);

	g711_decoder_cfg_t g711_cfg = DEFAULT_G711_DECODER_CONFIG();
	audio_element_handle_t sip_decoder = g711_decoder_init(&g711_cfg);

	rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
	rsp_cfg.src_rate = CODEC_SAMPLE_RATE;
	rsp_cfg.src_ch = CODEC_CHANNELS;
	rsp_cfg.dest_rate = I2S_SAMPLE_RATE;
	rsp_cfg.dest_ch = I2S_CHANNELS;
	audio_element_handle_t filter = rsp_filter_init(&rsp_cfg);

	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG();
	i2s_cfg.i2s_config.sample_rate = I2S_SAMPLE_RATE;
	i2s_cfg.use_alc = true;
	i2s_cfg.volume = spk_volume;

	i2s_stream_writer = i2s_stream_init(&i2s_cfg);
	audio_element_info_t i2s_info = {0};
	audio_element_getinfo(i2s_stream_writer, &i2s_info);
	i2s_info.bits = I2S_BITS;
	i2s_info.channels = I2S_CHANNELS;
	i2s_info.sample_rates = I2S_SAMPLE_RATE;
	audio_element_setinfo(i2s_stream_writer, &i2s_info);

	audio_pipeline_register(player, raw_write, "raw");
	audio_pipeline_register(player, sip_decoder, "sip_dec");
	audio_pipeline_register(player, filter, "filter");
	audio_pipeline_register(player, i2s_stream_writer, "i2s");

	const char *link_tag[4] = {"raw", "sip_dec", "filter", "i2s"};
	audio_pipeline_link(player, &link_tag[0], 4);

	ESP_LOGI(TAG, "Speaker has been created");
	return ESP_OK;
}

static esp_err_t recorder_pipeline_open(void)
{
	audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
	recorder = audio_pipeline_init(&pipeline_cfg);
	AUDIO_NULL_CHECK(TAG, recorder, return ESP_FAIL);

	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_INTERNAL_ADC_CFG();
	i2s_cfg.i2s_config.sample_rate = ADC_SAMPLE_RATE;
	i2s_cfg.use_alc = true;
	i2s_cfg.volume = mic_volume;

	i2s_set_adc_mode(ADC_UNIT, ADC_CHANNEL);
	adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN);

	i2s_stream_reader = i2s_stream_init(&i2s_cfg);
	audio_element_info_t i2s_info = {0};
	audio_element_getinfo(i2s_stream_reader, &i2s_info);
	i2s_info.bits = ADC_BITS;
	i2s_info.channels = ADC_CHANNELS;
	i2s_info.sample_rates = ADC_SAMPLE_RATE;
	audio_element_setinfo(i2s_stream_reader, &i2s_info);

	rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
	rsp_cfg.src_rate = ADC_SAMPLE_RATE;
	rsp_cfg.src_ch = ADC_CHANNELS;
	rsp_cfg.dest_rate = CODEC_SAMPLE_RATE;
	rsp_cfg.dest_ch = CODEC_CHANNELS;
	rsp_cfg.complexity = 5;
	audio_element_handle_t filter = rsp_filter_init(&rsp_cfg);

	g711_encoder_cfg_t g711_cfg = DEFAULT_G711_ENCODER_CONFIG();
	audio_element_handle_t sip_encoder = g711_encoder_init(&g711_cfg);

	raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
	raw_cfg.type = AUDIO_STREAM_READER;
	raw_read = raw_stream_init(&raw_cfg);
	audio_element_set_output_timeout(raw_read, portMAX_DELAY);

	audio_pipeline_register(recorder, i2s_stream_reader, "i2s");
	audio_pipeline_register(recorder, filter, "filter");
	audio_pipeline_register(recorder, sip_encoder, "sip_enc");
	audio_pipeline_register(recorder, raw_read, "raw");

	const char *link_tag[4] = {"i2s", "filter", "sip_enc", "raw"};
	audio_pipeline_link(recorder, &link_tag[0], 4);

	ESP_LOGI(TAG, "SIP recorder has been created");
	return ESP_OK;
}

static ip4_addr_t _get_network_ip(void)
{
	tcpip_adapter_ip_info_t ip;
	tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_ETH, &ip);
	return ip.ip;
}

static int _sip_event_handler(sip_event_msg_t *event)
{
	ip4_addr_t ip;
	switch ((int)event->type)
	{
		case SIP_EVENT_REQUEST_NETWORK_STATUS:
			ESP_LOGD(TAG, "SIP_EVENT_REQUEST_NETWORK_STATUS");
			ip = _get_network_ip();
			if (ip.addr) return true;
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
			xTimerStop(tmr, 0);
			while (tone_init) vTaskDelay(1);
			if (tone_player != NULL)
			{
				audio_pipeline_stop(tone_player);
				audio_pipeline_wait_for_stop(tone_player);
				audio_pipeline_deinit(tone_player);
				tone_player = NULL;
			}
			tone_pipeline_open();
			audio_pipeline_run(tone_player);
			xTimerStart(tmr, 0);
			break;
		case SIP_EVENT_BUSY:
			ESP_LOGI(TAG, "SIP_EVENT_BUSY");
			break;
		case SIP_EVENT_HANGUP:
			ESP_LOGI(TAG, "SIP_EVENT_HANGUP");
			xTimerStop(tmr, 0);
			break;
		case SIP_EVENT_AUDIO_SESSION_BEGIN:
			ESP_LOGI(TAG, "SIP_EVENT_AUDIO_SESSION_BEGIN");
			xTimerStop(tmr, 0);
			while (tone_init) vTaskDelay(1);
			if (tone_player != NULL)
			{
				audio_pipeline_stop(tone_player);
				audio_pipeline_wait_for_stop(tone_player);
				audio_pipeline_deinit(tone_player);
				tone_player = NULL;
			}
			player_pipeline_open();
			recorder_pipeline_open();
			audio_pipeline_run(player);
			audio_pipeline_run(recorder);
			break;
		case SIP_EVENT_AUDIO_SESSION_END:
			ESP_LOGI(TAG, "SIP_EVENT_AUDIO_SESSION_END");
			audio_pipeline_stop(player);
			audio_pipeline_wait_for_stop(player);
			audio_pipeline_deinit(player);
			audio_pipeline_stop(recorder);
			audio_pipeline_wait_for_stop(recorder);
			audio_pipeline_deinit(recorder);
			i2s_stream_reader = NULL;
			break;
		case SIP_EVENT_READ_AUDIO_DATA:
			return raw_stream_read(raw_read, (char *)event->data, event->data_len);
		case SIP_EVENT_WRITE_AUDIO_DATA:
			return raw_stream_write(raw_write, (char *)event->data, event->data_len);
		case SIP_EVENT_READ_DTMF:
			ESP_LOGI(TAG, "SIP_EVENT_READ_DTMF ID : %d ", ((char *)event->data)[0]);
			break;
	}
	return 0;
}

// ETHERNET ###################################################################

#if CONFIG_PHY_LAN8720
#include "eth_phy/phy_lan8720.h"
#define DEFAULT_ETHERNET_PHY_CONFIG phy_lan8720_default_ethernet_config
#elif CONFIG_PHY_TLK110
#include "eth_phy/phy_tlk110.h"
#define DEFAULT_ETHERNET_PHY_CONFIG phy_tlk110_default_ethernet_config
#elif CONFIG_PHY_IP101
#include "eth_phy/phy_ip101.h"
#define DEFAULT_ETHERNET_PHY_CONFIG phy_ip101_default_ethernet_config
#endif

#define PIN_PHY_POWER CONFIG_PHY_POWER_PIN
#define PIN_SMI_MDC 	CONFIG_PHY_SMI_MDC_PIN
#define PIN_SMI_MDIO 	CONFIG_PHY_SMI_MDIO_PIN

#ifdef CONFIG_PHY_USE_POWER_PIN
/**
* @brief re-define power enable func for phy
*
* @param enable true to enable, false to disable
*
* @note This function replaces the default PHY power on/off function.
* If this GPIO is not connected on your device (and PHY is always powered),
* you can use the default PHY-specific power on/off function.
*/
static void phy_device_power_enable_via_gpio(bool enable)
{
	assert(DEFAULT_ETHERNET_PHY_CONFIG.phy_power_enable);

	if (!enable) {
		DEFAULT_ETHERNET_PHY_CONFIG.phy_power_enable(false);
	}

	gpio_pad_select_gpio(PIN_PHY_POWER);
	gpio_set_direction(PIN_PHY_POWER, GPIO_MODE_OUTPUT);
	if (enable == true) {
		gpio_set_level(PIN_PHY_POWER, 1);
		ESP_LOGI(TAG, "Power On Ethernet PHY");
	} else {
		gpio_set_level(PIN_PHY_POWER, 0);
		ESP_LOGI(TAG, "Power Off Ethernet PHY");
	}

	vTaskDelay(1); // Allow the power up/down to take effect, min 300us

	if (enable) {
		/* call the default PHY-specific power on function */
		DEFAULT_ETHERNET_PHY_CONFIG.phy_power_enable(true);
	}
}
#endif

/**
* @brief gpio specific init
*
* @note RMII data pins are fixed in esp32:
* TXD0 <=> GPIO19
* TXD1 <=> GPIO22
* TX_EN <=> GPIO21
* RXD0 <=> GPIO25
* RXD1 <=> GPIO26
* CLK <=> GPIO0
*
*/
static void eth_gpio_config_rmii(void)
{
	phy_rmii_configure_data_interface_pins();
	phy_rmii_smi_configure_pins(PIN_SMI_MDC, PIN_SMI_MDIO);
}

/**
* @brief event handler for ethernet
*
* @param ctx
* @param event
* @return esp_err_t
*/
static esp_err_t event_handler(void *ctx, system_event_t *event)
{
	tcpip_adapter_ip_info_t ip;

	switch (event->event_id)
	{
		case SYSTEM_EVENT_ETH_CONNECTED:
			ESP_LOGI(TAG, "Ethernet Link Up");
			break;
		case SYSTEM_EVENT_ETH_DISCONNECTED:
			ESP_LOGI(TAG, "Ethernet Link Down");
			break;
		case SYSTEM_EVENT_ETH_START:
			ESP_LOGI(TAG, "Ethernet Started");
			break;
		case SYSTEM_EVENT_ETH_GOT_IP:
			memset(&ip, 0, sizeof(tcpip_adapter_ip_info_t));
			ESP_ERROR_CHECK(tcpip_adapter_get_ip_info(ESP_IF_ETH, &ip));
			ESP_LOGI(TAG, "Ethernet Got IP Addr");
			ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip.ip));
			ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip.netmask));
			ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip.gw));
			break;
		case SYSTEM_EVENT_ETH_STOP:
			ESP_LOGI(TAG, "Ethernet Stopped");
			break;
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

// CONFIG	#####################################################################

static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
	if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
	strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
		return 0;
	}
	return -1;
}

char ip_aux[16], gw_aux[16], mask_aux[16];
char sip_uri[64];

sc_config_t sc_config;
caller_config_t caller_config;

// MAIN #######################################################################

void app_main()
{
	esp_log_level_set("*", ESP_LOG_WARN);
	esp_log_level_set("APP", ESP_LOG_INFO);
	esp_log_level_set("MAIN", ESP_LOG_INFO);
	esp_log_level_set("SERVER", ESP_LOG_INFO);
	esp_log_level_set("HTTP_CLIENT", ESP_LOG_INFO);

	/* Init configuration storage */

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
		.max_files = 3,
		.format_if_mount_failed = true
	};

	// Use settings defined above to initialize and mount SPIFFS filesystem.
	esp_err_t ret = esp_vfs_spiffs_register(&conf);

	if (ret != ESP_OK) {
		if (ret == ESP_FAIL) {
			ESP_LOGE(TAG, "Failed to mount or format filesystem");
		} else if (ret == ESP_ERR_NOT_FOUND) {
			ESP_LOGE(TAG, "Failed to find SPIFFS partition");
		} else {
			ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
		}
	}

	size_t total = 0, used = 0;
	ret = esp_spiffs_info(NULL, &total, &used);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
	} else {
		ESP_LOGI(TAG, "SPIFFS Partition size: total: %dKB, used: %dB", total / 1024, used);
	}

	/* Init configuration variables */

	bool config_parsed = false;

	bool use_dhcp = true;
	tcpip_adapter_ip_info_t info;
	memset(&info, 0, sizeof(info));

	memset(ip_aux, 0, sizeof(ip_aux));
	memset(gw_aux, 0, sizeof(gw_aux));
	memset(mask_aux, 0, sizeof(mask_aux));

	memset(sc_config.sc_server, 0, sizeof(sc_config.sc_server));
	memset(sc_config.sc_url, 0, sizeof(sc_config.sc_url));
	memset(sc_config.sc_user, 0, sizeof(sc_config.sc_user));
	memset(sc_config.sc_pass, 0, sizeof(sc_config.sc_pass));

	caller_config.sip_enable = true;
	memset(sip_uri, 0, sizeof(sip_uri));
	memset(caller_config.sip_call, 0, sizeof(caller_config.sip_call));
	caller_config.invert_panic_button = false;

	/* Check if config file exists */

	struct stat st;
	if (stat("/spiffs/config.txt", &st) == 0) {

		ESP_LOGI(TAG, "Config file found");
		FILE* f = fopen("/spiffs/config.txt", "r");

		if (f != NULL) {

			char JSON_STRING[320];
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
				/* Config file parse error */
				ESP_LOGE(TAG, "Failed to parse JSON: %d\n", r);
			} else {
				/* Assume the top-level element is an object */
				if (r < 1 || t[0].type != JSMN_OBJECT) {
					ESP_LOGE(TAG, "Object expected\n");
				} else {
					/* Config file parsed */
					config_parsed = true;
					char s[8];

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
						} else if (jsoneq(JSON_STRING, &t[i], "server") == 0) {
							printf("- SC server: %.*s\n", t[i + 1].end - t[i + 1].start,
							JSON_STRING + t[i + 1].start);
							strncpy(sc_config.sc_server, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
							i++;
						} else if (jsoneq(JSON_STRING, &t[i], "url") == 0) {
							printf("- SC url: %.*s\n", t[i + 1].end - t[i + 1].start,
							JSON_STRING + t[i + 1].start);
							strncpy(sc_config.sc_url, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
							i++;
						} else if (jsoneq(JSON_STRING, &t[i], "user") == 0) {
							printf("- SC user: %.*s\n", t[i + 1].end - t[i + 1].start,
							JSON_STRING + t[i + 1].start);
							strncpy(sc_config.sc_user, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
							i++;
						} else if (jsoneq(JSON_STRING, &t[i], "pass") == 0) {
							printf("- SC password: %.*s\n", t[i + 1].end - t[i + 1].start,
							JSON_STRING + t[i + 1].start);
							strncpy(sc_config.sc_pass, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
							i++;
						} else if (jsoneq(JSON_STRING, &t[i], "sip_enable") == 0) {
							printf("- SIP enabled: %.*s\n", t[i + 1].end - t[i + 1].start,
							JSON_STRING + t[i + 1].start);
							if (strncmp(JSON_STRING + t[i + 1].start, "false", t[i + 1].end - t[i + 1].start) == 0) {
								caller_config.sip_enable = false;
							}
							i++;
						} else if (jsoneq(JSON_STRING, &t[i], "sip") == 0) {
							printf("- SIP uri: %.*s\n", t[i + 1].end - t[i + 1].start,
							JSON_STRING + t[i + 1].start);
							strncpy(sip_uri, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
							i++;
						} else if (jsoneq(JSON_STRING, &t[i], "call") == 0) {
							printf("- SIP call: %.*s\n", t[i + 1].end - t[i + 1].start,
							JSON_STRING + t[i + 1].start);
							strncpy(caller_config.sip_call, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
							i++;
						} else if (jsoneq(JSON_STRING, &t[i], "tone") == 0) {
							printf("- VOL tone: %.*s\n", t[i + 1].end - t[i + 1].start,
							JSON_STRING + t[i + 1].start);
							memset(s, 0, sizeof(s));
							strncpy(s, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
							tone_volume = atoi(s);
							i++;
						} else if (jsoneq(JSON_STRING, &t[i], "spk") == 0) {
							printf("- VOL speaker: %.*s\n", t[i + 1].end - t[i + 1].start,
							JSON_STRING + t[i + 1].start);
							memset(s, 0, sizeof(s));
							strncpy(s, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
							spk_volume = atoi(s);
							i++;
						} else if (jsoneq(JSON_STRING, &t[i], "mic") == 0) {
							printf("- VOL mic: %.*s\n", t[i + 1].end - t[i + 1].start,
							JSON_STRING + t[i + 1].start);
							memset(s, 0, sizeof(s));
							strncpy(s, JSON_STRING + t[i + 1].start, t[i + 1].end - t[i + 1].start);
							mic_volume = atoi(s);
							i++;
						} else if (jsoneq(JSON_STRING, &t[i], "invert_panic_button") == 0) {
							printf("- Invert panic button: %.*s\n", t[i + 1].end - t[i + 1].start,
							JSON_STRING + t[i + 1].start);
							if (strncmp(JSON_STRING + t[i + 1].start, "true", t[i + 1].end - t[i + 1].start) == 0) {
								caller_config.invert_panic_button = true;
							}
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
		/* Create default config */
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
			fprintf(f, "\"server\":\"" CONFIG_SERVER_IP "\",");
			fprintf(f, "\"url\":\"" CONFIG_SERVER_URL "\",");
			fprintf(f, "\"user\":\"" CONFIG_SERVER_USER "\",");
			fprintf(f, "\"pass\":\"" CONFIG_SERVER_PASS "\",");
			fprintf(f, "\"sip_enable\":true,");
			fprintf(f, "\"sip\":\"" CONFIG_SIP_URI "\",");
			fprintf(f, "\"call\":\"" CONFIG_SIP_CALL "\",");
			fprintf(f, "\"tone\":-10,");
			fprintf(f, "\"spk\":0,");
			fprintf(f, "\"mic\":0,");
			fprintf(f, "\"invert_panic_button\":false}");
			fclose(f);
			ESP_LOGW(TAG, "Default config file written");
		} else {
			ESP_LOGE(TAG, "Failed to open file for writing");
		}
		esp_restart();
	}

	ESP_LOGI(TAG, "Start io loop");
	xTaskCreate(io_task, "io_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);

	ESP_LOGI(TAG, "Start main loop");
	xTaskCreate(main_loop_task, "main_loop_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);

	ESP_LOGI(TAG, "Start networking");
	tcpip_adapter_init();
	ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

	/* Ethernet */

	eth_config_t config = DEFAULT_ETHERNET_PHY_CONFIG;
	config.phy_addr = CONFIG_PHY_ADDRESS;
	config.gpio_config = eth_gpio_config_rmii;
	config.tcpip_input = tcpip_adapter_eth_input;
	config.clock_mode = CONFIG_PHY_CLOCK_MODE;
	#ifdef CONFIG_PHY_USE_POWER_PIN
	config.phy_power_enable = phy_device_power_enable_via_gpio;
	#endif

	ESP_ERROR_CHECK(esp_eth_init(&config));
	ESP_ERROR_CHECK(esp_eth_enable()) ;

	if (use_dhcp || !config_parsed) {
		ESP_LOGI(TAG, "Using DHCP");
	} else {
		ESP_LOGI(TAG, "Using static IP");
		ESP_ERROR_CHECK(tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_ETH));
		ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_ETH, &info));
	}

	/* WiFi softAP */

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

	/* Timer for tone enerator */

	tmr = xTimerCreate("tone_player_timer", pdMS_TO_TICKS(interval), pdTRUE, (void *)tomerId, &play_tone);

	/* SIP service */

	if (caller_config.sip_enable && config_parsed) {
		ESP_LOGI(TAG, "Create SIP Service");
		sip_config_t sip_cfg = {
			.uri = sip_uri,
			.event_handler = _sip_event_handler,
			.send_options = true,
			#ifdef CONFIG_SIP_CODEC_G711A
			.acodec_type = SIP_ACODEC_G711A,
			#else
			.acodec_type = SIP_ACODEC_G711U,
			#endif
		};
		sip = esp_sip_init(&sip_cfg);
		esp_sip_start(sip);
	} else {
		ESP_LOGI(TAG, "No SIP Service");
	}

	/* Web server */

	ESP_LOGI(TAG, "Start server");
	ESP_ERROR_CHECK(start_server(FW_VERSION));

	/* Web client */

	if (config_parsed){
		ESP_LOGI(TAG, "Start client task");
		xTaskCreate(client_task, "client_task", 2048, &sc_config, tskIDLE_PRIORITY + 1, NULL);
	}
}
