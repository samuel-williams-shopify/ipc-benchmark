#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Opaque type for interrupt handle */
typedef struct Interrupt Interrupt;

/* Create a new interrupt handle */
Interrupt* interrupt_create(void);

/* Destroy an interrupt handle */
void interrupt_destroy(Interrupt* intr);

/* Get the file descriptor for select/poll */
int interrupt_get_fd(const Interrupt* intr);

/* Signal the interrupt */
int interrupt_signal(Interrupt* intr);

/* Clear the interrupt (read any pending signals) */
int interrupt_clear(Interrupt* intr);

/* Check if interrupt is set */
bool interrupt_check(Interrupt* intr); 