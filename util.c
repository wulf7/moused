/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2013-2019 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <sys/types.h>
#include <dev/evdev/input.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xlocale.h>

#include "util.h"
#include "util-evdev.h"
#include "util-list.h"

#define	HAVE_LOCALE_H	1

bool
streq(const char *str1, const char *str2)
{
	/* one NULL, one not NULL is always false */
	if (str1 && str2)
		return strcmp(str1, str2) == 0;
	return str1 == str2;
}

bool
strneq(const char *str1, const char *str2, int n)
{
	/* one NULL, one not NULL is always false */
	if (str1 && str2)
		return strncmp(str1, str2, n) == 0;
	return str1 == str2;
}

void *
zalloc(size_t size)
{
	void *p;

	/* We never need to alloc anything more than 1,5 MB so we can assume
	 * if we ever get above that something's going wrong */
	if (size > 1536 * 1024)
		assert(!"bug: internal malloc size limit exceeded");

	p = calloc(1, size);
	if (!p)
		abort();

	return p;
}

/**
 * strdup guaranteed to succeed. If the input string is NULL, the output
 * string is NULL. If the input string is a string pointer, we strdup or
 * abort on failure.
 */
char *
safe_strdup(const char *str)
{
	char *s;

	if (!str)
		return NULL;

	s = strdup(str);
	if (!s)
		abort();
	return s;
}

/**
 * Simple wrapper for asprintf that ensures the passed in-pointer is set
 * to NULL upon error.
 * The standard asprintf() call does not guarantee the passed in pointer
 * will be NULL'ed upon failure, whereas this wrapper does.
 *
 * @param strp pointer to set to newly allocated string.
 * This pointer should be passed to free() to release when done.
 * @param fmt the format string to use for printing.
 * @return The number of bytes printed (excluding the null byte terminator)
 * upon success or -1 upon failure. In the case of failure the pointer is set
 * to NULL.
 */
__attribute__ ((format (printf, 2, 3)))
int
xasprintf(char **strp, const char *fmt, ...)
{
	int rc = 0;
	va_list args;

	va_start(args, fmt);
	rc = vasprintf(strp, fmt, args);
	va_end(args);
	if ((rc == -1) && strp)
		*strp = NULL;

	return rc;
}

bool
safe_atoi_base(const char *str, int *val, int base)
{
	char *endptr;
	long v;

	assert(base == 10 || base == 16 || base == 8);

	errno = 0;
	v = strtol(str, &endptr, base);
	if (errno > 0)
		return false;
	if (str == endptr)
		return false;
	if (*str != '\0' && *endptr != '\0')
		return false;

	if (v > INT_MAX || v < INT_MIN)
		return false;

	*val = v;
	return true;
}

bool
safe_atou_base(const char *str, unsigned int *val, int base)
{
	char *endptr;
	unsigned long v;

	assert(base == 10 || base == 16 || base == 8);

	errno = 0;
	v = strtoul(str, &endptr, base);
	if (errno > 0)
		return false;
	if (str == endptr)
		return false;
	if (*str != '\0' && *endptr != '\0')
		return false;

	if ((long)v < 0)
		return false;

	*val = v;
	return true;
}

bool
safe_atod(const char *str, double *val)
{
	char *endptr;
	double v;
#ifdef HAVE_LOCALE_H
	locale_t c_locale;
#endif
	size_t slen = strlen(str);

	/* We don't have a use-case where we want to accept hex for a double
	 * or any of the other values strtod can parse */
	for (size_t i = 0; i < slen; i++) {
		char c = str[i];

		if (isdigit(c))
		       continue;
		switch(c) {
		case '+':
		case '-':
		case '.':
			break;
		default:
			return false;
		}
	}

#ifdef HAVE_LOCALE_H
	/* Create a "C" locale to force strtod to use '.' as separator */
	c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
	if (c_locale == (locale_t)0)
		return false;

	errno = 0;
	v = strtod_l(str, &endptr, c_locale);
	freelocale(c_locale);
#else
	/* No locale support in provided libc, assume it already uses '.' */
	errno = 0;
	v = strtod(str, &endptr);
#endif
	if (errno > 0)
		return false;
	if (str == endptr)
		return false;
	if (*str != '\0' && *endptr != '\0')
		return false;
	if (v != 0.0 && !isnormal(v))
		return false;

	*val = v;
	return true;
}

void
strv_free(char **strv) {
	char **s = strv;

	if (!strv)
		return;

	while (*s != NULL) {
		free(*s);
		*s = (char*)0x1; /* detect use-after-free */
		s++;
	}

	free (strv);
}

/**
 * Return the next word in a string pointed to by state before the first
 * separator character. Call repeatedly to tokenize a whole string.
 *
 * @param state Current state
 * @param len String length of the word returned
 * @param separators List of separator characters
 *
 * @return The first word in *state, NOT null-terminated
 */
