set(srcs
    "app/main.c"
    "app/midi_io.c"
    "app/solenoid.c"
    "app/wifi_softap.c"
    "app/http_file_server.c"
    "app/midi_parser.c"
)

set(include_dirs
    "app"
    "include"
)

set(requires fatfs esp_timer)

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "${include_dirs}"
                       REQUIRES "${requires}"
                       PRIV_REQUIRES esp_http_server esp_wifi esp_netif nvs_flash esp_event
)