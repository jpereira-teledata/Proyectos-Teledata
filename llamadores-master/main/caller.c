#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "APP";

#include "esp_wifi.h"
#include "driver/i2c.h"
#include "esp_sip.h"

#include "caller.h"
#include "client.h"

// pins
#define KEYBOARD_INT_GPIO 34
#define BOARD_INT_GPIO    35

// type
#define KEY_PRESSED   0
#define KEY_RELEASED  1
#define LED_TURN_ON   2
#define LED_TURN_OFF  3

// data
#define BD1_KEY       0
#define CL1_KEY       1
#define BD2_KEY       2
#define CL2_KEY       3
#define PAN_KEY       4
#define BAT_KEY       5
#define DINTEL_RED    6
#define DINTEL_GREEN  7
#define NURSE_KEY     8
#define RESOLVE_KEY   9
#define BLACK_KEY     10
#define GRAY_KEY      11
#define ON_LED        12
#define C1_LED        13
#define C2_LED        14
#define B_LED         15

#define TEMP_SENSOR_ADDR 0x48

#define BOARD_INPUT_ADDR 0x38
#define BOARD_INPUT_MASK 0x3F

#define KEYBOARD_INPUT_ADDR 0x39
#define KEYBOARD_INPUT_MASK 0xF0

struct m_event
{
	uint8_t type;
	uint8_t data;
};

#define I2C_MASTER_TX_BUF_DISABLE 0 /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0 /*!< I2C master doesn't need buffer */
#define WRITE_BIT I2C_MASTER_WRITE  /*!< I2C master write */
#define READ_BIT I2C_MASTER_READ    /*!< I2C master read */
#define ACK_CHECK_EN 0x1            /*!< I2C master will check ack from slave*/
#define ACK_CHECK_DIS 0x0           /*!< I2C master will not check ack from slave */
#define ACK_VAL 0x0                 /*!< I2C ack value */
#define NACK_VAL 0x1                /*!< I2C nack value */

QueueHandle_t xMainLoopQueue, xIOLoopQueue;

static esp_err_t i2c_master_driver_initialize(void)
{
	i2c_config_t conf = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = GPIO_NUM_32,
		.sda_pullup_en = GPIO_PULLUP_DISABLE,
		.scl_io_num = GPIO_NUM_33,
		.scl_pullup_en = GPIO_PULLUP_DISABLE,
		.master.clk_speed = 100000
	};
	return i2c_param_config(I2C_NUM_0, &conf);
}

float volatile temp;

static void i2c_read_temp(void)
{
	uint8_t data[2];

	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, TEMP_SENSOR_ADDR << 1 | READ_BIT, ACK_CHECK_EN);
	i2c_master_read(cmd, data, 2, ACK_VAL);
	i2c_master_stop(cmd);
	esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_RATE_MS);
	i2c_cmd_link_delete(cmd);

	if (ret == ESP_OK)
	{
		ESP_LOGD(TAG, "Temp read OK 0x%X", TEMP_SENSOR_ADDR);
		uint32_t regdata;
		regdata = (data[0] << 8) | data[1];
		temp = ((float)(int)regdata / 32) / 8;
	} else if (ret == ESP_ERR_TIMEOUT) {
		ESP_LOGW(TAG, "Bus is busy 0x%X", TEMP_SENSOR_ADDR);
	} else {
		temp = -127.0;
	}
}

static uint8_t i2c_read(uint8_t addr)
{
	uint8_t data;

	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, addr << 1 | READ_BIT, ACK_CHECK_EN);
	i2c_master_read_byte(cmd, &data, NACK_VAL);
	i2c_master_stop(cmd);
	esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_RATE_MS);
	i2c_cmd_link_delete(cmd);

	if (ret == ESP_OK)
	{
		ESP_LOGD(TAG, "Read OK 0x%X", addr);
		return data;
	} else if (ret == ESP_ERR_TIMEOUT) {
		ESP_LOGW(TAG, "Bus is busy 0x%X", addr);
	} else {
		ESP_LOGE(TAG, "Read Failed 0x%X", addr);
	}

	return 0xFF;
}

static void i2c_write(uint8_t addr, uint8_t data)
{
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, addr << 1 | WRITE_BIT, ACK_CHECK_EN);
	i2c_master_write_byte(cmd, data, ACK_CHECK_EN);
	i2c_master_stop(cmd);
	esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_RATE_MS);
	i2c_cmd_link_delete(cmd);

	if (ret == ESP_OK)
	{
		ESP_LOGD(TAG, "Write OK 0x%X", addr);
	} else if (ret == ESP_ERR_TIMEOUT) {
		ESP_LOGW(TAG, "Bus is busy 0x%X", addr);
	} else {
		ESP_LOGE(TAG, "Write Failed 0x%X", addr);
	}
}

