#include "tm1637.h"
#include "DWTDelay.h"
#include "string.h"
#include <stdarg.h>
#include <stdio.h>

// Кол-во сегментов контроллера (по даташиту)
#define SEG_CONTROL_MAX		4
// Яркость по умолчанию
#define BRIGHTNESS_DEF		2

// Структура настроек конкретного дисплея
typedef struct
{
    uint8_t Inited;
    GPIO_TypeDef *TM_DIO_Port;
    uint8_t TM_DIO_Pin;
    GPIO_TypeDef *TM_CLK_Port;
    uint8_t TM_CLK_Pin;
    uint8_t STR_BUF[SEG_CONTROL_MAX + 2];
}
TTM1637Display;

// Набор дисплеев
static TTM1637Display Displays[DISPLAYS_COUNT] = { 0 };

// Базовые использованне команды (см. даташит)
#define CMD_AUTO_INC_ADDR_MODE		0x40
#define CMD_RESET_ADDR				0xC0
#define CMD_SET_BRIGHTNESS			0x87

// Ограничение скорости обмена I/O по даташиту не более 250 КГц
// 250 КГц это 4 мкСек / 2 = 2 мкСек
#define CLOCK_DELAY_HALF			2

// Ногодрыг (сокращено для удобства)
#define DIO_HIGH(DN)    HAL_GPIO_WritePin(Displays[DN].TM_DIO_Port, Displays[DN].TM_DIO_Pin, GPIO_PIN_SET)
#define DIO_LOW(DN)     HAL_GPIO_WritePin(Displays[DN].TM_DIO_Port, Displays[DN].TM_DIO_Pin, GPIO_PIN_RESET)
#define DIO_GET(DN)     !HAL_GPIO_ReadPin(Displays[DN].TM_DIO_Port, Displays[DN].TM_DIO_Pin)
#define CLK_HIGH(DN)    HAL_GPIO_WritePin(Displays[DN].TM_CLK_Port, Displays[DN].TM_CLK_Pin, GPIO_PIN_SET)
#define CLK_LOW(DN)     HAL_GPIO_WritePin(Displays[DN].TM_CLK_Port, Displays[DN].TM_CLK_Pin, GPIO_PIN_RESET)

// Переключает ногу DIO в пушпул выход
static void DIO_OUT_MODE(uint8_t DisplayNo)
{
    // Тут помним что регистр порта состоит из H (8..15) & L (0..7)
    if (Displays[DisplayNo].TM_DIO_Pin < 8)
    {
        uint8_t Shift = 4 * Displays[DisplayNo].TM_DIO_Pin;
        Displays[DisplayNo].TM_DIO_Port->CRL |= (uint32_t) 1 << Shift;
        Displays[DisplayNo].TM_DIO_Port->CRL |= (uint32_t) 1 << (Shift + 1);
        Displays[DisplayNo].TM_DIO_Port->CRL &= ~((uint32_t) 1 << (Shift + 2));
        Displays[DisplayNo].TM_DIO_Port->CRL &= ~((uint32_t) 1 << (Shift + 3));
    }
    else
    {
        uint8_t Shift = 4 * (Displays[DisplayNo].TM_DIO_Pin - 8);
        Displays[DisplayNo].TM_DIO_Port->CRH |= (uint32_t) 1 << Shift;
        Displays[DisplayNo].TM_DIO_Port->CRH |= (uint32_t) 1 << (Shift + 1);
        Displays[DisplayNo].TM_DIO_Port->CRH &= ~((uint32_t) 1 << (Shift + 2));
        Displays[DisplayNo].TM_DIO_Port->CRH &= ~((uint32_t) 1 << (Shift + 3));
    }
}

