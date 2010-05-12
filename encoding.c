/* 
 * MacRuby implementation of Ruby 1.9 String.
 *
 * This file is covered by the Ruby license. See COPYING for more details.
 * 
 * Copyright (C) 2007-2010, Apple Inc. All rights reserved.
 * Copyright (C) 1993-2007 Yukihiro Matsumoto
 * Copyright (C) 2000 Network Applied Communication Laboratory, Inc.
 * Copyright (C) 2000 Information-technology Promotion Agency, Japan
 */

#include <string.h>

#include "ruby.h"
#include "ruby/encoding.h"
#include "encoding.h"

VALUE rb_cEncoding;

rb_encoding_t *default_internal = NULL;
static rb_encoding_t *default_external = NULL;
rb_encoding_t *rb_encodings[ENCODINGS_COUNT];

static void str_undefined_update_flags(rb_str_t *self) { abort(); }
static void str_undefined_make_data_binary(rb_str_t *self) { abort(); }
static bool str_undefined_try_making_data_uchars(rb_str_t *self) { abort(); }
static long str_undefined_length(rb_str_t *self, bool ucs2_mode) { abort(); }
static long str_undefined_bytesize(rb_str_t *self) { abort(); }
static character_boundaries_t str_undefined_get_character_boundaries(rb_str_t *self, long index, bool ucs2_mode) { abort(); }
static long str_undefined_offset_in_bytes_to_index(rb_str_t *self, long offset_in_bytes, bool ucs2_mode) { abort(); }
static void str_undefined_transcode_to_utf16(struct rb_encoding *src_enc, rb_str_t *self, long *pos, UChar **utf16, long *utf16_length) { abort(); }
static void str_undefined_transcode_from_utf16(struct rb_encoding *dst_enc, UChar *utf16, long utf16_length, long *pos, char **bytes, long *bytes_length) { abort(); }

static VALUE
mr_enc_s_list(VALUE klass, SEL sel)
{
    VALUE ary = rb_ary_new2(ENCODINGS_COUNT);
    for (unsigned int i = 0; i < ENCODINGS_COUNT; ++i) {
	rb_ary_push(ary, (VALUE)rb_encodings[i]);
    }
    return ary;
}

static VALUE
mr_enc_s_name_list(VALUE klass, SEL sel)
{
    VALUE ary = rb_ary_new();
    for (unsigned int i = 0; i < ENCODINGS_COUNT; ++i) {
	rb_encoding_t *encoding = RENC(rb_encodings[i]);
	// TODO: use US-ASCII strings
	rb_ary_push(ary, rb_usascii_str_new2(encoding->public_name));
	for (unsigned int j = 0; j < encoding->aliases_count; ++j) {
	    rb_ary_push(ary, rb_usascii_str_new2(encoding->aliases[j]));
	}
    }
    return ary;
}

static VALUE
mr_enc_s_aliases(VALUE klass, SEL sel)
{
    VALUE hash = rb_hash_new();
    for (unsigned int i = 0; i < ENCODINGS_COUNT; ++i) {
	rb_encoding_t *encoding = RENC(rb_encodings[i]);
	for (unsigned int j = 0; j < encoding->aliases_count; ++j) {
	    rb_hash_aset(hash, rb_usascii_str_new2(encoding->aliases[j]),
		    rb_usascii_str_new2(encoding->public_name));
	}
    }
    return hash;
}

static VALUE
mr_enc_s_find(VALUE klass, SEL sel, VALUE name)
{
    StringValue(name);
    rb_encoding_t *enc = rb_enc_find(RSTRING_PTR(name));
    if (enc == NULL) {
	rb_raise(rb_eArgError, "unknown encoding name - %s",
		RSTRING_PTR(name));
    }
    return (VALUE)enc;
}

static VALUE
mr_enc_s_default_internal(VALUE klass, SEL sel)
{
    return (VALUE)default_internal;
}

static VALUE
mr_enc_set_default_internal(VALUE klass, SEL sel, VALUE enc)
{
    default_internal = rb_to_encoding(enc);
    return (VALUE)default_internal;
}

static VALUE
mr_enc_s_default_external(VALUE klass, SEL sel)
{
    return (VALUE)default_external;
}