static void notify_key(uint8_t key, uint8_t state)
{
	struct m_event io_event;

	io_event.type = state;
	io_event.data = key;

	if (xMainLoopQueue != NULL)
	{
		if (xQueueSend(xMainLoopQueue, &io_event, 0 ) != pdPASS)
		{
			ESP_LOGE(TAG, "Failed to post the message on MainLoopQueue.");
		}
	} else {
		ESP_LOGE(TAG, "MainLoopQueue not created.");
	}
}

unsigned long IRAM_ATTR millis()
{
	return (unsigned long) (esp_timer_get_time() / 1000ULL);
}

#define temp_period 10000

void io_task(void *arg)
{
	ESP_LOGI(TAG, "GPIO config");

	// Configure GPIO
	gpio_pad_select_gpio(BOARD_INT_GPIO);
	gpio_pad_select_gpio(KEYBOARD_INT_GPIO);

	/* Set the GPIO as a inutput */
	gpio_set_direction(BOARD_INT_GPIO, GPIO_MODE_INPUT);
	gpio_set_direction(KEYBOARD_INT_GPIO, GPIO_MODE_INPUT);

	// Create queue
	xIOLoopQueue = xQueueCreate(16, sizeof(struct m_event));
	if (xIOLoopQueue == NULL) ESP_LOGE(TAG, "Failed to create IOLoopQueue.");

	struct m_event io_event;

	i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
	i2c_master_driver_initialize();

	uint8_t board_in, board_in_old;
	if (caller_config.invert_panic_button)
	{
		board_in = 0xEF;
		board_in_old = 0xEF;
	} else {
		board_in = 0xFF;
		board_in_old = 0xFF;
	}

	uint8_t board_out;
	board_out = 0xFF;

	i2c_write(BOARD_INPUT_ADDR, board_out);
	i2c_write(BOARD_INPUT_ADDR, board_out);

	uint8_t keyboard_in, keyboard_in_old;
	keyboard_in = 0x00;
	keyboard_in_old = 0x00;

	uint8_t keyboard_out;
	keyboard_out = 0xFF;

	i2c_write(KEYBOARD_INPUT_ADDR, keyboard_out);
	i2c_write(KEYBOARD_INPUT_ADDR, keyboard_out);

	// Pull-down detect
	keyboard_in = i2c_read(KEYBOARD_INPUT_ADDR);
	vTaskDelay(1 / portTICK_PERIOD_MS);
	keyboard_in |= i2c_read(KEYBOARD_INPUT_ADDR);
	vTaskDelay(1 / portTICK_PERIOD_MS);
	keyboard_in |= i2c_read(KEYBOARD_INPUT_ADDR);

	ESP_LOGI(TAG, "keyboard_in 0x%02X", keyboard_in);

	if (keyboard_in == 0xF0) {
		ESP_LOGI(TAG, "Pull-down resistors detected");
	} else {
		ESP_LOGE(TAG, "Pull-down resistors not detected");

		keyboard_out = 0xF0;

		i2c_write(KEYBOARD_INPUT_ADDR, keyboard_out);
		i2c_write(KEYBOARD_INPUT_ADDR, keyboard_out);
	}

	i2c_read_temp();
	ESP_LOGI(TAG, "Temperature %.1f", temp);
	unsigned long last_temp_update = millis();

	while(1)
	{
		if (millis() - last_temp_update > temp_period)
		{
			last_temp_update = millis();
			i2c_read_temp();
		}

		if (!gpio_get_level(BOARD_INT_GPIO))
		{
			board_in = i2c_read(BOARD_INPUT_ADDR);
			vTaskDelay(5 / portTICK_PERIOD_MS);
			board_in |= i2c_read(BOARD_INPUT_ADDR);
			vTaskDelay(5 / portTICK_PERIOD_MS);
			board_in |= i2c_read(BOARD_INPUT_ADDR);

			board_in |= ~BOARD_INPUT_MASK;

			if (~board_in & board_in_old)
			{
				uint8_t change;
				change = ~board_in & board_in_old;

				if (change & 0b00000001) notify_key(BD1_KEY, KEY_PRESSED);
				if (change & 0b00000010) notify_key(CL1_KEY, KEY_PRESSED);
				if (change & 0b00000100) notify_key(BD2_KEY, KEY_PRESSED);
				if (change & 0b00001000) notify_key(CL2_KEY, KEY_PRESSED);
				if (change & 0b00010000){
					if (caller_config.invert_panic_button){
						notify_key(PAN_KEY, KEY_RELEASED);
					} else {
						notify_key(PAN_KEY, KEY_PRESSED);
					}
				}
				if (change & 0b00100000) notify_key(BAT_KEY, KEY_PRESSED);
			}

			if (board_in & ~board_in_old)
			{
				uint8_t change;
				change = board_in & ~board_in_old;

				if (change & 0b00000001) notify_key(BD1_KEY, KEY_RELEASED);
				if (change & 0b00000010) notify_key(CL1_KEY, KEY_RELEASED);
				if (change & 0b00000100) notify_key(BD2_KEY, KEY_RELEASED);
				if (change & 0b00001000) notify_key(CL2_KEY, KEY_RELEASED);
				if (change & 0b00010000){
					if (caller_config.invert_panic_button){
						notify_key(PAN_KEY, KEY_PRESSED);
					} else {
						notify_key(PAN_KEY, KEY_RELEASED);
					}
				}
				if (change & 0b00100000) notify_key(BAT_KEY, KEY_RELEASED);
			}

			board_in_old = board_in;
		}

		if (!gpio_get_level(KEYBOARD_INT_GPIO))
		{
			// Read KEYS
			keyboard_in = i2c_read(KEYBOARD_INPUT_ADDR);
			vTaskDelay(5 / portTICK_PERIOD_MS);
			keyboard_in &= i2c_read(KEYBOARD_INPUT_ADDR);
			vTaskDelay(5 / portTICK_PERIOD_MS);
			keyboard_in &= i2c_read(KEYBOARD_INPUT_ADDR);

			keyboard_in &= ~KEYBOARD_INPUT_MASK;

			if (keyboard_in & ~keyboard_in_old)
			{
				uint8_t change;
				change = keyboard_in & ~keyboard_in_old;

				if (change & 0b00000001) notify_key(RESOLVE_KEY, KEY_PRESSED);
				if (change & 0b00000010) notify_key(GRAY_KEY, KEY_PRESSED);
				if (change & 0b00000100) notify_key(NURSE_KEY, KEY_PRESSED);
				if (change & 0b00001000) notify_key(BLACK_KEY, KEY_PRESSED);
			}

			if (~keyboard_in & keyboard_in_old)
			{
				uint8_t change;
				change = ~keyboard_in & keyboard_in_old;

				if (change & 0b00000001) notify_key(RESOLVE_KEY, KEY_RELEASED);
				if (change & 0b00000010) notify_key(GRAY_KEY, KEY_RELEASED);
				if (change & 0b00000100) notify_key(NURSE_KEY, KEY_RELEASED);
				if (change & 0b00001000) notify_key(BLACK_KEY, KEY_RELEASED);
			}

			keyboard_in_old = keyboard_in;
		}

		if (xQueueReceive(xIOLoopQueue, &io_event, 5))
		{
			if (io_event.data == DINTEL_RED)
			{
				if (io_event.type == LED_TURN_ON) board_out = board_out & 0b10111111;
				if (io_event.type == LED_TURN_OFF) board_out = board_out | 0b01000000;
				i2c_write(BOARD_INPUT_ADDR, board_out);
			}

			if (io_event.data == DINTEL_GREEN)
			{
				if (io_event.type == LED_TURN_ON) board_out = board_out & 0b01111111;
				if (io_event.type == LED_TURN_OFF) board_out = board_out | 0b10000000;
				i2c_write(BOARD_INPUT_ADDR, board_out);
			}

			if (io_event.data == ON_LED)
			{
				if (io_event.type == LED_TURN_ON) keyboard_out = keyboard_out & 0b11101111;
				if (io_event.type == LED_TURN_OFF) keyboard_out = keyboard_out | 0b00010000;
				i2c_write(KEYBOARD_INPUT_ADDR, keyboard_out);
			}

			if (io_event.data == C1_LED)
			{
				if (io_event.type == LED_TURN_ON) keyboard_out = keyboard_out & 0b11011111;
				if (io_event.type == LED_TURN_OFF) keyboard_out = keyboard_out | 0b00100000;
				i2c_write(KEYBOARD_INPUT_ADDR, keyboard_out);
			}

			if (io_event.data == C2_LED)
			{
				if (io_event.type == LED_TURN_ON) keyboard_out = keyboard_out & 0b10111111;
				if (io_event.type == LED_TURN_OFF) keyboard_out = keyboard_out | 0b01000000;
				i2c_write(KEYBOARD_INPUT_ADDR, keyboard_out);
			}

			if (io_event.data == B_LED)
			{
				if (io_event.type == LED_TURN_ON) keyboard_out = keyboard_out & 0b01111111;
				if (io_event.type == LED_TURN_OFF) keyboard_out = keyboard_out | 0b10000000;
				i2c_write(KEYBOARD_INPUT_ADDR, keyboard_out);
			}
		}

	}
}

