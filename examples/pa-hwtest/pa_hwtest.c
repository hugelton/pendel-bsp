/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* pendel-bsp hardware test for the Pocket AMP: the first firmware on a new unit. Not an amp: it only exercises the
 * board files (boards/pocketamp), one part at a time, and reports over USB serial (the unit has no UART to hand).
 * The board files were checked with it on a unit (2026-11-01).
 *
 *   screen   colour bars at the top: the panel, its colour order and its turn (MADCTL); a cell per button below
 *   buttons  six cells light while held (bit 0 PH6, 1 PH8, 2 PH9, 3 PA0, 4 PA1, 5 PA3)
 *   audio    off at power-on. 't' a 440 Hz tone (-12 dBFS) on both sides, 'l' / 'R' left / right only,
 *            'i' the input passed straight to the output, 'm' quiet. The input's peak is a bar and is printed
 *            raw (both as 24-bit and as the top 16 bits), so its format shows on the first run
 *   USB      CDC serial (115200 8N1, the rate is ignored): a status line every 500 ms, one-letter commands:
 *            h help, p print, c last crash, 9 toggle PA9 (the backlight, on when low), a every SARADC channel,
 *            A the audio link's registers, L the LED walk (PC5..PC9 one at a time, 1 s each),
 *            gXNV a pin: X port (A B C H), N pin (hex), V 0 / 1 / i (input), u UBOOT, r reboot
 *   encoder  PC0 / PC1 read every 1 ms as a quadrature pair: enc= counts its steps, ab= its two levels
 *            The SysEx soft key (F0 22 24 35 7D F7) enters UBOOT too.
 *
 * Safe by design: nothing writes the flash (no update entry: the Pocket AMP's layout is not FM-1's), the supply
 * levels are left as the boot loader set them, and two boots in a row that never reach the healthy point (2 s) drop
 * into UBOOT, where an image can always be written again. */
#include <stdint.h>
#define FELUCCA_CDC 1
#define FELUCCA_OTA 0             /* no update entry: nothing here may write the flash */
#define FELUCCA_USB_MANUFACTURER "Pendel"
#define FELUCCA_USB_PRODUCT "PA-HWTEST"   /* not "Felucca": an FM-1 on the same host must not be taken for it */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_timer.h"
#include "fm1_flash.h"
#include "pa_lcd_hw.h"
#include "pa_lcd_init.h"
#include "pa_input.h"
#include "pa_audio.h"
#include "fm1_adc.h"
#include "libc.c"
#include "usb.c"

extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _rt_start[], _rt_end[], _rt_load[];

static volatile uint32_t ms;
static volatile int32_t enc_steps;
static volatile uint32_t enc_ab, enc_bad;
static uint32_t boots_ok;
static struct { uint32_t magic, pending, failed; } guard __attribute__((section(".noinit")));

/* ---- audio: 64 frames a half, stereo, 24-bit right-aligned in int32 (as FM-1) ---- */
#define HALF_FRAMES 64u
#define HALF_WORDS (HALF_FRAMES * 2u)
enum { A_OFF, A_TONE, A_LEFT, A_RIGHT, A_THRU };
static int32_t abuf_out[2u * HALF_WORDS] __attribute__((aligned(4)));
static int32_t abuf_in[2u * HALF_WORDS] __attribute__((aligned(4)));
static volatile uint32_t a_mode, a_halves, a_irqs, a_pend, in_peak24, in_raw_last;
static uint32_t phase;
extern void isr_alnk0(void);

static int32_t tone(void)                          /* a sine-ish 440 Hz (two parabolas), Q15 */
{
    int32_t x, y;
    phase += 42834798u;                             /* 2^32 * 440 / 44118 */
    x = (int32_t)(phase >> 16) & 0x7FFF;            /* where in the half cycle */
    y = ((x * (32768 - x)) >> 15) * 4;
    return phase & 0x80000000u ? -y : y;
}

