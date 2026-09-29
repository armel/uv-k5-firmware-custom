/* Compact sprintf/snprintf/vsnprintf for the firmware.
 *
 * A drop-in for the three entry points of external/printf/printf.c that the
 * firmware calls, with the same output for every conversion it uses:
 * flags '0' '-' '+' ' ', width and precision (digits or '*'), and
 * d i u o x X c s %. Length modifiers l h hh j z t are accepted and ignored
 * (int and long are both 32 bits here).
 *
 * The digit and padding logic follows printf.c step for step, including its
 * quirks (precision zeros only when right-aligned, a 32-digit work buffer).
 *
 * LIMITATIONS - no caller uses any of these; check here before adding a
 * format string that does:
 *  - '#' flag: not a flag here. "%#x" prints "#x" (printf.c: "0xff").
 *  - %b, %p, %f/%e/%g: print the letter only (printf.c: binary, pointer;
 *    floats were already disabled in printf_config.h).
 *  - hh/h: no truncation. "%hhd" of 300 prints "300" (printf.c: "44").
 *  - long long (%lld, %llu): not supported (already disabled in
 *    printf_config.h, where printf.c prints nothing for it).
 *  - printf_, vprintf_, fctprintf, _putchar: not provided; a call fails to
 *    link. Only sprintf_, snprintf_ and vsnprintf_ exist.
 *  - buffer: must be valid, except snprintf/vsnprintf with count 0, which
 *    only return the length (as printf.c does).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "external/printf/printf.h"

#define F_LEFT   1U
#define F_ZERO   2U
#define F_PLUS   4U
#define F_SPACE  8U
#define F_PREC   16U
#define F_UPPER  32U

#define NTOA_SIZE 32U

typedef struct {
    char   *buf;
    size_t  max;
    size_t  idx;
} Out_t;

static void Put(Out_t *o, char c)
{
    if (o->idx < o->max)
        o->buf[o->idx] = c;
    o->idx++;
}

static void Spaces(Out_t *o, unsigned int n)
{
    while (n--)
        Put(o, ' ');
}

static unsigned int ReadNumber(const char **f)
{
    unsigned int n = 0;
    while (**f >= '0' && **f <= '9')
        n = n * 10U + (unsigned int)(*(*f)++ - '0');
    return n;
}

static void Number(Out_t *o, unsigned int v, bool neg, unsigned int base, unsigned int prec, unsigned int width, unsigned int flags)
{
    char         buf[NTOA_SIZE];
    unsigned int len = 0;

    if (!(flags & F_PREC) || v) {
        do {
            const unsigned int d = v % base;
            buf[len++] = (char)(d < 10 ? '0' + d : ((flags & F_UPPER) ? 'A' : 'a') + d - 10);
            v /= base;
        } while (v && len < NTOA_SIZE);
    }

    if (!(flags & F_LEFT)) {
        if (width && (flags & F_ZERO) && (neg || (flags & (F_PLUS | F_SPACE))))
            width--;
        while (len < prec && len < NTOA_SIZE)
            buf[len++] = '0';
        while ((flags & F_ZERO) && len < width && len < NTOA_SIZE)
            buf[len++] = '0';
    }

    if (len < NTOA_SIZE) {
        if (neg)
            buf[len++] = '-';
        else if (flags & F_PLUS)
            buf[len++] = '+';
        else if (flags & F_SPACE)
            buf[len++] = ' ';
    }

    const size_t start = o->idx;
    if (!(flags & (F_LEFT | F_ZERO)) && len < width)
        Spaces(o, width - len);
    while (len)
        Put(o, buf[--len]);
    if (flags & F_LEFT) {
        while (o->idx - start < width)
            Put(o, ' ');
    }
}

int vsnprintf_(char *buffer, size_t count, const char *f, va_list va)
{
    Out_t o = { buffer, count, 0 };

    while (*f) {
        if (*f != '%') {
            Put(&o, *f++);
            continue;
        }
        f++;

        unsigned int flags = 0;
        for (;; f++) {
            if      (*f == '0') flags |= F_ZERO;
            else if (*f == '-') flags |= F_LEFT;
            else if (*f == '+') flags |= F_PLUS;
            else if (*f == ' ') flags |= F_SPACE;
            else break;
        }

        unsigned int width = 0;
        if (*f == '*') {
            const int w = va_arg(va, int);
            if (w < 0) {
                flags |= F_LEFT;
                width = (unsigned int)-w;
            } else {
                width = (unsigned int)w;
            }
            f++;
        } else {
            width = ReadNumber(&f);
        }

        unsigned int prec = 0;
        if (*f == '.') {
            flags |= F_PREC;
            f++;
            if (*f == '*') {
                const int p = va_arg(va, int);
                prec = p > 0 ? (unsigned int)p : 0U;
                f++;
            } else {
                prec = ReadNumber(&f);
            }
        }

        while (*f == 'l' || *f == 'h' || *f == 'j' || *f == 'z' || *f == 't')
            f++;

        const char c = *f;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'o' || c == 'x' || c == 'X') {
            unsigned int base = 10;
            if (c == 'o')
                base = 8;
            else if (c == 'x' || c == 'X')
                base = 16;
            if (c == 'X')
                flags |= F_UPPER;
            if (flags & F_PREC)
                flags &= ~F_ZERO;
            if (c == 'd' || c == 'i') {
                const int v = va_arg(va, int);
                Number(&o, v > 0 ? (unsigned int)v : 0U - (unsigned int)v, v < 0, base, prec, width, flags);
            } else {
                flags &= ~(F_PLUS | F_SPACE);
                Number(&o, va_arg(va, unsigned int), false, base, prec, width, flags);
            }
        } else if (c == 'c' || c == 's') {
            const char   *p;
            char          ch;
            unsigned int  l = 0;
            if (c == 'c') {
                ch = (char)va_arg(va, int);
                p  = &ch;
                l  = 1;
                flags |= F_PREC;
                prec = 1;
            } else {
                p = va_arg(va, const char *);
                while (p[l] && (!(flags & F_PREC) || l < prec))
                    l++;
            }
            if (!(flags & F_LEFT) && l < width)
                Spaces(&o, width - l);
            for (unsigned int i = 0; i < l; i++)
                Put(&o, p[i]);
            if ((flags & F_LEFT) && l < width)
                Spaces(&o, width - l);
        } else {
            Put(&o, c);   // "%%" and unknown conversions print the character
        }
        f++;
    }

    if (count)
        buffer[o.idx < count ? o.idx : count - 1U] = 0;

    return (int)o.idx;
}

int snprintf_(char *buffer, size_t count, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    const int ret = vsnprintf_(buffer, count, format, va);
    va_end(va);
    return ret;
}

int sprintf_(char *buffer, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    const int ret = vsnprintf_(buffer, (size_t)-1, format, va);
    va_end(va);
    return ret;
}
