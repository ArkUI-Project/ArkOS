#ifndef ARK_E1000_H
#define ARK_E1000_H
#include "ark.h"
bool e1000_init(uint8_t mac[6]);
bool e1000_link(void);
bool e1000_send(const uint8_t *frame, size_t length);
size_t e1000_receive(uint8_t *frame, size_t capacity);
#endif
