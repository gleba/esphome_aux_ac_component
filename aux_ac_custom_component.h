// Custom ESPHome component for AUX-based air conditioners
// Need some soldering skills
// Detailed connection instruction is available on github: https://github.com/GrKoR/esphome_aux_ac_component

#include "esphome.h"
#include <stdarg.h>

static const char *TAG = "AirCon";
class AirCon;

#define AC_ROVEX_FIRMWARE_VERSION   "0.1.0"

// периодичность опроса кондиционера на предмет изменения состояния
// изменение параметров с пульта не сообщается в UART, поэтому надо запрашивать состояние, чтобы быть в курсе
// значение в миллисекундах
#define AC_STATES_REQUEST_INTERVAL   7000   // 7 sec default interval

// минимальная и максимальная температура в градусах Цельсия, ограничения самого кондиционера
#define AC_MIN_TEMPERATURE 16
#define AC_MAX_TEMPERATURE 32

// шаг изменения целевой температуры, градусы Цельсия
#define AC_TEMPERATURE_STEP 0.1

// состояния конечного автомата компонента
enum acsm_state : uint8_t {
    ACSM_IDLE = 0,              // ничего не делаем, ждем, на что бы среагировать
    ACSM_RECEIVING_PACKET,      // находимся в процессе получения пакета, никакие отправки в этом состоянии невозможны
    ACSM_PARSING_PACKET,        // разбираем полученный пакет
    //ACSM_SENDING_ANSWER,        // отправляем ответ на команду сплита
    ACSM_SENDING_PACKET,        // отправляем пакет сплиту
    //ACSM_WAITING_FOR_PACKET     // ждем ответ на нашу команду (получаем пакет или вываливаемся по таймауту, расчитываемому по длине ожидаемого пакета)
};

/**
 * Кондиционер отправляет пакеты следующей структуры:
 *   HEADER: 8 bytes
 *   BODY: 0..24 bytes
 *   CRC: 2 bytes
 *   Весь пакет максимум 34 байта
 *   По крайней мере все встреченные мной пакеты имели такой размер и структуру.
 **/
#define AC_HEADER_SIZE 8
#define AC_MAX_BODY_SIZE 24
#define AC_BUFFER_SIZE 34

/**
 * таймаут загрузки пакета
 * 
 * через такое количиство миллисекунд конечный автомат перейдет из состояния ACSM_RECEIVING_PACKET в ACSM_IDLE, если пакет не будет загружен
 * расчетное время передачи 1 бита при скорости 4800 примерно 0,208 миллисекунд;
 * 1 байт передается 11 битами (1 стартовый, 8 бит данных, 1 бит четности и 1 стоповый бит) или 2,30 мс.
 * максимальный размер пакета AC_BUFFER_SIZE = 34 байта => 78,2 мсек. Плюс накладные расходы.
 * Скорее всего на получение пакета должно хватать 100 мсек.
 * 
 * По факту проверка показала:
 *      - если отрабатывать по 1 символу из UART на один вызов loop, то на 10 байт пинг-пакета требуется 166 мсек.
 *        То есть примерно по 16,6 мсек на байт. Примем 17 мсек.
 *        Значит на максимальный пакет потребуется 17*34 = 578 мсек. Примем 600 мсек.
 *      - если отрабатывать пакет целиком или хотя бы имеющимися в буфере UART кусками, то на 10 байт пинг-пакета требуется 27 мсек.
 *        То есть примерно по 2,7 мсек. на байт. Что близко к расчетным идеальным значениям. Примем 3 мсек.
 *        Значит на максимальный пакет потребуется 3*34 = 102 мсек. Примем 150 мсек.
 *
 * Опыт показал, что 150 мсек вполне хватает на большие пакеты
 **/
#define AC_PACKET_TIMEOUT   150     // 150 мсек - отработка буфера UART за раз, 600 мсек - отработка буфера UART по 1 байту за вызов loop
#define AC_BYTE_TIME        3       // 3 или 17 мсек на байт в зависимости от принципов обработки буфера UART; возможно буду использовать для расчета таймаутов

// типы пакетов
#define AC_PTYPE_PING   0x01    // ping-пакет, рассылается кондиционером каждые 3 сек.; модуль на него отвечает 
#define AC_PTYPE_CMD    0x06    // команда сплиту; модуль отправляет такие команды, когда что-то хочет от сплита
#define AC_PTYPE_INFO   0x07    // информационный пакет; бывает 3 видов; один из них рассылается кондиционером самостоятельно раз в 10 мин. и все 3 могут быть ответом на запросы модуля
#define AC_PTYPE_INIT   0x09    // инициирующий пакет; присылается сплитом, если кнопка HEALTH на пульте нажимается 8 раз; как там и что работает - не разбирался.
#define AC_PTYPE_UNKN   0x0b    // какой-то странный пакет, отправляемый пультом при инициации и иногда при включении питания... как работает и зачем нужен - не разбирался, сплит на него вроде бы не реагирует

// типы команд
#define AC_CMD_STATUS_BIG       0x21    // большой пакет статуса кондиционера
#define AC_CMD_STATUS_SMALL     0x11    // маленький пакет статуса кондиционера
#define AC_CMD_STATUS_PERIODIC  0x2C    // иногда встречается, сплит её рассылает по своему разумению; (вроде бы может быть и другой код! надо больше данных)
#define AC_CMD_SET_PARAMS       0x01    // команда установки параметров кондиционера

// значения байтов в пакетах
#define AC_PACKET_START_BYTE    0xBB    // Стартовый байт любого пакета 0xBB, других не встречал
#define AC_PACKET_ANSWER        0x80    // признак ответа wifi-модуля

// заголовок пакета
struct packet_header_t {
    uint8_t start_byte;     // стартовый бит пакета, всегда 0xBB
    uint8_t _unknown1;      // не расшифрован
    uint8_t packet_type;    // тип пакета:
                            //      0x01 - пинг
                            //      0x06 - команда кондиционеру
                            //      0x07 - информационный пакет со статусом кондиционера
                            //      0x09 - (не разбирался) инициирование коннекта wifi-модуля с приложением на телефоне, с ESP работает и без этого
                            //      0x0b - (не разбирался) wifi-модуль так сигналит, когда не получает пинги от кондиционера и в каких-то еще случаях
    uint8_t wifi;           // признак пакета от wifi-модуля
                            //      0x80 - для всех сообщений, посылаемых модулем
                            //      0x00 - для всех сообщений, посылаемых кондиционером
    uint8_t ping_answer_01; // не расшифрован, почти всегда 0x00, только в ответе на ping этот байт равен 0x01
    uint8_t _unknown2;      // не расшифрован
    uint8_t body_length;    // длина тела пакета в байтах
    uint8_t _unknown3;      // не расшифрован
};

// CRC пакета
union packet_crc_t {
    uint16_t crc16;
    uint8_t crc[2];
};

struct packet_t {
    uint32_t msec;  // значение millis в момент определения корректности пакета
    packet_header_t * header;   // указатель на заголовок пакета
    packet_crc_t * crc;     // указатель на контрольную сумму пакета
    uint8_t * body;         // указатель на первый байт тела; можно приведением типов указателей обращаться к отдельным битам как к полям соответсвующей структуры
    uint8_t bytesLoaded;    //количество загруженных в пакет байт, включая CRC
    uint8_t data[AC_BUFFER_SIZE];
};

// тело ответа на пинг
struct packet_ping_answer_body_t {
    uint8_t byte_1C;        // первый байт всегда 0x1C
    uint8_t byte_27;        // второй байт тела пинг-ответа всегда 0x27
    uint8_t zero1;          // всегда 0x00 
    uint8_t zero2;          // всегда 0x00 
    uint8_t zero3;          // всегда 0x00 
    uint8_t zero4;          // всегда 0x00 
    uint8_t zero5;          // всегда 0x00 
    uint8_t zero6;          // всегда 0x00 
};

// тело большого информационного пакета
struct packet_big_info_body_t {
    uint8_t byte_01;        // всегда 0x01
    uint8_t cmd_answer;     // код команды, ответом на которую пришел данный пакет (0x21);
                            //      пакет может рассылаться и в дежурном режиме (без запроса со стороны wifi-модуля)
                            //      в этом случае тут могут быть значения, отличные от 0x21
    uint8_t byte_C0;        // не расшифрован, всегда 0xC0 
    uint8_t unknown1;       // не расшифрован, как-то связан с режимом работы сплита; как вариант, отражает режим работы
                            //      компрессора во внешнем блоке или что-то такое, потому что иногда включение сплита не сразу приводит к изменениям в этом байте
                            //
                            //      Встречались такие значения:
                            //      0x04 - сплит выключен, до этого работал (статус держится 1 час после выкл.)
                            //      0x05 - режим AUTO
                            //      0x24 - режим OFF
                            //      0x25 - режим COOL
                            //      0x39 - ??
                            //      0x45 - режим DRY
                            //      0x85 - режим HEAT
                            //      0xC4 - режим OFF, выключен давно, зима
                            //      0xC5 - режим FAN
    uint8_t zero1;          // всегда 0x00 
    uint8_t fanSpeed;       // в ответах на команды wifi-модуля в этом байте скорость работы вентилятора
                            //      fanSpeed: OFF=0x00, LOW=0x02, MID=0x04, HIGH=0x06, TURBO=0x07; режим CLEAN=0x01
                            //      в дежурных пакетах тут похоже что-то другое
    uint8_t zero2;          // всегда 0x00 
    uint8_t ambient_temperature_int;    // целая часть комнатной температуры воздуха с датчика на внутреннем блоке сплит-системы
                                        //      перевод по формуле T = Тin - 0x20 + Tid/10
                                        //      где
                                        //          Tin - целая часть температуры
                                        //          Tid - десятичная часть температуры
    uint8_t zero3;          // всегда 0x00 
    uint8_t outdoor_temperature;        // этот байт как-то связан с температурой во внешнем блоке. Требуются дополнительные исследования.
                                        //      При выключенном сплите характер изменения значения примерно соответствует изменению температуры на улице.
                                        //      При включенном сплите значение может очень сильно скакать.
                                        //      По схеме wiring diagram сплит-системы, во внешнем блоке есть термодатчик, отслеживающий температуру испарителя.
                                        //      Возможно, этот байт как раз и отражает изменение температуры на испарителе.
                                        //      Но я не смог разобраться, как именно перевести эти значения в градусы.
                                        //      Кроме того, зимой даже в минусовую температуру этот байт не уходит ниже 0x33 по крайней мере
                                        //      для температур в диапазоне -5..-10 градусов Цельсия.
    uint8_t zero4;          // всегда 0x00 
    uint8_t zero5;          // всегда 0x00 
    uint8_t zero6;          // всегда 0x00 
    uint8_t zero7;          // всегда 0x00 
    uint8_t zero8;          // всегда 0x00 
    uint8_t zero9;          // всегда 0x00 
    uint8_t zero10;         // всегда 0x00 
    uint8_t zero11;         // всегда 0x00 
    uint8_t zero12;         // всегда 0x00 
    uint8_t zero13;         // всегда 0x00 
    uint8_t zero14;         // всегда 0x00 
    uint8_t zero15;         // всегда 0x00 
    uint8_t zero16;         // всегда 0x00 
    uint8_t ambient_temperature_frac;   // дробная часть комнатной температуры воздуха с датчика на внутреннем блоке сплит-системы
                                        //      подробнее смотреть ambient_temperature_int
};

// тело малого информационного пакета
struct packet_small_info_body_t {
    uint8_t byte_01;        // не расшифрован, всегда 0x01
    uint8_t cmd_answer;     // код команды, ответом на которую пришел данный пакет (0x11);
                            //   в пакетах сплита другие варианты не встречаются
                            //   в отправляемых wifi-модулем пакетах тут может быть 0x01, если требуется установить режим работы 
    uint8_t target_temp_int_and_v_louver;   // целая часть целевой температуры и положение вертикальных жалюзи
                                            //      три младших бита - положение вертикальных жалюзи
                                            //          если они все = 0, то вертикальный SWING включен
                                            //          если они все = 1, то выключен вертикальный SWING
                                            //          протокол универсильный, другие комбинации битов могут задавать какие-то положения
                                            //          вертикальных жалюзи, но у меня на пульте таких возможностей нет, надо экспериментировать.
                                            //      пять старших бит - целая часть целевой температуры
                                            //      температура определяется по формуле:
                                            //          8 + (target_temp_int_and_v_louver >> 3) + (0.5 * (target_temp_frac >> 7))
    uint8_t h_louver;       // старшие 3 бита - положение горизонтальных жалюзи, остальное не изучено и всегда было 0
                            //      если все 3 бита = 0, то горизонтальный SWING включен
                            //      если все 3 бита = 1, то горизонтальный SWING отключен
                            //      надо изучить другие комбинации
    uint8_t target_temp_frac;               // старший бит - дробная часть целевой температуры
                                            //      остальные биты до конца не изучены:
                                            //      бит 6 был всегда 0
                                            //      биты 0..5 растут на 1 каждую минуту, возможно внутренний таймер для включения/выключения по времени
    uint8_t fan_speed;      // три старших бита - скорость вентилятора, остальные биты не известны
                            // AUTO = 0xA0, LOW = 0x60, MEDIUM = 0x40, HIGH = 0x20 
    uint8_t fan_turbo_and_mute;             // бит 7 = режим TURBO, бит 6 - режим MUTE; остальные не известны
    uint8_t mode;           // режим работы сплита:
                            //      AUTO : bits[7, 6, 5] = [0, 0, 0] 
                            //      COOL : bits[7, 6, 5] = [0, 0, 1] 
                            //      DRY  : bits[7, 6, 5] = [0, 1, 0] 
                            //      HEAT : bits[7, 6, 5] = [1, 0, 1] 
                            //      FAN  : bits[7, 6, 5] = [1, 1, 1]
                            //      Sleep function : bit 2 = 1
                            //      iFeel function : bit 3 = 1
    uint8_t zero1;          // всегда 0x00 
    uint8_t zero2;          // всегда 0x00 
    uint8_t status;         // бит 5 = 1: включен, обычный режим работы (когда можно включить нагрев, охлаждение и т.п.)
                            //      бит 2 = 1: режим самоочистки, должен запускаться только при бит 5 = 0
                            //      бит 0 и бит 1: активация режима ионизатора воздуха (не проверен, у меня его нет)
    uint8_t zero3;          // всегда 0x00 
    uint8_t display_and_mildew;     // бит4 = 1, чтобы погасить дисплей на внутреннем блоке сплита
                                    // бит3 = 1, чтобы включить функцию "антиплесень" (после отключения как-то прогревает или просушивает теплообменник, чтобы на нем не росла плесень) 
    uint8_t zero4;          // всегда 0x00 
    uint8_t target_temp_frac2;              // дробная часть целевой температуры, может быть только 0x00 и 0x05
                                            //      при установке температуры тут 0x00, а заданная температура передается в target_temp_int_and_v_louver и target_temp_frac
                                            //      после установки сплит в информационных пакетах тут начинает показывать дробную часть
                                            //      не очень понятно, зачем так сделано
};



