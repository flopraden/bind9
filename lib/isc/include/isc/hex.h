#ifndef ISC_HEX_H
#define ISC_HEX_H 1

#include <isc/lang.h>
#include <isc/types.h>

ISC_LANG_BEGINDECLS

/***
 *** Functions
 ***/

isc_result_t
isc_hex_totext(isc_region_t *source, int wordlength,
	       const char *wordbreak, isc_buffer_t *target);
/*
 * Convert data into hex encoded text.
 *
 * Notes:
 *	The hex encoded text in 'target' will be divided into
 *	words of at most 'wordlength' characters, separated by
 * 	the 'wordbreak' string.  No parentheses will surround
 *	the text.
 *
 * Requires:
 *	'source' is a region containing binary data
 *	'target' is a text buffer containing available space
 *	'wordbreak' points to a null-terminated string of
 *		zero or more whitespace characters
 *
 * Ensures:
 *	target will contain the hex encoded version of the data
 *	in source.  The 'used' pointer in target will be advanced as
 *	necessary.
 */

isc_result_t
isc_hex_decodestring(isc_mem_t *mctx, char *cstr, isc_buffer_t *target);
/*
 * Decode a null-terminated hex string.
 *
 * Requires:
 * 	'mctx' is non-null.
 *	'cstr' is non-null.
 *	'target' is a valid buffer.
 *
 * Returns:
 *	ISC_R_SUCCESS	-- the entire decoded representation of 'cstring'
 *			   fit in 'target'.
 *	ISC_R_BADHEX -- 'cstr' is not a valid hex encoding.
 *
 * 	Other error returns are any possible error code from:
 *		isc_lex_create(),
 *		isc_lex_openbuffer(),
 *		isc_hex_tobuffer().
 */

isc_result_t
isc_hex_tobuffer(isc_lex_t *lexer, isc_buffer_t *target, int length);
/*
 * Convert hex encoded text from a lexer context into data.
 *
 * Requires:
 *	'lex' is a valid lexer context
 *	'target' is a buffer containing binary data
 *	'length' is an integer
 *
 * Ensures:
 *	target will contain the data represented by the hex encoded
 *	string parsed by the lexer.  No more than length bytes will be read,
 *	if length is positive.  The 'used' pointer in target will be
 *	advanced as necessary.
 */


ISC_LANG_ENDDECLS

#endif /* ISC_HEX_H */
