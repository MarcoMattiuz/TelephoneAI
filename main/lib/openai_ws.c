#include "openai_ws.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "sdkconfig.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#ifndef CONFIG_OPENAI_API_KEY
#define CONFIG_OPENAI_API_KEY ""
#endif

#ifndef CONFIG_OPENAI_LIVE_MODEL
#define CONFIG_OPENAI_LIVE_MODEL "gpt-live-1"
#endif

#ifndef CONFIG_OPENAI_BACKEND_MODEL
#define CONFIG_OPENAI_BACKEND_MODEL "gpt-6-luna"
#endif

#ifndef CONFIG_OPENAI_VOICE
#define CONFIG_OPENAI_VOICE "marin"
#endif

#define GPT_LIVE_WS_URL "wss://api.openai.com/v1/live/sessions"
#define GPT_LIVE_MODEL CONFIG_OPENAI_LIVE_MODEL
#define GPT_LIVE_BACKEND_MODEL CONFIG_OPENAI_BACKEND_MODEL
#define GPT_LIVE_VOICE CONFIG_OPENAI_VOICE
#define GPT_LIVE_API_KEY CONFIG_OPENAI_API_KEY

static const char *TAG = "openai_ws";

static esp_websocket_client_handle_t s_websocket_client;
static bool s_live_session_started;
static bool s_text_prompt_sent;
static char s_ws_rx_buffer[8192];
static size_t s_ws_rx_len;
static char s_ws_tx_buffer[1024];

static void websocket_send_text(esp_websocket_client_handle_t client, const char *payload, const char *label)
{
    if (client == NULL) {
        ESP_LOGE(TAG, "Cannot send %s: websocket client is NULL", label);
        return;
    }

    int sent = esp_websocket_client_send_text(client, payload, strlen(payload), portMAX_DELAY);
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send %s", label);
        return;
    }

    ESP_LOGI(TAG, "Sent %s (%d bytes)", label, sent);
}

static void websocket_send_session_start(esp_websocket_client_handle_t client)
{
    int len = snprintf(
        s_ws_tx_buffer,
        sizeof(s_ws_tx_buffer),
        "{" \
        "\"type\":\"session.start\"," \
        "\"event_id\":\"esp32_session_start\"," \
        "\"session\":{" \
            "\"model\":\"%s\"," \
            "\"instructions\":\"ESP32 prototype for GPT-Live over WebSocket. Keep replies brief and confirm connectivity.\"," \
            "\"audio\":{" \
                "\"format\":{\"type\":\"audio/pcm\",\"rate\":24000}," \
                "\"output\":{\"voice\":\"%s\"}" \
            "}," \
            "\"delegation\":{" \
                "\"type\":\"responses\"," \
                "\"responses\":{" \
                    "\"model\":\"%s\"," \
                    "\"instructions\":\"Return concise, verified results for delegated backend work.\"," \
                    "\"tools\":[]," \
                    "\"tool_choice\":\"none\"," \
                    "\"parallel_tool_calls\":false" \
                "}" \
            "}" \
        "}" \
        "}",
        GPT_LIVE_MODEL,
        GPT_LIVE_VOICE,
        GPT_LIVE_BACKEND_MODEL);

    if (len < 0 || len >= (int)sizeof(s_ws_tx_buffer)) {
        ESP_LOGE(TAG, "Failed to build session.start payload");
        return;
    }

    websocket_send_text(client, s_ws_tx_buffer, "session.start");
}

static void websocket_send_text_prompt(esp_websocket_client_handle_t client)
{
    int len = snprintf(
        s_ws_tx_buffer,
        sizeof(s_ws_tx_buffer),
        "{" \
        "\"type\":\"response.item.create\"," \
        "\"event_id\":\"esp32_text_prompt_item\"," \
        "\"item\":{" \
            "\"type\":\"message\"," \
            "\"role\":\"user\"," \
            "\"content\":[{" \
                "\"type\":\"input_text\"," \
                "\"text\":\"scegli una parola con cui rispondere\"" \
            "}]" \
        "}" \
        "}");

    if (len < 0 || len >= (int)sizeof(s_ws_tx_buffer)) {
        ESP_LOGE(TAG, "Failed to build response.item.create payload");
        return;
    }

    websocket_send_text(client, s_ws_tx_buffer, "response.item.create");

    websocket_send_text(
        client,
        "{\"type\":\"response.create\",\"event_id\":\"esp32_text_prompt_run\"}",
        "response.create");
}

static bool websocket_extract_quoted_value(const char *json_text, const char *needle, char *value, size_t value_size)
{
    const char *start = strstr(json_text, needle);
    if (start == NULL) {
        return false;
    }

    start += strlen(needle);
    const char *end = strchr(start, '"');
    if (end == NULL || end <= start) {
        return false;
    }

    size_t copy_len = (size_t)(end - start);
    if (copy_len >= value_size) {
        copy_len = value_size - 1;
    }

    memcpy(value, start, copy_len);
    value[copy_len] = '\0';
    return true;
}

static bool websocket_extract_nested_event_type(const char *json_text, char *value, size_t value_size)
{
    const char *event_start = strstr(json_text, "\"event\":{");
    if (event_start == NULL) {
        return false;
    }

    const char *type_start = strstr(event_start, "\"type\":\"");
    if (type_start == NULL) {
        return false;
    }

    return websocket_extract_quoted_value(type_start, "\"type\":\"", value, value_size);
}