//****************************************************************************************************************************************************
//*************************************************** ПАРАМЕТРЫ РАБОТЫ КОНДИЦИОНЕРА ******************************************************************
//****************************************************************************************************************************************************
// для всех параметров ниже вариант X_UNTOUCHED = 0xFF означает, что этот параметр команды должен остаться тот, который уже установлен
// питание кондиционера
#define AC_POWER_MASK    0b00100000
enum ac_power : uint8_t { AC_POWER_OFF = 0x00, AC_POWER_ON = 0x20, AC_POWER_UNTOUCHED = 0xFF };

// режим очистки кондиционера, включается (или должен включаться) при AC_POWER_OFF
#define AC_CLEAN_MASK    0b00000100
enum ac_clean : uint8_t { AC_CLEAN_OFF = 0x00, AC_CLEAN_ON = 0x04, AC_CLEAN_UNTOUCHED = 0xFF };

// ФУНКЦИЯ НЕ ПРОВЕРЕНА! Ионизатора на моем кондиционере нет, поэтому проверить возможности нет.
// для включения ионизатора нужно установить второй бит в байте
// по результату этот бит останется установленным, но кондиционер еще и установит первый бит 
#define AC_HEALTH_MASK    0b00000010
enum ac_health : uint8_t { AC_HEALTH_OFF = 0x00, AC_HEALTH_ON = 0x02, AC_HEALTH_UNTOUCHED = 0xFF };
// Возможно, статус ионизатора. А может говорит не о включении, а об ошибке включения...
#define AC_HEALTH_STATUS_MASK    0b00000001
enum ac_health_status : uint8_t { AC_HEALTH_STATUS_OFF = 0x00, AC_HEALTH_STATUS_ON = 0x01, AC_HEALTH_STATUS_UNTOUCHED = 0xFF };

// целевая температура
#define AC_TEMP_TARGET_INT_PART_MASK    0b11111000
#define AC_TEMP_TARGET_FRAC_PART_MASK    0b10000000

// основные режимы работы кондиционера
#define AC_MODE_MASK    0b11100000
enum ac_mode : uint8_t { AC_MODE_AUTO = 0x00, AC_MODE_COOL = 0x20, AC_MODE_DRY = 0x40, AC_MODE_HEAT = 0x80, AC_MODE_FAN = 0xC0, AC_MODE_UNTOUCHED = 0xFF };

// Ночной режим (SLEEP). Комбинируется только с режимами COOL и HEAT. Автоматически выключается через 7 часов.
// COOL: температура +1 градус через час, еще через час дополнительные +1 градус, дальше не меняется.
// HEAT: температура -2 градуса через час, еще через час дополнительные -2 градуса, дальше не меняется.
// Восстанавливается ли температура через 7 часов при отключении режима - не понятно.
#define AC_SLEEP_MASK    0b00000100
enum ac_sleep : uint8_t { AC_SLEEP_OFF = 0x00, AC_SLEEP_ON = 0x04, AC_SLEEP_UNTOUCHED = 0xFF };

// функция iFeel - поддерживате температуру по датчику в пульте ДУ, а не во внутреннем блоке кондиционера
#define AC_IFEEL_MASK    0b00001000
enum ac_ifeel : uint8_t { AC_IFEEL_OFF = 0x00, AC_IFEEL_ON = 0x08, AC_IFEEL_UNTOUCHED = 0xFF };

// Вертикальные жалюзи. В протоколе зашита возможность двигать ими по всякому, но додлжна быть такая возможность на уровне железа.
#define AC_LOUVERV_MASK    0b00000111
enum ac_louver_V : uint8_t { AC_LOUVERV_SWING_UPDOWN = 0x00,  AC_LOUVERV_OFF = 0x07, AC_LOUVERV_UNTOUCHED = 0xFF }; // ToDo: надо протестировать значения 0x01, 0x02, 0x03, 0x04, 0x05, 0x06

// Горизонтальные жалюзи. В протоколе зашита возможность двигать ими по всякому, но додлжна быть такая возможность на уровне железа.
#define AC_LOUVERH_MASK    0b11100000
enum ac_louver_H : uint8_t { AC_LOUVERH_SWING_LEFTRIGHT = 0x00,  AC_LOUVERH_OFF = 0xE0, AC_LOUVERH_UNTOUCHED = 0xFF }; // ToDo: надо протестировать значения 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0

struct ac_louver {
    ac_louver_H louver_h;
    ac_louver_V louver_v;
};

// скорость вентилятора
#define AC_FANSPEED_MASK    0b11100000
enum ac_fanspeed : uint8_t { AC_FANSPEED_HIGH = 0x20, AC_FANSPEED_MEDIUM = 0x40, AC_FANSPEED_LOW = 0x60, AC_FANSPEED_AUTO = 0xA0, AC_FANSPEED_UNTOUCHED = 0xFF };

// TURBO работает только в режимах COOL и HEAT
#define AC_FANTURBO_MASK    0b01000000
enum ac_fanturbo : uint8_t { AC_FANTURBO_OFF = 0x00, AC_FANTURBO_ON = 0x40, AC_FANTURBO_UNTOUCHED = 0xFF };

// MUTE работает только в режиме FAN. В режиме COOL кондей команду принимает, но MUTE не устанавливается
#define AC_FANMUTE_MASK    0b10000000
enum ac_fanmute : uint8_t { AC_FANMUTE_OFF = 0x00, AC_FANMUTE_ON = 0x80, AC_FANMUTE_UNTOUCHED = 0xFF };

// включение-выключение дисплея на корпусе внутреннего блока
#define AC_DISPLAY_MASK    0b00010000
enum ac_display : uint8_t { AC_DISPLAY_ON = 0x00, AC_DISPLAY_OFF = 0x10, AC_DISPLAY_UNTOUCHED = 0xFF };

// включение-выключение функции "Антиплесень". 
#define AC_MILDEW_MASK    0b00001000
enum ac_mildew : uint8_t { AC_MILDEW_OFF = 0x00, AC_MILDEW_ON = 0x08, AC_MILDEW_UNTOUCHED = 0xFF };

/** команда для кондиционера
 * 
 * ВАЖНО! В коде используется копирование команд простым присваиванием.
 * Если в структуру будут введены указатели, то копирование надо будет изменить!
*/
struct ac_command_t {
    ac_power    power;
    float       temp_target;
    bool        temp_target_matter; // показывает, задана ли температура. Если false, то оставляем уже установленную
    float       temp_ambient;
    float       temp_outdoor;
    ac_clean    clean;
    ac_health   health;
    ac_health_status   health_status;
    ac_mode     mode;
    ac_sleep    sleep;
    ac_ifeel    iFeel;
    ac_louver   louver;
    ac_fanspeed fanSpeed;
    ac_fanturbo fanTurbo;
    ac_fanmute  fanMute;
    ac_display  display;
    ac_mildew   mildew;
};

typedef ac_command_t ac_state_t;  // текущее состояние параметров кондея можно хранить в таком же формате, как и комманды

//****************************************************************************************************************************************************
//************************************************ КОНЕЦ ПАРАМЕТРОВ РАБОТЫ КОНДИЦИОНЕРА **************************************************************
//****************************************************************************************************************************************************




/*****************************************************************************************************************************************************
 *                                      структуры и типы для последовательности команд
 *****************************************************************************************************************************************************
 * 
 * Последовательность команд позволяет выполнить несколько последовательных команд с контролем получаемых в ответ пакетов.
 * Если требуется, в получаемых в ответ пакетах пожно контролировать значение любых байт.
 * Для входящего пакета байт, значение которого не проверяется, должен быть установлен в AC_SEQUENCE_ANY_BYTE.
 * Контроль возможен только для входящих пакетов, исходящие отправляются "как есть".
 * 
 * Для исходящих пакетов значения CRC могут не рассчитываться, контрольная сумма будет рассчитана автоматически.
 * Для входящих пакетов значение CRC также можно не рассчитывать, установив байты CRC в AC_SEQUENCE_ANY_BYTE,
 * так как контроль CRC для получаемых пакетов выполняется автоматически при получении.
 * 
 * Для входящих пакетов в последовательности можно указать таймаут. Если таймаут равен 0, то используется значение AC_SEQUENCE_DEFAULT_TIMEOUT.
 * Если в течение указанного времени подходящий пакет не будет получен, то последовательность прерывается с ошибкой. 
 * Пинг-пакеты в последовательности игнорируются.
 * 
 * Пауза в последовательности задается значением timeout элемента AC_DELAY. Никакие другие параметры такого элемента можно не заполнять.
 * 
 **/
// максимальная длина последовательности; больше вроде бы не требовалось
#define AC_SEQUENCE_MAX_LEN     0x0F

// в пакетах никогда не встречалось значение 0xFF (только в CRC), поэтому решено его использовать как признак не важного значение байта
//#define AC_SEQUENCE_ANY_BYTE    0xFF

// дефолтный таймаут входящего пакета в миллисекундах
// если для входящего пакета в последовательности указан таймаут 0, то используется значение по-умолчанию
// если нужный пакет не поступил в течение указанного времени, то последовательность прерывается с ошибкой
#define AC_SEQUENCE_DEFAULT_TIMEOUT 500

enum sequence_item_type_t : uint8_t {
    AC_SIT_NONE     = 0x00,     // пустой элемент последовательности
    AC_SIT_DELAY    = 0x01,     // пауза в последовательности на нужное количество миллисекунд
    AC_SIT_FUNC     = 0x02      // рабочий элемент последовательности
};

// тип пакета в массиве последовательности
// информирует о том, что за пакет лежит в поле packet элемента последовательности
enum sequence_packet_type_t : uint8_t {
    AC_SPT_CLEAR            = 0x00,     // пустой пакет
    AC_SPT_RECEIVED_PACKET  = 0x01,     // полученный пакет
    AC_SPT_SENT_PACKET      = 0x02      // отправленный пакет
};

// элемент последовательности
struct sequence_item_t {
    sequence_item_type_t    item_type;      // тип элемента последовательности
    bool                    (AirCon::*func)();      // указатель на функцию, отрабатывающую шаг последовательности
    uint16_t                timeout;        // допустимый таймаут в ожидании пакета (применим только для входящих пакетов)
    uint32_t                msec;           // время старта текущего шага последовательности (для входящего пакета и паузы)
    sequence_packet_type_t  packet_type;    // тип пакета (входящий, исходящий или вовсе не пакет)
    packet_t                packet;         // данные пакета
    ac_command_t            cmd;            // новое состояние сплита, нужно для передачи кондиционеру команд
};
/*****************************************************************************************************************************************************/


class AirCon : public Component, public Climate {
    private:
        // время последнего запроса статуса у кондея
        uint32_t _dataMillis;

        // использую в дебажных задачах, чтобы разово выполнять какие-то запросы
        uint8_t _cnt;

        // состояние конечного автомата
        acsm_state _ac_state = ACSM_IDLE;

        // текущее состояние задаваемых пользователем параметров системы
        ac_state_t _current_ac_state;

        // флаг подключения к UART
        bool _hw_initialized = false;
        // указатель на UART, по которому общаемся с кондиционером
        UARTComponent *_ac_serial;

        // входящий и исходящий пакеты
        packet_t _inPacket;
        packet_t _outPacket;

        // последовательность пакетов текущий шаг в последовательности
        sequence_item_t _sequence[AC_SEQUENCE_MAX_LEN];
        uint8_t _sequence_current_step;

        // флаг успешного выполнения стартовой последовательности команд
        bool _startupSequenceComlete = false;

        // очистка последовательности команд
        void _clearSequence(){
            for (uint8_t i = 0; i < AC_SEQUENCE_MAX_LEN; i++) {
                _sequence[i].item_type = AC_SIT_NONE;
                _sequence[i].func = nullptr;
                _sequence[i].timeout = 0;
                _sequence[i].msec = 0;
                _sequence[i].packet_type = AC_SPT_CLEAR;
                _clearPacket(&_sequence[i].packet);
                _clearCommand(&_sequence[i].cmd);
            }
            _sequence_current_step = 0;
        }