// Переключает ногу DIO в режим входа
static void DIO_IN_MODE(uint8_t DisplayNo)
{
    if (Displays[DisplayNo].TM_DIO_Pin < 8)
    {
        uint8_t Shift = 4 * Displays[DisplayNo].TM_DIO_Pin;
        Displays[DisplayNo].TM_DIO_Port->CRL &= ~((uint32_t) 1 << Shift);
        Displays[DisplayNo].TM_DIO_Port->CRL &= ~((uint32_t) 1 << (Shift + 1));
        Displays[DisplayNo].TM_DIO_Port->CRL &= ~((uint32_t) 1 << (Shift + 2));
        Displays[DisplayNo].TM_DIO_Port->CRL |= (uint32_t) 1 << (Shift + 3);
    }
    else
    {
        uint8_t Shift = 4 * (Displays[DisplayNo].TM_DIO_Pin - 8);
        Displays[DisplayNo].TM_DIO_Port->CRH &= ~((uint32_t) 1 << Shift);
        Displays[DisplayNo].TM_DIO_Port->CRH &= ~((uint32_t) 1 << (Shift + 1));
        Displays[DisplayNo].TM_DIO_Port->CRH &= ~((uint32_t) 1 << (Shift + 2));
        Displays[DisplayNo].TM_DIO_Port->CRH |= (uint32_t) 1 << (Shift + 3);
    }
}

// Стартовая последовательность (см. даташит)
static void StartSequence(uint8_t DisplayNo)
{
    CLK_HIGH(DisplayNo);
    DIO_HIGH(DisplayNo);
    DWT_Delay_us(CLOCK_DELAY_HALF);
    DIO_LOW(DisplayNo);
}

// Завершающая последовательность (см. даташит)
static void StopSequence(uint8_t DisplayNo)
{
    CLK_LOW(DisplayNo);
    DIO_LOW(DisplayNo);
    DWT_Delay_us(CLOCK_DELAY_HALF);
    CLK_HIGH(DisplayNo);
    DWT_Delay_us(CLOCK_DELAY_HALF);
    DIO_HIGH(DisplayNo);
    DWT_Delay_us(CLOCK_DELAY_HALF * 2);
}

// Преобразуем бфуер. Заменяем символы на коды сегментов контроллера, не забывая двигать буфер если есть "."
// Так, например было "1.25" (4 символа), станет "XXX" (3 символа) - так как
static void TranslateBuffer(uint8_t DisplayNo, uint8_t *Buffer)
{
    uint8_t CommaFlag = 0;

    for (int i = 0; i < SEG_CONTROL_MAX + 1; i++)
    {
        if ((Displays[DisplayNo].STR_BUF[i] == '.') ||
            (Displays[DisplayNo].STR_BUF[i] == ','))
        {
            // Разделитель дробной части
            CommaFlag = 1;
            for (int j = i; j < SEG_CONTROL_MAX; j++)
                Displays[DisplayNo].STR_BUF[j] = Displays[DisplayNo].STR_BUF[j + 1];
            i--;
            continue;
        }

        switch (Displays[DisplayNo].STR_BUF[i])
        {
            // Основные цифры
            case '0': Displays[DisplayNo].STR_BUF[i] = 0x3F; break;
            case '1': Displays[DisplayNo].STR_BUF[i] = 0x06; break;
            case '2': Displays[DisplayNo].STR_BUF[i] = 0x5B; break;
            case '3': Displays[DisplayNo].STR_BUF[i] = 0x4F; break;
            case '4': Displays[DisplayNo].STR_BUF[i] = 0x66; break;
            case '5': Displays[DisplayNo].STR_BUF[i] = 0x6D; break;
            case '6': Displays[DisplayNo].STR_BUF[i] = 0x7D; break;
            case '7': Displays[DisplayNo].STR_BUF[i] = 0x07; break;
            case '8': Displays[DisplayNo].STR_BUF[i] = 0x7F; break;
            case '9': Displays[DisplayNo].STR_BUF[i] = 0x6F; break;

            // Латинские буквы
            case 'a':
            case 'A': Displays[DisplayNo].STR_BUF[i] = 0x77; break;
            case 'b':
            case 'B': Displays[DisplayNo].STR_BUF[i] = 0x7c; break;
            case 'c': Displays[DisplayNo].STR_BUF[i] = 0x58; break;
            case 'C': Displays[DisplayNo].STR_BUF[i] = 0x39; break;
            case 'd':
            case 'D': Displays[DisplayNo].STR_BUF[i] = 0x5E; break;
            case 'e':
            case 'E': Displays[DisplayNo].STR_BUF[i] = 0x79; break;
            case 'f':
            case 'F': Displays[DisplayNo].STR_BUF[i] = 0x71; break;
            case 'h': Displays[DisplayNo].STR_BUF[i] = 0x74; break;
            case 'H': Displays[DisplayNo].STR_BUF[i] = 0x76; break;
            case 'j':
            case 'J': Displays[DisplayNo].STR_BUF[i] = 0x1E; break;
            case 'i':
            case 'I': Displays[DisplayNo].STR_BUF[i] = 0x06; break;
            case 'l':
            case 'L': Displays[DisplayNo].STR_BUF[i] = 0x38; break;
            case 'n':
            case 'N': Displays[DisplayNo].STR_BUF[i] = 0x54; break;
            case 'o':
            case 'O': Displays[DisplayNo].STR_BUF[i] = 0x5C; break;
            case 'p':
            case 'P': Displays[DisplayNo].STR_BUF[i] = 0x73; break;
            case 'q':
            case 'Q': Displays[DisplayNo].STR_BUF[i] = 0x67; break;
            case 'r':
            case 'R': Displays[DisplayNo].STR_BUF[i] = 0x50; break;
            case 's':
            case 'S': Displays[DisplayNo].STR_BUF[i] = 0x6D; break;
            case 't':
            case 'T': Displays[DisplayNo].STR_BUF[i] = 0x78; break;
            case 'u': Displays[DisplayNo].STR_BUF[i] = 0x1C; break;
            case 'U': Displays[DisplayNo].STR_BUF[i] = 0x3E; break;
            case 'y':
            case 'Y': Displays[DisplayNo].STR_BUF[i] = 0x6E; break;

            // Прочие символы
            case '^': Displays[DisplayNo].STR_BUF[i] = 0x63; break;
            case '-': Displays[DisplayNo].STR_BUF[i] = 0x40; break;
            case '_': Displays[DisplayNo].STR_BUF[i] = 0x08; break;
            case '=': Displays[DisplayNo].STR_BUF[i] = 0x48; break;
            case '\\': Displays[DisplayNo].STR_BUF[i] = 0x64; break;
            case '/': Displays[DisplayNo].STR_BUF[i] = 0x52; break;
            case '[': Displays[DisplayNo].STR_BUF[i] = 0x39; break;
            case ']': Displays[DisplayNo].STR_BUF[i] = 0x52; break;
            case ' ': Displays[DisplayNo].STR_BUF[i] = 0x00; break;

            // Если не знаем символ, обнуляем
            default:
            {
                Displays[DisplayNo].STR_BUF[i] = 0x00;
            }
        }

        // Добавляем бит точки
        if (CommaFlag)
        {
            CommaFlag = 0;
            if (i > 0)
                Displays[DisplayNo].STR_BUF[i - 1] |= 0x80;
        }
    }
}

