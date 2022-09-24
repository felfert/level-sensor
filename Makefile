#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := level-sensor

include $(IDF_PATH)/make/project.mk

otaupdate: all
	scp build/$(PROJECT_NAME).bin otaserver:/var/www/html/fsun/esp8266_updates/
	espupdate iot001.fe.think
