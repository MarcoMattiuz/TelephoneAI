# Embed the server root certificate into the final binary
#
# (If this was a component, we would set COMPONENT_EMBED_TXTFILES here.)
idf_component_register(SRCS "main.c" "time_sync.c" "lib/app_entry.c" "lib/openai_ws.c" "lib/inmp441_i2s.c"
                    INCLUDE_DIRS "include" "lib"
                    PRIV_REQUIRES esp-tls esp_wifi esp_timer nvs_flash esp_websocket_client esp_driver_i2s
                    EMBED_TXTFILES server_root_cert.pem local_server_cert.pem)