void fm1_alnk0_irq(void)
{
    uint8_t p = pa_audio_pending();
    a_irqs++;
    a_pend |= p;
    if (p & PA_AUDIO_IN)
        pa_audio_ack_in();
    if (p & PA_AUDIO_OUT) {
        uint32_t h = pa_audio_free_half(), i, pk = in_peak24;
        int32_t *o = &abuf_out[h * HALF_WORDS], *in = &abuf_in[pa_audio_in_half() * HALF_WORDS];
        for (i = 0; i < HALF_FRAMES; i++) {
            int32_t l = 0, r = 0, s = (int32_t)((uint32_t)in[2u * i] << 8) >> 8, a = s < 0 ? -s : s;   /* 24-bit, not sign-extended */
            if ((uint32_t)a > pk)
                pk = (uint32_t)a;
            switch (a_mode) {
            case A_TONE: l = r = tone() >> 2; break;      /* -12 dBFS */
            case A_LEFT: l = tone() >> 2; break;
            case A_RIGHT: r = tone() >> 2; break;
            case A_THRU: o[2u * i] = s; o[2u * i + 1u] = in[2u * i + 1u]; continue;
            default: break;
            }
            o[2u * i] = l << 7;                      /* Q15 -> 24-bit, as FM-1 */
            o[2u * i + 1u] = r << 7;
        }
        in_raw_last = (uint32_t)in[0];
        in_peak24 = pk;
        pa_audio_ack_out();
        a_halves++;
    }
}

void fm1_timer5_irq(void)                           /* 10 kHz: ms, USB at 2 kHz, buttons every 10 ms */
{
    static uint32_t sub, last, acc, k;
    uint32_t t = fm1_ticks();
    fm1_timer5_ack();
    acc += t - last;
    last = t;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        ms++;
    }
    if (++sub == 5u) {
        sub = 0;
        usb_poll();
    }
    if (++k == 100u) {
        k = 0;
        pa_input_poll();
    }
    if (k % 10u == 0u) {                            /* 1 kHz: the encoder (PC0, PC1) as a quadrature pair */
        static const int8_t STEP[16] = { 0, 1, -1, 2, -1, 0, 2, 1, 1, 2, 0, -1, 2, -1, 1, 0 };
        uint32_t ab = FM1_PR(FM1_PC, FM1_IN) & 3u;
        int32_t d = STEP[(enc_ab << 2) | ab];
        if (d == 2)
            enc_bad++;                              /* both lines changed in one ms: a step missed */
        else
            enc_steps += d;
        enc_ab = ab;
    }
}
extern void isr_timer5(void);

/* ---- screen ---- */
#define C(x) ((uint16_t)((((x) >> 8) | ((x) << 8)) & 0xFFFFu))
enum { RED = 0xF800, GRN = 0x07E0, BLU = 0x001F, WHT = 0xFFFF, BLK = 0, DIM = 0x2104, CYN = 0x07FF, YEL = 0xFFE0 };
static uint16_t row_px[240];

static void lcd_cmd(uint8_t c, const uint8_t *d, uint32_t n)
{
    static uint8_t buf[16];
    uint32_t i;
    pa_lcd_send_cmd(c);
    pa_lcd_wait();
    pa_lcd_deselect();
    if (!n)
        return;
    for (i = 0; i < n && i < sizeof buf; i++)
        buf[i] = d[i];
    pa_lcd_send_data(buf, n);
    pa_lcd_wait();
    pa_lcd_deselect();
}

static void lcd_init(void)
{
    uint32_t i;
    pa_lcd_hw_init();
    fm1_delay_ms(50);
    lcd_cmd(0x01, 0, 0);                            /* SWRESET */
    fm1_delay_ms(150);
    for (i = 0; i < sizeof PA_LCD_INIT / sizeof PA_LCD_INIT[0]; i++) {
        if (PA_LCD_INIT[i].cmd == PA_LCD_INIT_DELAY)
            fm1_delay_ms(PA_LCD_INIT[i].len);
        else
            lcd_cmd(PA_LCD_INIT[i].cmd, PA_LCD_INIT[i].data, PA_LCD_INIT[i].len);
    }
}

static void lcd_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t rgb)
{
    uint8_t a[4];
    uint32_t i;
    for (i = 0; i < w; i++)
        row_px[i] = C(rgb);
    a[0] = 0; a[1] = (uint8_t)x; a[2] = 0; a[3] = (uint8_t)(x + w - 1u);
    lcd_cmd(0x2A, a, 4);
    a[1] = (uint8_t)y; a[3] = (uint8_t)(y + h - 1u);
    lcd_cmd(0x2B, a, 4);
    lcd_cmd(0x2C, 0, 0);
    for (i = 0; i < h; i++) {
        pa_lcd_send_data(row_px, w * 2u);
        pa_lcd_wait();
        pa_lcd_deselect();
    }
}