static void led_set_level(uint8_t led, bool state)
{
	struct m_event io_event;

	io_event.data = led;

	if (state)
	{
		io_event.type = LED_TURN_ON;
	} else {
		io_event.type = LED_TURN_OFF;
	}

	if (xIOLoopQueue != NULL)
	{
		if (xQueueSend( xIOLoopQueue, &io_event, 0 ) != pdPASS)
		{
			ESP_LOGE(TAG, "Failed to post the message on IOLoopQueue.");
		}
	} else {
		ESP_LOGE(TAG, "IOLoopQueue not created.");
	}
}

#define config_timeout  4000 	  // ms

#define blink_period      1000 	// ms
#define blink_period_fast 200	  // ms

typedef enum{
	ON,             // Encendido
	BLINK,          // Parpadeo con periodo blink_period ms
	BLINK_FAST,     // Parpadeo con periodo blink_period_fast ms
	OFF             // Apagado
} led_mode_t;

led_mode_t ON_LED_mode;
led_mode_t C1_LED_mode;
led_mode_t C2_LED_mode;
led_mode_t B_LED_mode;

led_mode_t DINTEL_RED_mode;
led_mode_t DINTEL_GREEN_mode;

unsigned long last_blink_update;
unsigned long last_blink_fast_update;

bool blink_status = true;
bool blink_fast_status = true;

