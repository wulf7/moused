/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2013-2015 Red Hat, Inc.
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

#pragma once

#define	versionsort	alphasort

#define LIBINPUT_ATTRIBUTE_PRINTF(_format, _args) \
	__attribute__ ((format (printf, _format, _args)))

#define	bit(x_) (1UL << (x_))

#define	ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])
/**
 * Iterate through the array _arr, assigning the variable elem to each
 * element. elem only exists within the loop.
 */
#define ARRAY_FOR_EACH(_arr, _elem) \
	for (__typeof__((_arr)[0]) *_elem = _arr; \
	     _elem < (_arr) + ARRAY_LENGTH(_arr); \
	     _elem++)

#define min(a, b) (((a) < (b)) ? (a) : (b))

#define	EVENT_CODE_UNDEFINED 0xffff

/* Supported device interfaces */
enum device_if {
	DEVICE_IF_UNKNOWN = -1,
	DEVICE_IF_EVDEV = 0,
	DEVICE_IF_SYSMOUSE,
};

/* Recognized device types */
enum device_type {
	DEVICE_TYPE_UNKNOWN = -1,
	DEVICE_TYPE_MOUSE = 0,
	DEVICE_TYPE_POINTINGSTICK,
	DEVICE_TYPE_TOUCHPAD,
	DEVICE_TYPE_TOUCHSCREEN,
	DEVICE_TYPE_TABLET,
	DEVICE_TYPE_TABLET_PAD,
	DEVICE_TYPE_KEYBOARD,
	DEVICE_TYPE_JOYSTICK,
};

struct device {
	const char *path;
	enum device_if iftype;
	enum device_type type;
	char name[80];
	char uniq[80];
	struct input_id id;
};

struct input_prop {
	unsigned int prop;
	bool enabled;
};

/**
 * @ingroup base
 *
 * Log handler type for custom logging.
 *
 * @param priority The priority of the current message
 * @param format Message format in printf-style
 * @param args Message arguments
 */
typedef void moused_log_handler(int priority, int errnum,
				const char *format, va_list args);
typedef int strv_foreach_callback_t(const char *str, size_t index, void *data);

bool	streq(const char *str1, const char *str2);
bool	strneq(const char *str1, const char *str2, int n);
void *	zalloc(size_t size);
char*	safe_strdup(const char *str);
__attribute__ ((format (printf, 2, 3)))
int	xasprintf(char **strp, const char *fmt, ...);
bool	safe_atoi_base(const char *str, int *val, int base);
static inline bool
safe_atoi(const char *str, int *val)
{
	return (safe_atoi_base(str, val, 10));
}
bool	safe_atou_base(const char *str, unsigned int *val, int base);
static inline bool
safe_atou(const char *str, unsigned int *val)
{
	return (safe_atou_base(str, val, 10));
}
bool	safe_atod(const char *str, double *val);
void	strv_free(char **strv);
const char *next_word(const char **state, size_t *len, const char *separators);
char **	strv_from_string(const char *in, const char *separators,
	    size_t *num_elements);
int	strv_for_each_n(const char **strv, size_t max,
	    strv_foreach_callback_t func, void *data);
bool	strendswith(const char *str, const char *suffix);
bool	strstartswith(const char *str, const char *prefix);
bool	parse_dimension_property(const char *prop, size_t *w, size_t *h);
bool	parse_range_property(const char *prop, int *hi, int *lo);
bool	parse_boolean_property(const char *prop, bool *b);
bool	parse_evcode_property(const char *prop, struct input_event *events,
	    size_t *nevents);
bool	parse_input_prop_property(const char *prop,
	    struct input_prop *props_out, size_t *nprops);
