// SPDX-License-Identifier: BSD-3-Clause

#include <ctype.h>

/*
 * ASCII-вариант без таблиц: простые диапазоны. Локали нет.
 * Значение аргумента приводится к unsigned char по стандарту.
 */

int isalnum(int c)
{
	return isalpha(c) || isdigit(c);
}

int isalpha(int c)
{
	unsigned char u = (unsigned char)c;

	return ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')) ? 1 : 0;
}

int iscntrl(int c)
{
	unsigned char u = (unsigned char)c;

	return (u < 0x20 || u == 0x7f) ? 1 : 0;
}

int isdigit(int c)
{
	unsigned char u = (unsigned char)c;

	return (u >= '0' && u <= '9') ? 1 : 0;
}

int isgraph(int c)
{
	return (isprint(c) && c != ' ') ? 1 : 0;
}

int islower(int c)
{
	unsigned char u = (unsigned char)c;

	return (u >= 'a' && u <= 'z') ? 1 : 0;
}

int isprint(int c)
{
	unsigned char u = (unsigned char)c;

	return (u >= 0x20 && u < 0x7f) ? 1 : 0;
}

int ispunct(int c)
{
	return (isgraph(c) && !isalnum(c)) ? 1 : 0;
}

int isspace(int c)
{
	unsigned char u = (unsigned char)c;

	return (u == ' ' || u == '\t' || u == '\n' || u == '\v' ||
		u == '\f' || u == '\r') ? 1 : 0;
}

int isupper(int c)
{
	unsigned char u = (unsigned char)c;

	return (u >= 'A' && u <= 'Z') ? 1 : 0;
}

int isxdigit(int c)
{
	unsigned char u = (unsigned char)c;

	return ((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') ||
		(u >= 'A' && u <= 'F')) ? 1 : 0;
}

int tolower(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 'a';

	return c;
}

int toupper(int c)
{
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 'A';

	return c;
}
