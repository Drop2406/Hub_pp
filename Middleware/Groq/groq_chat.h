#ifndef GROQ_CHAT_H
#define GROQ_CHAT_H

#include <stddef.h>
#include "esp_err.h"

esp_err_t groq_chat_init(const char *api_key);

esp_err_t groq_chat_ask(
    const char *user_text,
    char *answer_buffer,
    size_t answer_capacity
);

#endif