static VALUE
mr_enc_set_default_external(VALUE klass, SEL sel, VALUE enc)
{
    default_external = rb_to_encoding(enc);
    return (VALUE)default_external;
}

static VALUE
mr_enc_name(VALUE self, SEL sel)
{
    return rb_usascii_str_new2(RENC(self)->public_name);
}

static VALUE
mr_enc_inspect(VALUE self, SEL sel)
{
    return rb_sprintf("#<%s:%s>", rb_obj_classname(self),
	    RENC(self)->public_name);
}

static VALUE
mr_enc_names(VALUE self, SEL sel)
{
    rb_encoding_t *encoding = RENC(self);

    VALUE ary = rb_ary_new2(encoding->aliases_count + 1);
    rb_ary_push(ary, rb_usascii_str_new2(encoding->public_name));
    for (unsigned int i = 0; i < encoding->aliases_count; ++i) {
	rb_ary_push(ary, rb_usascii_str_new2(encoding->aliases[i]));
    }
    return ary;
}

static VALUE
mr_enc_ascii_compatible_p(VALUE self, SEL sel)
{
    return RENC(self)->ascii_compatible ? Qtrue : Qfalse;
}

static VALUE
mr_enc_dummy_p(VALUE self, SEL sel)
{
    return Qfalse;
}

static void
define_encoding_constant(const char *name, rb_encoding_t *encoding)
{
    char c = name[0];
    if ((c >= '0') && (c <= '9')) {
	// constants can't start with a number
	return;
    }

    char *name_copy = strdup(name);
    if ((c >= 'a') && (c <= 'z')) {
	// the first character must be upper case
	name_copy[0] = c - ('a' - 'A');
    }

    // '.' and '-' must be transformed into '_'
    for (int i = 0; name_copy[i]; ++i) {
	if ((name_copy[i] == '.') || (name_copy[i] == '-')) {
	    name_copy[i] = '_';
	}
    }
    rb_define_const(rb_cEncoding, name_copy, (VALUE)encoding);
    free(name_copy);
}

extern void enc_init_ucnv_encoding(rb_encoding_t *encoding);

enum {
    ENCODING_TYPE_SPECIAL = 0,
    ENCODING_TYPE_UCNV
};

static void
add_encoding(
	unsigned int encoding_index, // index of the encoding in the encodings
				     // array
	unsigned int rb_encoding_type,
	const char *public_name, // public name for the encoding
	unsigned char min_char_size,
	bool single_byte_encoding, // in the encoding a character takes only
				   // one byte
	bool ascii_compatible, // is the encoding ASCII compatible or not
	... // aliases for the encoding (should no include the public name)
	    // - must end with a NULL
	)
{
    assert(encoding_index < ENCODINGS_COUNT);

    // create an array for the aliases
    unsigned int aliases_count = 0;
    va_list va_aliases;
    va_start(va_aliases, ascii_compatible);
    while (va_arg(va_aliases, const char *) != NULL) {
	++aliases_count;
    }
    va_end(va_aliases);
    const char **aliases = (const char **)
	malloc(sizeof(const char *) * aliases_count);
    va_start(va_aliases, ascii_compatible);
    for (unsigned int i = 0; i < aliases_count; ++i) {
	aliases[i] = va_arg(va_aliases, const char *);
    }
    va_end(va_aliases);

    // create the MacRuby object
    NEWOBJ(encoding, rb_encoding_t);
    encoding->basic.flags = 0;
    encoding->basic.klass = rb_cEncoding;
    rb_encodings[encoding_index] = encoding;
    GC_RETAIN(encoding); // it should never be deallocated

    // fill the fields
    encoding->index = encoding_index;
    encoding->public_name = public_name;
    encoding->min_char_size = min_char_size;
    encoding->single_byte_encoding = single_byte_encoding;
    encoding->ascii_compatible = ascii_compatible;
    encoding->aliases_count = aliases_count;
    encoding->aliases = aliases;

    // fill the default implementations with aborts
    encoding->methods.update_flags = str_undefined_update_flags;
    encoding->methods.make_data_binary = str_undefined_make_data_binary;
    encoding->methods.try_making_data_uchars =
	str_undefined_try_making_data_uchars;
    encoding->methods.length = str_undefined_length;
    encoding->methods.bytesize = str_undefined_bytesize;
    encoding->methods.get_character_boundaries =
	str_undefined_get_character_boundaries;
    encoding->methods.offset_in_bytes_to_index =
	str_undefined_offset_in_bytes_to_index;
    encoding->methods.transcode_to_utf16 =
	str_undefined_transcode_to_utf16;
    encoding->methods.transcode_from_utf16 =
	str_undefined_transcode_from_utf16;

    switch (rb_encoding_type) {
	case ENCODING_TYPE_SPECIAL:
	    break;
	case ENCODING_TYPE_UCNV:
	    enc_init_ucnv_encoding(encoding);
	    break;
	default:
	    abort();
    }
}

