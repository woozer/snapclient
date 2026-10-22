#pragma once
#define configTICK_RATE_HZ 1000
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)p)
#define portEXIT_CRITICAL(p) ((void)p)
#define portENTER_CRITICAL_ISR(p) ((void)p)
#define portEXIT_CRITICAL_ISR(p) ((void)p)
