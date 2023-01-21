#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := level-sensor

CFLAGS += -DLWIP_DHCP_GET_NTP_SRV=1
CXXFLAGS += -DLWIP_DHCP_GET_NTP_SRV=1
include $(IDF_PATH)/make/project.mk

$(eval $(subst https://,,$(shell grep CONFIG_OTA_URI sdkconfig)))
UFILE := $(notdir "$(CONFIG_OTA_URI)")
$(eval $(shell openssl x509 -noout -subject -in main/client.crt -nameopt sep_multiline | grep CN=))

otasave: all
	scp build/$(PROJECT_NAME).bin otaserver:/var/www/html/fsun/esp8266_updates/$(UFILE)

otaupdate: otasave
	espupdate $(CN)
