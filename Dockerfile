FROM espressif/idf:v5.2.3
SHELL ["/bin/bash", "-c"]

ARG WIFI_SSID=YOUR_WIFI_SSID
ARG WIFI_PASS=YOUR_WIFI_PASSWORD

COPY . /project
WORKDIR /project

# Inject WiFi credentials into sdkconfig defaults
RUN python3 -c "\
import sys;\
c = open('sdkconfig.defaults').read();\
c = c.replace('YOUR_WIFI_SSID', sys.argv[1]);\
c = c.replace('YOUR_WIFI_PASSWORD', sys.argv[2]);\
open('sdkconfig.defaults','w').write(c)" \
"$WIFI_SSID" "$WIFI_PASS"

# Build firmware for ESP32-S3
RUN source $IDF_PATH/export.sh && idf.py set-target esp32s3 && idf.py build