static void websocket_handle_text_message(const char *json_text)
{
    char type_text[64];
    if (!websocket_extract_quoted_value(json_text, "\"type\":\"", type_text, sizeof(type_text))) {
        ESP_LOGW(TAG, "Received JSON message without type: %s", json_text);
        return;
    }

    ESP_LOGI(TAG, "OpenAI event: %s", type_text);

    if (strcmp(type_text, "session.created") == 0 || strcmp(type_text, "session.started") == 0 || strcmp(type_text, "session.updated") == 0) {
        if (strcmp(type_text, "session.started") == 0 && !s_live_session_started) {
            s_live_session_started = true;
            ESP_LOGI(TAG, "Live session started");

            if (!s_text_prompt_sent) {
                s_text_prompt_sent = true;
                websocket_send_text_prompt(s_websocket_client);
            }
        }
    } else if (strcmp(type_text, "error") == 0) {
        char message[256];
        char client_event_id[96];

        if (websocket_extract_quoted_value(json_text, "\"message\":\"", message, sizeof(message))) {
            ESP_LOGE(TAG, "OpenAI error: %s", message);
        }

        if (websocket_extract_quoted_value(json_text, "\"client_event_id\":\"", client_event_id, sizeof(client_event_id))) {
            ESP_LOGE(TAG, "Error client_event_id: %s", client_event_id);
        }
    } else if (strcmp(type_text, "response.event") == 0) {
        char nested_type[96];

        if (websocket_extract_nested_event_type(json_text, nested_type, sizeof(nested_type))) {
            ESP_LOGI(TAG, "Delegated response event: %s", nested_type);

            if (strcmp(nested_type, "response.output_text.delta") == 0) {
                char delta[256];
                if (websocket_extract_quoted_value(json_text, "\"delta\":\"", delta, sizeof(delta))) {
                    ESP_LOGI(TAG, "Text delta: %s", delta);
                }
            } else if (strcmp(nested_type, "response.output_text.done") == 0) {
                char text[256];
                if (websocket_extract_quoted_value(json_text, "\"text\":\"", text, sizeof(text))) {
                    ESP_LOGI(TAG, "Text done: %s", text);
                }
            }
        }
    } else if (strcmp(type_text, "session.commentary.appended") == 0 ||
               strcmp(type_text, "session.instructions.appended") == 0 ||
               strcmp(type_text, "session.thinking.appended") == 0) {
        char client_event_id[96];

        if (websocket_extract_quoted_value(json_text, "\"client_event_id\":\"", client_event_id, sizeof(client_event_id))) {
            ESP_LOGI(TAG, "Ack for %s", client_event_id);
        }
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WSS connected");
        websocket_send_session_start(s_websocket_client);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WSS disconnected");
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(TAG, "WSS data received, opcode=0x%02X len=%d", data->op_code, data->data_len);
            
        /* Ignore keepalive/control frames with no payload. */
        if (data->data_len == 0 && data->payload_offset == 0 && data->fin) {
            ESP_LOGD(TAG, "Ignoring empty websocket frame");
            break;
        }

        if (data->payload_offset == 0) {
            s_ws_rx_len = 0;
        }

        if (s_ws_rx_len + (size_t)data->data_len < sizeof(s_ws_rx_buffer)) {
            memcpy(&s_ws_rx_buffer[s_ws_rx_len], data->data_ptr, data->data_len);
            s_ws_rx_len += (size_t)data->data_len;
        } else {
            ESP_LOGW(TAG, "Dropping oversized websocket message");
            s_ws_rx_len = 0;
            break;
        }

        if (data->fin) {
            s_ws_rx_buffer[s_ws_rx_len] = '\0';
            ESP_LOGI(TAG, "Data: %s", s_ws_rx_buffer);
            if (s_ws_rx_len > 0) {
                websocket_handle_text_message(s_ws_rx_buffer);
            }
            s_ws_rx_len = 0;
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WSS error: tls=%d stack=%d cert_flags=%d ws=%d",
                 data->error_handle.esp_tls_last_esp_err,
                 data->error_handle.esp_tls_stack_err,
                 data->error_handle.esp_tls_cert_verify_flags,
                 data->error_handle.error_type);
        break;
    default:
        break;
    }
}

esp_err_t openai_ws_start(void)
{
    if (strlen(GPT_LIVE_API_KEY) == 0) {
        ESP_LOGE(TAG, "OPENAI_API_KEY is empty; configure it in menuconfig");
        return ESP_ERR_INVALID_STATE;
    }

    char headers[512];
    int header_len = snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", GPT_LIVE_API_KEY);
    if (header_len < 0 || header_len >= (int)sizeof(headers)) {
        ESP_LOGE(TAG, "Failed to build websocket authorization header");
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_websocket_client_config_t websocket_cfg = {
        .uri = GPT_LIVE_WS_URL,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
        .headers = headers,
        .disable_auto_reconnect = false,
        .task_stack = 8192,
    };

    s_websocket_client = esp_websocket_client_init(&websocket_cfg);
    if (s_websocket_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_websocket_register_events(
        s_websocket_client,
        WEBSOCKET_EVENT_ANY,
        websocket_event_handler,
        NULL));

    ESP_ERROR_CHECK(esp_websocket_client_start(s_websocket_client));
    return ESP_OK;
}
