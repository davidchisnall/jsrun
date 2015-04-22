/*
 * Copyright (c) 2015 David Chisnall
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * $FreeBSD$
 */
#include "jsrun.h"
#include <assert.h>
#include <stdbool.h>

/**
 * Helper that adds an immutable integer property to the object at the top of
 * the stack.
 */
static inline void
add_immutable_int_prop(duk_context *ctx, const char *name, duk_int_t value)
{
	duk_push_string(ctx, name);
	duk_push_int(ctx, value);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE |
			DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_ENUMERABLE);
}

/*******************************************************************************
 * Functions for ArrayBuffer objects
 ******************************************************************************/

/**
 * Constructor for ArrayBuffer objects.  
 */
static duk_ret_t
array_buffer_constructor(duk_context *ctx)
{
	if (!duk_is_constructor_call(ctx))
	{
		return 0;
	}
	duk_int_t size = duk_get_int(ctx, 0);
	duk_push_this(ctx);
	duk_push_fixed_buffer(ctx, size);
	duk_put_prop_string(ctx, -2, "\xFF" "buffer");
	add_immutable_int_prop(ctx, "length", size);
	return 1;
}

// FIXME: isView(), transfer() need implementations


/*******************************************************************************
 * Handler functions for typed array proxy objects.  
 ******************************************************************************/

/**
 * Helper that determines if the top object on the stack is a number, or a
 * string that was coerced from a number.
 */
static inline bool
is_number(duk_context *ctx)
{
	if (duk_is_number(ctx, -1))
	{
		return true;
	}
	// Copy the top value and coerce it to a number
	duk_dup_top(ctx);
	duk_to_number(ctx, -1);
	// If the value coerced to a number compares equal to the original, then
	// it's a number.
	bool ret = duk_equals(ctx, -1, -2);
	duk_pop(ctx); // Value coerced to number
	return ret;
}

static duk_ret_t
typed_array_handler_has(duk_context *ctx)
{
	// Key is on the top of the stack
	if (is_number(ctx))
	{
		duk_uint_t idx = duk_get_uint(ctx, -1);
		// Copy the typed array ref to the top of the stack.
		duk_dup(ctx, -2);
		duk_get_prop_string(ctx, -1, "length");
		duk_uint_t length = duk_get_uint(ctx, -1);
		duk_pop(ctx); // length
		if (idx + 1 < length)
		{
			duk_push_boolean(ctx, true);
			return 1;
		}
		duk_pop(ctx); // typed array
		// Now we fall through to the normal case and handle the property as a
		// property.
	}
	duk_push_boolean(ctx, duk_has_prop(ctx, -2));
	return 1;
}
static duk_ret_t
typed_array_handler_enumerate(duk_context *ctx)
{
	duk_get_prop_string(ctx, -1, "length");
	duk_uint_t length = duk_get_uint(ctx, -1);
	duk_pop(ctx); // length
	duk_push_global_object(ctx);
	duk_get_prop_string(ctx, -1, "Object");
	duk_remove(ctx, -2);
	duk_get_prop_string(ctx, -1, "getOwnPropertyNames");
	duk_swap_top(ctx, -2);
	duk_dup(ctx, 0);
	assert(duk_is_function(ctx, -3));
	assert(duk_is_object(ctx, -2));
	assert(duk_is_object(ctx, -1));
	// FIXME: Error handling
	duk_call_method(ctx, 1);
	// Stack now contains an array of properties.
	duk_get_prop_string(ctx, -1, "push");
	for (duk_uint_t i=0 ; i<length ; i++)
	{
		duk_dup(ctx, -1); // push()
		duk_dup(ctx, -3); // array
		duk_push_uint(ctx, i);
		duk_to_string(ctx, -1);
		duk_call_method(ctx, 1);
		duk_pop(ctx); // undefined (result of push())
	}
	duk_pop(ctx); // push()
	return 1;
}

static inline void *
typed_array_get_inbounds(duk_context *ctx, size_t type_size)
{
	if (is_number(ctx))
	{
		duk_to_number(ctx, -1);
		duk_uint_t idx = duk_get_uint(ctx, -1);
		// Copy the typed array ref to the top of the stack.
		duk_dup(ctx, -2);
		duk_get_prop_string(ctx, -1, "byteLength");
		duk_uint_t length = duk_get_uint(ctx, -1);
		duk_pop(ctx); // length
		// Check that reading the entire value won't go over the end of the buffer
		if (((idx + 1) * type_size) <= length)
		{
			// Get the ArrayBuffer
			duk_get_prop_string(ctx, -1, "buffer");
			// Get the Duktape.Buffer
			duk_get_prop_string(ctx, -1, "\xFF" "buffer");
			duk_size_t size;
			char *buffer = duk_get_buffer(ctx, -1, &size);
			assert(idx * type_size + type_size <= size);
			return buffer + idx * type_size;
		}
		duk_pop(ctx); // typed array
		// Now we fall through to the normal case and handle the property as a
		// property.
	}
	return NULL;
}