static void fm1_fault(const fm1_crash_t *c)
{
    (void)c;
    pa_audio_stop();
    lcd_rect(0, 0, 240, 240, RED);
    fm1_delay_ms(1500);
    fm1_reboot();
}

#define KEY_X(i) (8u + (i) * 38u)
#define MODE_Y 150u

static void draw_static(void)
{
    uint32_t i;
    static const uint16_t bars[6] = { RED, GRN, BLU, WHT, YEL, CYN };
    lcd_rect(0, 0, 240, 240, BLK);
    for (i = 0; i < 6; i++)                         /* the colour bars, left to right: R G B white yellow cyan */
        lcd_rect(i * 40u, 0, 40, 24, bars[i]);
    lcd_rect(0, 24, 8, 8, WHT);                     /* a corner mark: top left, to see the panel's turn */
    for (i = 0; i < PA_KEYS; i++)
        lcd_rect(KEY_X(i), 60, 32, 40, DIM);
}

static void draw_bar(uint32_t y, uint32_t v, uint32_t max, uint16_t col, uint32_t *shown)
{
    uint32_t w = v >= max ? 224u : v / (max / 224u + 1u);
    if (w == *shown)
        return;
    *shown = w;
    if (w)
        lcd_rect(8, y, w, 10, col);
    if (w < 224u)
        lcd_rect(8u + w, y, 224u - w, 10, DIM);
}

/* ---- serial ---- */
static void put(char c)
{
    if (!cdc.dtr || co_w - co_r >= CO_N)
        return;
    cdc_out[co_w % CO_N] = (uint8_t)c;
    RING_PUBLISH();
    co_w++;
}
static void puts_(const char *s) { while (*s) put(*s++); }
static void putd(const char *k, uint32_t v)
{
    char b[10];
    uint32_t n = 0;
    puts_(k);
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n)
        put(b[--n]);
    put(' ');
}
static void puthex(const char *k, uint32_t v)
{
    uint32_t i;
    puts_(k);
    for (i = 8; i--;)
        put("0123456789ABCDEF"[(v >> (4u * i)) & 15u]);
    put(' ');
}

static uint32_t jedec, pa9_on = 1, led_walk, led_t;
static uint8_t gcmd[3], gn;               /* a 'g' command being typed */   /* PA9 is driven low for on (PA_LCD_PA9_ON 0) */

static void status(void)
{
    static const char *const MODE[] = { "off", "tone", "left", "right", "thru" };
    putd("ms=", ms);
    puthex("keys=", pa_in.down);
    puthex("raw=", pa_input_read());
    puts_("audio="); puts_(MODE[a_mode % 5u]); put(' ');
    putd("halves=", a_halves);
    putd("irqs=", a_irqs);
    putd("in_peak24=", in_peak24);
    putd("in_peak16=", in_peak24 >> 8);
    puthex("in_raw=", in_raw_last);
    puthex("board=", pa_board_inputs());
    putd("enc=", (uint32_t)enc_steps);
    putd("ab=", enc_ab);
    putd("enc_bad=", enc_bad);
    {
        int32_t v = fm1_adc_read(2);                /* PA10: the master volume pot? */
        putd("vol=", v < 0 ? 9999u : (uint32_t)v);
    }
    putd("pa9=", pa9_on);
    puthex("flash_id=", jedec);
    putd("usb_cfg=", usb.config);
    puts_("\r\n");
}

static uint32_t port_of(char x)
{
    return x == 'A' ? FM1_PA : x == 'B' ? FM1_PB : x == 'C' ? FM1_PC : x == 'H' ? FM1_PH : 99u;
}

