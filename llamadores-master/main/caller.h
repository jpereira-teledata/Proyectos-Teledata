#ifndef CALLER_H
#define CALLER_H

typedef struct {
	bool sip_enable;
	char sip_call[16];
	bool invert_panic_button;
} caller_config_t;

extern caller_config_t caller_config;

void main_loop_task(void *arg);

void io_task(void *arg);

#endif
