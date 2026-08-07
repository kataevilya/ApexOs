#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    outb(CMOS_ADDR, 0x0A);
    io_wait();
    return (inb(CMOS_DATA) & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + (v / 16) * 10);
}

void rtc_read(struct rtc_time *out) {
    /* Ждём конца "update in progress" — иначе можем прочитать поля
       в момент их обновления и получить противоречивое время.
       Ограничиваем ожидание, а не ждём бесконечно (сломанный CMOS/
       нестандартный эмулятор не должны вешать чтение времени). */
    int guard = 0;
    while (update_in_progress()) {
        if (guard++ > 1000000) {
            break;
        }
    }

    uint8_t second = cmos_read(0x00);
    uint8_t minute = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day = cmos_read(0x07);
    uint8_t month = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);
    uint8_t status_b = cmos_read(0x0B);

    int is_binary = (status_b & 0x04) != 0;
    int is_24h = (status_b & 0x02) != 0;

    int pm = (hour & 0x80) != 0;
    hour &= 0x7F;

    if (!is_binary) {
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour = bcd_to_bin(hour);
        day = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year = bcd_to_bin(year);
    }

    if (!is_24h) {
        if (hour == 12) {
            hour = 0;
        }
        if (pm) {
            hour = (uint8_t)(hour + 12);
        }
    }

    out->second = second;
    out->minute = minute;
    out->hour = hour;
    out->day = day;
    out->month = month;
    out->year = (uint16_t)(2000 + year);
}