void led_process(unsigned long current_time)
{
	if (current_time - last_blink_update > blink_period)
	{
		last_blink_update = current_time;
		blink_status = !blink_status;

		if (ON_LED_mode == BLINK) led_set_level(ON_LED, blink_status);
		if (C1_LED_mode == BLINK) led_set_level(C1_LED, blink_status);
		if (C2_LED_mode == BLINK) led_set_level(C2_LED, blink_status);
		if (B_LED_mode == BLINK) led_set_level(B_LED, blink_status);
		if (DINTEL_RED_mode == BLINK) led_set_level(DINTEL_RED, blink_status);
		if (DINTEL_GREEN_mode == BLINK) led_set_level(DINTEL_GREEN, blink_status);
	}

	if (current_time - last_blink_fast_update > blink_period_fast)
	{
		last_blink_fast_update = current_time;
		blink_fast_status = !blink_fast_status;

		if (ON_LED_mode == BLINK_FAST) led_set_level(ON_LED, blink_fast_status);
		if (C1_LED_mode == BLINK_FAST) led_set_level(C1_LED, blink_fast_status);
		if (C2_LED_mode == BLINK_FAST) led_set_level(C2_LED, blink_fast_status);
		if (B_LED_mode == BLINK_FAST) led_set_level(B_LED, blink_fast_status);
		if (B_LED_mode == BLINK_FAST) led_set_level(B_LED, blink_fast_status);
		if (DINTEL_RED_mode == BLINK_FAST) led_set_level(DINTEL_RED, blink_fast_status);
		if (DINTEL_GREEN_mode == BLINK_FAST) led_set_level(DINTEL_GREEN, blink_fast_status);
	}

}

void ON_LED_set_mode(led_mode_t mode)
{
	ON_LED_mode = mode;
	if (mode == ON) led_set_level(ON_LED, true);
	if (mode == OFF) led_set_level(ON_LED, false);
}

void C1_LED_set_mode(led_mode_t mode)
{
	C1_LED_mode = mode;
	if (mode == ON) led_set_level(C1_LED, true);
	if (mode == OFF) led_set_level(C1_LED, false);
}

void C2_LED_set_mode(led_mode_t mode)
{
	C2_LED_mode = mode;
	if (mode == ON) led_set_level(C2_LED, true);
	if (mode == OFF) led_set_level(C2_LED, false);
}

void B_LED_set_mode(led_mode_t mode)
{
	B_LED_mode = mode;
	if (mode == ON) led_set_level(B_LED, true);
	if (mode == OFF) led_set_level(B_LED, false);
}

