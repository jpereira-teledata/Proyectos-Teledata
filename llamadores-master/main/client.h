#ifndef CLIENT_H
#define CLIENT_H

typedef struct {
	char sc_server[32];
	char sc_url[32];
	char sc_user[16];
	char sc_pass[16];
} sc_config_t;

extern sc_config_t sc_config;

typedef enum{
	BED1,       	// Ticket con operation=call y button=bed1
	BED2,       	// Ticket con operation=call y button=bed2
	BATH,       	// Ticket con operation=call y button=bath
	PRIORITY,     // Ticket con operation=call y button=priority
	SERVE,        // Ticket con operation=serve
	RESOLVE,      // Ticket con operation=resolve
} ticket_t;

void http_post(ticket_t new_ticket);

void client_task(void *arg);

#endif
