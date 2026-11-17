// SPDX-License-Identifier: BSD-3-Clause
//
// strtod/atof/strtol/strtoul/atoi/atol для mini-libc.
//
// strtod: [пробелы][знак](цифры[.цифры] | .цифры)([eE][знак]цифры)?
//         плюс "inf"/"infinity"/"nan" (регистр не важен).
// Мантисса копится в unsigned long long (до 19 значащих цифр),
// экспонента применяется последовательным умножением/делением на 10.

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

static double pow10_pos(int e)
{
	double r = 1.0;

	while (e >= 20) {
		r *= 1e20;
		e -= 20;
	}

	while (e-- > 0)
		r *= 10.0;

	return r;
}

double strtod(const char *nptr, char **endptr)
{
	const char *s = nptr;
	const char *start;
	unsigned long long mant = 0;
	int mant_digits = 0;
	int exp10 = 0;
	int seen_digit = 0;
	int seen_dot = 0;
	int neg = 0;
	double val;

	if (endptr != NULL)
		*endptr = (char *)nptr;

	while (isspace((unsigned char)*s))
		s++;

	/* Знак. */
	if (*s == '+' || *s == '-') {
		neg = (*s == '-');
		s++;
	}

	/* inf / infinity / nan. */
	if ((s[0] == 'i' || s[0] == 'I') &&
	    (s[1] == 'n' || s[1] == 'N') &&
	    (s[2] == 'f' || s[2] == 'F')) {
		s += 3;

		if ((s[0] == 'i' || s[0] == 'I') &&
		    (s[1] == 'n' || s[1] == 'N') &&
		    (s[2] == 'i' || s[2] == 'I') &&
		    (s[3] == 't' || s[3] == 'T') &&
		    (s[4] == 'y' || s[4] == 'Y'))
			s += 5;

		if (endptr != NULL)
			*endptr = (char *)s;

		return neg ? -__builtin_huge_val() : __builtin_huge_val();
	}

	if ((s[0] == 'n' || s[0] == 'N') &&
	    (s[1] == 'a' || s[1] == 'A') &&
	    (s[2] == 'n' || s[2] == 'N')) {
		s += 3;

		if (endptr != NULL)
			*endptr = (char *)s;

		return __builtin_nan("");
	}

	start = s;

	/* Мантисса. */
	while (1) {
		if (*s >= '0' && *s <= '9') {
			seen_digit = 1;

			if (mant_digits < 19) {
				mant = mant * 10 + (unsigned)(*s - '0');
				mant_digits++;
			}

			if (seen_dot)
				exp10--;

			s++;
			continue;
		}

		if (*s == '.' && !seen_dot) {
			seen_dot = 1;
			s++;
			continue;
		}

		break;
	}

	if (!seen_digit) {
		/* Не число: откат к началу. */
		if (endptr != NULL)
			*endptr = (char *)nptr;
		return 0.0;
	}

	/* Экспонента. */
	if (*s == 'e' || *s == 'E') {
		const char *save = s;
		int esign = 0;
		int eval = 0;
		int edigits = 0;

		s++;

		if (*s == '+' || *s == '-') {
			esign = (*s == '-');
			s++;
		}

		while (*s >= '0' && *s <= '9') {
			if (eval < 100000)
				eval = eval * 10 + (*s - '0');

			edigits++;
			s++;
		}

		if (edigits > 0) {
			exp10 += esign ? -eval : eval;
		} else {
			s = save;	/* 'e' оказался не экспонентой */
		}
	}

	if (endptr != NULL)
		*endptr = (char *)s;

	if (s == start)
		return 0.0;

	val = (double)mant;

	if (exp10 > 0) {
		val *= pow10_pos(exp10);

		if (val > 1.7976931348623157e308) {
			errno = ERANGE;
			val = __builtin_huge_val();
		}
	} else if (exp10 < 0) {
		int e = -exp10;

		if (e > 320) {
			/* постепенное занижение, чтобы не уйти в 0 одним шагом */
			val /= pow10_pos(320);
			e -= 320;
		}

		val /= pow10_pos(e);

		if (val == 0.0 && mant != 0)
			errno = ERANGE;
	}

	return neg ? -val : val;
}

double atof(const char *nptr)
{
	return strtod(nptr, NULL);
}

long strtol(const char *nptr, char **endptr, int base)
{
	const char *s = nptr;
	unsigned long long acc = 0;
	int neg = 0;
	int any = 0;

	if (endptr != NULL)
		*endptr = (char *)nptr;

	while (isspace((unsigned char)*s))
		s++;

	if (*s == '+' || *s == '-') {
		neg = (*s == '-');
		s++;
	}

	if ((base == 0 || base == 16) && s[0] == '0' &&
	    (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	} else if (base == 0 && s[0] == '0') {
		base = 8;
	} else if (base == 0) {
		base = 10;
	}

	while (1) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			break;

		if (d >= base)
			break;

		acc = acc * (unsigned)base + (unsigned)d;
		any = 1;
		s++;
	}

	if (!any)
		return 0;

	if (endptr != NULL)
		*endptr = (char *)s;

	if (neg)
		return (long)(-(long long)acc);

	return (long)acc;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
	const char *s = nptr;
	unsigned long long acc = 0;
	int neg = 0;
	int any = 0;

	if (endptr != NULL)
		*endptr = (char *)nptr;

	while (isspace((unsigned char)*s))
		s++;

	if (*s == '+' || *s == '-') {
		neg = (*s == '-');
		s++;
	}

	if ((base == 0 || base == 16) && s[0] == '0' &&
	    (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	} else if (base == 0 && s[0] == '0') {
		base = 8;
	} else if (base == 0) {
		base = 10;
	}

	while (1) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			break;

		if (d >= base)
			break;

		acc = acc * (unsigned)base + (unsigned)d;
		any = 1;
		s++;
	}

	if (!any)
		return 0;

	if (endptr != NULL)
		*endptr = (char *)s;

	if (neg)
		return (unsigned long)(-(long long)acc);

	return (unsigned long)acc;
}

int atoi(const char *nptr)
{
	return (int)strtol(nptr, NULL, 10);
}

long atol(const char *nptr)
{
	return strtol(nptr, NULL, 10);
}

int abs(int j)
{
	return j < 0 ? -j : j;
}

long labs(long j)
{
	return j < 0 ? -j : j;
}
