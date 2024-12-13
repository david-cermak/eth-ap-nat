# Embed the server root certificate into the final binary
idf_build_get_property(project_dir PROJECT_DIR)
idf_component_register(SRCS "simple_ota_example.c" "wifi_connect.c" "wifi_lazy.c"
                    INCLUDE_DIRS ".")