const char *
next_word(const char **state, size_t *len, const char *separators)
{
	const char *next = *state;
	size_t l;

	if (!*next)
		return NULL;

	next += strspn(next, separators);
	if (!*next) {
		*state = next;
		return NULL;
	}

	l = strcspn(next, separators);
	*state = next + l;
	*len = l;

	return next;
}

/**
 * Return a null-terminated string array with the tokens in the input
 * string, e.g. "one two\tthree" with a separator list of " \t" will return
 * an array [ "one", "two", "three", NULL ] and num elements 3.
 *
 * Use strv_free() to free the array.
 *
 * Another example:
 *   result = strv_from_string("+1-2++3--4++-+5-+-", "+-", &nelem)
 *   result == [ "1", "2", "3", "4", "5", NULL ] and nelem == 5
 *
 * @param in Input string
 * @param separators List of separator characters
 * @param num_elements Number of elements found in the input string
 *
 * @return A null-terminated string array or NULL on errors
 */
char **
strv_from_string(const char *in, const char *separators, size_t *num_elements)
{
	assert(in != NULL);
	assert(separators != NULL);
	assert(num_elements != NULL);

	const char *s = in;
	size_t l, nelems = 0;
	while (next_word(&s, &l, separators) != NULL)
		nelems++;

	if (nelems == 0) {
		*num_elements = 0;
		return NULL;
	}

	size_t strv_len = nelems + 1; /* NULL-terminated */
	char **strv = zalloc(strv_len * sizeof *strv);

	size_t idx = 0;
	const char *word;
	s = in;
	while ((word = next_word(&s, &l, separators)) != NULL) {
		char *copy = strndup(word, l);
		if (!copy) {
                        strv_free(strv);
			*num_elements = 0;
			return NULL;
		}

		strv[idx++] = copy;
	}

	*num_elements = nelems;

	return strv;
}

/**
 * Return true if str ends in suffix, false otherwise. If the suffix is the
 * empty string, strendswith() always returns false.
 */
bool
strendswith(const char *str, const char *suffix)
{
	size_t slen = strlen(str);
	size_t suffixlen = strlen(suffix);
	size_t offset;

	if (slen == 0 || suffixlen == 0 || suffixlen > slen)
		return false;

	offset = slen - suffixlen;
	return strneq(&str[offset], suffix, suffixlen);
}

static inline bool
strstartswith(const char *str, const char *prefix)
{
	if (str == NULL)
		return false;

	size_t prefixlen = strlen(prefix);

	return prefixlen > 0 ? strneq(str, prefix, strlen(prefix)) : false;
}

/**
 * Parses a simple dimension string in the form of "10x40". The two
 * numbers must be positive integers in decimal notation.
 * On success, the two numbers are stored in w and h. On failure, w and h
 * are unmodified.
 *
 * @param prop The value of the property
 * @param w Returns the first component of the dimension
 * @param h Returns the second component of the dimension
 * @return true on success, false otherwise
 */
bool
parse_dimension_property(const char *prop, size_t *w, size_t *h)
{
	int x, y;

	if (!prop)
		return false;

	if (sscanf(prop, "%dx%d", &x, &y) != 2)
		return false;

	if (x <= 0 || y <= 0)
		return false;

	*w = (size_t)x;
	*h = (size_t)y;
	return true;
}

/**
 * Parses a string of the format "a:b" where both a and b must be integer
 * numbers and a > b. Also allowed is the special string value "none" which
 * amounts to unsetting the property.
 *
 * @param prop The value of the property
 * @param hi Set to the first digit or 0 in case of 'none'
 * @param lo Set to the second digit or 0 in case of 'none'
 * @return true on success, false otherwise
 */
bool
parse_range_property(const char *prop, int *hi, int *lo)
{
	int first, second;

	if (!prop)
		return false;

	if (streq(prop, "none")) {
		*hi = 0;
		*lo = 0;
		return true;
	}

	if (sscanf(prop, "%d:%d", &first, &second) != 2)
		return false;

	if (second >= first)
		return false;

	*hi = first;
	*lo = second;

	return true;
}

bool
parse_boolean_property(const char *prop, bool *b)
{
	if (!prop)
		return false;

	if (streq(prop, "1"))
		*b = true;
	else if (streq(prop, "0"))
		*b = false;
	else
		return false;

	return true;
}

