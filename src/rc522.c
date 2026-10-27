/* rc522.c - minimal MFRC522 driver: init + UID read */
#include "rc522.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "rc522";

#define REG_COMMAND      0x01
#define REG_COM_IRQ      0x04
#define REG_ERROR        0x06
#define REG_FIFO_DATA    0x09
#define REG_FIFO_LEVEL   0x0A
#define REG_CONTROL      0x0C
#define REG_BIT_FRAMING  0x0D
#define REG_COLL         0x0E
#define REG_MODE         0x11
#define REG_TX_CONTROL   0x14
#define REG_TX_ASK       0x15
#define REG_T_MODE       0x2A
#define REG_T_PRESCALER  0x2B
#define REG_T_RELOAD_H   0x2C
#define REG_T_RELOAD_L   0x2D
#define REG_VERSION      0x37

#define CMD_IDLE         0x00
#define CMD_TRANSCEIVE   0x0C
#define CMD_SOFT_RESET   0x0F

static esp_err_t spi_tx_rx(rc522_t *r, const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = { .length = len * 8, .tx_buffer = tx, .rx_buffer = rx };
    return spi_device_transmit(r->spi, &t);
}

static void write_reg(rc522_t *r, uint8_t addr, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)(addr << 1), val };
    spi_tx_rx(r, tx, NULL, 2);
}

static uint8_t read_reg(rc522_t *r, uint8_t addr)
{
    uint8_t tx[2] = { (uint8_t)((addr << 1) | 0x80), 0x00 };
    uint8_t rx[2] = { 0 };
    spi_tx_rx(r, tx, rx, 2);
    return rx[1];
}

static void read_fifo(rc522_t *r, uint8_t *buf, uint8_t n)
{
    uint8_t tx[9], rx[9] = { 0 };
    if (n > 8) n = 8;
    memset(tx, (REG_FIFO_DATA << 1) | 0x80, n);
    tx[n] = 0x00;
    spi_tx_rx(r, tx, rx, (size_t)n + 1);
    memcpy(buf, rx + 1, n);
}

static void set_bit(rc522_t *r, uint8_t addr, uint8_t mask)
{
    write_reg(r, addr, read_reg(r, addr) | mask);
}

