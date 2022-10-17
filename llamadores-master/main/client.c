#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_http_client.h"

#include "client.h"

#define MAX_HTTP_RECV_BUFFER 512

static const char *TAG = "HTTP_CLIENT";

QueueHandle_t xHTTPClientQueue;

char path[128];
char post_data[128];

ticket_t ticket;

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
	if (evt->event_id == HTTP_EVENT_ON_DATA)
	{
		if (!esp_http_client_is_chunked_response(evt->client))
		{
			printf("HTTP POST rersponse = %.*s\n", evt->data_len, (char*)evt->data);
		}
	}
	return ESP_OK;
}

void http_post(ticket_t new_ticket)
{
	if(xHTTPClientQueue != NULL)
	{
		if(xQueueSend( xHTTPClientQueue, &new_ticket, 0 ) != pdPASS)
		{
			ESP_LOGE(TAG, "Failed to post the message on xHTTPClientQueue.");
		}
	} else {
		ESP_LOGE(TAG, "xHTTPClientQueue not created.");
	}
}

void client_task(void *arg)
{
	xHTTPClientQueue = xQueueCreate(16, sizeof(ticket_t));
	if (xHTTPClientQueue == NULL) ESP_LOGE(TAG, "Failed to create xHTTPClientQueue.");

	while(1)
	{
		if (xQueueReceive(xHTTPClientQueue, &ticket, 100))
		{
			ESP_LOGI(TAG, "New ticket");

			memset(path, 0, sizeof(path));

			sprintf(path,"/%s/web/webservices/llamadores_ws.php", sc_config.sc_url);

			esp_http_client_config_t config = {
				.host = sc_config.sc_server,
				.path = path,
				.transport_type = HTTP_TRANSPORT_OVER_TCP,
				.event_handler = _http_event_handler,
				.timeout_ms = 20000,
			};
			esp_http_client_handle_t client = esp_http_client_init(&config);

			// POST
			esp_http_client_set_method(client, HTTP_METHOD_POST);

			memset(post_data, 0, sizeof(post_data));

			switch(ticket){
				case BED1:
					sprintf(post_data,"auth_user=%s&auth_pwd=%s&operation=call&button=bed1", sc_config.sc_user, sc_config.sc_pass);
					break;
				case BED2:
					sprintf(post_data,"auth_user=%s&auth_pwd=%s&operation=call&button=bed2", sc_config.sc_user, sc_config.sc_pass);
					break;
				case BATH:
					sprintf(post_data,"auth_user=%s&auth_pwd=%s&operation=call&button=bath", sc_config.sc_user, sc_config.sc_pass);
					break;
				case PRIORITY:
					sprintf(post_data,"auth_user=%s&auth_pwd=%s&operation=call&button=priority", sc_config.sc_user, sc_config.sc_pass);
					break;
				case SERVE:
					sprintf(post_data,"auth_user=%s&auth_pwd=%s&operation=serve", sc_config.sc_user, sc_config.sc_pass);
					break;
				case RESOLVE:
					sprintf(post_data,"auth_user=%s&auth_pwd=%s&operation=resolve", sc_config.sc_user, sc_config.sc_pass);
					break;
				default:
					ESP_LOGE(TAG, "Ticket type error");
					break;
			}

			esp_http_client_set_post_field(client, post_data, strlen(post_data));

			esp_err_t err = esp_http_client_perform(client);

			if (err == ESP_OK)
			{
				ESP_LOGI(TAG, "HTTP POST status = %d", esp_http_client_get_status_code(client));
			} else {
				ESP_LOGE(TAG, "HTTP POST request failed = %s", esp_err_to_name(err));
			}

			esp_http_client_cleanup(client);
		}
	}
}
