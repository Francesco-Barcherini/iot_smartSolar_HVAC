CONTIKI_PROJECT = hvac-node
all: $(CONTIKI_PROJECT)

CONTIKI = ../..

# Include CoAP resources
MODULES_REL += ./resources

# Include CoAP module
include $(CONTIKI)/Makefile.dir-variables
MODULES += $(CONTIKI_NG_APP_LAYER_DIR)/coap
MODULES += $(CONTIKI_NG_LIB_DIR)/json

# Suppress warning unused-function
CFLAGS += -Wno-unused-function -Wno-unused-variable

include $(CONTIKI)/Makefile.include