        // выполняет всю логику очередного шага последовательности команд
        void _doSequence(){
            if (!hasSequence()) return;

            // если шаг уже максимальный из возможных
            if (_sequence_current_step >= AC_SEQUENCE_MAX_LEN) {
                // значит последовательность закончилась, надо её очистить
                // при очистке последовательности будет и _sequence_current_step обнулён
                _debugMsg(F("Sequence [step %u]: maximum step reached"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _sequence_current_step);
                _clearSequence();
                return;
            }

            // смотрим тип текущего элемента в последовательности
            switch (_sequence[_sequence_current_step].item_type) {
                case AC_SIT_FUNC: {
                    // если указатель на функцию пустой, то прерываем последовательность
                    if (_sequence[_sequence_current_step].func == nullptr) {
                        _debugMsg(F("Sequence [step %u]: function pointer is NULL, sequence broken"), ESPHOME_LOG_LEVEL_WARN, __LINE__, _sequence_current_step);
                        _clearSequence();
                        return;
                    }

                    // сохраняем время начала паузы
                    if (_sequence[_sequence_current_step].msec == 0) {
                        _sequence[_sequence_current_step].msec = millis();
                        _debugMsg(F("Sequence [step %u]: step started"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _sequence_current_step);
                    }

                    // если таймаут не указан, берем значение по-умолчанию
                    if (_sequence[_sequence_current_step].timeout == 0 ) _sequence[_sequence_current_step].timeout = AC_SEQUENCE_DEFAULT_TIMEOUT;

                    // если время вышло, то отчитываемся в лог и очищаем последовательность
                    if (millis() - _sequence[_sequence_current_step].msec >= _sequence[_sequence_current_step].timeout) {
                        _debugMsg(F("Sequence  [step %u]: step timed out (%u ms)"), ESPHOME_LOG_LEVEL_WARN, __LINE__, _sequence_current_step, millis() - _sequence[_sequence_current_step].msec);
                        _clearSequence();
                        return;
                    }

                    // можно вызывать функцию
                    // она самомтоятельно загружает отправляемые/полученные пакеты в packet последовательности
                    // а также самостоятельно увеличивает счетчик шагов последовательности _sequence_current_step
                    // единственное исключение - таймауты
                    if (!(this->*_sequence[_sequence_current_step].func)()) {
                        _debugMsg(F("Sequence  [step %u]: error was occur in step function"), ESPHOME_LOG_LEVEL_WARN, __LINE__, _sequence_current_step, millis() - _sequence[_sequence_current_step].msec);
                        _clearSequence();
                        return;
                    }
                    break;
                }

                case AC_SIT_DELAY: {    // это пауза в последовательности
                    // пауза задается параметром timeout элемента последовательности
                    // начало паузы сохраняется в параметре msec
                    
                    // сохраняем время начала паузы
                    if (_sequence[_sequence_current_step].msec == 0) {
                        _sequence[_sequence_current_step].msec = millis();
                        _debugMsg(F("Sequence [step %u]: begin delay (%u ms)"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _sequence_current_step, _sequence[_sequence_current_step].timeout);
                    }

                    // если время вышло, то переходим на следующий шаг
                    if (millis() - _sequence[_sequence_current_step].msec >= _sequence[_sequence_current_step].timeout) {
                        _debugMsg(F("Sequence  [step %u]: delay culminated (plan = %u ms, fact = %u ms)"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _sequence_current_step, _sequence[_sequence_current_step].timeout, millis() - _sequence[_sequence_current_step].msec);
                        _sequence_current_step++;
                    }
                    break;
                }

                case AC_SIT_NONE:   // шаги закончились
                default:            // или какой-то мусор в последовательности
                    // надо очистить последовательность и уходить
                    _debugMsg(F("Sequence [step %u]: sequence complete"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _sequence_current_step);
                    _clearSequence();
                    break;
            }
        }

        // заполняет структуру команды нейтральными значениями
        void _clearCommand(ac_command_t * cmd){
            cmd->clean = AC_CLEAN_UNTOUCHED;
            cmd->display = AC_DISPLAY_UNTOUCHED;
            cmd->fanMute = AC_FANMUTE_UNTOUCHED;
            cmd->fanSpeed = AC_FANSPEED_UNTOUCHED;
            cmd->fanTurbo = AC_FANTURBO_UNTOUCHED;
            cmd->health = AC_HEALTH_UNTOUCHED;
            cmd->health_status = AC_HEALTH_STATUS_UNTOUCHED;
            cmd->iFeel = AC_IFEEL_UNTOUCHED;
            cmd->louver.louver_h = AC_LOUVERH_UNTOUCHED;
            cmd->louver.louver_v = AC_LOUVERV_UNTOUCHED;
            cmd->mildew = AC_MILDEW_UNTOUCHED;
            cmd->mode = AC_MODE_UNTOUCHED;
            cmd->power = AC_POWER_UNTOUCHED;
            cmd->sleep = AC_SLEEP_UNTOUCHED;
            cmd->temp_target = 0;
            cmd->temp_target_matter = false;
            cmd->temp_ambient = 0;
            cmd->temp_outdoor = 0;
        };

        // очистка буфера размером AC_BUFFER_SIZE
        void _clearBuffer(uint8_t * buf){
            memset(buf, 0, AC_BUFFER_SIZE);
        }

        // очистка структуры пакета по указателю
        void _clearPacket(packet_t * pckt){
            if (pckt == nullptr) {
                _debugMsg(F("Clear packet error: pointer is NULL!"), ESPHOME_LOG_LEVEL_ERROR, __LINE__);
                return;
            }
            pckt->crc = nullptr;
            pckt->header = (packet_header_t *)(pckt->data);    // заголовок же всегда стартует с начала пакета
            pckt->msec = 0;
            pckt->bytesLoaded = 0;
            pckt->body = nullptr;
            _clearBuffer(pckt->data);
        }

        // очистка входящего пакета
        void _clearInPacket(){
            _clearPacket(&_inPacket);
        }

        // очистка исходящего пакета
        void _clearOutPacket(){
            _clearPacket(&_outPacket);
            _outPacket.header->start_byte = AC_PACKET_START_BYTE;   // для исходящего сразу ставим стартовый байт
            _outPacket.header->wifi = AC_PACKET_ANSWER;             // для исходящего пакета сразу ставим признак ответа
        }

        // копирует пакет из одной структуры в другую с корректным переносом указателей на заголовки и т.п.
        void _copyPacket(packet_t *dest, packet_t *src){
            if (dest == nullptr) return;
            if (src == nullptr) return;

            dest->msec = src->msec;
            dest->bytesLoaded = src->bytesLoaded;
            memcpy(dest->data, src->data, AC_BUFFER_SIZE);
            dest->header = (packet_header_t *)&dest->data;
            if (dest->header->body_length > 0) dest->body = &dest->data[AC_HEADER_SIZE];
            dest->crc = (packet_crc_t *)&dest->data[AC_HEADER_SIZE + dest->header->body_length];
        }

       // устанавливает состояние конечного автомата
       // можно и напрямую устанавливать переменную, но для целей отладки лучше так
       void _setStateMachineState(acsm_state state = ACSM_IDLE){
            if (_ac_state == state) return;  // состояние не меняется

            _ac_state = state;

            switch (state) {
                case ACSM_IDLE:
                    _debugMsg(F("State changed to ACSM_IDLE."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);
                    break;

                case ACSM_RECEIVING_PACKET:
                    _debugMsg(F("State changed to ACSM_RECEIVING_PACKET."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);
                    break;
                    
                case ACSM_PARSING_PACKET:
                    _debugMsg(F("State changed to ACSM_PARSING_PACKET."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);
                    break;
                    
                case ACSM_SENDING_PACKET:
                    _debugMsg(F("State changed to ACSM_SENDING_PACKET."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);
                    break;
                
                default:
                    _debugMsg(F("State changed to ACSM_IDLE by default. Given state is %02X."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, state);
                    _ac_state = ACSM_IDLE;
                    break;
            }
       }

        // состояние конечного автомата: IDLE
        void _doIdleState(){
            // вначале нужно выполнить очередной шаг последовательности команд
            _doSequence();

            // Если нет входящих данных, значит можно отправить исходящий пакет, если он есть
            if (_ac_serial->available() == 0) {
                // если есть пакет на отправку, то надо отправлять
                // вначале думал, что сейчас отправка пакетов тут не нужна, т.к. состояние ACSM_SENDING_PACKET устанавливается сразу в парсере пакетов
                // но потом понял, что у нас пакеты уходят не только когда надо отвечать, но и мы можем быть инициаторами
                // поэтому вызов отправки тут пригодится
                if (_outPacket.msec > 0) _setStateMachineState(ACSM_SENDING_PACKET);
                // иначе просто выходим
                return;
            };


            if (_ac_serial->peek() == AC_PACKET_START_BYTE) {
                // если во входящий пакет что-то уже загружено, значит это какие-то ошибочные данные или неизвестные пакеты
                // надо эту инфу вывалить в лог
                if (_inPacket.bytesLoaded > 0){
                    _debugPrintPacket(&_inPacket, ESPHOME_LOG_LEVEL_DEBUG, __LINE__);
                }
                _clearInPacket();
                _inPacket.msec = millis();
                _setStateMachineState(ACSM_RECEIVING_PACKET);
                //******************************************** экспериментальная секция *************************************************************
                // пробуем сократить время ответа с помощью прямых вызовов обработчиков, а не через состояние IDLE
                //_doReceivingPacketState();
                // получилось всё те же 123 мсек. Только изредка падает до 109 мсек. Странно.
                // логический анализатор показал примерно то же время от начала запроса до окончания ответа.
                // запрос имеет длительность 18 мсек (лог.анализатор говорит 22,5 мсек).
                // ответ имеет длительность 41 мсек по лог.анализатору.
                // длительность паузы между запросом и ответом порядка 60 мсек.
                // Скорее всего за один вызов _doReceivingPacketState не удается загрузить весь пакет (на момент вызова не все байы поступили в буфер UART)
                // и поэтому программа отдает управление ESPHome для выполнения своих задач
                // Стоит ли переделать код наоборот для непрерывного выполнения всё время, пока ожидается посылка - не знаю. Может быть такой риалтайм и не нужен.
                //***********************************************************************************************************************************

            } else {
                while (_ac_serial->available() > 0)
                {
                    // если наткнулись на старт пакета, то выходим из while
                    if (_ac_serial->peek() == AC_PACKET_START_BYTE) break;

                    // читаем байт в буфер входящего пакета
                    _inPacket.data[_inPacket.bytesLoaded] = _ac_serial->read();
                    _inPacket.bytesLoaded++;

                    // если буфер уже полон, надо его вывалить в лог и очистить
                    if (_inPacket.bytesLoaded >= AC_BUFFER_SIZE){
                        _debugMsg(F("Some unparsed data on the bus:"), ESPHOME_LOG_LEVEL_DEBUG, __LINE__);
                        _debugPrintPacket(&_inPacket, ESPHOME_LOG_LEVEL_DEBUG, __LINE__);
                        _clearInPacket();
                    }
                }
            }
        };

        // состояние конечного автомата: ACSM_RECEIVING_PACKET
        void _doReceivingPacketState(){
            while (_ac_serial-> available() > 0) {
                // если в буфере пакета данных уже под завязку, то надо сообщить о проблеме и выйти
                if (_inPacket.bytesLoaded >= AC_BUFFER_SIZE) {
                    _debugMsg(F("Receiver: packet buffer overflow!"), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                    _debugPrintPacket(&_inPacket, ESPHOME_LOG_LEVEL_WARN, __LINE__);
                    _clearInPacket();
                    _setStateMachineState(ACSM_IDLE);
                    return;
                }

                _inPacket.data[_inPacket.bytesLoaded] = _ac_serial->read();
                _inPacket.bytesLoaded++;

                // данных достаточно для заголовка
                if (_inPacket.bytesLoaded == AC_HEADER_SIZE) {
                    // указатель заголовка установлен еще при обнулении пакета, его можно не трогать
                    //_inPacket.header = (packet_header_t *)(_inPacket.data);

                    // уже знаем размер пакета и можем установить указатели на тело пакета и CRC
                    _inPacket.crc = (packet_crc_t *)&(_inPacket.data[AC_HEADER_SIZE + _inPacket.header->body_length]);
                    if (_inPacket.header->body_length > 0) _inPacket.body = &(_inPacket.data[AC_HEADER_SIZE]);

                    _debugMsg(F("Header loaded: timestamp = %010u, start byte = %02X, packet type = %02X, body size = %02X"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _inPacket.msec, _inPacket.header->start_byte, _inPacket.header->packet_type, _inPacket.header->body_length);
                }

                // если все байты пакета загружены, надо его распарсить
                // максимальный по размеру пакет будет упираться в размер буфера. если такой пакет здесь не уйдет на парсинг,
                // то на следующей итерации будет ошибка о переполнении буфера, которая в начале цикла while
                if (_inPacket.bytesLoaded == AC_HEADER_SIZE + _inPacket.header->body_length + 2) {
                    _debugMsg(F("Packet loaded: timestamp = %010u, start byte = %02X, packet type = %02X, body size = %02X, crc = [%02X, %02X]."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _inPacket.msec, _inPacket.header->start_byte, _inPacket.header->packet_type, _inPacket.header->body_length, _inPacket.crc->crc[0], _inPacket.crc->crc[1]);
                    _debugMsg(F("Loaded %02u bytes for a %u ms."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _inPacket.bytesLoaded, (millis() - _inPacket.msec));
                    _debugPrintPacket(&_inPacket, ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);
                    _setStateMachineState(ACSM_PARSING_PACKET);
                    //******************************************** экспериментальная секция ********************************************************
                    // стараемся сократить лаг между запросом и ответом
                    //_doParsingPacket();
                    // если так (проходить без захода в состояние IDLE), то время сокращается до 123 мсек.
                    //******************************************************************************************************************************
                    return;
                }
            }

            // если пакет не загружен, а время вышло, то надо вернуться в IDLE
            if (millis() - _inPacket.msec >= AC_PACKET_TIMEOUT) {
                _debugMsg(F("Receiver: packet timed out!"), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                _debugPrintPacket(&_inPacket, ESPHOME_LOG_LEVEL_WARN, __LINE__);
                _clearInPacket();
                _setStateMachineState(ACSM_IDLE);
                return;
            }
        };

        // состояние конечного автомата: ACSM_PARSING_PACKET
        void _doParsingPacket(){
            if (!_checkCRC(&_inPacket)) {
                _debugMsg(F("Parser: packet CRC fail!"), ESPHOME_LOG_LEVEL_ERROR, __LINE__);
                _debugPrintPacket(&_inPacket, ESPHOME_LOG_LEVEL_ERROR, __LINE__);
                _clearInPacket();
                _setStateMachineState(ACSM_IDLE);
                return;
            }

            bool stateChangedFlag = false;  // флаг, показывающий, изменилось ли состояние кондиционера
            uint8_t stateByte = 0;          // переменная для временного сохранения текущих параметров сплита для проверки их изменения
            float stateFloat = 0.0;         // переменная для временного сохранения текущих параметров сплита для проверки их изменения

            // вначале выводим полученный пакет в лог, чтобы он шел до информации об ответах и т.п.
            _debugPrintPacket(&_inPacket, ESPHOME_LOG_LEVEL_DEBUG, __LINE__);

            // разбираем тип пакета
            switch (_inPacket.header->packet_type) {
                case AC_PTYPE_PING: { // ping-пакет, рассылается кондиционером каждые 3 сек.; модуль на него отвечает
                    _debugMsg(F("Parser: ping packet received"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);
                    // надо отправлять ответ на пинг
                    _clearOutPacket();
                    _outPacket.msec = millis();
                    //_outPacket.msec = _inPacket.msec;   // делал так, чтобы посмотреть задуржку между запросом и ответом; получилось от начала запроса до отправки ответа порядка 165 мсек., если отправка идет не сразу, а через состояние IDLE
                    //_outPacket.header->start_byte = AC_PACKET_START_BYTE; // не нужно, уже при обнудении исходящего пакета поставили
                    _outPacket.header->packet_type = AC_PTYPE_PING;
                    //_outPacket.header->wifi = AC_PACKET_ANSWER;   // не нужно, уже при обнудении исходящего пакета поставили
                    _outPacket.header->ping_answer_01 = 0x01;       // только в ответе на пинг этот байт равен 0x01; что означает не ясно
                    _outPacket.header->body_length = 8;             // в ответе на пинг у нас тело 8 байт
                    _outPacket.body = &(_outPacket.data[AC_HEADER_SIZE]);

                    // заполняем тело пакета
                    packet_ping_answer_body_t * ping_body;
                    ping_body = (packet_ping_answer_body_t *) (_outPacket.body);
                    ping_body->byte_1C = 0x1C;
                    ping_body->byte_27 = 0x27;

                    // расчет контрольной суммы и прописывание её в пакет
                    _outPacket.crc = (packet_crc_t *)&(_outPacket.data[AC_HEADER_SIZE + _outPacket.header->body_length]);
                    _setCRC16(&_outPacket);
                    _outPacket.bytesLoaded = AC_HEADER_SIZE + _outPacket.header->body_length + 2;

                    _debugMsg(F("Parser: generated ping answer. Waiting for sending."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);
                    
                    // до отправки пинг-ответа проверяем, не выполнялась ли стартовая последовательность команд
                    // по задумке она выполняется после подключения к кондиционеру после ответа на первый пинг
                    // нужна для максимально быстрого определния текущих параметров кондиционера
                    if (!_startupSequenceComlete){
                        _startupSequenceComlete = startupSequence();
                    }
                    
                    // изначально предполагал, что передачу пакета на отправку выполнит обработчик IDLE, но показалось, что слишком долго
                    // логика отправки через IDLE в том, что получение запросов может быть важнее отправки ответов и IDLE позволяет реализовать такой приоритет
                    //_setStateMachineState(ACSM_IDLE);
                    // но потом решил всё же напрямую отправлять в отправку
                    // в этом случае пинг-ответ заканчивает отправку спустя 144 мсек после стартового байта пинг-запроса
                    _setStateMachineState(ACSM_SENDING_PACKET);
                    // решил провести эксперимент
                    //******************************************** экспериментальная секция ***************************************************************
                    // получилось от начала запроса до отправки ответа порядка 165 мсек., если отправка идет не сразу, а через состояние IDLE
                    // Если сразу отсюда отправляться в обработчик отправки, то время сокращается до 131 мсек. Основные потери идут до входа в парсер пакетов
                    //_setStateMachineState(ACSM_SENDING_PACKET);
                    //_doSendingPacketState();
                    //_clearInPacket();
                    //return;
                    //*************************************************************************************************************************************
                    break;
                }
                
                case AC_PTYPE_CMD:  { // команда сплиту; модуль отправляет такие команды, когда что-то хочет от сплита
                    //  сплит такие команды отправлять не должен, поэтому жалуемся в лог
                    _debugMsg(F("Parser: packet type=0x06 received. This isn't expected."), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                    // очищаем пакет
                    _clearInPacket();
                    _setStateMachineState(ACSM_IDLE);
                    break;
                }
                
                case AC_PTYPE_INFO: { // информационный пакет; бывает 3 видов; один из них рассылается кондиционером самостоятельно раз в 10 мин. и все 3 могут быть ответом на запросы модуля
                    // смотрим тип поступившего пакета по второму байту тела
                    _debugMsg(F("Parser: status packet received"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);
                    switch (_inPacket.body[1]) {
                        case AC_CMD_STATUS_SMALL:   { // маленький пакет статуса кондиционера
                            _debugMsg(F("Parser: status packet type = small"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);
                            stateChangedFlag = false;

                            // будем обращаться к телу пакета через указатель на структуру
                            packet_small_info_body_t * small_info_body;
                            small_info_body = (packet_small_info_body_t *) (_inPacket.body);
                            
                            // в малом пакете передается большое количество установленных пользователем параметров работы
                            stateFloat = 8 + (small_info_body->target_temp_int_and_v_louver >> 3) + 0.5*(float)(small_info_body->target_temp_frac >> 7);
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.temp_target != stateFloat);
                            _current_ac_state.temp_target = stateFloat;
                            _current_ac_state.temp_target_matter = true;

                            stateByte = small_info_body->target_temp_int_and_v_louver & AC_LOUVERV_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.louver.louver_v != (ac_louver_V)stateByte);
                            _current_ac_state.louver.louver_v = (ac_louver_V)stateByte;
                            //_current_ac_state.louver.louver_v = (ac_louver_V)(small_info_body->target_temp_int_and_v_louver & AC_LOUVERV_MASK);

                            stateByte = small_info_body->h_louver & AC_LOUVERH_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.louver.louver_h != (ac_louver_H)stateByte);
                            _current_ac_state.louver.louver_h = (ac_louver_H)stateByte;
                            //_current_ac_state.louver.louver_h = (ac_louver_H)(small_info_body->h_louver & AC_LOUVERH_MASK);

                            stateByte = small_info_body->fan_speed & AC_FANSPEED_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.fanSpeed != (ac_fanspeed)stateByte);
                            _current_ac_state.fanSpeed = (ac_fanspeed)stateByte;
                            //_current_ac_state.fanSpeed = (ac_fanspeed)(small_info_body->fan_speed & AC_FANSPEED_MASK);

                            stateByte = small_info_body->fan_turbo_and_mute & AC_FANTURBO_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.fanTurbo != (ac_fanturbo)stateByte);
                            _current_ac_state.fanTurbo = (ac_fanturbo)stateByte;
                            //_current_ac_state.fanTurbo = (ac_fanturbo)(small_info_body->fan_turbo_and_mute & AC_FANTURBO_MASK);

                            stateByte = small_info_body->fan_turbo_and_mute & AC_FANMUTE_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.fanMute != (ac_fanmute)stateByte);
                            _current_ac_state.fanMute = (ac_fanmute)stateByte;
                            //_current_ac_state.fanMute = (ac_fanmute)(small_info_body->fan_turbo_and_mute & AC_FANMUTE_MASK);
                            
                            stateByte = small_info_body->mode & AC_MODE_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.mode != (ac_mode)stateByte);
                            _current_ac_state.mode = (ac_mode)stateByte;
                            //_current_ac_state.mode = (ac_mode)(small_info_body->mode & AC_MODE_MASK);
                            
                            stateByte = small_info_body->mode & AC_SLEEP_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.sleep != (ac_sleep)stateByte);
                            _current_ac_state.sleep = (ac_sleep)stateByte;
                            //_current_ac_state.sleep = (ac_sleep)(small_info_body->mode & AC_SLEEP_MASK);
                            
                            stateByte = small_info_body->mode & AC_IFEEL_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.iFeel != (ac_ifeel)stateByte);
                            _current_ac_state.iFeel = (ac_ifeel)stateByte;
                            //_current_ac_state.iFeel = (ac_ifeel)(small_info_body->mode & AC_IFEEL_MASK);
                            
                            stateByte = small_info_body->status & AC_POWER_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.power != (ac_power)stateByte);
                            _current_ac_state.power = (ac_power)stateByte;
                            //_current_ac_state.power = (ac_power)(small_info_body->status & AC_POWER_MASK);
                            
                            stateByte = small_info_body->status & AC_HEALTH_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.health != (ac_health)stateByte);
                            _current_ac_state.health = (ac_health)stateByte;
                            //_current_ac_state.health = (ac_health)(small_info_body->status & AC_HEALTH_MASK);
                            
                            stateByte = small_info_body->status & AC_HEALTH_STATUS_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.health_status != (ac_health_status)stateByte);
                            _current_ac_state.health_status = (ac_health_status)stateByte;
                            //_current_ac_state.health_status = (ac_health_status)(small_info_body->status & AC_HEALTH_STATUS_MASK);
                            
                            stateByte = small_info_body->status & AC_CLEAN_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.clean != (ac_clean)stateByte);
                            _current_ac_state.clean = (ac_clean)stateByte;
                            //_current_ac_state.clean = (ac_clean)(small_info_body->status & AC_CLEAN_MASK);
                            
                            stateByte = small_info_body->display_and_mildew & AC_DISPLAY_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.display != (ac_display)stateByte);
                            _current_ac_state.display = (ac_display)stateByte;
                            //_current_ac_state.display = (ac_display)(small_info_body->display_and_mildew & AC_DISPLAY_MASK);
                            
                            stateByte = small_info_body->display_and_mildew & AC_MILDEW_MASK;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.mildew != (ac_mildew)stateByte);
                            _current_ac_state.mildew = (ac_mildew)stateByte;
                            //_current_ac_state.mildew = (ac_mildew)(small_info_body->display_and_mildew & AC_MILDEW_MASK);

                            // уведомляем об изменении статуса сплита
                            if (stateChangedFlag) stateChanged();
                            break;
                        }
                        
                        case AC_CMD_STATUS_BIG:         // большой пакет статуса кондиционера
                        case AC_CMD_STATUS_PERIODIC:  { // раз в 10 минут разсылается сплитом, структура аналогична большому пакету статуса
                            // вроде как AC_CMD_STATUS_PERIODIC могут быть и с другими кодами, но пока забъю на это
                            _debugMsg(F("Parser: status packet type = big or periodic"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);
                            stateChangedFlag = false;

                            // будем обращаться к телу пакета через указатель на структуру
                            packet_big_info_body_t * big_info_body;
                            big_info_body = (packet_big_info_body_t *) (_inPacket.body);
                            
                            // температура воздуха в помещении по версии сплит-систему
                            stateFloat = big_info_body->ambient_temperature_int - 0x20 + (float)big_info_body->ambient_temperature_frac/10.0;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.temp_ambient != stateFloat);
                            _current_ac_state.temp_ambient = stateFloat;
                            
                            // некая температура из наружного блока, скорее всего температура испарителя
                            // TODO: формула расчета неправильная! Нужно исследовать на опыте, какая температура при каких условиях
                            stateFloat = big_info_body->outdoor_temperature - 0x20;
                            stateChangedFlag = stateChangedFlag || (_current_ac_state.temp_outdoor != stateFloat);
                            _current_ac_state.temp_outdoor = stateFloat;

                            // уведомляем об изменении статуса сплита
                            if (stateChangedFlag) stateChanged();
                            break;
                        }
                        
                        case AC_CMD_SET_PARAMS: {   // такой статусный пакет присылается кондиционером в ответ на команду установки параметров
                            // в теле пакета нет ничего примечательного
                            // в байтах 2 и 3 тела похоже передается CRC пакета поступившей команды, на которую сплит отвечает
                            // но я решил этот момент не проверять и не контролировать
                            // корректную установку параметров можно определить, запросив статус кондиционера сразу после получения этой команды кондея
                            break;
                        }

                        default:
                            _debugMsg(F("Parser: status packet type = unknown (%02X)"), ESPHOME_LOG_LEVEL_WARN, __LINE__, _inPacket.body[1]);
                            break;
                    }
                    _setStateMachineState(ACSM_IDLE);
                    break;
                }
                
                case AC_PTYPE_INIT: // инициирующий пакет; присылается сплитом, если кнопка HEALTH на пульте нажимается 8 раз; как там и что работает - не разбирался.
                case AC_PTYPE_UNKN: // какой-то странный пакет, отправляемый пультом при инициации и иногда при включении питания... как работает и зачем нужен - не разбирался, сплит на него вроде бы не реагирует
                default:
                    // игнорируем. Для нашего случая эти пакеты не важны
                    _setStateMachineState(ACSM_IDLE);
                    break;
            }

            // если есть последовательность команд, то надо отработать проверку последовательности
            if (hasSequence()) _doSequence();
            
            // после разбора входящего пакета его надо очистить
            _clearInPacket();
        }

        // состояние конечного автомата: ACSM_SENDING_PACKET
        void _doSendingPacketState(){
            // если нет исходящего пакета, то выходим
            if ((_outPacket.msec == 0) || (_outPacket.crc == nullptr) || (_outPacket.bytesLoaded == 0)) {
                _debugMsg(F("Sender: no packet to send."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);
                //_clearOutPacket();    // смысла нет обнулять, пакет же пустой
                _setStateMachineState(ACSM_IDLE);
                return;
            }

            _debugMsg(F("Sender: sending packet."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);

            _ac_serial->write_array(_outPacket.data, _outPacket.bytesLoaded);
            _ac_serial->flush();

            _debugPrintPacket(&_outPacket, ESPHOME_LOG_LEVEL_DEBUG, __LINE__);
            _debugMsg(F("Sender: %u bytes sent (%u ms)."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _outPacket.bytesLoaded, millis()-_outPacket.msec);
            _clearOutPacket();

            _setStateMachineState(ACSM_IDLE);
        };

        /**
         * вывод отладочной информации в лог
         * 
         * dbgLevel - уровень сообщения, определен в ESPHome. За счет его использования можно из ESPHome управлять полнотой сведений в логе.
         * msg - сообщение, выводимое в лог
         * line - строка, на которой произошел вызов (удобно при отладке)
         */
        void _debugMsg(const String &msg, uint8_t dbgLevel = ESPHOME_LOG_LEVEL_DEBUG, unsigned int line = 0, ... ){
            if (dbgLevel < ESPHOME_LOG_LEVEL_NONE) dbgLevel = ESPHOME_LOG_LEVEL_NONE;
            if (dbgLevel > ESPHOME_LOG_LEVEL_VERY_VERBOSE) dbgLevel = ESPHOME_LOG_LEVEL_VERY_VERBOSE;

            // ***TODO*** Пока сделано через Ж: сообщение копируется в массив и потом выводится....
            // это костыль, чтобы передать неизвестное количество аргументов
            char _msg[128];
            msg.toCharArray(_msg, 128);
            
            if (line == 0) line = __LINE__; // если строка не передана, берем текущую строку

            va_list vl;
            va_start(vl, line);
            esp_log_vprintf_(dbgLevel, TAG, line, _msg, vl);
            va_end(vl);
        }

        /**
         * выводим данные пакета в лог для отладки
         * 
         * dbgLevel - уровень сообщения, определен в ESPHome. За счет его использования можно из ESPHome управлять полнотой сведений в логе.
         * packet - указатель на пакет для вывода;
         *          если указатель на crc равен nullptr или первый байт в буфере не AC_PACKET_START_BYTE, то считаем, что передан битый пакет
         *          или не пакет вовсе. Для такого выводим только массив байт.
         *          Для нормального пакета данные выводятся с форматированием. 
         * line - строка, на которой произошел вызов (удобно при отладке)
         **/
        void _debugPrintPacket(packet_t * packet, uint8_t dbgLevel = ESPHOME_LOG_LEVEL_DEBUG, unsigned int line = 0){
            // определяем, полноценный ли пакет нам передан
            bool notAPacket = false;
            // указатель заголовка всегда установден на начало буфера
            //notAPacket = notAPacket || (packet->header == nullptr);
            notAPacket = notAPacket || (packet->crc == nullptr);
            notAPacket = notAPacket || (packet->data[0] != AC_PACKET_START_BYTE);

            String st = "";
            char textBuf[10];

            // заполняем время получения пакета
            memset(textBuf, 0, 10);
            sprintf(textBuf, "%010u", packet->msec);
            st = st + textBuf + ": ";

            // формируем преамбулы
            if (packet == &_inPacket) {
                st += "[<=] ";      // преамбула входящего пакета
            } else if (packet == &_outPacket) {
                st += "[=>] ";      // преамбула исходящего пакета
            } else {
                st += "[--] ";      // преамбула для "непакета"
            }

            // формируем данные
            for (int i=0; i<packet->bytesLoaded; i++){
                // для нормальных пакетов надо заключить заголовок в []
                if ((!notAPacket) && (i == 0)) st += "[";
                // для нормальных пакетов надо заключить CRC в []
                if ((!notAPacket) && (i == packet->header->body_length+AC_HEADER_SIZE)) st += "[";
                
                memset(textBuf, 0, 10);
                sprintf(textBuf, "%02X", packet->data[i]);
                st += textBuf;

                // для нормальных пакетов надо заключить заголовок в []
                if ((!notAPacket) && (i == AC_HEADER_SIZE-1)) st += "]";
                // для нормальных пакетов надо заключить CRC в []
                if ((!notAPacket) && (i == packet->header->body_length+AC_HEADER_SIZE+2-1)) st += "]";

                st += " ";
            }
            
            if (line == 0) line = __LINE__;
            _debugMsg(st, dbgLevel, line);
        }

        /**
         * расчет CRC16 для блока данных data длиной len
         *      data    - данные для расчета CRC16, указатель на массив байт
         *      len     - длина блока данных для расчета, в байтах
         * 
         * возвращаем uint16_t CRC16
         **/
        uint16_t _CRC16(uint8_t *data, uint8_t len){
            uint32_t crc = 0;

            // выделяем буфер для расчета CRC и копируем в него переданные данные
            // это нужно для того, чтобы в случае нечетной длины данных можно было дополнить тело пакета
            // одним нулевым байтом и не попортить загруженный пакет (ведь в загруженном сразу за телом идёт CRC)
            uint8_t _crcBuffer[AC_BUFFER_SIZE];
            memset(_crcBuffer, 0, AC_BUFFER_SIZE);
            memcpy(_crcBuffer, data, len);

            // если длина данных нечетная, то надо сделать четной, дополнив данные в конце нулевым байтом
            // но так как выше буфер заполняли нулями, то отдельно тут присваивать 0x00 нет смысла
            if ((len%2) == 1) len++;

            // рассчитываем CRC16
            uint32_t word = 0;
            for (uint8_t i=0; i < len; i+=2){
                word = (_crcBuffer[i] << 8) + _crcBuffer[i+1];
                crc += word;
            }
            crc = (crc >> 16) + (crc & 0xFFFF);
            crc = ~ crc;

            return crc & 0xFFFF;
        }

        // расчитываем CRC16 и заполняем эти данные в структуре пакета
        void _setCRC16(packet_t* pack = nullptr){
            // если пакет не указан, то устанавливаем CRC для исходящего пакета
            if (pack == nullptr) pack = &_outPacket;

            packet_crc_t crc;
            crc.crc16 =  _CRC16(pack->data, AC_HEADER_SIZE + pack->header->body_length);

            // если забыли указатель на crc установить, то устанавливаем
            if (pack->crc == nullptr) pack->crc = (packet_crc_t *) &(pack->data[AC_HEADER_SIZE + pack->header->body_length]);

            pack->crc->crc[0] = crc.crc[1];
            pack->crc->crc[1] = crc.crc[0];
            return;
        }

        // проверяет CRC пакета по указателю
        bool _checkCRC(packet_t* pack = nullptr){
            // если пакет не указан, то проверяем входящий
            if (pack == nullptr) pack = &_inPacket;
            if (pack->bytesLoaded < AC_HEADER_SIZE) {
                _debugMsg(F("CRC check: incoming packet size error."), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                return false;
            }
            // если забыли указатель на crc установить, то устанавливаем
            if (pack->crc == nullptr) pack->crc = (packet_crc_t *) &(pack->data[AC_HEADER_SIZE + pack->header->body_length]);

            packet_crc_t crc;
            crc.crc16 =  _CRC16(pack->data, AC_HEADER_SIZE + pack->header->body_length);

            return ((pack->crc->crc[0] == crc.crc[1]) && (pack->crc->crc[1] == crc.crc[0]));
        }

        // заполняет пакет по ссылке командой запроса маленького пакета статуса
        void _fillStatusSmall(packet_t * pack = nullptr){
            // по умолчанию заполняем исходящий пакет
            if (pack == nullptr) pack = &_outPacket;

            // присваиваем параметры пакета
            pack->msec = millis();
            pack->header->start_byte = AC_PACKET_START_BYTE;
            pack->header->wifi = AC_PACKET_ANSWER;             // для исходящего пакета ставим признак ответа
            pack->header->packet_type = AC_PTYPE_CMD;
            pack->header->body_length = 2;    // тело команды 2 байта
            pack->body = &(pack->data[AC_HEADER_SIZE]);
            pack->body[0] = AC_CMD_STATUS_SMALL;
            pack->body[1] = 0x01;   // он всегда 0x01
            pack->bytesLoaded = AC_HEADER_SIZE + pack->header->body_length + 2;

            // рассчитываем и записываем в пакет CRC
            pack->crc = (packet_crc_t *) &(pack->data[AC_HEADER_SIZE + pack->header->body_length]);
            _setCRC16(pack);
        }

        // заполняет пакет по ссылке командой запроса большого пакета статуса
        void _fillStatusBig(packet_t * pack = nullptr){
            // по умолчанию заполняем исходящий пакет
            if (pack == nullptr) pack = &_outPacket;

            // присваиваем параметры пакета
            pack->msec = millis();
            pack->header->start_byte = AC_PACKET_START_BYTE;
            pack->header->wifi = AC_PACKET_ANSWER;             // для исходящего пакета ставим признак ответа
            pack->header->packet_type = AC_PTYPE_CMD;
            pack->header->body_length = 2;    // тело команды 2 байта
            pack->body = &(pack->data[AC_HEADER_SIZE]);
            pack->body[0] = AC_CMD_STATUS_BIG;
            pack->body[1] = 0x01;   // он всегда 0x01
            pack->bytesLoaded = AC_HEADER_SIZE + pack->header->body_length + 2;

            // рассчитываем и записываем в пакет CRC
            pack->crc = (packet_crc_t *) &(pack->data[AC_HEADER_SIZE + pack->header->body_length]);
            _setCRC16(pack);
        }

        /**
         * заполняет пакет по ссылке командой установки параметров
         * указатель на пакет может отсутствовать, тогда заполняется _outPacket
         * указатель на команду также может отсутствовать, тогда используется текущее состояние из _current_ac_state
         * все *__UNTOUCHED параметры заполняются из _current_ac_state
         **/
        void _fillSetCommand(bool clrPacket = false, packet_t * pack = nullptr, ac_state_t *cmd = nullptr){
            // по умолчанию заполняем исходящий пакет
            if (pack == nullptr) pack = &_outPacket;

            /*
            _debugMsg(F("_fillSetCommand: packet on start"), ESPHOME_LOG_LEVEL_INFO, __LINE__);
            _debugPrintPacket(pack, ESPHOME_LOG_LEVEL_INFO, __LINE__);
            */

            // очищаем пакет, если это указано
            if (clrPacket) _clearPacket(pack);
            /*
            _debugMsg(F("_fillSetCommand: packet after clear"), ESPHOME_LOG_LEVEL_INFO, __LINE__);
            _debugPrintPacket(pack, ESPHOME_LOG_LEVEL_INFO, __LINE__);
            */

            // заполняем его параметрами из _current_ac_state
            if (cmd != &_current_ac_state) _fillSetCommand(false, pack, &_current_ac_state);
            /*
            _debugMsg(F("_fillSetCommand: packet after _fillSetCommand(pack, &_current_ac_state)"), ESPHOME_LOG_LEVEL_INFO, __LINE__);
            _debugPrintPacket(pack, ESPHOME_LOG_LEVEL_INFO, __LINE__);
            */

            // если команда не указана, значит выходим
            if (cmd == nullptr) return;
            
            // команда указана, дополнительно внесем в пакет те параметры, которые установлены в команде

            // присваиваем параметры пакета
            pack->msec = millis();
            pack->header->start_byte = AC_PACKET_START_BYTE;
            pack->header->wifi = AC_PACKET_ANSWER;             // для исходящего пакета ставим признак ответа
            pack->header->packet_type = AC_PTYPE_CMD;
            pack->header->body_length = 15;    // тело команды 15 байт, как у Small status
            pack->body = &(pack->data[AC_HEADER_SIZE]);
            pack->body[0] = AC_CMD_SET_PARAMS;  // устанавливаем параметры
            pack->body[1] = 0x01;   // он всегда 0x01
            pack->bytesLoaded = AC_HEADER_SIZE + pack->header->body_length + 2;

            /*
            _debugMsg(F("_fillSetCommand: packet header set"), ESPHOME_LOG_LEVEL_INFO, __LINE__);
            */

            // целевая температура кондиционера
            if (cmd->temp_target_matter){
                // устраняем выход за границы диапазона (это ограничение самого кондиционера)
                if (cmd->temp_target < AC_MIN_TEMPERATURE) cmd->temp_target = AC_MIN_TEMPERATURE;
                if (cmd->temp_target > AC_MAX_TEMPERATURE) cmd->temp_target = AC_MAX_TEMPERATURE;

                // целая часть температуры
                pack->body[2] = (pack->body[2] & ~AC_TEMP_TARGET_INT_PART_MASK) | (((uint8_t)(cmd->temp_target) - 8) << 3);
                
                // дробная часть температуры
                if (cmd->temp_target - (uint8_t)(cmd->temp_target) > 0) {
                    pack->body[4] = (pack->body[4] & ~AC_TEMP_TARGET_FRAC_PART_MASK) | 1;
                } else {
                    pack->body[4] = (pack->body[4] & ~AC_TEMP_TARGET_FRAC_PART_MASK) | 0;
                }
            }

            // вертикальные жалюзи
            if (cmd->louver.louver_v != AC_LOUVERV_UNTOUCHED){
                pack->body[2] = (pack->body[2] & ~AC_LOUVERV_MASK) | cmd->louver.louver_v;
            }

            // горизонтальные жалюзи
            if (cmd->louver.louver_h != AC_LOUVERH_UNTOUCHED){
                pack->body[3] = (pack->body[3] & ~AC_LOUVERH_MASK) | cmd->louver.louver_h;
            }

            // скорость вентилятора
            if (cmd->fanSpeed != AC_FANSPEED_UNTOUCHED){
                pack->body[5] = (pack->body[5] & ~AC_FANSPEED_MASK) | cmd->fanSpeed;
            }

            // спец.режимы вентилятора: TURBO
            if (cmd->fanTurbo != AC_FANTURBO_UNTOUCHED){
                pack->body[6] = (pack->body[6] & ~AC_FANTURBO_MASK) | cmd->fanTurbo;
            }

            // спец.режимы вентилятора: MUTE
            if (cmd->fanMute != AC_FANMUTE_UNTOUCHED){
                pack->body[6] = (pack->body[6] & ~AC_FANMUTE_MASK) | cmd->fanMute;
            }

            // режим кондея
            if (cmd->mode != AC_MODE_UNTOUCHED){
                pack->body[7] = (pack->body[7] & ~AC_MODE_MASK) | cmd->mode;
            }
            if (cmd->sleep != AC_SLEEP_UNTOUCHED){
                pack->body[7] = (pack->body[7] & ~AC_SLEEP_MASK) | cmd->sleep;
            }
            if (cmd->iFeel != AC_IFEEL_UNTOUCHED){
                pack->body[7] = (pack->body[7] & ~AC_IFEEL_MASK) | cmd->iFeel;
            }

            // питание вкл/выкл
            if (cmd->power != AC_POWER_UNTOUCHED){
                pack->body[10] = (pack->body[10] & ~AC_POWER_MASK) | cmd->power;
            }
            if (cmd->clean != AC_CLEAN_UNTOUCHED){
                pack->body[10] = (pack->body[10] & ~AC_CLEAN_MASK) | cmd->clean;
            }
            if (cmd->health != AC_HEALTH_UNTOUCHED){
                pack->body[10] = (pack->body[10] & ~AC_HEALTH_MASK) | cmd->health;
            }

            // дисплей
            if (cmd->display != AC_DISPLAY_UNTOUCHED){
                pack->body[12] = (pack->body[12] & ~AC_DISPLAY_MASK) | cmd->display;
            }

            // антиплесень
            if (cmd->mildew != AC_MILDEW_UNTOUCHED){
                pack->body[12] = (pack->body[12] & ~AC_MILDEW_MASK) | cmd->mildew;
            }

            // рассчитываем и записываем в пакет CRC
            pack->crc = (packet_crc_t *) &(pack->data[AC_HEADER_SIZE + pack->header->body_length]);
            _setCRC16(pack);

            /*
            _debugMsg(F("_fillSetCommand: packet at the finish"), ESPHOME_LOG_LEVEL_INFO, __LINE__);
            _debugPrintPacket(pack, ESPHOME_LOG_LEVEL_INFO, __LINE__);
            */
        }

    public:
        // сенсоры, отображающие параметры сплита
        Sensor *sensor_ambient_temperature = new Sensor();
        Sensor *sensor_outdoor_temperature = new Sensor();

        AirCon(){ initAC(); };

        AirCon(UARTComponent *parent) { initAC(parent); };

        // инициализация объекта
        void initAC(UARTComponent *parent = nullptr){
            _dataMillis = millis();
            _cnt = 0;
            _clearInPacket();
            _clearOutPacket();

            _setStateMachineState(ACSM_IDLE);
            _ac_serial = parent;
            _hw_initialized = (_ac_serial != nullptr);

            // заполняем структуру состояния начальными значениями
            _clearCommand((ac_command_t *)&_current_ac_state);

            // очищаем последовательность пакетов
            _clearSequence();

            // выполнена ли уже стартовая последовательность команд (сбор информации о статусе кондея)
            _startupSequenceComlete = false;
        };

        float get_setup_priority() const override { return esphome::setup_priority::DATA; }

        bool get_initialized(){ return _hw_initialized; };

        // возвращает, есть ли елементы в последовательности команд
        bool hasSequence(){
            return (_sequence[0].item_type != AC_SIT_NONE);
        }

        // вызывается, если параметры кондиционера изменились
        void stateChanged(){
            _debugMsg(F("State changed, let's publish it."), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);

            /*************************** POWER & MODE ***************************/
            this->mode = climate::CLIMATE_MODE_OFF;
            this->action = climate::CLIMATE_ACTION_OFF;
            if (_current_ac_state.power == AC_POWER_ON){
                switch (_current_ac_state.mode) {
                    case AC_MODE_AUTO:
                        this->mode = climate::CLIMATE_MODE_AUTO;
                        this->action = climate::CLIMATE_ACTION_IDLE;
                        break;
                    
                    case AC_MODE_COOL:
                        this->mode = climate::CLIMATE_MODE_COOL;
                        this->action = climate::CLIMATE_ACTION_IDLE;
                        break;
                    
                    case AC_MODE_DRY:
                        this->mode = climate::CLIMATE_MODE_DRY;
                        this->action = climate::CLIMATE_ACTION_DRYING;
                        break;
                    
                    case AC_MODE_HEAT:
                        this->mode = climate::CLIMATE_MODE_HEAT;
                        this->action = climate::CLIMATE_ACTION_IDLE;
                        break;
                    
                    case AC_MODE_FAN:
                        this->mode = climate::CLIMATE_MODE_FAN_ONLY;
                        this->action = climate::CLIMATE_ACTION_FAN;
                        break;
                    
                    default:
                        _debugMsg(F("Warning: unknown air conditioner mode."), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                        break;
                }
            } else {
                this->mode = climate::CLIMATE_MODE_OFF;
                this->action = climate::CLIMATE_ACTION_OFF;
            }

            _debugMsg(F("Climate mode: %i"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, this->mode);

            /*************************** FAN SPEED ***************************/
            this->fan_mode = climate::CLIMATE_FAN_OFF;
            switch (_current_ac_state.fanSpeed) {
                case AC_FANSPEED_HIGH:
                    this->fan_mode = climate::CLIMATE_FAN_HIGH;
                    break;
                
                case AC_FANSPEED_MEDIUM:
                    this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
                    break;
                
                case AC_FANSPEED_LOW:
                    this->fan_mode = climate::CLIMATE_FAN_LOW;
                    break;
                
                case AC_FANSPEED_AUTO:
                    this->fan_mode = climate::CLIMATE_FAN_AUTO;
                    break;
                
                default:
                    _debugMsg(F("Warning: unknown fan speed."), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                    break;
            }

            /*************************** FAN TURBO MODE ***************************/
            // TURBO работает только в режимах COOL и HEAT
            switch (_current_ac_state.fanTurbo) {
                case AC_FANTURBO_ON:
                    if ((_current_ac_state.mode == AC_MODE_HEAT) || (_current_ac_state.mode == AC_MODE_COOL)) {
                        // используем режим CLIMATE_FAN_FOCUS как TURBO
                        this->fan_mode = climate::CLIMATE_FAN_FOCUS;
                    }
                    break;
                
                case AC_FANTURBO_OFF:
                default:
                    // ничего не меняем
                    break;
            }

            /*************************** FAN MUTE MODE ***************************/
            // MUTE работает только в режиме FAN. В режиме COOL кондей команду принимает, но MUTE не устанавливается
            switch (_current_ac_state.fanMute) {
                case AC_FANMUTE_ON:
                    if (_current_ac_state.mode == AC_MODE_FAN) {
                        // используем режим CLIMATE_FAN_DIFFUSE как MUTE
                        this->fan_mode = climate::CLIMATE_FAN_DIFFUSE;
                    }
                    break;
                
                case AC_FANMUTE_OFF:
                default:
                    // ничего не меняем
                    break;
            }

            _debugMsg(F("Climate fan mode: %i"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, this->fan_mode);

            /*************************** LOUVERs ***************************/
            this->swing_mode = climate::CLIMATE_SWING_OFF;
            if (_current_ac_state.louver.louver_h == AC_LOUVERH_SWING_LEFTRIGHT){
                this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
            }
            if (_current_ac_state.louver.louver_v == AC_LOUVERV_SWING_UPDOWN){
                if (_current_ac_state.louver.louver_h == AC_LOUVERH_SWING_LEFTRIGHT){
                    this->swing_mode = climate::CLIMATE_SWING_BOTH;
                } else {
                    this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
                }
            }

            _debugMsg(F("Climate swing mode: %i"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, this->swing_mode);

            /*************************** TEMPERATURE ***************************/
            this->target_temperature = _current_ac_state.temp_target;
            _debugMsg(F("Target temperature: %f"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, this->target_temperature);

            this->current_temperature = _current_ac_state.temp_ambient;
            _debugMsg(F("Room temperature: %f"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, this->current_temperature);
            

            /*********************************************************************/
            /*************************** PUBLISH STATE ***************************/
            /*********************************************************************/
            this->publish_state();
            // температура в комнате
            sensor_ambient_temperature->publish_state(_current_ac_state.temp_ambient);
            // температура уличного блока
            sensor_outdoor_temperature->publish_state(_current_ac_state.temp_outdoor);
        }

        // вызывается пользователем из интерфейса ESPHome или Home Assistant
        void control(const ClimateCall &call) override {
            bool hasCommand = false;
            ac_command_t    cmd;
            _clearCommand(&cmd);    // не забываем очищать, а то будет мусор

            // User requested mode change
            if (call.get_mode().has_value()) {
                hasCommand = true;
                ClimateMode mode = *call.get_mode();
                // Send mode to hardware
                switch (mode) {
                    case climate::CLIMATE_MODE_OFF:
                        cmd.power = AC_POWER_OFF;
                        break;
                    
                    case climate::CLIMATE_MODE_COOL:
                        cmd.power = AC_POWER_ON;
                        cmd.mode = AC_MODE_COOL;
                        break;
                    
                    case climate::CLIMATE_MODE_HEAT:
                        cmd.power = AC_POWER_ON;
                        cmd.mode = AC_MODE_HEAT;
                        break;
                    
                    case climate::CLIMATE_MODE_AUTO:
                        cmd.power = AC_POWER_ON;
                        cmd.mode = AC_MODE_AUTO;
                        break;
                    
                    case climate::CLIMATE_MODE_FAN_ONLY:
                        cmd.power = AC_POWER_ON;
                        cmd.mode = AC_MODE_FAN;
                        break;
                    
                    case climate::CLIMATE_MODE_DRY:
                        cmd.power = AC_POWER_ON;
                        cmd.mode = AC_MODE_DRY;
                        break;
                }

                this->mode = mode;
            }

            // User requested fan_mode change
            if (call.get_fan_mode().has_value()) {
                hasCommand = true;
                ClimateFanMode fanmode = *call.get_fan_mode();
                // Send fan mode to hardware
                switch (fanmode) {
                    case climate::CLIMATE_FAN_OFF:
                    case climate::CLIMATE_FAN_ON:
                        // don't know what to do here =)
                        break;
                    
                    case climate::CLIMATE_FAN_MIDDLE:
                        // ROVEX ALS1: unused
                        break;
                    
                    case climate::CLIMATE_FAN_AUTO:
                        cmd.fanSpeed = AC_FANSPEED_AUTO;
                        cmd.fanTurbo = AC_FANTURBO_OFF;   // changing fan speed cancels fan TURBO mode for ROVEX air conditioner 
                        cmd.fanMute = AC_FANMUTE_OFF;
                        break;
                    
                    case climate::CLIMATE_FAN_LOW:
                        cmd.fanSpeed = AC_FANSPEED_LOW;
                        cmd.fanTurbo = AC_FANTURBO_OFF;   // changing fan speed cancels fan TURBO mode for ROVEX air conditioner 
                        cmd.fanMute = AC_FANMUTE_OFF;
                        break;
                    
                    case climate::CLIMATE_FAN_MEDIUM:
                        cmd.fanSpeed = AC_FANSPEED_MEDIUM;
                        cmd.fanTurbo = AC_FANTURBO_OFF;   // changing fan speed cancels fan TURBO mode for ROVEX air conditioner 
                        cmd.fanMute = AC_FANMUTE_OFF;
                        break;
                    
                    case climate::CLIMATE_FAN_HIGH:
                        cmd.fanSpeed = AC_FANSPEED_HIGH;
                        cmd.fanTurbo = AC_FANTURBO_OFF;   // changing fan speed cancels fan TURBO mode for ROVEX air conditioner
                        cmd.fanMute = AC_FANMUTE_OFF;
                        break;
                    
                    case climate::CLIMATE_FAN_FOCUS:
                        // TURBO fan mode
                        // TURBO fan mode is suitable in COOL and HEAT modes for Rovex air conditioner.
                        // Other modes don't accept TURBO fan mode.
                        // May be other AUX-based air conditioners do the same.
                        if (       cmd.mode == AC_MODE_COOL
                                or cmd.mode == AC_MODE_HEAT
                                or _current_ac_state.mode == AC_MODE_COOL
                                or _current_ac_state.mode == AC_MODE_HEAT) {
                            cmd.fanTurbo = AC_FANTURBO_ON;
                            }
                        else {
                            // need this for return correct fan_mode to the UI
                            switch (_current_ac_state.fanSpeed) {
                                case AC_FANSPEED_AUTO:
                                    fanmode = climate::CLIMATE_FAN_AUTO;
                                    break;

                                case AC_FANSPEED_LOW:
                                    fanmode = climate::CLIMATE_FAN_LOW;
                                    break;

                                case AC_FANSPEED_MEDIUM:
                                    fanmode = climate::CLIMATE_FAN_MEDIUM;
                                    break;

                                case AC_FANSPEED_HIGH:
                                    fanmode = climate::CLIMATE_FAN_HIGH;
                                    break;
                            }
                        }
                        break;
                    
                    case climate::CLIMATE_FAN_DIFFUSE:
                        // MUTE fan mode
                        // MUTE fan mode is suitable in FAN mode only for Rovex air conditioner.
                        // In COOL mode AC receives command without any changes.
                        // May be other AUX-based air conditioners do the same.
                        if (                     cmd.mode == AC_MODE_FAN
                                or _current_ac_state.mode == AC_MODE_FAN) {
                            cmd.fanMute = AC_FANMUTE_ON;
                            }
                        else {
                            // need this for return correct fan_mode to the UI
                            switch (_current_ac_state.fanSpeed) {
                                case AC_FANSPEED_AUTO:
                                    fanmode = climate::CLIMATE_FAN_AUTO;
                                    break;

                                case AC_FANSPEED_LOW:
                                    fanmode = climate::CLIMATE_FAN_LOW;
                                    break;

                                case AC_FANSPEED_MEDIUM:
                                    fanmode = climate::CLIMATE_FAN_MEDIUM;
                                    break;
                                    
                                case AC_FANSPEED_HIGH:
                                    fanmode = climate::CLIMATE_FAN_HIGH;
                                    break;
                            }
                        }
                        break;
                }

                this->fan_mode = fanmode;
            }

            // User requested swing_mode change
            if (call.get_swing_mode().has_value()) {
                hasCommand = true;
                ClimateSwingMode swingmode = *call.get_swing_mode();
                // Send fan mode to hardware
                switch (swingmode) {
                    // The protocol allows other combinations for SWING.
                    // For example "turn the louvers to the desired position or "spread to the sides" / "concentrate in the center".
                    // But the ROVEX IR-remote does not provide this features. Therefore this features haven't been tested.
                    // May be suitable for other models of AUX-based ACs.
                    case climate::CLIMATE_SWING_OFF:
                        cmd.louver.louver_h = AC_LOUVERH_OFF;
                        cmd.louver.louver_v = AC_LOUVERV_OFF;
                        break;

                    case climate::CLIMATE_SWING_BOTH:
                        cmd.louver.louver_h = AC_LOUVERH_SWING_LEFTRIGHT;
                        cmd.louver.louver_v = AC_LOUVERV_SWING_UPDOWN;
                        break;

                    case climate::CLIMATE_SWING_VERTICAL:
                        cmd.louver.louver_h = AC_LOUVERH_OFF;
                        cmd.louver.louver_v = AC_LOUVERV_SWING_UPDOWN;
                        break;

                    case climate::CLIMATE_SWING_HORIZONTAL:
                        cmd.louver.louver_h = AC_LOUVERH_SWING_LEFTRIGHT;
                        cmd.louver.louver_v = AC_LOUVERV_OFF;
                        break;
                }

                this->swing_mode = swingmode;
            }

            if (call.get_target_temperature().has_value()) {
                hasCommand = true;
                // User requested target temperature change
                float temp = *call.get_target_temperature();
                // Send target temp to climate
                if (temp > AC_MAX_TEMPERATURE) temp = AC_MAX_TEMPERATURE;
                if (temp < AC_MIN_TEMPERATURE) temp = AC_MIN_TEMPERATURE;
                cmd.temp_target = temp;
                cmd.temp_target_matter = true;
            }
            if (hasCommand) {
                commandSequence(&cmd);
                this->publish_state(); // Publish updated state
            }
        }

        ClimateTraits traits() override {
            // The capabilities of the climate device
            auto traits = climate::ClimateTraits();
            traits.set_supports_current_temperature(true);              // if the climate device supports reporting a current temperature
            traits.set_supports_two_point_target_temperature(false);    // if the climate device's target temperature should be split in target_temperature_low and target_temperature_high instead of just the single target_temperature
            traits.set_supports_auto_mode(true);                        // automatic control
            traits.set_supports_cool_mode(true);                        // lowers current temperature
            traits.set_supports_heat_mode(true);                        // increases current temperature
            traits.set_supports_fan_only_mode(true);                    // only turns on fan
            traits.set_supports_dry_mode(true);                         // removes humidity from air
            traits.set_supports_away(false);                            // away mode means that the climate device supports two different target temperature settings: one target temp setting for "away" mode and one for non-away mode.

            /* *************** TODO: надо сделать информирование о текущем режиме, сплит поддерживает *************** */
            traits.set_supports_action(true);                          // if the climate device supports reporting the active current action of the device with the action property.

            // optionally, if it has a fan which can be configured in different ways: on, off, auto, high, medium, low, middle, focus, diffuse
            traits.set_supports_fan_mode_on(false);
            traits.set_supports_fan_mode_off(false);
            traits.set_supports_fan_mode_auto(true);
            traits.set_supports_fan_mode_low(true);
            traits.set_supports_fan_mode_medium(true);
            traits.set_supports_fan_mode_high(true);
            traits.set_supports_fan_mode_middle(false);
            traits.set_supports_fan_mode_focus(true);       // использую для режима TURBO
            traits.set_supports_fan_mode_diffuse(true);     // использую для режима MUTE

            // optionally, if it has a swing which can be configured in different ways: off, both, vertical, horizontal
            traits.set_supports_swing_mode_off(true);
            traits.set_supports_swing_mode_both(true);
            traits.set_supports_swing_mode_vertical(true);
            traits.set_supports_swing_mode_horizontal(true);

            // tells the frontend what range of temperatures the climate device should display (gauge min/max values)
            traits.set_visual_min_temperature(AC_MIN_TEMPERATURE);
            traits.set_visual_max_temperature(AC_MAX_TEMPERATURE);
            // the step with which to increase/decrease target temperature. This also affects with how many decimal places the temperature is shown.
            traits.set_visual_temperature_step(AC_TEMPERATURE_STEP);

            return traits;
        }

        // отправка запроса на маленький статусный пакет
        bool sq_requestSmallStatus(){
            // если исходящий пакет не пуст, то выходим и ждем освобождения
            if (_outPacket.bytesLoaded > 0) return true;

            _fillStatusSmall(&_outPacket);
            _fillStatusSmall(&_sequence[_sequence_current_step].packet);
            _sequence[_sequence_current_step].packet_type = AC_SPT_SENT_PACKET;

            // Отчитываемся в лог
            _debugMsg(F("Sequence [step %u]: small status request generated:"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _sequence_current_step);
            _debugPrintPacket(&_outPacket, ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);

            // увеличиваем текущий шаг
            _sequence_current_step++;
            return true;
        }

        // проверка ответа на запрос маленького статусного пакета
        bool sq_controlSmallStatus(){
            // если по каким-то причинам нет входящего пакета, значит проверять нам нечего - просто выходим
            if (_inPacket.bytesLoaded == 0) return true;

            // Пинги игнорируем
            if (_inPacket.header->packet_type == AC_PTYPE_PING) return true;

            // сохраняем полученный пакет в последовательность, чтобы на возможных следующих шагах с ним можно было работать
            _copyPacket(&_sequence[_sequence_current_step].packet, &_inPacket);
            _sequence[_sequence_current_step].packet_type = AC_SPT_RECEIVED_PACKET;

            // проверяем ответ
            bool relevant = true;
            relevant = (relevant && (_inPacket.header->packet_type == AC_PTYPE_INFO));
            relevant = (relevant && (_inPacket.header->body_length == 0x0F));
            relevant = (relevant && (_inPacket.body[0] == 0x01));
            relevant = (relevant && (_inPacket.body[1] == AC_CMD_STATUS_SMALL));

            // если пакет подходит, значит можно переходить к следующему шагу
            if (relevant) {
                _debugMsg(F("Sequence [step %u]: correct small status packet received"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _sequence_current_step);
                _sequence_current_step++;
            } else {
                // если пакет не подходящий, то отчитываемся в лог...
                _debugMsg(F("Sequence [step %u]: irrelevant incoming packet"), ESPHOME_LOG_LEVEL_WARN, __LINE__, _sequence_current_step);
                _debugMsg(F("Incoming packet:"), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                _debugPrintPacket(&_inPacket, ESPHOME_LOG_LEVEL_WARN, __LINE__);
                _debugMsg(F("Sequence packet needed: PACKET_TYPE = %02X, CMD = %02X"), ESPHOME_LOG_LEVEL_WARN, __LINE__, AC_PTYPE_INFO, AC_CMD_STATUS_SMALL);
                // ...и прерываем последовательность, так как вернем false
            }
            return relevant;
        }

        // отправка запроса на большой статусный пакет
        bool sq_requestBigStatus(){
            // если исходящий пакет не пуст, то выходим и ждем освобождения
            if (_outPacket.bytesLoaded > 0) return true;

            _fillStatusBig(&_outPacket);
            _fillStatusBig(&_sequence[_sequence_current_step].packet);
            _sequence[_sequence_current_step].packet_type = AC_SPT_SENT_PACKET;

            // Отчитываемся в лог
            _debugMsg(F("Sequence [step %u]: big status request generated:"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _sequence_current_step);
            _debugPrintPacket(&_outPacket, ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);

            // увеличиваем текущий шаг
            _sequence_current_step++;
            return true;
        }

        // проверка ответа на запрос большого статусного пакета
        bool sq_controlBigStatus(){
            // если по каким-то причинам нет входящего пакета, значит проверять нам нечего - просто выходим
            if (_inPacket.bytesLoaded == 0) return true;

            // Пинги игнорируем
            if (_inPacket.header->packet_type == AC_PTYPE_PING) return true;

            // сохраняем полученный пакет в последовательность, чтобы на возможных следующих шагах с ним можно было работать
            _copyPacket(&_sequence[_sequence_current_step].packet, &_inPacket);
            _sequence[_sequence_current_step].packet_type = AC_SPT_RECEIVED_PACKET;

            // проверяем ответ
            bool relevant = true;
            relevant = (relevant && (_inPacket.header->packet_type == AC_PTYPE_INFO));
            relevant = (relevant && (_inPacket.header->body_length == 0x18));
            relevant = (relevant && (_inPacket.body[0] == 0x01));
            relevant = (relevant && (_inPacket.body[1] == AC_CMD_STATUS_BIG));

            // если пакет подходит, значит можно переходить к следующему шагу
            if (relevant) {
                _debugMsg(F("Sequence [step %u]: correct big status packet received"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _sequence_current_step);
                _sequence_current_step++;
            } else {
                // если пакет не подходящий, то отчитываемся в лог...
                _debugMsg(F("Sequence [step %u]: irrelevant incoming packet"), ESPHOME_LOG_LEVEL_WARN, __LINE__, _sequence_current_step);
                _debugMsg(F("Incoming packet:"), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                _debugPrintPacket(&_inPacket, ESPHOME_LOG_LEVEL_WARN, __LINE__);
                _debugMsg(F("Sequence packet needed: PACKET_TYPE = %02X, CMD = %02X"), ESPHOME_LOG_LEVEL_WARN, __LINE__, AC_PTYPE_INFO, AC_CMD_STATUS_BIG);
                // ...и прерываем последовательность
            }
            return relevant;
        }

        // отправка запроса на выполнение команды
        bool sq_requestDoCommand(){
            // если исходящий пакет не пуст, то выходим и ждем освобождения
            if (_outPacket.bytesLoaded > 0) return true;

            _fillSetCommand(true, &_outPacket, &_sequence[_sequence_current_step].cmd);
            _fillSetCommand(true, &_sequence[_sequence_current_step].packet, &_sequence[_sequence_current_step].cmd);
            _sequence[_sequence_current_step].packet_type = AC_SPT_SENT_PACKET;

            // Отчитываемся в лог
            _debugMsg(F("Sequence [step %u]: doCommand request generated:"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _sequence_current_step);
            _debugPrintPacket(&_outPacket, ESPHOME_LOG_LEVEL_VERBOSE, __LINE__);

            // увеличиваем текущий шаг
            _sequence_current_step++;
            return true;
        }

        // проверка ответа на выполнение команды
        bool sq_controlDoCommand(){
            // если по каким-то причинам нет входящего пакета, значит проверять нам нечего - просто выходим
            if (_inPacket.bytesLoaded == 0) return true;

            // Пинги игнорируем
            if (_inPacket.header->packet_type == AC_PTYPE_PING) return true;

            // сохраняем полученный пакет в последовательность, чтобы на возможных следующих шагах с ним можно было работать
            _copyPacket(&_sequence[_sequence_current_step].packet, &_inPacket);
            _sequence[_sequence_current_step].packet_type = AC_SPT_RECEIVED_PACKET;

            // проверяем ответ
            bool relevant = true;
            relevant = (relevant && (_inPacket.header->packet_type == AC_PTYPE_INFO));
            relevant = (relevant && (_inPacket.header->body_length == 0x04));
            relevant = (relevant && (_inPacket.body[0] == 0x01));
            relevant = (relevant && (_inPacket.body[1] == AC_CMD_SET_PARAMS));
            // байты 2 и 3 обычно равны CRC отправленного пакета с командой
            relevant = (relevant && (_inPacket.body[2] == _sequence[_sequence_current_step-1].packet.crc->crc[0]));
            relevant = (relevant && (_inPacket.body[3] == _sequence[_sequence_current_step-1].packet.crc->crc[1]));

            // если пакет подходит, значит можно переходить к следующему шагу
            if (relevant) {
                _debugMsg(F("Sequence [step %u]: correct doCommand packet received"), ESPHOME_LOG_LEVEL_VERBOSE, __LINE__, _sequence_current_step);
                _sequence_current_step++;
            } else {
                // если пакет не подходящий, то отчитываемся в лог...
                _debugMsg(F("Sequence [step %u]: irrelevant incoming packet"), ESPHOME_LOG_LEVEL_WARN, __LINE__, _sequence_current_step);
                _debugMsg(F("Incoming packet:"), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                _debugPrintPacket(&_inPacket, ESPHOME_LOG_LEVEL_WARN, __LINE__);
                _debugMsg(F("Sequence packet needed: PACKET_TYPE = %02X, CMD = %02X"), ESPHOME_LOG_LEVEL_WARN, __LINE__, AC_PTYPE_INFO, AC_CMD_STATUS_BIG);
                // ...и прерываем последовательность
            }
            return relevant;
        }

        // запрос маленького пакета статуса кондиционера
        void getStatusSmall(){
            // если какая-то последовательность загружена и выполняется, то мы не можем сформировать новую последовательность команд
            if (hasSequence()) {
                _debugMsg(F("getStatusSmall: there is another sequence loaded. Can't create new sequence."), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                return;
            }

            uint8_t step = 0;

            /**************************************************************************************/
            //step++;   // - getSmallInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_requestSmallStatus;
            //_sequence[step].timeout = 0;  // пусть будет таймаут по-умолчанию
            /**************************************************************************************/

            /**************************************************************************************/
            step++;   // - control getSmallInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_controlSmallStatus;
            //_sequence[step].timeout = 1000;
            /**************************************************************************************/

            _debugMsg(F("getStatusSmall: loaded"), ESPHOME_LOG_LEVEL_DEBUG, __LINE__);
        }

        // запрос большого пакета статуса кондиционера
        void getStatusBig(){
            // если какая-то последовательность загружена и выполняется, то мы не можем сформировать новую последовательность команд
            if (hasSequence()) {
                _debugMsg(F("getStatusBig: there is another sequence loaded. Can't create new sequence."), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                return;
            }

            uint8_t step = 0;

            /**************************************************************************************/
            //step++;   // - getBigInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_requestBigStatus;
            //_sequence[step].timeout = 0;  // пусть будет таймаут по-умолчанию
            /**************************************************************************************/

            /**************************************************************************************/
            step++;   // - control getSmallInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_controlBigStatus;
            //_sequence[step].timeout = 0;  // пусть будет таймаут по-умолчанию
            /**************************************************************************************/

            _debugMsg(F("getStatusBig: loaded"), ESPHOME_LOG_LEVEL_DEBUG, __LINE__);
        }
        
        /** стартовая последовательность пакетов
         * 
         * нужна, чтобы не ждать долго обновления статуса кондиционера
         * запускаем сразу, как только удалось подключиться к кондиционеру и прошел первый пинг-пакет
         * возвращаемое значение будет присвоено флагу выполнения последовательности
         * то есть при возврате false последовательность считается не запущенной и будет вызоваться до тех пор, пока не вернет true 
        **/
        bool startupSequence(){
            // если какая-то последовательность загружена и выполняется, то мы не можем сформировать новую последовательность команд
            if (hasSequence()) {
                _debugMsg(F("startupSequence: there is another sequence loaded. Can't create new sequence."), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                return false;
            }

            // очищаем последовательность на всякий случай
            _clearSequence();

            uint8_t step = 0;

            /**************************************************************************************/
            //step++;   // - getSmallInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_requestSmallStatus;
            //_sequence[step].timeout = 0;  // пусть будет таймаут по-умолчанию
            /**************************************************************************************/

            /**************************************************************************************/
            step++;   // - control getSmallInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_controlSmallStatus;
            //_sequence[step].timeout = 0;  // пусть будет таймаут по-умолчанию
            /**************************************************************************************/
            
            /**************************************************************************************/
            /* Пауза тут была только для теста
            step++;   // - delay
            _sequence[step].item_type = AC_SIT_DELAY;
            _sequence[step].timeout = 5000;
            */
            /**************************************************************************************/

            /**************************************************************************************/
            step++;   // - getBigInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_requestBigStatus;
            //_sequence[step].timeout = 0;  // пусть будет таймаут по-умолчанию
            /**************************************************************************************/

            /**************************************************************************************/
            step++;   // - control getSmallInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_controlBigStatus;
            //_sequence[step].timeout = 0;  // пусть будет таймаут по-умолчанию
            /**************************************************************************************/

            _debugMsg(F("startupSequence: loaded"), ESPHOME_LOG_LEVEL_DEBUG, __LINE__);
            return true;
        }

        /** загружает на выполнение команду
         *
         * стандартная последовательность - это запрос маленького статусного пакета, выполнение команды и повторный запрос
         * такого же статуса для проверки, что всё включилось, ну и для обновления интерфейсов всяких связанных компонентов
        **/
        void commandSequence(ac_command_t * cmd){
            uint8_t step = 0;

            // если какая-то последовательность загружена и выполняется, то мы не можем сформировать новую последовательность команд
            // вместо этого дополняем существующую
            if (hasSequence()) {
                for (size_t i = 0; i < AC_SEQUENCE_MAX_LEN; i++) {
                    if (_sequence[i].item_type != AC_SIT_NONE){
                        step = i;
                        break;
                    }
                }
                // если дополнить не можем, то тогда ругаемся и выходим
                if (step >= AC_SEQUENCE_MAX_LEN-1){
                    _debugMsg(F("commandSequence: there is another sequence loaded. Can't create new sequence."), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                    return;
                }
            }

            /**************************************************************************************/
            //step++;   // - getSmallInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_requestSmallStatus;
            //_sequence[step].timeout = 0;  // пусть будет таймаут по-умолчанию
            /**************************************************************************************/

            /**************************************************************************************/
            step++;   // - control getSmallInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_controlSmallStatus;
            //_sequence[step].timeout = 1000;
            /**************************************************************************************/

            /**************************************************************************************/
            step++;   // - set params
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_requestDoCommand;
            // так как в структуре команды нет указателей, то простое присваивание возможно
            _sequence[step].cmd = *cmd;
            //_sequence[step].timeout = 0;  // пусть будет таймаут по-умолчанию
            /**************************************************************************************/

            /**************************************************************************************/
            step++;   // - control of params setting
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_controlDoCommand;
            //_sequence[step].timeout = 1000;
            /**************************************************************************************/

            /**************************************************************************************/
            step++;   // - getSmallInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_requestSmallStatus;
            //_sequence[step].timeout = 0;  // пусть будет таймаут по-умолчанию
            /**************************************************************************************/

            /**************************************************************************************/
            step++;   // - control getSmallInfo
            _sequence[step].item_type = AC_SIT_FUNC;
            _sequence[step].func = &AirCon::sq_controlSmallStatus;
            //_sequence[step].timeout = 1000;
            /**************************************************************************************/

            _debugMsg(F("commandSequence: loaded"), ESPHOME_LOG_LEVEL_DEBUG, __LINE__);
        }

        // загружает на выполнение последовательность команд на включение/выключение
        void powerSequence(ac_power pwr = AC_POWER_ON){
            if (pwr == AC_POWER_UNTOUCHED) return;  // выходим, чтобы не тратить время

            // если какая-то последовательность загружена и выполняется, то мы не можем сформировать новую последовательность команд
            if (hasSequence()) {
                _debugMsg(F("powerSequence: there is another sequence loaded. Can't create new sequence."), ESPHOME_LOG_LEVEL_WARN, __LINE__);
                return;
            }

            ac_command_t    cmd;
            _clearCommand(&cmd);    // не забываем очищать, а то будет мусор
            cmd.power = pwr;
            commandSequence(&cmd);

            _debugMsg(F("powerSequence: loaded (power = %02X)"), ESPHOME_LOG_LEVEL_DEBUG, __LINE__, pwr);
        }

        void setup() override {
        };

        void loop() override {
            if (!get_initialized()) return;

            switch (_ac_state) {
                case ACSM_RECEIVING_PACKET:
                    // находимся в процессе получения пакета, никакие отправки в этом состоянии невозможны
                    _doReceivingPacketState();
                    break;
                
                case ACSM_PARSING_PACKET:
                    // разбираем полученный пакет
                    _doParsingPacket();
                    break;
                
                case ACSM_SENDING_PACKET:
                    // отправляем пакет сплиту
                    _doSendingPacketState();
                    break;

                case ACSM_IDLE: // ничего не делаем, ждем, на что бы среагировать
                default:        // если состояние какое-то посторонее, то считаем, что IDLE
                    _doIdleState();
                    break;
            }


            packet_t    pack;
            _clearPacket(&pack);
            ac_command_t    cmd;
            _clearCommand(&cmd);
            
            // раз в 8 сек что-то можем тестировать
            if ((millis()-_dataMillis) > AC_STATES_REQUEST_INTERVAL){
                _dataMillis = millis();

                // обычный wifi-модуль запрашивает маленький пакет статуса
                // но нам никто не мешает запрашивать и большой и маленький, чтобы чаще обновлять комнатную температуру
                //getStatusSmall();
                // запрос сразу двух пакетов статуса есть в стартовой последовательности команд
                startupSequence();
                
                //*********************************************************************
                // ниже всякое отладочное
                //*********************************************************************
                
                _cnt++;

                //if (_cnt == 2) getStatusSmall();
                //if (_cnt == 4) getStatusBig();

                /*
                if (_cnt == 2) {
                    _clearPacket(&pack);
                    _clearCommand(&cmd);

                    _debugMsg(F("Packet 0:"), ESPHOME_LOG_LEVEL_INFO, __LINE__);
                    _debugPrintPacket(&pack, ESPHOME_LOG_LEVEL_INFO, __LINE__);

                    _fillSetCommand(false, &pack);
                    
                    _debugMsg(F("Packet 1:"), ESPHOME_LOG_LEVEL_INFO, __LINE__);
                    _debugPrintPacket(&pack, ESPHOME_LOG_LEVEL_INFO, __LINE__);

                    _fillSetCommand(false, &pack, &cmd);

                    _debugMsg(F("Packet 2:"), ESPHOME_LOG_LEVEL_INFO, __LINE__);
                    _debugPrintPacket(&pack, ESPHOME_LOG_LEVEL_INFO, __LINE__);

                    _clearCommand(&cmd);
                    cmd.power = AC_POWER_ON;
                    cmd.display = AC_DISPLAY_OFF;
                    _fillSetCommand(false, &pack, &cmd);
                    
                    _debugMsg(F("Packet 3:"), ESPHOME_LOG_LEVEL_INFO, __LINE__);
                    _debugPrintPacket(&pack, ESPHOME_LOG_LEVEL_INFO, __LINE__);

                    _clearCommand(&cmd);
                    cmd.power = AC_POWER_ON;
                    cmd.display = AC_DISPLAY_ON;
                    _fillSetCommand(false, &pack, &cmd);
                    
                    _debugMsg(F("Packet 4:"), ESPHOME_LOG_LEVEL_INFO, __LINE__);
                    _debugPrintPacket(&pack, ESPHOME_LOG_LEVEL_INFO, __LINE__);

                    _clearCommand(&cmd);
                    cmd.power = AC_POWER_OFF;
                    _fillSetCommand(false, &pack, &cmd);
                    
                    _debugMsg(F("Packet 5:"), ESPHOME_LOG_LEVEL_INFO, __LINE__);
                    _debugPrintPacket(&pack, ESPHOME_LOG_LEVEL_INFO, __LINE__);
                }
                */

                /*
                if (_cnt == 2) {
                    cmd.display = AC_DISPLAY_OFF;
                    commandSequence(&cmd);
                }
                if (_cnt == 3) powerSequence(AC_POWER_ON);
                if (_cnt == 4) {
                    cmd.display = AC_DISPLAY_ON;
                    commandSequence(&cmd);
                }
                if (_cnt == 5) powerSequence(AC_POWER_OFF);
                */

                if (_cnt > 7) _cnt = 0;
            }
        };
};

AirCon acAirCon;


/************************************************************************************************************
 * 
 * 
 * 
 * 
 ************************************************************************************************************/
class AirConFirmwareVersion: public PollingComponent, public TextSensor {
    public:
        AirConFirmwareVersion() :  PollingComponent(1*60*1000) {}   // 1 minute update interval

        void setup() override {
        }

        void update() override {
            publish_state(AC_ROVEX_FIRMWARE_VERSION);
        }
};
