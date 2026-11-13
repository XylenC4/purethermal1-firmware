#include "stm32f4xx_hal.h"

extern PCD_HandleTypeDef hpcd_USB_OTG_FS;

void SysTick_Handler(void) { HAL_IncTick(); }
void OTG_FS_IRQHandler(void) { HAL_PCD_IRQHandler(&hpcd_USB_OTG_FS); }
void HardFault_Handler(void) { NVIC_SystemReset(); }
void MemManage_Handler(void) { NVIC_SystemReset(); }
void BusFault_Handler(void) { NVIC_SystemReset(); }
void UsageFault_Handler(void) { NVIC_SystemReset(); }
void NMI_Handler(void) { }
