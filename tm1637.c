#include "tm1637.h"
#include "DWTDelay.h"
#include "string.h"
#include <stdarg.h>
#include <stdio.h>

// Кол-во сегментов контроллера (по даташиту)
#define SEG_CONTROL_MAX		4

// Буфер для вывода printf
static uint8_t STR_BUF[SEG_CONTROL_MAX + 2] = { 0 };

// Базовые использованне команды (см. даташит)
#define CMD_AUTO_INC_ADDR_MODE		0x40
#define CMD_RESET_ADDR				0xC0
#define CMD_SET_BRIGHTNESS			0x87

// Конфигурация пинов
static GPIO_TypeDef* TM_DIO_Port = NULL;
static uint8_t TM_DIO_Pin = 0;
static GPIO_TypeDef* TM_CLK_Port = NULL;
static uint8_t TM_CLK_Pin = 0;

// Ограничение скорости обмена I/O по даташиту не более 250 КГц
// 250 КГц это 4 мкСек / 2 = 2 мкСек
#define CLOCK_DELAY_HALF	2

// Тактирование портов использованных ног
#define DIO_CLK_ENABLE() 	__HAL_RCC_GPIOA_CLK_ENABLE()
#define CLK_CLK_ENABLE() 	__HAL_RCC_GPIOA_CLK_ENABLE()

// Ногодрыг (сокращено для удобства)
#define DIO_HIGH()			HAL_GPIO_WritePin(TM_DIO_Port, TM_DIO_Pin, GPIO_PIN_SET)
#define DIO_LOW()			HAL_GPIO_WritePin(TM_DIO_Port, TM_DIO_Pin, GPIO_PIN_RESET)
#define DIO_GET()			!HAL_GPIO_ReadPin(TM_DIO_Port, TM_DIO_Pin)
#define CLK_HIGH()			HAL_GPIO_WritePin(TM_CLK_Port, TM_CLK_Pin, GPIO_PIN_SET)
#define CLK_LOW()			HAL_GPIO_WritePin(TM_CLK_Port, TM_CLK_Pin, GPIO_PIN_RESET)

// Переключает ногу DIO в пушпул выход
static void DIO_OUT_MODE(void)
{
	// Тут помним что регистр порта состоит из H (8..15) & L (0..7)
	if (TM_DIO_Pin < 8)
	{
		uint8_t Shift = 4 * TM_DIO_Pin;
		TM_DIO_Port->CRL |= (uint32_t)1 << Shift;
		TM_DIO_Port->CRL |= (uint32_t)1 << (Shift + 1);
		TM_DIO_Port->CRL &= ~((uint32_t)1 << (Shift + 2));
		TM_DIO_Port->CRL &= ~((uint32_t)1 << (Shift + 3));
	}
	else
	{
		uint8_t Shift = 4 * (TM_DIO_Pin - 8);
		TM_DIO_Port->CRH |= (uint32_t)1 << Shift;
		TM_DIO_Port->CRH |= (uint32_t)1 << (Shift + 1);
		TM_DIO_Port->CRH &= ~((uint32_t)1 << (Shift + 2));
		TM_DIO_Port->CRH &= ~((uint32_t)1 << (Shift + 3));
	}
}

// Переключает ногу DIO в режим входа
static void DIO_IN_MODE(void)
{
	if (TM_DIO_Pin < 8)
	{
		uint8_t Shift = 4 * TM_DIO_Pin;
		TM_DIO_Port->CRL &= ~((uint32_t)1 << Shift);
		TM_DIO_Port->CRL &= ~((uint32_t)1 << (Shift + 1));
		TM_DIO_Port->CRL &= ~((uint32_t)1 << (Shift + 2));
		TM_DIO_Port->CRL |= (uint32_t)1 << (Shift + 3);
	}
	else
	{
		uint8_t Shift = 4 * (TM_DIO_Pin - 8);
		TM_DIO_Port->CRH &= ~((uint32_t)1 << Shift);
		TM_DIO_Port->CRH &= ~((uint32_t)1 << (Shift + 1));
		TM_DIO_Port->CRH &= ~((uint32_t)1 << (Shift + 2));
		TM_DIO_Port->CRH |= (uint32_t)1 << (Shift + 3);
	}
}

// Стартовая последовательность (см. даташит)
static void StartSequence(void)
{
	CLK_HIGH();
	DIO_HIGH();
	DWT_Delay_us(CLOCK_DELAY_HALF);
	DIO_LOW();
}

