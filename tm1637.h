/*
 * tm1637.h
 *
 *  Created on:		Jan 25, 2023
 *      Author:		Istomin A.O.
 *      Version:	1.1
 */

/*
 * [!] ВНИМАНИЕ! Не забываем проверить __HAL_RCC_GPIO*_CLK_ENABLEE что используются в связке с вашими
 * дисплеями.
 *
 * Так же, если используется ruduced libc / newlib-nano необходимо включить опции библиотеки
 * libc USE FLOAT IN PRINTF [-u_printf_float] в противном случае вывод "%f" "%Lf" будет невозможен
 *
 */

#ifndef TM1637_TM1637_H_
#define TM1637_TM1637_H_

#include "stm32f1xx_hal.h"

// Колв-во дисплеев
#define DISPLAYS_COUNT		1

// Тип выравнивания (по левому краю или правому)
typedef enum
{
    daLeft = 0, daRight
}
TM1638_Alignment;

/*
 * Базовая инициализация:
 * DisplayNo - индекс дисплея
 * DIO_Port  - порт пина DIO, например GPIOC
 * DIO_Pin   - номер пина DIO, например 1
 * CLK_Port  - порт пина CLK
 * DIO_Pin   - номер пина CLK
 *
 * Возврат:
 * 0 - контроллер не ответил или проблема с GPIO
 * 1 - все отлично
 */
uint8_t TM1638_Init(uint8_t DisplayNo, GPIO_TypeDef *DIO_Port, uint8_t DIO_Pin,
                    GPIO_TypeDef *CLK_Port, uint8_t CLK_Pin);

/*
 * Установить яркость
 * DisplayNo - индекс дисплея
 * Level     - яркость:
 * 0 - дисплей отключен
 * 1 - минимальная
 * 8 - максимальная
 *
 * Возврат:
 * 0 - контроллер не ответил или проблема с GPIO
 * 1 - все отлично
 */
uint8_t TM1638_SetBrightness(uint8_t DisplayNo, uint8_t Level);

/*
 * Вывести на дисплей, используется стандартный алгоритм printf.
 * DisplayNo - индекс дисплея
 * Alignment - определяет выравнивание слева [daLeft] или справа [daRight],
 * format    - формат вывода
 * ...       - аргументы
 *
 * printf("%u", 10)		"10  " или "  10"
 * printf("%d", -38)	"-38 " или " -38"
 * printf("%.1f", 4.51)	"4.5 " или " 4.5"
 * и т.д.
 * можно просто текст
 * printf("fail")		"FAIL"
 *
 */
void TM1638_printf(uint8_t DisplayNo, TM1638_Alignment Alignment,
                   const char *format, ...);

/*
 * DisplayNo - индекс дисплея
 * Очистить дисплей
 *
 * Возврат:
 * 0 - контроллер не ответил или проблема с GPIO
 * 1 - все отлично
 */
uint8_t TM1638_ClearDisplay(uint8_t DisplayNo);

#endif /* TM1637_TM1637_H_ */
