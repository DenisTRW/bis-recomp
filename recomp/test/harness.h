/* Minimale Laufzeit für Tests: flacher Speicher + Dummy-Hooks. */
#pragma once
#include "recomp.h"

#define HARNESS_STACK_TOP 0x023f0000u

extern unsigned harness_todo_count;
int harness_load_arm9(const char *path);