#define TYPED_ARRAY_CASE(name, type)\
static duk_ret_t                                                              \
typed_array_handler_##name##_get(duk_context *ctx)                            \
{                                                                             \
	duk_pop(ctx);                                                             \
	type *ptr = typed_array_get_inbounds(ctx, sizeof(type));                  \
	if (ptr)                                                                  \
	{                                                                         \
		duk_push_number(ctx, (double)*ptr);                                   \
	}                                                                         \
	else                                                                      \
	{                                                                         \
		assert(duk_get_top(ctx) == 2);                                        \
		duk_get_prop(ctx, 0);                                                 \
	}                                                                         \
	return 1;                                                                 \
}                                                                             \
static duk_ret_t                                                              \
typed_array_handler_##name##_set(duk_context *ctx)                            \
{                                                                             \
	duk_dup(ctx, 0);                                                          \
	duk_dup(ctx, 1);                                                          \
	type *ptr = typed_array_get_inbounds(ctx, sizeof(type));                  \
	duk_pop(ctx);                                                             \
	duk_pop(ctx);                                                             \
	if (ptr)                                                                  \
	{                                                                         \
		*ptr = (type)duk_get_number(ctx, 2);                                  \
	}                                                                         \
	else                                                                      \
	{                                                                         \
		assert(duk_get_top(ctx) == 3);                                        \
		duk_put_prop(ctx, 0);                                                 \
	}                                                                         \
	return 0;                                                                 \
}                                                                             \
static duk_ret_t                                                              \
typed_array_##name##_constructor(duk_context *ctx)                            \
{                                                                             \
	if (duk_is_number(ctx, 0))                                                \
	{                                                                         \
		duk_push_global_object(ctx);                                          \
		duk_get_prop_string(ctx, -1, "ArrayBuffer");                          \
		duk_uint_t size = duk_get_uint(ctx, 0);                               \
		duk_push_uint(ctx, size * sizeof(type));                              \
		duk_new(ctx, 1);                                                      \
		duk_swap_top(ctx, 0);                                                 \
		duk_pop_2(ctx);                                                       \
		/* At this point, we've replaced the length with a buffer */          \
		assert(duk_get_top(ctx) == 1);                                        \
	}                                                                         \
	duk_get_prop_string(ctx, 0, "constructor");                               \
	if (duk_is_c_function(ctx, -1))                                           \
	{                                                                         \
		duk_c_function constructor = duk_get_c_function(ctx, -1);             \
		if (constructor == array_buffer_constructor)                          \
		{                                                                     \
			duk_push_this(ctx);                                               \
			duk_push_string(ctx, "buffer");                                   \
			duk_dup(ctx, 0); /* [this, "buffer", ArrayBuffer] */              \
			duk_get_prop_string(ctx, -1, "length");                           \
			duk_uint_t byteLength = duk_get_uint(ctx, -1);                    \
			duk_pop(ctx); /* byte length */                                   \
			duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE |                    \
					DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_HAVE_ENUMERABLE | \
					DUK_DEFPROP_ENUMERABLE);                                  \
			/* New object is now top of the stack */                          \
			add_immutable_int_prop(ctx, "byteLength", byteLength);            \
			add_immutable_int_prop(ctx, "byteOffset", 0);                     \
			add_immutable_int_prop(ctx, "length", byteLength / sizeof(type)); \
			duk_get_prop_string(ctx, -1, "\xFF" "handlers");                  \
			duk_push_global_object(ctx);                                      \
			duk_get_prop_string(ctx, -1, "Proxy");                            \
			duk_remove(ctx, -2); /* global object. */                         \
			duk_insert(ctx, -3); /* [Proxy, new object, handlers] */          \
			duk_new(ctx, 2);                                                  \
			return 1;                                                         \
		}                                                                     \
	}                                                                         \
	return DUK_RET_TYPE_ERROR;                                                \
}
#include "arraykinds.inc"


static duk_ret_t
typed_array_constructor(duk_context *ctx)
{
	return DUK_RET_TYPE_ERROR;
}


/*******************************************************************************
 * Functions for DataView objects
 ******************************************************************************/