esp_err_t rc522_init(rc522_t *r, spi_host_device_t host, int cs_pin, int rst_pin)
{
    memset(r, 0, sizeof(*r));
    r->rst_gpio = rst_pin;

    /* rst_pin < 0: this reader shares the reset line of another reader (or
     * has none), so no hard reset is done - it would also reset the other
     * reader after it was configured. */
    if (rst_pin >= 0) {
        gpio_config_t io = { .pin_bit_mask = 1ULL << rst_pin, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = cs_pin,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(host, &devcfg, &r->spi));

    if (rst_pin >= 0) {                                   /* hard reset */
        gpio_set_level(rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Soft reset. The wait is BOUNDED: with no reader connected an unbounded
     * loop would spin forever and the system would never finish booting. */
    write_reg(r, REG_COMMAND, CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    for (int i = 0; i < 20 && (read_reg(r, REG_COMMAND) & (1 << 4)); i++)
        vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t ver = read_reg(r, REG_VERSION);
    if (ver == 0x00 || ver == 0xFF)
        ESP_LOGW(TAG, "reader on CS GPIO%d does not answer (version reg 0x%02X) - check wiring",
                 cs_pin, ver);
    else
        ESP_LOGI(TAG, "reader on CS GPIO%d ready (version reg 0x%02X)", cs_pin, ver);

    /* Timer for timeout handling (same values as the Arduino MFRC522 library) */
    write_reg(r, REG_T_MODE,      0x80);   /* TAuto: timer starts after transmit */
    write_reg(r, REG_T_PRESCALER, 0xA9);   /* ~25 ms timeout                     */
    write_reg(r, REG_T_RELOAD_H,  0x03);
    write_reg(r, REG_T_RELOAD_L,  0xE8);
    write_reg(r, REG_MODE,        0x3D);
    write_reg(r, REG_TX_ASK,      0x40);

    uint8_t tx = read_reg(r, REG_TX_CONTROL);              /* antenna on */
    if ((tx & 0x03) != 0x03) write_reg(r, REG_TX_CONTROL, tx | 0x03);
    return ESP_OK;
}

/* Low-level transceive; send_bits < 8 means a partial last byte.
 * The register sequence matches the Arduino MFRC522 library (idle -> clear
 * IRQs -> flush FIFO -> fill FIFO -> bit framing -> transceive -> StartSend),
 * which is what simulators such as Wokwi expect. */
static bool transceive(rc522_t *r, uint8_t *send, uint8_t send_len, uint8_t send_bits,
                       uint8_t *back, uint8_t back_size, uint8_t *back_len)
{
    write_reg(r, REG_COMMAND, CMD_IDLE);
    write_reg(r, REG_COM_IRQ, 0x7F);          /* clear all IRQs */
    write_reg(r, REG_FIFO_LEVEL, 0x80);       /* flush the FIFO */
    for (uint8_t i = 0; i < send_len; i++) write_reg(r, REG_FIFO_DATA, send[i]);
    write_reg(r, REG_BIT_FRAMING, send_bits);
    write_reg(r, REG_COMMAND, CMD_TRANSCEIVE);
    set_bit(r, REG_BIT_FRAMING, 0x80);        /* StartSend */

    /* Wait for RxIrq (0x20) or IdleIrq (0x10); bail out on TimerIrq (0x01) */
    bool done = false;
    for (int guard = 2000; guard > 0; guard--) {
        uint8_t irq = read_reg(r, REG_COM_IRQ);
        if (irq & 0x30) { done = true; break; }
        if (irq & 0x01) break;                /* reader timeout: no card */
    }
    if (!done) return false;

    if (read_reg(r, REG_ERROR) & 0x13) return false;   /* BufferOvfl|ParityErr|ProtocolErr */

    uint8_t n = read_reg(r, REG_FIFO_LEVEL);
    if (n == 0 || n > back_size) return false;
    read_fifo(r, back, n);
    *back_len = n;
    return true;
}

bool rc522_read_uid(rc522_t *dev, uint8_t uid[4])
{
    uint8_t atqa[2], resp[5], len;

    /* same preparation as the Arduino library's PICC_RequestA() */
    write_reg(dev, 0x12, 0x00);                                   /* TxModeReg   */
    write_reg(dev, 0x13, 0x00);                                   /* RxModeReg   */
    write_reg(dev, 0x24, 0x26);                                   /* ModWidthReg */
    write_reg(dev, REG_COLL, read_reg(dev, REG_COLL) & (uint8_t)~0x80);

    /* REQA (7 bits) */
    if (!transceive(dev, (uint8_t[]){0x26}, 1, 7, atqa, 2, &len)) return false;
    ESP_LOGI(TAG, "card answered REQA (ATQA %02X%02X)", atqa[1], atqa[0]);

    /* Anticollision CL1: SEL + NVB = 0x93, 0x20 -> 4 UID bytes + BCC */
    if (!transceive(dev, (uint8_t[]){0x93, 0x20}, 2, 0, resp, 5, &len)) {
        ESP_LOGW(TAG, "anticollision got no answer");
        return false;
    }
    if (len != 5) {
        ESP_LOGW(TAG, "anticollision: %d bytes instead of 5", len);
        return false;
    }
    uint8_t bcc = resp[0] ^ resp[1] ^ resp[2] ^ resp[3];
    if (bcc != resp[4]) {
        ESP_LOGW(TAG, "anticollision: bad check byte, got %02X %02X %02X %02X %02X (calculated %02X)",
                 resp[0], resp[1], resp[2], resp[3], resp[4], bcc);
        return false;
    }

    memcpy(uid, resp, 4);
    return true;
}