// This Init function is called very early. Do not use any runtime method
// because things may not be initialized properly yet.
void
Init_PreEncoding(void)
{
    add_encoding(ENCODING_BINARY,      ENCODING_TYPE_SPECIAL, "ASCII-8BIT",  1, true,  true,  "BINARY", NULL);
    add_encoding(ENCODING_ASCII,       ENCODING_TYPE_UCNV,    "US-ASCII",    1, true,  true,  "ASCII", "ANSI_X3.4-1968", "646", NULL);
    add_encoding(ENCODING_UTF8,        ENCODING_TYPE_UCNV,    "UTF-8",       1, false, true,  "CP65001", "locale", NULL);
    add_encoding(ENCODING_UTF16BE,     ENCODING_TYPE_UCNV,    "UTF-16BE",    2, false, false, NULL);
    add_encoding(ENCODING_UTF16LE,     ENCODING_TYPE_UCNV,    "UTF-16LE",    2, false, false, NULL);
    add_encoding(ENCODING_UTF32BE,     ENCODING_TYPE_UCNV,    "UTF-32BE",    4, false, false, "UCS-4BE", NULL);
    add_encoding(ENCODING_UTF32LE,     ENCODING_TYPE_UCNV,    "UTF-32LE",    4, false, false, "UCS-4LE", NULL);
    add_encoding(ENCODING_ISO8859_1,   ENCODING_TYPE_UCNV,    "ISO-8859-1",  1, true,  true,  "ISO8859-1", NULL);
    add_encoding(ENCODING_MACROMAN,    ENCODING_TYPE_UCNV,    "macRoman",    1, true,  true,  NULL);
    add_encoding(ENCODING_MACCYRILLIC, ENCODING_TYPE_UCNV,    "macCyrillic", 1, true,  true,  NULL);
    add_encoding(ENCODING_BIG5,        ENCODING_TYPE_UCNV,    "Big5",        1, false, true,  "CP950", NULL);
    // FIXME: the ICU conversion tables do not seem to match Ruby's Japanese conversion tables
    add_encoding(ENCODING_EUCJP,       ENCODING_TYPE_UCNV,    "EUC-JP",      1, false, true,  "eucJP", NULL);
    //add_encoding(ENCODING_EUCJP,     ENCODING_TYPE_RUBY, "EUC-JP",      1, false, true,  "eucJP", NULL);
    //add_encoding(ENCODING_SJIS,      ENCODING_TYPE_RUBY, "Shift_JIS",   1, false, true, "SJIS", NULL);
    //add_encoding(ENCODING_CP932,     ENCODING_TYPE_RUBY, "Windows-31J", 1, false, true, "CP932", "csWindows31J", NULL);

    default_external = rb_encodings[ENCODING_UTF8];
    default_internal = rb_encodings[ENCODING_UTF8];
}

