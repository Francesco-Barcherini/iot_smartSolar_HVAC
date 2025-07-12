CONTIKI_PROJECT = energy-node
all: $(CONTIKI_PROJECT)

CONTIKI = ../..

# Include emlearn
MODULES_REL += /home/iot_ubuntu_intel/.local/lib/python3.10/site-packages/emlearn
TARGET_LIBFILES += -lm
INC += /home/iot_ubuntu_intel/.local/lib/python3.10/site-packages/emlearn


# Include CoAP resources
MODULES_REL += ./resources

# Include CoAP module
include $(CONTIKI)/Makefile.dir-variables
MODULES += $(CONTIKI_NG_APP_LAYER_DIR)/coap

# Suppress warning unused-function
CFLAGS += -Wno-unused-function -Wno-unused-variable

# # Include JSON
# MODULES += $(CONTIKI_NG_LIB_DIR)/json

include $(CONTIKI)/Makefile.include

