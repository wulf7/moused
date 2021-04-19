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
#define	ARRAY_FOR_EACH(_arr, _elem) \
	for (size_t _i = 0; _i < ARRAY_LENGTH(_arr) && (_elem = &_arr[_i]); _i++)

struct libinput;
struct udev_device;

/* Recognized device types */
enum device_type {
	MOUSE_PROTO_MOUSE,
	MOUSE_PROTO_POINTINGSTICK,
	MOUSE_PROTO_TOUCHPAD,
	MOUSE_PROTO_TOUCHSCREEN,
	MOUSE_PROTO_TABLET,
	MOUSE_PROTO_TABLET_PAD,
	MOUSE_PROTO_KEYBOARD,
	MOUSE_PROTO_JOYSTICK,
};

/**
 * @ingroup base
 *
 * Log priority for internal logging messages.
 */
enum libinput_log_priority {
	LIBINPUT_LOG_PRIORITY_DEBUG = 10,
	LIBINPUT_LOG_PRIORITY_INFO = 20,
	LIBINPUT_LOG_PRIORITY_ERROR = 30,
};

/**
 * @ingroup base
 *
 * Log handler type for custom logging.
 *
 * @param libinput The libinput context
 * @param priority The priority of the current message
 * @param format Message format in printf-style
 * @param args Message arguments
 *
 * @see libinput_log_set_priority
 * @see libinput_log_get_priority
 * @see libinput_log_set_handler
 */
typedef void (*libinput_log_handler)(struct libinput *libinput,
				     enum libinput_log_priority priority,
				     const char *format, va_list args)
	   LIBINPUT_ATTRIBUTE_PRINTF(3, 0);

static inline struct udev_device *
udev_device_get_parent(struct udev_device *udev_device)
{
	return (NULL);
}
static inline char const *
udev_device_get_devnode(struct udev_device *udev_device)
{
	return ("");
}
static inline char const *
udev_device_get_property_value(struct udev_device *udev_device,
    char const *property)
{
	return (NULL);
}

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
char **	strv_from_string(const char *in, const char *separators);
bool	strendswith(const char *str, const char *suffix);
bool	parse_dimension_property(const char *prop, size_t *w, size_t *h);
bool	parse_range_property(const char *prop, int *hi, int *lo);
bool	parse_evcode_property(const char *prop, struct input_event *events,
	    size_t *nevents);
bool	parse_input_prop_property(const char *prop, unsigned int *props_out,
	    size_t *nprops);