// Завершающая последовательность (см. даташит)
static void StopSequence(void)
{
	CLK_LOW();
	DIO_LOW();
	DWT_Delay_us(CLOCK_DELAY_HALF);
	CLK_HIGH();
	DWT_Delay_us(CLOCK_DELAY_HALF);
	DIO_HIGH();
	DWT_Delay_us(CLOCK_DELAY_HALF * 2);
}

// Преобразуем бфуер. Заменяем символы на коды сегментов контроллера, не забывая двигать буфер если есть "."
// Так, например было "1.25" (4 символа), станет "XXX" (3 символа) - так как
static void TranslateBuffer(uint8_t* Buffer)
{
	uint8_t CommaFlag = 0;

	for (int i=0; i<SEG_CONTROL_MAX + 1; i++)
	{
		if ((STR_BUF[i] == '.') || (STR_BUF[i] == ','))
		{
			// Разделитель дробной части
			CommaFlag = 1;
			for (int j=i; j<SEG_CONTROL_MAX; j++)
				STR_BUF[j] = STR_BUF[j + 1];
			i--;
			continue;
		}

		switch (STR_BUF[i])
		{
			// Основные цифры
			case '0': STR_BUF[i] = 0x3F; break;
			case '1': STR_BUF[i] = 0x06; break;
			case '2': STR_BUF[i] = 0x5B; break;
			case '3': STR_BUF[i] = 0x4F; break;
			case '4': STR_BUF[i] = 0x66; break;
			case '5': STR_BUF[i] = 0x6D; break;
			case '6': STR_BUF[i] = 0x7D; break;
			case '7': STR_BUF[i] = 0x07; break;
			case '8': STR_BUF[i] = 0x7F; break;
			case '9': STR_BUF[i] = 0x6F; break;

			// Латинские буквы
			case 'a':
			case 'A': STR_BUF[i] = 0x77; break;
			case 'b':
			case 'B': STR_BUF[i] = 0x7c; break;
			case 'c': STR_BUF[i] = 0x58; break;
			case 'C': STR_BUF[i] = 0x39; break;
			case 'd':
			case 'D': STR_BUF[i] = 0x5E; break;
			case 'e':
			case 'E': STR_BUF[i] = 0x79; break;
			case 'f':
			case 'F': STR_BUF[i] = 0x71; break;
			case 'h': STR_BUF[i] = 0x74; break;
			case 'H': STR_BUF[i] = 0x76; break;
			case 'j':
			case 'J': STR_BUF[i] = 0x1E; break;
			case 'i':
			case 'I': STR_BUF[i] = 0x06; break;
			case 'l':
			case 'L': STR_BUF[i] = 0x38; break;
			case 'n':
			case 'N': STR_BUF[i] = 0x54; break;
			case 'o':
			case 'O': STR_BUF[i] = 0x5C; break;
			case 'p':
			case 'P': STR_BUF[i] = 0x73; break;
			case 'q':
			case 'Q': STR_BUF[i] = 0x67; break;
			case 'r':
			case 'R': STR_BUF[i] = 0x50; break;
			case 's':
			case 'S': STR_BUF[i] = 0x6D; break;
			case 't':
			case 'T': STR_BUF[i] = 0x78; break;
			case 'u': STR_BUF[i] = 0x1C; break;
			case 'U': STR_BUF[i] = 0x3E; break;
			case 'y':
			case 'Y': STR_BUF[i] = 0x6E; break;

			// Прочие символы
			case '^': STR_BUF[i] = 0x63; break;
			case '-': STR_BUF[i] = 0x40; break;
			case '_': STR_BUF[i] = 0x08; break;
			case '=': STR_BUF[i] = 0x48; break;
			case '\\': STR_BUF[i] = 0x64; break;
			case '/': STR_BUF[i] = 0x52; break;
			case '[': STR_BUF[i] = 0x39; break;
			case ']': STR_BUF[i] = 0x52; break;
			case ' ': STR_BUF[i] = 0x00; break;

			// Если не знаем символ, обнуляем
			default:
			{
				STR_BUF[i] = 0x00;
			}
		}

		// Добавляем бит точки
		if (CommaFlag)
		{
			CommaFlag = 0;
			if (i > 0) STR_BUF[i-1] |= 0x80;
		}
	}
}