static bool
parse_evcode_string(const char *s, int *type_out, int *code_out)
{
	int type, code;

	if (strstartswith(s, "EV_")) {
		type = libevdev_event_type_from_name(s);
		if (type == -1)
			return false;

		code = EVENT_CODE_UNDEFINED;
	} else {
		struct map {
			const char *str;
			int type;
		} map[] = {
			{ "KEY_", EV_KEY },
			{ "BTN_", EV_KEY },
			{ "ABS_", EV_ABS },
			{ "REL_", EV_REL },
			{ "SW_", EV_SW },
		};
		bool found = false;

		ARRAY_FOR_EACH(map, m) {
			if (!strstartswith(s, m->str))
				continue;

			type = m->type;
			code = libevdev_event_code_from_name(type, s);
			if (code == -1)
				return false;

			found = true;
			break;
		}
		if (!found)
			return false;
	}

	*type_out = type;
	*code_out = code;

	return true;
}

/**
 * Parses a string of the format "+EV_ABS;+KEY_A;-BTN_TOOL_DOUBLETAP;-ABS_X;"
 * where each element must be + or - (enable/disable) followed by a named event
 * type OR a named event code OR a tuple in the form of EV_KEY:0x123, i.e. a
 * named event type followed by a hex event code.
 *
 * events must point to an existing array of size nevents.
 * nevents specifies the size of the array in events and returns the number
 * of items, elements exceeding nevents are simply ignored, just make sure
 * events is large enough for your use-case.
 *
 * The results are returned as input events with type and code set, all
 * other fields undefined. Where only the event type is specified, the code
 * is set to EVENT_CODE_UNDEFINED.
 *
 * On success, events contains nevents events with each event's value set to 1
 * or 0 depending on the + or - prefix.
 */
bool
parse_evcode_property(const char *prop, struct input_event *events, size_t *nevents)
{
	bool rc = false;
	/* A randomly chosen max so we avoid crazy quirks */
	struct input_event evs[32];

	memset(evs, 0, sizeof evs);

	size_t ncodes;
	char **strv = strv_from_string(prop, ";", &ncodes);
	if (!strv || ncodes == 0 || ncodes > ARRAY_LENGTH(evs))
		goto out;

	ncodes = min(*nevents, ncodes);
	for (size_t idx = 0; strv[idx]; idx++) {
		char *s = strv[idx];
		bool enable;

		switch (*s) {
		case '+': enable = true; break;
		case '-': enable = false; break;
		default:
			goto out;
		}

		s++;

		int type, code;

		if (strstr(s, ":") == NULL) {
			if (!parse_evcode_string(s, &type, &code))
				goto out;
		} else {
			int consumed;
			char stype[13] = {0}; /* EV_FF_STATUS + '\0' */

			if (sscanf(s, "%12[A-Z_]:%x%n", stype, &code, &consumed) != 2 ||
			    strlen(s) != (size_t)consumed ||
			    (type = libevdev_event_type_from_name(stype)) == -1 ||
			    code < 0 || code > libevdev_event_type_get_max(type))
			    goto out;
		}

		evs[idx].type = type;
		evs[idx].code = code;
		evs[idx].value = enable;
	}

	memcpy(events, evs, ncodes * sizeof *events);
	*nevents = ncodes;
	rc = true;

out:
	strv_free(strv);
	return rc;
}

/**
 * Parses a string of the format "+INPUT_PROP_BUTTONPAD;-INPUT_PROP_POINTER;+0x123;"
 * where each element must be a named input prop OR a hexcode in the form
 * 0x1234. The prefix for each element must be either '+' (enable) or '-' (disable).
 *
 * props must point to an existing array of size nprops.
 * nprops specifies the size of the array in props and returns the number
 * of elements, elements exceeding nprops are simply ignored, just make sure
 * props is large enough for your use-case.
 *
 * On success, props contains nprops elements.
 */
bool
parse_input_prop_property(const char *prop, struct input_prop *props_out, size_t *nprops)
{
	bool rc = false;
	struct input_prop props[INPUT_PROP_CNT]; /* doubling up on quirks is a bug */

	size_t count;
	char **strv = strv_from_string(prop, ";", &count);
	if (!strv || count == 0 || count > ARRAY_LENGTH(props))
		goto out;

	count = min(*nprops, count);
	for (size_t idx = 0; strv[idx]; idx++) {
		char *s = strv[idx];
		unsigned int prop;
		bool enable;

		switch (*s) {
		case '+': enable = true; break;
		case '-': enable = false; break;
		default:
			goto out;
		}

		s++;

		if (safe_atou_base(s, &prop, 16)) {
			if (prop > INPUT_PROP_MAX)
				goto out;
		} else {
			int val = libevdev_property_from_name(s);
			if (val == -1)
				goto out;
			prop = (unsigned int)val;
		}
		props[idx].prop = prop;
		props[idx].enabled = enable;
	}

	memcpy(props_out, props, count * sizeof *props);
	*nprops = count;
	rc = true;

out:
	strv_free(strv);
	return rc;
}
