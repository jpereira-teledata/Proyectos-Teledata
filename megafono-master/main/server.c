#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_ota_ops.h"

#include "esp_http_server.h"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE  8192

struct file_server_data {
    /* Scratch buffer for temporary storage during file transfer */
    char scratch[SCRATCH_BUFSIZE];
};

static const char *TAG = "SERVER";

static esp_err_t index_html_get_handler(httpd_req_t *req)
{
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[]   asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);
    return ESP_OK;
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[]   asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
    return ESP_OK;
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *resp = ((struct file_server_data *)req->user_ctx)->scratch;

    uint8_t chipid[6];
    esp_efuse_mac_get_default(chipid);

    size_t s;
    s = sprintf(resp, "{\"chip_id\":\"%X\",\"version\":\"%s\"}", (unsigned int)chipid, "v2");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, s);
    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    FILE* fd = fopen("/spiffs/config.txt", "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read file");
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending file...");
    httpd_resp_set_type(req, "application/json");

    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
    size_t chunksize;
    do {
        /* Read file in chunks into the scratch buffer */
        chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);

        /* Send the buffer contents as HTTP response chunk */
        if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
            fclose(fd);
            ESP_LOGE(TAG, "File sending failed!");
            /* Abort sending file */
            httpd_resp_sendstr_chunk(req, NULL);
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
            return ESP_FAIL;
        }

        /* Keep looping till the whole file is sent */
    } while (chunksize != 0);

    /* Close file after sending complete */
    fclose(fd);
    ESP_LOGI(TAG, "File sending complete");

    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char* filepath = "/spiffs/config.txt";
    FILE *fd = NULL;
    struct stat file_stat;

    if (stat(filepath, &file_stat) == 0) {
        ESP_LOGE(TAG, "File %s already exists, deleting...", filepath);
		unlink(filepath);
    }

    fd = fopen(filepath, "w");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to create file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Receiving file...");

    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *buff = ((struct file_server_data *)req->user_ctx)->scratch;
    int received;

    /* Content length of the request gives
     * the size of the file being uploaded */
    int remaining = req->content_len;

    while (remaining > 0) {

        ESP_LOGI(TAG, "Remaining size : %d", remaining);
        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, buff, MIN(remaining, SCRATCH_BUFSIZE))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }

            /* In case of unrecoverable error,
             * close and delete the unfinished file*/
            fclose(fd);
            unlink(filepath);

            ESP_LOGE(TAG, "File reception failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            return ESP_FAIL;
        }

        /* Write buffer content to file on storage */
        if (received && (received != fwrite(buff, 1, received, fd))) {
            /* Couldn't write everything to file!
             * Storage may be full? */
            fclose(fd);
            unlink(filepath);

            ESP_LOGE(TAG, "File write failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to storage");
            return ESP_FAIL;
        }

        /* Keep track of remaining size of
         * the file left to be uploaded */
        remaining -= received;
    }

    /* Close file upon upload completion */
    fclose(fd);
    ESP_LOGI(TAG, "File reception complete");

    httpd_resp_sendstr(req, "Configuration saved successfully");
    return ESP_OK;
}

/* Receive .bin file */
esp_err_t ota_update_post_handler(httpd_req_t *req)
{
	esp_ota_handle_t ota_handle;

	/* Retrieve the pointer to scratch buffer for temporary storage */
    char *ota_buff = ((struct file_server_data *)req->user_ctx)->scratch;
	int remaining = req->content_len;
	int received;
	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

	ESP_LOGI("OTA", "File Size: %dkB", remaining / 1024);

	esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE("OTA", "Error With OTA Begin, Cancelling OTA");
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to OTA Begin, Cancelling OTA");
		return ESP_FAIL;
	} else {
		ESP_LOGI("OTA", "Writing to partition %s subtype 0x%02X at offset 0x%X", update_partition->label, update_partition->subtype, update_partition->address);
	}

    while (remaining > 0) {

        ESP_LOGI(TAG, "Remaining size: %dkB", remaining / 1024);

        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, ota_buff, MIN(remaining, SCRATCH_BUFSIZE))) < 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
				        ESP_LOGE("OTA", "Socket Timeout remaining %d", remaining);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA Socket Timeout");
                return ESP_FAIL;
                //continue;
            }
            ESP_LOGE("OTA", "Other Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA Other Error");
            return ESP_FAIL;
        }

        esp_ota_write(ota_handle, ota_buff, received);

        /* Keep track of remaining size of
         * the file left to be uploaded */
        remaining -= received;
    }

	ESP_LOGI("OTA", "All recieved");

	if (esp_ota_end(ota_handle) == ESP_OK) {
		if(esp_ota_set_boot_partition(update_partition) == ESP_OK) {
			const esp_partition_t *boot_partition = esp_ota_get_boot_partition();
			ESP_LOGI("OTA", "Next boot partition subtype %d at offset 0x%x", boot_partition->subtype, boot_partition->address);
			ESP_LOGI("OTA", "Please Restart System...");
		} else {
			ESP_LOGE("OTA", "Flashed Error");
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA Flashed Error");
			return ESP_FAIL;
		}
	} else {
		ESP_LOGE("OTA", "End Error");
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA End Error");
		return ESP_FAIL;
	}

	httpd_resp_sendstr(req, "OTA File uploaded successfully");
	return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
	ESP_LOGW(TAG, "Reboot!");
	httpd_resp_send_chunk(req, NULL, 0);
	vTaskDelay(2000/portTICK_PERIOD_MS);
	esp_restart();
  return ESP_OK;
}

esp_err_t start_server(void)
{
    static struct file_server_data *server_data = NULL;

    if (server_data) {
        ESP_LOGE(TAG, "Server already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* Allocate memory for server data */
    server_data = calloc(1, sizeof(struct file_server_data));
    if (!server_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for server data");
        return ESP_ERR_NO_MEM;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server");
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file server!");
        return ESP_FAIL;
    }

    httpd_uri_t index = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_html_get_handler,
		    .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &index);

    httpd_uri_t favicon = {
        .uri       = "/favicon.ico",
        .method    = HTTP_GET,
        .handler   = favicon_get_handler,
		    .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &favicon);

    httpd_uri_t info = {
        .uri       = "/info",
        .method    = HTTP_GET,
        .handler   = info_get_handler,
		    .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &info);

    httpd_uri_t cnfg = {
        .uri       = "/conf",
        .method    = HTTP_GET,
        .handler   = config_get_handler,
		    .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &cnfg);

    httpd_uri_t save = {
        .uri       = "/save",
        .method    = HTTP_POST,
        .handler   = save_post_handler,
		    .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &save);

  	httpd_uri_t ota_update = {
  		.uri = "/update",
  		.method = HTTP_POST,
  		.handler = ota_update_post_handler,
  		.user_ctx  = server_data    // Pass server data as context
  	};
  	httpd_register_uri_handler(server, &ota_update);

  	httpd_uri_t reboot = {
  		.uri = "/reboot",
  		.method = HTTP_POST,
  		.handler = reboot_handler,
  		.user_ctx  = server_data    // Pass server data as context
  	};
  	httpd_register_uri_handler(server, &reboot);

    return ESP_OK;
}