static duk_ret_t
data_view_constructor(duk_context *ctx)
{
	if (!duk_is_constructor_call(ctx))
	{
		return 0;
	}
	duk_int_t args = duk_get_top(ctx);
	if (args < 1)
	{
		return DUK_RET_TYPE_ERROR;
	}
	duk_int_t offset = args > 1 ? duk_get_int(ctx, 1) : 0;
	// Put the buffer on the top of the stack
	duk_dup(ctx, 0);
	// TODO: Check that arg 0 really is an ArrayBuffer

	duk_push_this(ctx);
	duk_push_string(ctx, "buffer");
	duk_dup(ctx, 0);
	duk_get_prop_string(ctx, -1, "length");
	duk_int_t length = duk_get_int(ctx, -1);
	duk_pop(ctx); // length
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE |
			DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_ENUMERABLE);

	duk_int_t byteLength = args > 2 ? duk_get_int(ctx, 2)
	                                  : (length - offset);
	if (length - offset < byteLength)
	{
		byteLength = length - offset;
	}
	add_immutable_int_prop(ctx, "byteLength", byteLength);
	add_immutable_int_prop(ctx, "byteOffset", offset);
	return 1;
}

/**
 * Helper function for DataView accessors, which returns a pointer to the
 * correct element in the array.
 */
static inline void *
get_buffer_pointer(duk_context *ctx, size_t type_size)
{
	duk_int_t offset = duk_get_int(ctx, 0);
	duk_push_this(ctx);
	duk_get_prop_string(ctx, -1, "byteOffset");
	offset += duk_get_int(ctx, -1);
	duk_pop(ctx);
	duk_get_prop_string(ctx, -1, "byteLength");
	duk_int_t length = duk_get_int(ctx, -1);
	duk_pop(ctx);
	if (offset + type_size >= length)
	{
		return 0;
	}
	duk_get_prop_string(ctx, -1, "buffer");
	duk_get_prop_string(ctx, -1, "\xFF" "buffer");
	duk_size_t size;
	char *buffer = duk_get_buffer(ctx, -1, &size);
	assert(offset + type_size < size);
	return buffer + offset;
}

#define TYPED_ARRAY_CASE(name, type) \
static duk_ret_t                                                               \
dataview_get_##name##_method(duk_context *ctx)                                 \
{                                                                              \
	type *addr = get_buffer_pointer(ctx, sizeof(type));                        \
	if (addr == NULL)                                                          \
	{                                                                          \
		return DUK_RET_RANGE_ERROR;                                            \
	}                                                                          \
	duk_push_number(ctx, (double)*addr);                                       \
	return 1;                                                                  \
}                                                                              \
                                                                               \
static duk_ret_t                                                               \
dataview_set_##name##_method(duk_context *ctx)                                 \
{                                                                              \
	type *addr = get_buffer_pointer(ctx, sizeof(type));                        \
	if (addr == NULL)                                                          \
	{                                                                          \
		return DUK_RET_RANGE_ERROR;                                            \
	}                                                                          \
	duk_double_t val = duk_get_number(ctx, 1);                                 \
	*addr = (type)val;                                                         \
	return 1;                                                                  \
}

// TODO: It's not clear from the spec what should happen if we're given a
// negative offset that is still within the bounds of the buffer.

#include "arraykinds.inc"

void *
duk_typed_array_buffer_get(duk_context *ctx, duk_size_t *size)
{
	// If this is a raw buffer, then just return its contents.
	if (duk_is_buffer(ctx, -1))
	{
		return duk_get_buffer(ctx, -1, size);
	}
	// If it's an objects, then see if it is something that wraps a buffer
	if (duk_is_object(ctx, -1))
	{
		duk_get_prop_string(ctx, 0, "constructor");
		if (!duk_is_c_function(ctx, -1))
		{
			duk_pop(ctx); // constructor
			return NULL;
		}
		duk_c_function constructor = duk_get_c_function(ctx, -1);
		if (constructor == array_buffer_constructor)
		{
			duk_get_prop_string(ctx, -3, "\xFF" "buffer");
			void *ptr = duk_get_buffer(ctx, -1, size);
			duk_pop_3(ctx);
			return ptr;
		}
		else
		{
			// If it's not an array buffer, then see if it's something that
			// contains an array buffer as a buffer property.
			duk_get_prop_string(ctx, -1, "buffer");
			void *ptr = duk_typed_array_buffer_get(ctx, size);
			duk_pop_2(ctx); // constructor, array buffer
			return ptr;
		}
	}
	return NULL;
}

void *
duk_push_array_buffer(duk_context *ctx, duk_size_t size)
{
	duk_push_global_object(ctx);
	duk_get_prop_string(ctx, -1, "ArrayBuffer");
	duk_push_uint(ctx, size);
	duk_new(ctx, 1);
	duk_get_prop_string(ctx, -1, "\xFF" "buffer");
	void *ptr = duk_get_buffer(ctx, -1, NULL);
	duk_pop(ctx); // internal buffer
	duk_remove(ctx, -2);
	return ptr;
}

