#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Carica sessione persistita (sopravvive al respawn watchdog).
void core_auth_init(void);

// Verifica credenziali (user/pass in chiaro dal form)
bool core_auth_check_credentials(const char *user, const char *pass);

// Token sessione (32 byte hex) dopo login valido
bool core_auth_create_session_token(char *out_token, size_t out_len);
bool core_auth_validate_session_token(const char *token);

void core_auth_clear_session(void);

#ifdef __cplusplus
}
#endif