void
Init_Encoding(void)
{
    // rb_cEncoding is defined earlier in Init_PreVM().
    rb_set_class_path(rb_cEncoding, rb_cObject, "Encoding");
    rb_const_set(rb_cObject, rb_intern("Encoding"), rb_cEncoding);

    rb_undef_alloc_func(rb_cEncoding);

    rb_objc_define_method(rb_cEncoding, "to_s", mr_enc_name, 0);
    rb_objc_define_method(rb_cEncoding, "inspect", mr_enc_inspect, 0);
    rb_objc_define_method(rb_cEncoding, "name", mr_enc_name, 0);
    rb_objc_define_method(rb_cEncoding, "names", mr_enc_names, 0);
    rb_objc_define_method(rb_cEncoding, "dummy?", mr_enc_dummy_p, 0);
    rb_objc_define_method(rb_cEncoding, "ascii_compatible?",
	    mr_enc_ascii_compatible_p, 0);
    rb_objc_define_method(*(VALUE *)rb_cEncoding, "list", mr_enc_s_list, 0);
    rb_objc_define_method(*(VALUE *)rb_cEncoding, "name_list",
	    mr_enc_s_name_list, 0);
    rb_objc_define_method(*(VALUE *)rb_cEncoding, "aliases",
	    mr_enc_s_aliases, 0);
    rb_objc_define_method(*(VALUE *)rb_cEncoding, "find", mr_enc_s_find, 1);
    rb_objc_define_method(*(VALUE *)rb_cEncoding, "compatible?",
	    mr_enc_s_is_compatible, 2); // in string.c

    //rb_define_method(rb_cEncoding, "_dump", enc_dump, -1);
    //rb_define_singleton_method(rb_cEncoding, "_load", enc_load, 1);

    rb_objc_define_method(*(VALUE *)rb_cEncoding, "default_external",
	    mr_enc_s_default_external, 0);
    rb_objc_define_method(*(VALUE *)rb_cEncoding, "default_external=",
	    mr_enc_set_default_external, 1);
    rb_objc_define_method(*(VALUE *)rb_cEncoding, "default_internal",
	    mr_enc_s_default_internal, 0);
    rb_objc_define_method(*(VALUE *)rb_cEncoding, "default_internal=",
	    mr_enc_set_default_internal, 1);
    //rb_define_singleton_method(rb_cEncoding, "locale_charmap", rb_locale_charmap, 0);

    // Create constants.
    for (unsigned int i = 0; i < ENCODINGS_COUNT; i++) {
	rb_encoding_t *enc = rb_encodings[i];
	define_encoding_constant(enc->public_name, enc);
	for (unsigned int j = 0; j < enc->aliases_count; j++) {
	    define_encoding_constant(enc->aliases[j], enc);
	}
    }
}

// MRI C-API compatibility.

rb_encoding_t *
rb_enc_find(const char *name)
{
    for (unsigned int i = 0; i < ENCODINGS_COUNT; i++) {
	rb_encoding_t *enc = rb_encodings[i];
	if (strcasecmp(enc->public_name, name) == 0) {
	    return enc;
	}
	for (unsigned int j = 0; j < enc->aliases_count; j++) {
	    const char *alias = enc->aliases[j];
	    if (strcasecmp(alias, name) == 0) {
		return enc;
	    }
	}
    }
    return NULL;
}

VALUE
rb_enc_from_encoding(rb_encoding_t *enc)
{
    return (VALUE)enc;
}

rb_encoding_t *
rb_enc_get(VALUE obj)
{
    if (IS_RSTR(obj)) {
	return RSTR(obj)->encoding;
    }
    // TODO support symbols
    return NULL;
}

rb_encoding_t *
rb_to_encoding(VALUE obj)
{
    rb_encoding_t *enc;
    if (CLASS_OF(obj) == rb_cEncoding) {
	enc = RENC(obj);
    }
    else {
	StringValue(obj);
	enc = rb_enc_find(RSTRING_PTR(obj));
	if (enc == NULL) {
	    rb_raise(rb_eArgError, "unknown encoding name - %s",
		    RSTRING_PTR(obj));
	}
    }
    return enc;
}

const char *
rb_enc_name(rb_encoding_t *enc)
{
    return RENC(enc)->public_name;
}

VALUE
rb_enc_name2(rb_encoding_t *enc)
{
    return rb_usascii_str_new2(rb_enc_name(enc));
}

long
rb_enc_mbminlen(rb_encoding_t *enc)
{
    return enc->min_char_size;    
}

long
rb_enc_mbmaxlen(rb_encoding_t *enc)
{
    return enc->single_byte_encoding ? 1 : 10; // XXX 10?
}

rb_encoding_t *
rb_locale_encoding(void)
{
    // XXX
    return rb_encodings[ENCODING_UTF8];
}

void
rb_enc_set_default_external(VALUE encoding)
{
    assert(CLASS_OF(encoding) == rb_cEncoding);
    default_external = RENC(encoding); 
}