// Записать байт
static uint8_t WriteByte(uint8_t Data)
{
	uint8_t ACK = 0;

	// Shifted write byte
	for (uint8_t i=0; i<8; i++)
    {
        CLK_LOW();

        if (Data & 0x01)
            DIO_HIGH();
        else
            DIO_LOW();
        Data >>= 1;

        DWT_Delay_us(CLOCK_DELAY_HALF);

        CLK_HIGH();
        DWT_Delay_us(CLOCK_DELAY_HALF);
    }
	CLK_LOW();

	// Читаем ответ контроллера - ACK
	DWT_Delay_us(CLOCK_DELAY_HALF);
	DIO_IN_MODE();
	ACK = DIO_GET();
	DIO_OUT_MODE();
	CLK_HIGH();
	DWT_Delay_us(CLOCK_DELAY_HALF);

	return ACK;
}

uint8_t TM1638_SetBrightness(uint8_t Level)
{
	uint8_t ACK = 0;

	StartSequence();
	ACK = WriteByte(CMD_SET_BRIGHTNESS + Level);
	StopSequence();
	return ACK;
}

uint8_t TM1638_ClearDisplay(void)
{
	uint8_t ACK = 1;

	StartSequence();
	WriteByte(CMD_AUTO_INC_ADDR_MODE);
    StopSequence();

    StartSequence();
    WriteByte(CMD_RESET_ADDR);
    for (uint8_t i=0; i<SEG_CONTROL_MAX; i++)
    {
    	if (!WriteByte(0x00))
    	{
    		ACK = 0;
    		break;
    	}
    }
    StopSequence();

    return ACK;
}

void TM1638_printf(TM1638_Alignment Alignment, const char* format, ...)
{
	if (NULL == format) return;
	memset(STR_BUF, 0x00, sizeof(STR_BUF));

	// Выводим в буфер
	va_list args;
	va_start(args, format);
	vsnprintf((char*)STR_BUF, SEG_CONTROL_MAX + 2, format, args);
	va_end(args);

	// Транслируем буфер текст -> коды сегментов
	TranslateBuffer(&STR_BUF[0]);

	// Выравнивание
	uint8_t PosShift = SEG_CONTROL_MAX-1;
	if (daRight == Alignment)
	{
		if (0 == STR_BUF[SEG_CONTROL_MAX-1])
		for (int8_t i=SEG_CONTROL_MAX-1; i>=0; i--)
		{
			if ((0 != STR_BUF[i]) || (PosShift != SEG_CONTROL_MAX-1))
			{
				STR_BUF[PosShift] = STR_BUF[i];
				STR_BUF[i] = 0x00;
				PosShift--;
			}
		}
	}

	// Пишем бфуер
	StartSequence();
	WriteByte(CMD_AUTO_INC_ADDR_MODE);
    StopSequence();

    StartSequence();
    WriteByte(CMD_RESET_ADDR);

    for (uint8_t i=0; i<SEG_CONTROL_MAX; i++)
    {
    	if (!WriteByte(STR_BUF[i])) break;
    }
    StopSequence();
}

uint8_t TM1638_Init(GPIO_TypeDef* DIO_Port, uint8_t DIO_Pin, GPIO_TypeDef* CLK_Port, uint8_t CLK_Pin)
{
	if ((NULL == DIO_Port) || (NULL == CLK_Port))
		return 0;

	TM_DIO_Port = DIO_Port;
	TM_CLK_Port = CLK_Port;
	TM_DIO_Pin = DIO_Pin;
	TM_CLK_Pin = CLK_Pin;

	// Тактирование
	DIO_CLK_ENABLE();
	CLK_CLK_ENABLE();

	// Инициализация пинов
	DIO_CLK_ENABLE();
	GPIO_InitTypeDef PinCfg =
	{
		.Mode = GPIO_MODE_OUTPUT_PP,
		.Pin = TM_DIO_Pin,
		.Pull = GPIO_PULLUP,
		.Speed = GPIO_SPEED_FREQ_HIGH
	};
	HAL_GPIO_Init(TM_DIO_Port, &PinCfg);

	PinCfg.Pin = TM_CLK_Pin;
	HAL_GPIO_Init(TM_CLK_Port, &PinCfg);

	// Задержки DWT
	DWT_Init();

	DIO_OUT_MODE();

	// Устанавливаем яркость среднюю, и очищаем дисплей
	return TM1638_SetBrightness(2) && TM1638_ClearDisplay();
}
