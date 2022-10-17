#ifndef SERVER_H
#define SERVER_H

extern float temp;

extern int tone_volume;
extern int spk_volume;
extern int mic_volume;

esp_err_t start_server(int fw_version);

#endif
