#include "pcm_http_server.h"
#include "pcm_storage.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_http_server.h"

static const char *TAG = "pcm_http";

#define SEND_CHUNK_BYTES 4096

static esp_err_t pcm_download_handler(httpd_req_t *req)
{
    FILE *f = fopen(PCM_FILE_PATH, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No capture file found");
        return ESP_FAIL;
    }

    size_t file_size = 0;
    pcm_storage_get_size(&file_size);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"capture.pcm\"");

    char content_length[32];
    snprintf(content_length, sizeof(content_length), "%u", (unsigned)file_size);
    httpd_resp_set_hdr(req, "Content-Length", content_length);

    static char buf[SEND_CHUNK_BYTES];
    size_t bytes_read;
    while ((bytes_read = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)bytes_read) != ESP_OK) {
            ESP_LOGE(TAG, "Send chunk failed, aborting");
            fclose(f);
            return ESP_FAIL;
        }
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "Sent %u bytes", (unsigned)file_size);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    size_t file_size = 0;
    esp_err_t ret = pcm_storage_get_size(&file_size);

    char body[128];
    if (ret == ESP_OK) {
        snprintf(body, sizeof(body),
                 "{\"status\":\"ready\",\"bytes\":%u,\"samples\":%u,\"rate\":16000}",
                 (unsigned)file_size, (unsigned)(file_size / sizeof(int16_t)));
    } else {
        snprintf(body, sizeof(body), "{\"status\":\"no_file\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

esp_err_t pcm_http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t download_uri = {
        .uri = "/capture.pcm",
        .method = HTTP_GET,
        .handler = pcm_download_handler,
    };
    httpd_register_uri_handler(server, &download_uri);

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    httpd_register_uri_handler(server, &status_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return ESP_OK;
}
