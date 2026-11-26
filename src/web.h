// The bridge's own web page on port 80: status, bus log, settings, firmware upload.
#pragma once

#include "bwa_hub.h"
#include "bwa_net.h"
#include "settings.h"

void web_start(Settings& settings, bwa::Hub& hub, bwa::NetServer& net);
void web_loop();
