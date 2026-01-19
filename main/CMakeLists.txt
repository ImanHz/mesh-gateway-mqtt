file(GLOB_RECURSE srcs "main.c" "src/wifi.c" "src/uart.c" "src/mqtt.c" "src/gpio.c")
idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "include/")
