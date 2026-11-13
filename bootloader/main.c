/*
 * PureThermal USB DFU bootloader (32 KB at 0x08000000).
 *
 * On reset:
 *   - an application request (PTBOOT_REQ_DFU in no-init RAM), or
 *   - no valid application at PTAPP_BASE, or
 *   - PTBOOT_BOOTLOOP_MAX application starts that never ran 5 s stable
 * keep the device in USB DFU mode (0483:df11, LED blinks fast).
 * Otherwise jump straight to the application, before touching any clock.
 *
 * The ROM bootloader of the STM32F411 cannot do USB DFU with the PureThermal 1
 * crystal (14.318 MHz is not a multiple of 1 MHz), so this one brings up the
 * PLL exactly like the application does for either board revision.
 */
#include "stm32f4xx_hal.h"
#include "usbd_core.h"
#include "usbd_dfu.h"
#include "ptboot.h"
#include "mxconstants.h"

#define PTBOOT_VERSION 0x00010000u   /* 1.0.0 */

extern USBD_DescriptorsTypeDef DFU_Desc;

pt_board_t pt_board;
unsigned int pt_hse_value;
USBD_HandleTypeDef hUsbDeviceFS;

void Error_Handler(void)
{
	NVIC_SystemReset();
}

static int app_valid(void)
{
	uint32_t sp = *(volatile uint32_t *)PTAPP_BASE;
	uint32_t pc = *(volatile uint32_t *)(PTAPP_BASE + 4);
	return sp > PT_RAM_BASE && sp <= PT_RAM_END && (sp & 3) == 0 &&
	       pc > PTAPP_BASE && pc < PTAPP_BASE + PTAPP_MAX_SIZE && (pc & 1);
}

static void __attribute__((noreturn)) jump_to_app(void)
{
	uint32_t sp = *(volatile uint32_t *)PTAPP_BASE;
	uint32_t pc = *(volatile uint32_t *)(PTAPP_BASE + 4);
	SCB->VTOR = PTAPP_BASE;
	__DSB();
	__ISB();
	/* switch stacks and branch in one asm block: no C code may run on the
	 * bootloader stack after MSP changes */
	__asm volatile("msr msp, %0\n\tbx %1\n" : : "r"(sp), "r"(pc) : "memory");
	__builtin_unreachable();
}

/* Same board detection as the application (Src/main.c). */
static void board_detect(void)
{
	GPIO_InitTypeDef g = {0};
	__HAL_RCC_GPIOB_CLK_ENABLE();
	g.Pin = GPIO_PIN_4;
	g.Mode = GPIO_MODE_OUTPUT_PP;
	g.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOB, &g);
	HAL_GPIO_WritePin(GPIOB, GPIO_PIN_4, GPIO_PIN_RESET);
	g.Mode = GPIO_MODE_INPUT;
	HAL_GPIO_Init(GPIOB, &g);
	pt_board = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4) ? PT_BOARD_PT2 : PT_BOARD_PT1;
	pt_hse_value = (pt_board == PT_BOARD_PT1) ? 14318182U : 8000000U;
}

/* Same PLL setup as the application: 96 MHz SYSCLK, 48 MHz USB. */
static void clock_config(void)
{
	RCC_OscInitTypeDef osc = {0};
	RCC_ClkInitTypeDef clk = {0};

	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

	osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	osc.HSEState = RCC_HSE_ON;
	osc.PLL.PLLState = RCC_PLL_ON;
	osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
	if (pt_board == PT_BOARD_PT1) {
		osc.PLL.PLLM = 11; osc.PLL.PLLN = 295; osc.PLL.PLLP = RCC_PLLP_DIV4; osc.PLL.PLLQ = 8;
	} else {
		osc.PLL.PLLM = 4;  osc.PLL.PLLN = 96;  osc.PLL.PLLP = RCC_PLLP_DIV2; osc.PLL.PLLQ = 4;
	}
	if (HAL_RCC_OscConfig(&osc) != HAL_OK)
		Error_Handler();

	clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
	clk.APB1CLKDivider = RCC_HCLK_DIV2;
	clk.APB2CLKDivider = RCC_HCLK_DIV1;
	if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_3) != HAL_OK)
		Error_Handler();

	HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq() / 1000);
	HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);
	HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}

int main(void)
{
	volatile pt_noinit_t *ni = PT_NOINIT;
	int stay = 0;

	pt_noinit_validate();
	ni->reset_flags = RCC->CSR;
	RCC->CSR |= RCC_CSR_RMVF;
	ni->bootloader_version = PTBOOT_VERSION;

	if (ni->boot_request == PTBOOT_REQ_DFU)
		stay = 1;
	ni->boot_request = 0;

	if (!stay && app_valid()) {
		if (ni->boot_attempts < PTBOOT_BOOTLOOP_MAX) {
			ni->boot_attempts++;
			jump_to_app();
		}
		/* the app keeps dying before it is stable: stop and wait for a fix */
	}
	ni->boot_attempts = 0;

	HAL_Init();
	board_detect();
	clock_config();

	__HAL_RCC_GPIOA_CLK_ENABLE();
	GPIO_InitTypeDef g = {0};
	g.Pin = SYSTEM_LED_Pin;
	g.Mode = GPIO_MODE_OUTPUT_PP;
	g.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(SYSTEM_LED_GPIO_Port, &g);

	USBD_Init(&hUsbDeviceFS, &DFU_Desc, DEVICE_FS);
	USBD_RegisterClass(&hUsbDeviceFS, &USBD_DFU);
	USBD_Start(&hUsbDeviceFS);

	uint32_t last = 0, reboot_at = 0;
	for (;;) {
		if (dfu_poll(&hUsbDeviceFS) && !reboot_at)
			reboot_at = HAL_GetTick() + 50;
		if (reboot_at && (int32_t)(HAL_GetTick() - reboot_at) >= 0) {
			USBD_Stop(&hUsbDeviceFS);
			HAL_Delay(20);
			NVIC_SystemReset();
		}
		if (HAL_GetTick() - last >= 100) {
			last = HAL_GetTick();
			HAL_GPIO_TogglePin(SYSTEM_LED_GPIO_Port, SYSTEM_LED_Pin);
		}
	}
}