void
init_typed_array(duk_context *ctx)
{
	duk_push_global_object(ctx);
	// Set up ArrayBuffer constructor
	duk_push_c_function(ctx, array_buffer_constructor, 1);
	duk_push_string(ctx, "ArrayBuffer");
	duk_put_prop_string(ctx, -2, "name");
	// Set up ArrayBuffer prototype
	duk_push_object(ctx);
	duk_dup(ctx, -2);
	duk_put_prop_string(ctx, -2, "constructor");
	duk_push_int(ctx, 0);
	duk_put_prop_string(ctx, -2, "length");
	duk_compact(ctx, -1);
	// TODO: Add slice method
	duk_put_prop_string(ctx, -2, "prototype");
	duk_put_prop_string(ctx, -2, "ArrayBuffer");

	// Add the DataView constructor
	duk_push_c_function(ctx, data_view_constructor, 1);
	duk_push_string(ctx, "DataView");
	duk_put_prop_string(ctx, -2, "name");
	// Add the DataView prototype 
	duk_push_object(ctx);
	duk_dup(ctx, -2);
	duk_put_prop_string(ctx, -2, "constructor");
	duk_push_int(ctx, 0);
	duk_put_prop_string(ctx, -2, "length");
	duk_compact(ctx, -1);
#define TYPED_ARRAY_CASE(name, type)\
	duk_push_c_function(ctx, dataview_get_##name##_method, 1);\
	duk_put_prop_string(ctx, -2, "get" #name);\
	duk_push_c_function(ctx, dataview_set_##name##_method, 2);\
	duk_put_prop_string(ctx, -2, "set" #name);
#include "arraykinds.inc"
	duk_put_prop_string(ctx, -2, "prototype");
	duk_put_prop_string(ctx, -2, "DataView");


	// Set up the TypedArray prototype and constructor.  These are then the
	// constructor and prototype for the concrete TypedArray subtypes.
	duk_idx_t ta_constructor = duk_push_c_function(ctx,
			typed_array_constructor, -1);
	duk_idx_t ta_prototype = duk_push_object(ctx);
	duk_dup(ctx, -2); // [constructor, prototype, constructor]
	duk_dup(ctx, -2);
	duk_put_prop_string(ctx, -2, "prototype");
	duk_push_string(ctx, "TypedArray");
	duk_put_prop_string(ctx, -2, "name");
	duk_pop(ctx); // TypedArray constructor.
	// Set Array and Array.prototype as the prototypes of the ta constructor
	// and prototype
	duk_push_global_object(ctx);
	duk_dup(ctx, ta_constructor);
	// TODO: There are a load of methods that should be defined on TypedArray,
	// many of which can be stolen from Array.
	duk_put_prop_string(ctx, -2, "TypedArray");
#ifndef NDEBUG
	duk_idx_t top = duk_get_top(ctx);
#endif
	// FIXME: Typed array constructor should be variadic
	// FIXME: Set constructor and prototype to have their prototypes be the
	// generic typed array one
#define TYPED_ARRAY_CASE(name, type)                                           \
	duk_push_c_function(ctx, typed_array_##name##_constructor, 1);             \
	duk_dup(ctx, ta_constructor);                                              \
	duk_set_prototype(ctx, -2);                                                \
	duk_push_object(ctx);                                                      \
	duk_dup(ctx, ta_prototype);                                                \
	duk_set_prototype(ctx, -2);                                                \
	duk_dup(ctx, -2);                                                          \
	duk_put_prop_string(ctx, -2, "constructor");                               \
	duk_push_object(ctx); /* handlers object */                                \
	duk_push_c_function(ctx, typed_array_handler_has, 2);                      \
	duk_put_prop_string(ctx, -2, "has");                                       \
	duk_push_c_function(ctx, typed_array_handler_##name##_get, 3);             \
	duk_put_prop_string(ctx, -2, "get");                                       \
	duk_push_c_function(ctx, typed_array_handler_##name##_set, 4);             \
	duk_put_prop_string(ctx, -2, "set");                                       \
	duk_push_c_function(ctx, typed_array_handler_enumerate, 1);                \
	duk_put_prop_string(ctx, -2, "enumerate");                                 \
	duk_push_c_function(ctx, typed_array_handler_enumerate, 1);                \
	duk_put_prop_string(ctx, -2, "ownKeys");                                   \
	duk_put_prop_string(ctx, -2, "\xFF" "handlers");                           \
	duk_put_prop_string(ctx, -2, "prototype");                                 \
	duk_put_prop_string(ctx, -2, #name "Array");                               \
	assert(top == duk_get_top(ctx));
#include "arraykinds.inc"
	duk_pop(ctx); // Global object
	duk_pop(ctx); // TypedArray prototype
	duk_pop(ctx); // TypedArray constructor.
	duk_pop(ctx); // Global object
}