void DINTEL_RED_set_mode(led_mode_t mode)
{
	DINTEL_RED_mode = mode;
	if (mode == ON) led_set_level(DINTEL_RED, true);
	if (mode == OFF) led_set_level(DINTEL_RED, false);
	if (mode == BLINK)
	{
		last_blink_update = millis();
		blink_status = true;
		led_set_level(DINTEL_RED, blink_status);
	}
}

void DINTEL_GREEN_set_mode(led_mode_t mode){
	DINTEL_GREEN_mode = mode;
	if (mode == ON) led_set_level(DINTEL_GREEN, true);
	if (mode == OFF) led_set_level(DINTEL_GREEN, false);
}

extern sip_handle_t sip;

void main_loop_task(void *arg)
{
	xMainLoopQueue = xQueueCreate(16, sizeof(struct m_event));
	if (xMainLoopQueue == NULL) ESP_LOGE(TAG, "Failed to create MainLoopQueue.");

	struct m_event event;

	sip_state_t sip_state, sip_state_old;
	sip_state_old = SIP_STATE_NONE;
	sip_state = SIP_STATE_NONE;

	bool bed1_activated = false;
	bool bed2_activated = false;
	bool bath_activated = false;
	bool priority_activated = false;
	bool enfermera_present = false;

	if (caller_config.sip_enable)
	{
		ON_LED_set_mode(BLINK_FAST);
	} else {
		ON_LED_set_mode(ON);
	}

	C1_LED_mode = OFF;
	C2_LED_mode = OFF;
	B_LED_mode = OFF;
	DINTEL_RED_mode = OFF;
	DINTEL_GREEN_mode = OFF;

	unsigned long config_timer_start = millis();
	bool config_timer = false;
	bool wifi_ap_on = false;

	while(1)
	{
		led_process(millis());

		if (config_timer && !wifi_ap_on)
		{
			unsigned long elapsed_time;
			elapsed_time = millis() - config_timer_start;

			if (elapsed_time > (4 * config_timeout / 4))
			{
				C1_LED_set_mode(BLINK_FAST);
				C2_LED_set_mode(BLINK_FAST);
				B_LED_set_mode(BLINK_FAST);

				ESP_LOGI(TAG, "Start AP");
				ESP_ERROR_CHECK(esp_wifi_start());

				wifi_ap_on = true;
			} else if (elapsed_time > (3 * config_timeout / 4)){
				B_LED_set_mode(ON);
			} else if (elapsed_time > (2 * config_timeout / 4)){
				C2_LED_set_mode(ON);
			} else if (elapsed_time > (1 * config_timeout / 4)){
				C1_LED_set_mode(ON);
			}

			vTaskDelay(100 / portTICK_PERIOD_MS);
		}

		sip_state = esp_sip_get_state(sip);

		if (xMainLoopQueue != NULL)
		{
			if (xQueueReceive( xMainLoopQueue, &event, 10 / portTICK_PERIOD_MS))
			{
				if (event.type == KEY_PRESSED)
				{
					ESP_LOGI(TAG, "KEY_PRESSED %d", event.data);

					if (event.data == GRAY_KEY && !bed1_activated && !bed2_activated && !bath_activated && !priority_activated && !enfermera_present)
					{
						if (config_timer)
						{
							C1_LED_set_mode(OFF);
							C2_LED_set_mode(OFF);
							B_LED_set_mode(OFF);
							config_timer = false;

							ESP_LOGI(TAG, "Stop AP");
							ESP_ERROR_CHECK(esp_wifi_stop());

							wifi_ap_on = false;
						} else {
							config_timer_start = millis();
							config_timer = true;
						}
					}

					if (!config_timer)
					{
						if (event.data == PAN_KEY && !priority_activated)
						{
							ESP_LOGI(TAG, "PRIORITY");

							priority_activated = true;

							if (sip_state & SIP_STATE_REGISTERED) esp_sip_uac_invite(sip, caller_config.sip_call);

							http_post(PRIORITY);

							if (!enfermera_present)
							{
								B_LED_set_mode(BLINK);
								DINTEL_RED_set_mode(BLINK);
							} else {
								B_LED_set_mode(BLINK_FAST);
							}
						}

						if (event.data == BAT_KEY && !bath_activated)
						{
							ESP_LOGI(TAG, "BATH");

							bath_activated = true;

							if (sip_state & SIP_STATE_REGISTERED) esp_sip_uac_invite(sip, caller_config.sip_call);

							http_post(BATH);

							if (!enfermera_present && !priority_activated)
							{
								B_LED_set_mode(ON);
								DINTEL_RED_set_mode(ON);
							} else {
								if (!priority_activated) B_LED_set_mode(BLINK_FAST);
							}
						}

						if (event.data == BD1_KEY && !bed1_activated)
						{
							ESP_LOGI(TAG, "BED1");

							bed1_activated = true;

							if (sip_state & SIP_STATE_REGISTERED) esp_sip_uac_invite(sip, caller_config.sip_call);

							http_post(BED1);

							if (!enfermera_present)
							{
								C1_LED_set_mode(ON);
								DINTEL_RED_set_mode(ON);
							} else {
								C1_LED_set_mode(BLINK_FAST);
							}
						}

						if (event.data == BD2_KEY && !bed2_activated)
						{
							ESP_LOGI(TAG, "BED2");

							bed2_activated = true;

							if (sip_state & SIP_STATE_REGISTERED) esp_sip_uac_invite(sip, caller_config.sip_call);

							http_post(BED2);

							if (!enfermera_present)
							{
								C2_LED_set_mode(ON);
								DINTEL_RED_set_mode(ON);
							} else {
								C2_LED_set_mode(BLINK_FAST);
							}
						}

						if (event.data == CL1_KEY || event.data == CL2_KEY || event.data == BLACK_KEY)
						{
							ESP_LOGI(TAG, "CALL");

							if (sip_state & SIP_STATE_REGISTERED)
							{
								ESP_LOGI(TAG, "SIP Invite");
								esp_sip_uac_invite(sip, caller_config.sip_call);
							}

							if (sip_state & SIP_STATE_ON_CALL)
							{
								ESP_LOGI(TAG, "SIP Bye");
								esp_sip_uac_bye(sip);
							}

							if ((sip_state & SIP_STATE_CALLING) || (sip_state & SIP_STATE_SESS_PROGRESS))
							{
								ESP_LOGI(TAG, "SIP Cancel");
								esp_sip_uac_cancel(sip);
							}
						}

						if (event.data == NURSE_KEY && !enfermera_present)
						{
							if (bed1_activated || bed2_activated || bath_activated || priority_activated)
							{
								ESP_LOGI(TAG, "SERVE");

								enfermera_present = true;

								if (bed1_activated) C1_LED_set_mode(BLINK_FAST);
								if (bed2_activated) C2_LED_set_mode(BLINK_FAST);
								if (bath_activated || priority_activated) B_LED_set_mode(BLINK_FAST);

								http_post(SERVE);

								DINTEL_RED_set_mode(OFF);
								DINTEL_GREEN_set_mode(ON);
							}
						}

						if (event.data == RESOLVE_KEY && enfermera_present)
						{
							ESP_LOGI(TAG, "RESOLVE");

							bed1_activated = false;
							bed2_activated = false;
							bath_activated = false;
							priority_activated = false;
							enfermera_present = false;

							C1_LED_set_mode(OFF);
							C2_LED_set_mode(OFF);
							B_LED_set_mode(OFF);

							http_post(RESOLVE);

							DINTEL_RED_set_mode(OFF);
							DINTEL_GREEN_set_mode(OFF);
						}
					}
				}

				if (event.type == KEY_RELEASED)
				{
					ESP_LOGI(TAG, "KEY_RELEASED %d", event.data);

					if (event.data == GRAY_KEY)
					{
						if (config_timer && (millis() - config_timer_start) < config_timeout)
						{
							C1_LED_set_mode(OFF);
							C2_LED_set_mode(OFF);
							B_LED_set_mode(OFF);
							config_timer = false;
						}
					}
				}
			}
		} else {
			ESP_LOGE(TAG, "MainLoopQueue not created.");
		}

		if (caller_config.sip_enable)
		{
			if (sip_state != sip_state_old)
			{
				if (sip_state < SIP_STATE_REGISTERED)
				{
					ON_LED_set_mode(BLINK_FAST);
				} else {
					if (sip_state > SIP_STATE_REGISTERED)
					{
						ON_LED_set_mode(BLINK);
					} else {
						ON_LED_set_mode(ON);
					}
				}

				if (sip_state & SIP_STATE_RINGING)
				{
					ESP_LOGI(TAG, "SIP Automatic answer");
					esp_sip_uas_answer(sip, true);
				}
			}
		}

		sip_state_old = sip_state;
	}
}