static void pin_set(char x, char n, char v)
{
    uint32_t p = port_of(x), b = (uint32_t)(n >= 'a' ? n - 'a' + 10 : n >= 'A' ? n - 'A' + 10 : n - '0'), m;
    if (p == 99u || b > 15u || (v != '0' && v != '1' && v != 'i')) {
        puts_("g: gXNV, X in A B C H, N 0..F, V 0 1 i\r\n");
        return;
    }
    m = 1u << b;
    FM1_PR(p, FM1_DIE) |= m;
    if (v == 'i') {
        FM1_PR(p, FM1_DIR) |= m;
    } else {
        if (v == '1')
            FM1_PR(p, FM1_OUT) |= m;
        else
            FM1_PR(p, FM1_OUT) &= ~m;
        FM1_PR(p, FM1_DIR) &= ~m;
    }
    put('P'); put(x); put(n); put('='); put(v);
    puthex(" in=", FM1_PR(p, FM1_IN));
    puts_("\r\n");
}

static void audio_regs(void)
{
    puthex("CON0=", PA_ALNK_CON0); puthex("CON1=", PA_ALNK_CON1); puthex("CON2=", PA_ALNK_CON2);
    puthex("CON3=", PA_ALNK_CON3); puthex("LEN=", PA_ALNK_LEN); puthex("ADR0=", PA_ALNK_ADR0);
    puthex("ADR1=", PA_ALNK_ADR1); puthex("CLK_CON2=", PA_CLK_CON2); puthex("IOMAP5=", PA_IOMAP_CON5);
    puthex("ICFG=", FM1_ICFG(FM1_IRQ_ALNK0)); puthex("VEC=", FM1_VEC[FM1_IRQ_ALNK0]);
    puthex("pend_seen=", a_pend); putd("irqs=", a_irqs);
    puthex("PA_OUT=", FM1_PR(FM1_PA, FM1_OUT)); puthex("PA_DIR=", FM1_PR(FM1_PA, FM1_DIR));
    puthex("PA_IN=", FM1_PR(FM1_PA, FM1_IN)); puthex("PC_OUT=", FM1_PR(FM1_PC, FM1_OUT));
    puthex("PC_DIR=", FM1_PR(FM1_PC, FM1_DIR));
    puts_("\r\n");
}

static void command(char c)
{
    if (gn) {                                       /* the rest of a 'g' command */
        gcmd[gn - 1u] = (uint8_t)c;
        if (++gn == 4u) {
            gn = 0;
            pin_set((char)gcmd[0], (char)gcmd[1], (char)gcmd[2]);
        }
        return;
    }
    switch (c) {
    case 'g': gn = 1; break;
    case 'A': audio_regs(); break;
    case 'L':
        led_walk = 1;
        led_t = ms;
        puts_("LED walk: PC5..PC9, each low for 1 s, then each high for 1 s\r\n");
        break;
    case 'h':
        puts_("h help, p print, c last crash, t tone, l left, R right, i input thru, m quiet, 9 PA9, a ADC, A audio regs, L LED walk, gXNV pin, u UBOOT, r reboot\r\n");
        break;
    case 'p': status(); break;
    case 't': a_mode = A_TONE; break;
    case 'l': a_mode = A_LEFT; break;
    case 'R': a_mode = A_RIGHT; break;
    case 'i': a_mode = A_THRU; break;
    case 'm': a_mode = A_OFF; break;
    case '9':
        pa9_on ^= 1u;
        pa_lcd_power(pa9_on);
        puts_(pa9_on ? "PA9 on\r\n" : "PA9 off\r\n");
        break;
    case 'a': {                                     /* every SARADC channel once, 0..1023 (-1: no answer) */
        uint32_t ch;
        int32_t v;
        for (ch = 0; ch < 16u; ch++) {
            v = fm1_adc_read(ch);
            put('a'); put("0123456789ABCDEF"[ch]);
            putd("=", v < 0 ? 9999u : (uint32_t)v);
        }
        puts_("\r\n");
        break;
    }
    case 'c':
        if (fm1_crash.magic != FM1_CRASH_MAGIC) {
            puts_("no crash on record\r\n");
            break;
        }
        putd("count=", fm1_crash.count); putd("vec=", fm1_crash.vec); putd("up_ms=", fm1_crash.uptime_ms);
        puthex("pc=", fm1_crash.pc); puthex("rets=", fm1_crash.rets); puthex("sp=", fm1_crash.sp);
        puts_("\r\n");
        break;
    case 'u':
        puts_("UBOOT\r\n");
        fm1_delay_ms(30);
        pa_audio_stop();
        usb_detach();
        fm1_enter_uboot();
        break;
    case 'r':
        puts_("reboot\r\n");
        fm1_delay_ms(30);
        pa_audio_stop();
        usb_detach();
        fm1_reboot();
        break;
    }
}

