#ifndef DNS_SSU_H
#define DNS_SSU_H 1

#include <isc/types.h>
#include <isc/lang.h>
#include <isc/list.h>
#include <isc/rwlock.h>

#include <dns/types.h>

ISC_LANG_BEGINDECLS

#define DNS_SSUMATCHTYPE_NAME 0
#define DNS_SSUMATCHTYPE_SUBDOMAIN 1
#define DNS_SSUMATCHTYPE_WILDCARD 2
#define DNS_SSUMATCHTYPE_SELF 3

isc_result_t
dns_ssutable_create(isc_mem_t *mctx, dns_ssutable_t **table);
/*
 *	Creates a table that will be used to store simple-secure-update rules.
 *	Note: all locking must be provided by the client.
 *
 *	Requires:
 *		'mctx' is a valid memory context
 *		'table' is not NULL, and '*table' is NULL
 *
 *	Returns:
 *		ISC_R_SUCCESS
 *		ISC_R_NOMEMORY
 */

void
dns_ssutable_destroy(dns_ssutable_t **table);
/*
 *	Destroys a simple-secure-update rule table.  *table is set to NULL.
 *
 *	Requires:
 *		'table' is not NULL
 *		'*table' is a valid SSU table
 */

isc_result_t
dns_ssutable_addrule(dns_ssutable_t *table, isc_boolean_t grant,
		     dns_name_t *identity, unsigned int matchtype,
		     dns_name_t *name, unsigned int ntypes,
		     dns_rdatatype_t *types);
/*
 *	Adds a new rule to a simple-secure-update rule table.  The rule
 *	either grants or denies update privileges of an identity (or set of
 *	identities) to modify a name (or set of names) or certain types present
 *	at that name.
 *
 *	Notes:
 *		If 'matchtype' is SELF, this rule only matches if the name
 *		to be updated matches the signing identity.
 *
 *		If 'ntypes' is 0, this rule applies to all types except
 *		NS, SOA, SIG, and NXT.
 *
 *		If 'types' includes ANY, this rule applies to all types
 *		except NXT.
 *
 *	Requires:
 *		'table' is a valid SSU table
 *		'identity' is a valid absolute name
 *		'matchtype' must be one of the defined constants.
 *		'name' is a valid absolute name
 *		If 'ntypes' > 0, 'types' must not be NULL
 *
 *	Returns:
 *		ISC_R_SUCCESS
 *		ISC_R_NOMEMORY
 */

isc_boolean_t
dns_ssutable_checkrules(dns_ssutable_t *table, dns_name_t *signer,
			dns_name_t *name, dns_rdatatype_t type);
/*
 *	Checks that the attempted update of (name, type) is allowed according
 *	to the rules specified in the simple-secure-update rule table.  If
 *	no rules are matched, access is denied.  If signer is NULL, access
 *	is denied.
 *
 *	Requires:
 *		'table' is a valid SSU table
 *		'signer' is NULL or a valid absolute name
 *		'name' is a valid absolute name
 */


isc_boolean_t	dns_ssurule_isgrant(const dns_ssurule_t *rule);
dns_name_t *	dns_ssurule_identity(const dns_ssurule_t *rule);
unsigned int	dns_ssurule_matchtype(const dns_ssurule_t *rule);
dns_name_t *	dns_ssurule_name(const dns_ssurule_t *rule);
unsigned int	dns_ssurule_types(const dns_ssurule_t *rule,
				  dns_rdatatype_t **types);
/*
 * Accessor functions to extract rule components
 */

isc_result_t	dns_ssutable_firstrule(const dns_ssutable_t *table,
				       dns_ssurule_t **rule);
/*
 * Initiates a rule iterator.  There is no need to maintain any state.
 *
 * Returns:
 *	ISC_R_SUCCESS
 *	ISC_R_NOMORE
 */

isc_result_t	dns_ssutable_nextrule(dns_ssurule_t *rule,
				      dns_ssurule_t **nextrule);
/*
 * Returns the next rule in the table.
 *
 * Returns:
 *	ISC_R_SUCCESS
 *	ISC_R_NOMORE
 */

ISC_LANG_ENDDECLS

#endif /* DNS_SSU_H */
