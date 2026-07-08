idf_component_register(SRCS "barcode_scanner.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos
                       PRIV_REQUIRES usb usb_host_hid)
