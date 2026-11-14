/* Spielkarte (ROM-Bus), siehe card.c */
#pragma once
#include <stdint.h>

int card_load_rom(const char *path);
const uint8_t *card_rom(void);
uint32_t card_rom_size(void);
uint32_t card_chip_id(void);

void card_write_cmd(uint32_t off, uint32_t v, int size);   /* 0x040001a8..af */
uint32_t card_write_romctrl(uint32_t v);                    /* 0x040001a4 */
uint32_t card_read_romctrl(void);
uint32_t card_read_data(void);                              /* 0x04100010 */
int card_transfer_active(void);