// Записать байт
static uint8_t WriteByte(uint8_t DisplayNo, uint8_t Data)
{
    uint8_t ACK = 0;

    // Shifted write byte
    for (uint8_t i = 0; i < 8; i++)
    {
        CLK_LOW(DisplayNo);

        if (Data & 0x01)
            DIO_HIGH(DisplayNo);
        else
            DIO_LOW(DisplayNo);
        Data >>= 1;

        DWT_Delay_us(CLOCK_DELAY_HALF);

        CLK_HIGH(DisplayNo);
        DWT_Delay_us(CLOCK_DELAY_HALF);
    }
    CLK_LOW(DisplayNo);

    // Читаем ответ контроллера - ACK
    DWT_Delay_us(CLOCK_DELAY_HALF);
    DIO_IN_MODE(DisplayNo);
    ACK = DIO_GET(DisplayNo);
    DIO_OUT_MODE(DisplayNo);
    CLK_HIGH(DisplayNo);
    DWT_Delay_us(CLOCK_DELAY_HALF);

    return ACK;
}

uint8_t TM1638_SetBrightness(uint8_t DisplayNo, uint8_t Level)
{
    uint8_t ACK = 0;

    StartSequence(DisplayNo);
    ACK = WriteByte(DisplayNo, CMD_SET_BRIGHTNESS + Level);
    StopSequence(DisplayNo);
    return ACK;
}

