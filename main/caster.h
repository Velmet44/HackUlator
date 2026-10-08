#pragma once

/* PKC dirty-rect caster: streams framebuffer regions over UART0 @ 1 Mbaud
 * (wire-compatible with the pikachu serial viewer) and receives remote
 * key bytes from the host. */

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void caster_start(void);
void caster_stop(void);
int  caster_is_active(void);

/* Stream one RGB565 rect. Rows are read with the given stride (pixels),
 * so the framebuffer can be streamed directly with no copy. */
void caster_send_rect(int x, int y, int w, int h, const uint16_t *pixels,
                      int stride);

/* 1 while a previous frame is still draining the UART wire (callers should
 * skip, not queue: the next flush will carry the fresh pixels). 0 when idle
 * or inactive. Never blocks. */
int caster_tx_busy(void);

/* Queue of raw key bytes received from the host viewer. NULL if inactive. */
QueueHandle_t caster_get_rx_queue(void);