static void app_main(void)
{
    uint32_t i, last_status = 0, last_bar = 0, shown_keys = ~0u, shown_mode = ~0u, peak_w = ~0u;
    {
        uint32_t f = irq_save();
        jedec = FL_FAR(fl_jedec_ram)();             /* read only: which flash part this unit has */
        irq_restore(f);
    }
    lcd_init();
    draw_static();
    lcd_cmd(0x29, 0, 0);                            /* DISPON after the first frame */
    pa_input_init();
    (void)pa_board_inputs();                        /* PC0, PC1, PA10, PC10 inputs, no pulls (the encoder is PC0/PC1) */
    usb_start();
    pa_audio_init(abuf_out, abuf_in, HALF_WORDS, isr_alnk0, 3);
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    if (fm1_crash.magic == FM1_CRASH_MAGIC)         /* the last run crashed ('c' has the details) */
        lcd_rect(200, 228, 24, 10, YEL);
    for (;;) {
        uint32_t now = ms;
        fm1_wdt_feed();
        usb_retry(now);
        if (usb.uboot_req)                          /* the SysEx soft key */
            command('u');
        if (!boots_ok && now > 2000u) {             /* it runs: the next boot is not a loop */
            boots_ok = 1;
            guard.pending = 0;
            guard.failed = 0;
        }
        if (shown_keys != pa_in.down) {
            uint32_t d = shown_keys ^ pa_in.down;
            for (i = 0; i < PA_KEYS; i++)
                if (d & (1u << i))
                    lcd_rect(KEY_X(i), 60, 32, 40, (pa_in.down >> i) & 1u ? CYN : DIM);
            shown_keys = pa_in.down;
        }
        if (shown_mode != a_mode) {                 /* the audio mode as a colour strip */
            static const uint16_t MC[5] = { DIM, GRN, BLU, RED, YEL };
            shown_mode = a_mode;
            lcd_rect(8, MODE_Y, 224, 12, MC[a_mode % 5u]);
        }
        if (now - last_bar >= 40u) {
            last_bar = now;
            draw_bar(176, in_peak24, 1u << 23, a_halves ? GRN : RED, &peak_w);   /* red: the link is not running */
            in_peak24 = 0;
        }
        while (ci_r != ci_w) {
            char c = (char)cdc_in[ci_r % CI_N];
            ci_r++;
            command(c);
        }
        if (led_walk && now - led_t >= 1000u) {      /* step 1..5: PCn low, others high; 6..10: PCn high, others low */
            uint32_t i, k = led_walk - 1u, lo = k < 5u;
            led_t = now;
            for (i = 5; i <= 9u; i++) {
                uint32_t on = (i == 5u + k % 5u) == lo ? 0u : 1u;
                if (on)
                    FM1_PR(FM1_PC, FM1_OUT) |= 1u << i;
                else
                    FM1_PR(FM1_PC, FM1_OUT) &= ~(1u << i);
            }
            putd("LED step ", led_walk); puts_(lo ? "low: PC" : "high: PC"); put((char)('5' + k % 5u)); puts_("\r\n");
            if (++led_walk > 10u) {
                led_walk = 0;
                FM1_PR(FM1_PC, FM1_OUT) |= 0x3E0u;   /* back to all high */
                puts_("LED walk done\r\n");
            }
        }
        if (now - last_status >= 500u) {
            last_status = now;
            status();
        }
        lcd_rect(232, 232, 8, 8, usb.config ? ((now >> 9) & 1u ? GRN : DIM) : RED);   /* USB + heartbeat */
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d;
    fm1_time_init();
    fm1_wdt_arm(0x0D);
    if (guard.magic != 0x50415431u) {               /* "PAT1" */
        guard.magic = 0x50415431u;
        guard.failed = 0;
        guard.pending = 0;
    }
    if (guard.pending)
        guard.failed++;
    guard.pending = 1;
    if (guard.failed >= 2u) {
        guard.failed = 0;
        guard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
    fm1_mailbox_clear();              /* as Felucca: the UBOOT request ("usb_update_mode") the soft key leaves at
                                       * 0x01C7FD80 must not outlive this boot (SRAM keeps it through a short power-off) */
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    app_main();
    for (;;)
        ;
}