uint8_t TM1638_ClearDisplay(uint8_t DisplayNo)
{
    uint8_t ACK = 1;

    StartSequence(DisplayNo);
    WriteByte(DisplayNo, CMD_AUTO_INC_ADDR_MODE);
    StopSequence(DisplayNo);

    StartSequence(DisplayNo);
    WriteByte(DisplayNo, CMD_RESET_ADDR);
    for (uint8_t i = 0; i < SEG_CONTROL_MAX; i++)
    {
        if (!WriteByte(DisplayNo, 0x00))
        {
            ACK = 0;
            break;
        }
    }
    StopSequence(DisplayNo);

    return ACK;
}

void TM1638_printf(uint8_t DisplayNo, TM1638_Alignment Alignment,
                   const char *format, ...)
{
    if (NULL == format)
        return;
    memset(Displays[DisplayNo].STR_BUF, 0x00,
           sizeof(Displays[DisplayNo].STR_BUF));

    // Выводим в буфер
    va_list args;
    va_start(args, format);
    vsnprintf((char*) Displays[DisplayNo].STR_BUF, SEG_CONTROL_MAX + 2, format, args);
    va_end(args);

    // Транслируем буфер текст -> коды сегментов
    TranslateBuffer(DisplayNo, &Displays[DisplayNo].STR_BUF[0]);

    // Выравнивание
    uint8_t PosShift = SEG_CONTROL_MAX - 1;
    if (daRight == Alignment)
    {
        if (0 == Displays[DisplayNo].STR_BUF[SEG_CONTROL_MAX - 1])
        {
            for (int8_t i = SEG_CONTROL_MAX - 1; i >= 0; i--)
            {
                if ((0 != Displays[DisplayNo].STR_BUF[i]) ||
                    (PosShift != SEG_CONTROL_MAX - 1))
                {
                    Displays[DisplayNo].STR_BUF[PosShift] =
                        Displays[DisplayNo].STR_BUF[i];
                    Displays[DisplayNo].STR_BUF[i] = 0x00;
                    PosShift--;
                }
            }
        }
    }

    // Пишем бфуер
    StartSequence(DisplayNo);
    WriteByte(DisplayNo, CMD_AUTO_INC_ADDR_MODE);
    StopSequence(DisplayNo);

    StartSequence(DisplayNo);
    WriteByte(DisplayNo, CMD_RESET_ADDR);

    for (uint8_t i = 0; i < SEG_CONTROL_MAX; i++)
    {
        if (!WriteByte(DisplayNo, Displays[DisplayNo].STR_BUF[i]))
            break;
    }

    StopSequence(DisplayNo);
}

uint8_t TM1638_Init(uint8_t DisplayNo, GPIO_TypeDef *DIO_Port, uint8_t DIO_Pin,
                    GPIO_TypeDef *CLK_Port, uint8_t CLK_Pin)
{
    if ((NULL == DIO_Port) ||
        (NULL == CLK_Port) ||
        (DisplayNo > DISPLAYS_COUNT - 1) ||
        (Displays[DisplayNo].Inited))
        return 0;

    memset(Displays[DisplayNo].STR_BUF, 0x00,
           sizeof(Displays[DisplayNo].STR_BUF));

    Displays[DisplayNo].TM_DIO_Port = DIO_Port;
    Displays[DisplayNo].TM_CLK_Port = CLK_Port;
    Displays[DisplayNo].TM_DIO_Pin = DIO_Pin;
    Displays[DisplayNo].TM_CLK_Pin = CLK_Pin;

    // Инициализация пинов
    GPIO_InitTypeDef PinCfg =
    {
        .Mode = GPIO_MODE_OUTPUT_PP,
        .Pin = Displays[DisplayNo].TM_DIO_Pin,
        .Pull = GPIO_PULLUP,
        .Speed = GPIO_SPEED_FREQ_HIGH
    };
    HAL_GPIO_Init(Displays[DisplayNo].TM_DIO_Port, &PinCfg);

    PinCfg.Pin = Displays[DisplayNo].TM_CLK_Pin;
    HAL_GPIO_Init(Displays[DisplayNo].TM_CLK_Port, &PinCfg);

    // Задержки DWT
    if (!DWT_Inited())
        DWT_Init();

    DIO_OUT_MODE(DisplayNo);
    Displays[DisplayNo].Inited = 1;

    // Устанавливаем яркость среднюю, и очищаем дисплей
    return TM1638_SetBrightness(DisplayNo, BRIGHTNESS_DEF)
            && TM1638_ClearDisplay(DisplayNo);
}
