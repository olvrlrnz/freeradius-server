/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_eap_aka.c
 * @brief Implements the AKA part of EAP-AKA
 *
 * @author Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 *
 * @copyright 2016 The FreeRADIUS server project
 * @copyright 2016 Network RADIUS SARL <sales@networkradius.com>
 */
RCSID("$Id$")

#include "../../eap.h"
#include "eap_types.h"
#include "eap_aka.h"
#include "sim_proto.h"

#include <freeradius-devel/rad_assert.h>

#ifndef EAP_TLS_MPPE_KEY_LEN
#  define EAP_TLS_MPPE_KEY_LEN     32
#endif

FR_NAME_NUMBER const aka_state_table[] = {
	{ "IDENTITY",				EAP_AKA_SERVER_IDENTITY				},
	{ "CHALLENGE",				EAP_AKA_SERVER_CHALLENGE			},
	{ "SUCCESS-NOTIFICATION",		EAP_AKA_SERVER_SUCCESS_NOTIFICATION 		},
	{ "SUCCESS",				EAP_AKA_SERVER_SUCCESS				},
	{ "FAILURE-NOTIFICATION",		EAP_AKA_SERVER_FAILURE_NOTIFICATION		},
	{ "FAILURE",				EAP_AKA_SERVER_FAILURE				},
	{ NULL }
};

static rlm_rcode_t mod_process(UNUSED void *arg, eap_session_t *eap_session);

static CONF_PARSER submodule_config[] = {
	{ FR_CONF_OFFSET("network_id", FR_TYPE_STRING | FR_TYPE_REQUIRED, rlm_eap_aka_t, network_id ) },
	{ FR_CONF_OFFSET("request_identity", FR_TYPE_BOOL, rlm_eap_aka_t, request_identity ), .dflt = "no" },
	{ FR_CONF_OFFSET("protected_success", FR_TYPE_BOOL, rlm_eap_aka_t, protected_success ), .dflt = "no" },
	{ FR_CONF_OFFSET("virtual_server", FR_TYPE_STRING, rlm_eap_aka_t, virtual_server) },
	CONF_PARSER_TERMINATOR
};

static int eap_aka_compose(eap_session_t *eap_session)
{
	eap_aka_session_t	*eap_aka_session = talloc_get_type_abort(eap_session->opaque, eap_aka_session_t);
	vp_cursor_t		cursor;
	vp_cursor_t		to_encode;
	VALUE_PAIR		*head = NULL, *vp;
	REQUEST			*request = eap_session->request;
	ssize_t			ret;
	fr_sim_encode_ctx_t	encoder_ctx = {
					.root = dict_aka_root,
					.keys = &eap_aka_session->keys,

					.iv = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
						0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
					.iv_included = false,

					.hmac_md = eap_aka_session->mac_md,
					.eap_packet = eap_session->this_round->request,
					.hmac_extra = NULL,
					.hmac_extra_len = 0
				};
	fr_dict_attr_t const	*encr = fr_dict_attr_child_by_num(dict_sim_root, FR_EAP_AKA_ENCR_DATA);

	fr_pair_cursor_init(&cursor, &eap_session->request->reply->vps);
	fr_pair_cursor_init(&to_encode, &head);

	while ((fr_pair_cursor_next_by_ancestor(&cursor, dict_aka_root, TAG_ANY))) {
		vp = fr_pair_cursor_remove(&cursor);

		/*
		 *	Silently discard encrypted attributes until
		 *	the peer should have k_encr.  These can be
		 *	added by policy, and seem to cause
		 *	wpa_supplicant to fail if sent before the challenge.
		 */
		if (!eap_aka_session->allow_encrypted && fr_dict_parent_common(encr, vp->da, true)) {
			RWDEBUG("Silently discarding &reply:%s: Encrypted attributes not allowed in this round",
				vp->da->name);
			talloc_free(vp);
			continue;
		}

		fr_pair_cursor_append(&to_encode, vp);
	}

	RDEBUG2("Encoding EAP-AKA attributes");
	rdebug_pair_list(L_DBG_LVL_2, request, head, NULL);

	eap_session->this_round->request->type.num = eap_aka_session->type;
	eap_session->this_round->request->id = eap_aka_session->aka_id++ & 0xff;
	eap_session->this_round->set_request_id = true;

	ret = fr_sim_encode(eap_session->request, head, &encoder_ctx);
	fr_pair_cursor_first(&to_encode);
	fr_pair_cursor_free(&to_encode);

	if (ret < 0) {
		RPEDEBUG("Failed encoding EAP-AKA data");
		return -1;
	}
	return 0;
}

/** Send an EAP-AKA identity request to the supplicant
 *
 * There are three types of user identities that can be implemented
 * - Permanent identities such as 0123456789098765@myoperator.com
 *   Permanent identities can be identified by the leading zero followed by
 *   by 15 digits (the IMSI number).
 * - Ephemeral identities (pseudonyms).  These are identities assigned for
 *   identity privacy so the user can't be tracked.  These can identities
 *   can either be generated as per the 3GPP 'Security aspects of non-3GPP accesses'
 *   document section 14, where a set of up to 16 encryption keys are used
 *   to reversibly encrypt the IMSI. Alternatively the pseudonym can be completely
 *   randomised and stored in a datastore.
 * - A fast resumption ID which resolves to data used for fast resumption.
 *
 * In order to perform full authentication the original IMSI is required for
 * forwarding to the HLR. In the case where we can't match/decrypt the pseudonym,
 * or can't perform fast resumption, we need to request the full identity from
 * the supplicant.
 *
 * @param[in] eap_session	to continue.
 * @return
 *	- 0 on success.
 *	- <0 on failure.
 */
static int eap_aka_send_identity_request(eap_session_t *eap_session)
{
	REQUEST			*request = eap_session->request;
	eap_aka_session_t	*eap_aka_session = talloc_get_type_abort(eap_session->opaque, eap_aka_session_t);
	VALUE_PAIR		*vp;
	RADIUS_PACKET		*packet;
	fr_cursor_t		cursor;

	RDEBUG2("Sending AKA-Identity (%s)", fr_int2str(sim_id_request_table, eap_aka_session->id_req, "<INVALID>"));
	eap_session->this_round->request->code = FR_EAP_CODE_REQUEST;
	eap_aka_session->allow_encrypted = false;	/* In case this is after failed fast-resumption */

	packet = request->reply;
	fr_cursor_init(&cursor, &packet->vps);

	/*
	 *	Set the subtype to identity request
	 */
	vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_SUBTYPE);
	vp->vp_uint16 = FR_EAP_AKA_SUBTYPE_VALUE_AKA_IDENTITY;
	fr_cursor_append(&cursor, vp);

	/*
	 *	Select the right type of identity request attribute
	 */
	switch (eap_aka_session->id_req) {
	case SIM_ANY_ID_REQ:
		vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_ANY_ID_REQ);
		break;

	case SIM_PERMANENT_ID_REQ:
		vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_PERMANENT_ID_REQ);
		break;

	case SIM_FULLAUTH_ID_REQ:
		vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_FULLAUTH_ID_REQ);
		break;

	default:
		rad_assert(0);
	}
	vp->vp_bool = true;
	fr_cursor_append(&cursor, vp);

	/*
	 *	Encode the packet
	 */
	if (eap_aka_compose(eap_session) < 0) {
	failure:
		fr_pair_list_free(&packet->vps);
		return -1;
	}

	/*
	 *	Digest the packet contents, updating our checkcode.
	 */
	if (!eap_aka_session->checkcode_state &&
	    fr_sim_crypto_init_checkcode(eap_aka_session, &eap_aka_session->checkcode_state,
	    				 eap_aka_session->checkcode_md) < 0) {
		RPEDEBUG("Failed initialising checkcode");
		goto failure;
	}
	if (fr_sim_crypto_update_checkcode(eap_aka_session->checkcode_state, eap_session->this_round->request) < 0) {
		RPEDEBUG("Failed updating checkcode");
		goto failure;
	}

	return 0;
}

/** Send the challenge itself
 *
 * Challenges will come from one of three places eventually:
 *
 * 1  from attributes like FR_EAP_AKA_RANDx
 *	    (these might be retrieved from a database)
 *
 * 2  from internally implemented SIM authenticators
 *	    (a simple one based upon XOR will be provided)
 *
 * 3  from some kind of SS7 interface.
 *
 * For now, they only come from attributes.
 * It might be that the best way to do 2/3 will be with a different
 * module to generate/calculate things.
 */
static int eap_aka_send_challenge(eap_session_t *eap_session)
{
	static uint8_t		hmac_zero[16] = { 0x00 };

	REQUEST			*request = eap_session->request;
	eap_aka_session_t	*eap_aka_session = talloc_get_type_abort(eap_session->opaque, eap_aka_session_t);
	VALUE_PAIR		**to_peer, *vp;
	RADIUS_PACKET		*packet;
	fr_sim_vector_src_t	src = SIM_VECTOR_SRC_AUTO;

	rad_assert(request);
	rad_assert(request->reply);

	/*
	 *	to_peer is the data to the client
	 */
	packet = eap_session->request->reply;
	to_peer = &packet->vps;

	RDEBUG2("Acquiring UMTS vector(s)");

	/*
	 *	Toggle the AMF high bit to indicate we're doing AKA'
	 */
	if (eap_aka_session->type == FR_EAP_AKA_PRIME) {
		uint8_t	amf_buff[2] = { 0x80, 0x00 };	/* Set the AMF separation bit high */

		vp = fr_pair_afrom_child_num(packet, fr_dict_root(fr_dict_internal), FR_SIM_AMF);
		fr_pair_value_memcpy(vp, amf_buff, sizeof(amf_buff));
		fr_pair_replace(&request->control, vp);
	}

	/*
	 *	Get vectors from attribute or generate
	 *	them using COMP128-* or Milenage.
	 */
	if (fr_sim_vector_umts_from_attrs(eap_session, request->control, &eap_aka_session->keys, &src) < 0) {
	    	REDEBUG("Failed retrieving UMTS vectors");
		return RLM_MODULE_FAIL;
	}

	/*
	 *	Don't leave the AMF hanging around
	 */
	if (eap_aka_session->type == FR_EAP_AKA_PRIME) fr_pair_delete_by_num(&request->control, 0, FR_SIM_AMF, TAG_ANY);

	/*
	 *	All set, calculate keys!
	 */
	switch (eap_aka_session->kdf) {
	case FR_EAP_AKA_KDF_VALUE_EAP_AKA_PRIME_WITH_CK_PRIME_IK_PRIME:
		fr_sim_crypto_kdf_1_umts(&eap_aka_session->keys);
		break;

	default:
		fr_sim_crypto_kdf_0_umts(&eap_aka_session->keys);
		break;
	}
	if (RDEBUG_ENABLED3) fr_sim_crypto_keys_log(request, &eap_aka_session->keys);

	RDEBUG2("Sending AKA-Challenge");
	eap_session->this_round->request->code = FR_EAP_CODE_REQUEST;

	/*
	 *	Set the subtype to challenge
	 */
	MEM(vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_SUBTYPE));
	vp->vp_uint16 = FR_EAP_AKA_SUBTYPE_VALUE_AKA_CHALLENGE;
	fr_pair_replace(to_peer, vp);

	/*
	 *	Indicate we'd like to use protected success messages
	 */
	if (eap_aka_session->send_result_ind) {
		MEM(vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_RESULT_IND));
		vp->vp_bool = true;
		fr_pair_replace(to_peer, vp);
	}

	/*
	 *	We support EAP-AKA' and the peer should use that
	 *	if it's able to...
	 */
	if (eap_aka_session->send_at_bidding) {
		MEM(vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_BIDDING));
		vp->vp_uint16 = FR_EAP_AKA_BIDDING_VALUE_PREFER_AKA_PRIME;
		fr_pair_replace(to_peer, vp);
	}

	/*
	 *	Send the network name and KDF to the peer
	 */
	if (eap_aka_session->type == FR_EAP_AKA_PRIME) {
		if (!eap_aka_session->keys.network_len) {
			REDEBUG2("No network name available, can't set EAP-AKA-KDF-Input");
		failure:
			fr_pair_list_free(&packet->vps);
			return -1;
		}
		MEM(vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_KDF_INPUT));
		fr_pair_value_bstrncpy(vp, eap_aka_session->keys.network, eap_aka_session->keys.network_len);
		fr_pair_replace(to_peer, vp);

		MEM(vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_KDF));
		vp->vp_uint16 = eap_aka_session->kdf;
		fr_pair_replace(to_peer, vp);
	}

	/*
	 *	Okay, we got the challenge! Put it into an attribute.
	 */
	MEM(vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_RAND));
	fr_pair_value_memcpy(vp, eap_aka_session->keys.umts.vector.rand, SIM_VECTOR_UMTS_RAND_SIZE);
	fr_pair_replace(to_peer, vp);

	/*
	 *	Send the AUTN value to the client, so it can authenticate
	 *	whoever has knowledge of the Ki.
	 */
	MEM(vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_AUTN));
	fr_pair_value_memcpy(vp, eap_aka_session->keys.umts.vector.autn, SIM_VECTOR_UMTS_AUTN_SIZE);
	fr_pair_replace(to_peer, vp);

	/*
	 *	need to include an AT_MAC attribute so that it will get
	 *	calculated.
	 */
	MEM(vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_MAC));
	fr_pair_value_memcpy(vp, hmac_zero, sizeof(hmac_zero));
	fr_pair_replace(to_peer, vp);

	/*
	 *	If we have checkcode data, send that to the peer
	 *	for validation.
	 */
	if (eap_aka_session->checkcode_state) {
		ssize_t	slen;

		slen = fr_sim_crypto_finalise_checkcode(eap_aka_session->checkcode, &eap_aka_session->checkcode_state);
		if (slen < 0) {
			RPEDEBUG("Failed calculating checkcode");
			goto failure;
		}
		eap_aka_session->checkcode_len = slen;

		MEM(vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_CHECKCODE));
		fr_pair_value_memcpy(vp, eap_aka_session->checkcode, slen);
	/*
	 *	If we don't have checkcode data, then we exchanged
	 *	no identity packets, so checkcode is zero.
	 */
	} else {
		MEM(vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_CHECKCODE));
		eap_aka_session->checkcode_len = 0;
	}
	fr_pair_replace(to_peer, vp);

	/*
	 *	We've sent the challenge so the peer should now be able
	 *	to accept encrypted attributes.
	 */
	eap_aka_session->allow_encrypted = true;

	/*
	 *	Encode the packet
	 */
	if (eap_aka_compose(eap_session) < 0) goto failure;

	return 0;
}

/** Send a success notification
 *
 */
static int eap_aka_send_eap_success_notification(eap_session_t *eap_session)
{
	REQUEST			*request = eap_session->request;
	RADIUS_PACKET		*packet = eap_session->request->reply;
	eap_aka_session_t	*eap_aka_session = talloc_get_type_abort(eap_session->opaque, eap_aka_session_t);
	fr_cursor_t		cursor;
	VALUE_PAIR		*vp;

	RDEBUG2("Sending AKA-Notification (Success)");
	eap_session->this_round->request->code = FR_EAP_CODE_REQUEST;

	if (!fr_cond_assert(eap_aka_session->challenge_success)) return -1;

	fr_cursor_init(&cursor, &packet->vps);

	/*
	 *	Set the subtype to notification
	 */
	vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_SUBTYPE);
	vp->vp_uint16 = FR_EAP_AKA_SUBTYPE_VALUE_AKA_NOTIFICATION;
	fr_cursor_append(&cursor, vp);

	vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_NOTIFICATION);
	vp->vp_uint16 = FR_EAP_AKA_NOTIFICATION_VALUE_SUCCESS;
	fr_cursor_append(&cursor, vp);

	/*
	 *	Need to include an AT_MAC attribute so that it will get
	 *	calculated.
	 */
	vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_MAC);
	fr_pair_replace(&packet->vps, vp);

	/*
	 *	Encode the packet
	 */
	if (eap_aka_compose(eap_session) < 0) {
		fr_pair_list_free(&packet->vps);
		return -1;
	}

	return 0;
}

/** Send a success message with MPPE-keys
 *
 * The only work to be done is the add the appropriate SEND/RECV
 * attributes derived from the MSK.
 */
static int eap_aka_send_eap_success(eap_session_t *eap_session)
{
	REQUEST			*request = eap_session->request;
	uint8_t			*p;
	eap_aka_session_t	*eap_aka_session;

	RDEBUG2("Sending EAP-Success");

	eap_session->this_round->request->code = FR_EAP_CODE_SUCCESS;
	eap_session->finished = true;

	eap_aka_session = talloc_get_type_abort(eap_session->opaque, eap_aka_session_t);

	p = eap_aka_session->keys.msk;
	eap_add_reply(eap_session->request, "MS-MPPE-Recv-Key", p, EAP_TLS_MPPE_KEY_LEN);
	p += EAP_TLS_MPPE_KEY_LEN;
	eap_add_reply(eap_session->request, "MS-MPPE-Send-Key", p, EAP_TLS_MPPE_KEY_LEN);

	return 0;
}

/** Send a failure message
 *
 */
static int eap_aka_send_eap_failure_notification(eap_session_t *eap_session)
{
	REQUEST			*request = eap_session->request;
	RADIUS_PACKET		*packet = eap_session->request->reply;
	fr_cursor_t		cursor;
	VALUE_PAIR		*vp;
	eap_aka_session_t	*eap_aka_session = talloc_get_type_abort(eap_session->opaque, eap_aka_session_t);

	fr_cursor_init(&cursor, &packet->vps);

	vp = fr_pair_find_by_child_num(packet->vps, dict_sim_root, FR_EAP_AKA_NOTIFICATION, TAG_ANY);
	if (!vp) {
		vp = fr_pair_afrom_child_num(packet, dict_sim_root, FR_EAP_AKA_NOTIFICATION);
		vp->vp_uint16 = FR_EAP_AKA_NOTIFICATION_VALUE_GENERAL_FAILURE;
		fr_cursor_append(&cursor, vp);
	}

	/*
	 *	Change the failure notification depending where
	 *	we are in the state machine.
	 */
	if (eap_aka_session->challenge_success) {
		vp->vp_uint16 &= ~0x4000;	/* Unset phase bit */
	} else {
		vp->vp_uint16 |= 0x4000;	/* Set phase bit */
	}
	vp->vp_uint16 &= ~0x8000;		/* In both cases success bit should be low */

	RDEBUG2("Sending AKA-Notification (%pV)", &vp->data);
	eap_session->this_round->request->code = FR_EAP_CODE_REQUEST;

	/*
	 *	Set the subtype to notification
	 */
	vp = fr_pair_afrom_child_num(packet, dict_aka_root, FR_EAP_AKA_SUBTYPE);
	vp->vp_uint16 = FR_EAP_AKA_SUBTYPE_VALUE_AKA_NOTIFICATION;
	fr_cursor_append(&cursor, vp);

	/*
	 *	If we're after the challenge phase
	 *	then we need to include a MAC to
	 *	protect notifications.
	 */
	if (eap_aka_session->challenge_success) {
		vp = fr_pair_afrom_child_num(packet, dict_sim_root, FR_EAP_AKA_MAC);
		fr_pair_replace(&packet->vps, vp);
	}

	/*
	 *	Encode the packet
	 */
	if (eap_aka_compose(eap_session) < 0) {
		fr_pair_list_free(&packet->vps);
		return -1;
	}

	return 0;
}

static int eap_aka_send_eap_failure(eap_session_t *eap_session)
{
	REQUEST		*request = eap_session->request;

	RDEBUG2("Sending EAP-Failure");

	eap_session->this_round->request->code = FR_EAP_CODE_FAILURE;
	eap_session->finished = true;

	return 0;
}

/** Run the server state machine
 *
 */
static void eap_aka_state_enter(eap_session_t *eap_session, eap_aka_server_state_t new_state)
{
	REQUEST			*request = eap_session->request;
	eap_aka_session_t	*eap_aka_session = talloc_get_type_abort(eap_session->opaque, eap_aka_session_t);

	if (new_state != eap_aka_session->state) {
		RDEBUG2("Changed state %s -> %s",
			fr_int2str(aka_state_table, eap_aka_session->state, "<unknown>"),
			fr_int2str(aka_state_table, new_state, "<unknown>"));
		eap_aka_session->state = new_state;
	} else {
		RDEBUG2("Reentering state %s",
			fr_int2str(aka_state_table, eap_aka_session->state, "<unknown>"));
	}

	switch (new_state) {
	/*
	 *	Send an EAP-AKA Identity request
	 */
	case EAP_AKA_SERVER_IDENTITY:
		if (eap_aka_send_identity_request(eap_session) < 0) {
		notify_failure:
			eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE_NOTIFICATION);
			return;
		}
		break;

	/*
	 *	Send the EAP-AKA Challenge message.
	 */
	case EAP_AKA_SERVER_CHALLENGE:
		if (eap_aka_send_challenge(eap_session) < 0) goto notify_failure;
		break;

	/*
	 *	Sent a protected success notification
	 */
	case EAP_AKA_SERVER_SUCCESS_NOTIFICATION:
		if (eap_aka_send_eap_success_notification(eap_session) < 0) goto notify_failure;
		break;

	/*
	 *	Send the EAP Success message (we're done)
	 */
	case EAP_AKA_SERVER_SUCCESS:
		if (eap_aka_send_eap_success(eap_session) < 0) goto notify_failure;
		return;

	/*
	 *	Send a general failure notification
	 */
	case EAP_AKA_SERVER_FAILURE_NOTIFICATION:
		if (eap_aka_send_eap_failure_notification(eap_session) < 0) {	/* Fallback to EAP-Failure */
			eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE);
		}
		return;

	/*
	 *	Send an EAP-Failure (we're done)
	 */
	case EAP_AKA_SERVER_FAILURE:
		eap_aka_send_eap_failure(eap_session);
		return;

	default:
		rad_assert(0);	/* Invalid transition */
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE_NOTIFICATION);
		return;
	}
}

static int process_eap_aka_identity(eap_session_t *eap_session, VALUE_PAIR *vps)
{
	REQUEST			*request = eap_session->request;
	eap_aka_session_t	*eap_aka_session = talloc_get_type_abort(eap_session->opaque, eap_aka_session_t);
	VALUE_PAIR		*id;
	fr_sim_id_type_t	type = SIM_ID_TYPE_UNKNOWN;
	fr_sim_method_hint_t	method = SIM_METHOD_HINT_UNKNOWN;

	/*
	 *	Digest the identity response
	 */
	if (fr_sim_crypto_update_checkcode(eap_aka_session->checkcode_state, eap_session->this_round->response) < 0) {
		RPEDEBUG("Failed updating checkcode");
		return -1;
	}

	/*
	 *	See if we got an AT_IDENTITY
	 */
	id = fr_pair_find_by_child_num(vps, dict_aka_root, FR_EAP_AKA_IDENTITY, TAG_ANY);
	if (id) {
	 	if (fr_sim_id_type(&type, &method,
				   eap_session->identity, talloc_array_length(eap_session->identity) - 1) < 0) {
			RWDEBUG2("Failed parsing identity: %s", fr_strerror());
		}
		/*
		 *	Update cryptographic identity
		 */
		talloc_const_free(eap_aka_session->keys.identity);
		eap_aka_session->keys.identity_len = id->vp_length;
		MEM(eap_aka_session->keys.identity = talloc_memdup(eap_aka_session, id->vp_strvalue, id->vp_length));
	}

	/*
	 *	@TODO Run a virtual server to see if we can use the
	 *	identity we just acquired, or whether we need to
	 *	negotiate the next permissive ID.
	 */

	/*
	 *	Negotiate the next permissive form
	 *	if identity, or fail.
	 */
	switch (eap_aka_session->id_req) {
	case SIM_ANY_ID_REQ:
		eap_aka_session->id_req = SIM_FULLAUTH_ID_REQ;
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_IDENTITY);
		break;

	case SIM_FULLAUTH_ID_REQ:
		eap_aka_session->id_req = SIM_PERMANENT_ID_REQ;
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_IDENTITY);
		break;

	case SIM_PERMANENT_ID_REQ:
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_CHALLENGE);
//		REDEBUG2("Failed to negotiate a usable identity");
//		eap_aka_state_enter(eap_session, eap_aka_session, EAP_AKA_SERVER_FAILURE_NOTIFICATION);
		break;

	case SIM_NO_ID_REQ:
		rad_assert(0);
		return -1;
	}

	return 0;
}

/**  Process an EAP-AKA/Response/Challenge
 *
 * Verify that MAC, and RES match what we expect.
 */
static int process_eap_aka_challenge(eap_session_t *eap_session, VALUE_PAIR *vps)
{
	REQUEST			*request = eap_session->request;
	eap_aka_session_t	*eap_aka_session = talloc_get_type_abort(eap_session->opaque, eap_aka_session_t);

	uint8_t			calc_mac[SIM_MAC_DIGEST_SIZE];
	ssize_t			slen;
	VALUE_PAIR		*vp = NULL, *mac, *checkcode;

	mac = fr_pair_find_by_child_num(vps, dict_aka_root, FR_EAP_AKA_MAC, TAG_ANY);
	if (!mac) {
		REDEBUG("Missing AT_MAC attribute");
		return -1;
	}
	if (mac->vp_length != SIM_MAC_DIGEST_SIZE) {
		REDEBUG("EAP-AKA-MAC has incorrect length, expected %u bytes got %zu bytes",
			SIM_MAC_DIGEST_SIZE, mac->vp_length);
		return -1;
	}

	slen = fr_sim_crypto_sign_packet(calc_mac, eap_session->this_round->response, true,
					 eap_aka_session->mac_md,
					 eap_aka_session->keys.k_aut, eap_aka_session->keys.k_aut_len,
					 NULL, 0);
	if (slen < 0) {
		RPEDEBUG("Failed calculating MAC");
		return -1;
	} else if (slen == 0) {
		REDEBUG("Missing EAP-AKA-MAC attribute in packet buffer");
		return -1;
	}

	if (memcmp(mac->vp_octets, calc_mac, sizeof(calc_mac)) == 0) {
		RDEBUG2("EAP-AKA-MAC matches calculated MAC");
	} else {
		REDEBUG("EAP-AKA-MAC does not match calculated MAC");
		RHEXDUMP_INLINE(L_DBG_LVL_2, mac->vp_octets, SIM_MAC_DIGEST_SIZE, "Received");
		RHEXDUMP_INLINE(L_DBG_LVL_2, calc_mac, SIM_MAC_DIGEST_SIZE, "Expected");
		return -1;
	}

	/*
	 *	If the peer doesn't include a checkcode then that
	 *	means they don't support it, and we can't validate
	 *	their view of the identity packets.
	 */
	checkcode = fr_pair_find_by_child_num(vps, dict_aka_root, FR_EAP_AKA_CHECKCODE, TAG_ANY);
	if (checkcode) {
		if (checkcode->vp_length != eap_aka_session->checkcode_len) {
			REDEBUG("Checkcode length (%zu) does not match calculated checkcode length (%zu)",
				checkcode->vp_length, eap_aka_session->checkcode_len);
			return -1;
		}

		if (memcmp(checkcode->vp_octets, eap_aka_session->checkcode, eap_aka_session->checkcode_len) == 0) {
			RDEBUG("EAP-AKA-Checkcode matches calculated checkcode");
		} else {
			REDEBUG("EAP-AKA-Checkcode does not match calculated checkcode");
			RHEXDUMP_INLINE(L_DBG_LVL_2, checkcode->vp_octets, checkcode->vp_length, "Received");
			RHEXDUMP_INLINE(L_DBG_LVL_2, eap_aka_session->checkcode,
					eap_aka_session->checkcode_len, "Expected");
			return -1;
		}
	/*
	 *	Only print something if we calculated a checkcode
	 */
	} else if (eap_aka_session->checkcode_len > 0){
		RDEBUG2("Peer didn't include EAP-AKA-Checkcode, skipping checkcode validation");
	}

	vp = fr_pair_find_by_child_num(vps, dict_aka_root, FR_EAP_AKA_RES, TAG_ANY);
	if (!vp) {
		REDEBUG("Missing EAP-AKA-RES from challenge response");
		return -1;
	}

	if (vp->vp_length != eap_aka_session->keys.umts.vector.xres_len) {
		REDEBUG("EAP-AKA-RES length (%zu) does not match XRES length (%zu)",
			vp->vp_length, eap_aka_session->keys.umts.vector.xres_len);
		return -1;
	}

  	if (memcmp(vp->vp_octets, eap_aka_session->keys.umts.vector.xres, vp->vp_length)) {
    		REDEBUG("EAP-AKA-RES from client does match XRES");
		RHEXDUMP_INLINE(L_DBG_LVL_2, vp->vp_octets, vp->vp_length, "RES  :");
		RHEXDUMP_INLINE(L_DBG_LVL_2, eap_aka_session->keys.umts.vector.xres,
				eap_aka_session->keys.umts.vector.xres_len, "XRES :");
		return -1;
	}

	RDEBUG2("EAP-AKA-RES matches XRES");

	eap_aka_session->challenge_success = true;

	/*
	 *	If the peer wants a Success notification, then
	 *	send a success notification, otherwise send a
	 *	normal EAP-Success.
	 */
	if (fr_pair_find_by_child_num(vps, dict_aka_root, FR_EAP_AKA_RESULT_IND, TAG_ANY)) {
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_SUCCESS_NOTIFICATION);
	} else {
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_SUCCESS);
	}

	return 0;
}

/** Process the Peer's response and advantage the state machine
 *
 */
static rlm_rcode_t mod_process(UNUSED void *arg, eap_session_t *eap_session)
{
	REQUEST			*request = eap_session->request;
	eap_aka_session_t	*eap_aka_session = talloc_get_type_abort(eap_session->opaque, eap_aka_session_t);

	fr_sim_decode_ctx_t	ctx = {
					.keys = &eap_aka_session->keys,
					.root = dict_aka_root
				};
	VALUE_PAIR		*vp, *vps, *subtype_vp;
	vp_cursor_t		cursor;

	eap_aka_subtype_t	subtype;

	int			ret;

	/*
	 *	RFC 4187 says we ignore the contents of the
	 *	next packet after we send our success notification
	 *	and always send a success.
	 */
	if (eap_aka_session->state == EAP_AKA_SERVER_SUCCESS_NOTIFICATION) {
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_SUCCESS);
		return RLM_MODULE_HANDLED;
	}

	/* vps is the data from the client */
	vps = request->packet->vps;

	fr_pair_cursor_init(&cursor, &request->packet->vps);
	fr_pair_cursor_last(&cursor);

	ret = fr_sim_decode(eap_session->request,
			    &cursor,
			    eap_session->this_round->response->type.data,
			    eap_session->this_round->response->type.length,
			    &ctx);
	/*
	 *	RFC 4187 says we *MUST* notify, not just
	 *	send an EAP-Failure in this case where
	 *	we cannot decode an EAP-AKA packet.
	 */
	if (ret < 0) {
		RPEDEBUG2("Failed decoding EAP-AKA attributes");
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE_NOTIFICATION);
		return RLM_MODULE_HANDLED;	/* We need to process more packets */
	}

	vp = fr_pair_cursor_current(&cursor);
	if (vp && RDEBUG_ENABLED2) {
		RDEBUG2("EAP-AKA decoded attributes");
		rdebug_pair_list(L_DBG_LVL_2, request, vp, NULL);
	}

	subtype_vp = fr_pair_find_by_child_num(vps, dict_aka_root, FR_EAP_AKA_SUBTYPE, TAG_ANY);
	if (!subtype_vp) {
		REDEBUG("Missing EAP-AKA-Subtype");
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE_NOTIFICATION);
		return RLM_MODULE_HANDLED;				/* We need to process more packets */
	}
	subtype = subtype_vp->vp_uint16;

	switch (eap_aka_session->state) {
	/*
	 *	Here we expected the peer to send
	 *	us identities for validation.
	 */
	case EAP_AKA_SERVER_IDENTITY:
		switch (subtype) {
		case EAP_AKA_IDENTITY:
			if (process_eap_aka_identity(eap_session, vps) == 0) return RLM_MODULE_HANDLED;
			eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE_NOTIFICATION);
			return RLM_MODULE_HANDLED;	/* We need to process more packets */

		/*
		 *	Case 1 where we're allowed to send an EAP-Failure
		 *
		 *	This can happen in the case of a conservative
		 *	peer, where it refuses to provide the permanent
		 *	identity.
		 */
		case EAP_AKA_CLIENT_ERROR:
		{
			char buff[20];

			vp = fr_pair_find_by_child_num(vps, dict_aka_root, FR_EAP_AKA_CLIENT_ERROR_CODE, TAG_ANY);
			if (!vp) {
				REDEBUG("EAP-AKA Peer rejected AKA-Identity (%s) with client-error message but "
					"has not supplied a client error code",
					fr_int2str(sim_id_request_table, eap_aka_session->id_req, "<INVALID>"));
			} else {
				REDEBUG("Client rejected AKA-Identity (%s) with error: %s (%i)",
					fr_int2str(sim_id_request_table, eap_aka_session->id_req, "<INVALID>"),
					fr_pair_value_enum(vp, buff), vp->vp_uint16);
			}
			eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE);
			return RLM_MODULE_REJECT;
		}

		case EAP_AKA_NOTIFICATION:
		notification:
		{
			char buff[20];

			vp = fr_pair_afrom_child_num(vps, dict_aka_root, FR_EAP_AKA_NOTIFICATION);
			if (!vp) {
				REDEBUG2("Received AKA-Notification with no notification code");
				eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE_NOTIFICATION);
				return RLM_MODULE_HANDLED;			/* We need to process more packets */
			}

			/*
			 *	Case 3 where we're allowed to send an EAP-Failure
			 */
			if (!(vp->vp_uint16 & 0x8000)) {
				REDEBUG2("AKA-Notification %s (%i) indicates failure", fr_pair_value_enum(vp, buff),
					 vp->vp_uint16);
				eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE);
				return RLM_MODULE_REJECT;
			}

			/*
			 *	...if it's not a failure, then re-enter the
			 *	current state.
			 */
			REDEBUG2("Got AKA-Notification %s (%i)", fr_pair_value_enum(vp, buff), vp->vp_uint16);
			eap_aka_state_enter(eap_session, eap_aka_session->state);
			return RLM_MODULE_HANDLED;
		}

		default:
		unexpected_subtype:
			/*
			 *	RFC 4187 says we *MUST* notify, not just
			 *	send an EAP-Failure in this case.
			 */
			REDEBUG("Unexpected subtype %pV", &subtype_vp->data);
			eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE_NOTIFICATION);
			return RLM_MODULE_HANDLED;				/* We need to process more packets */
		}

		break;

	/*
	 *	Process the response to our previous challenge.
	 */
	case EAP_AKA_SERVER_CHALLENGE:
		switch (subtype) {
		case EAP_AKA_CHALLENGE:
			if (process_eap_aka_challenge(eap_session, vps) == 0) return RLM_MODULE_HANDLED;
			eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE_NOTIFICATION);
			return RLM_MODULE_HANDLED;				/* We need to process more packets */

		case EAP_AKA_SYNCHRONIZATION_FAILURE:
			REDEBUG("EAP-AKA Peer synchronization failure");	/* We can't handle these yet */
			eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE_NOTIFICATION);
			return RLM_MODULE_HANDLED;				/* We need to process more packets */

		/*
		 *	Case 1 where we're allowed to send an EAP-Failure
		 */
		case EAP_AKA_CLIENT_ERROR:
		{
			char buff[20];

			vp = fr_pair_find_by_child_num(vps, dict_aka_root, FR_EAP_AKA_CLIENT_ERROR_CODE, TAG_ANY);
			if (!vp) {
				REDEBUG("EAP-AKA Peer rejected AKA-Challenge with client-error message but "
					"has not supplied a client error code");
			} else {
				REDEBUG("Client rejected AKA-Challenge with error: %s (%i)",
					fr_pair_value_enum(vp, buff), vp->vp_uint16);
			}
			eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE);
			return RLM_MODULE_REJECT;
		}

		/*
		 *	Case 2 where we're allowed to send an EAP-Failure
		 */
		case EAP_AKA_AUTHENTICATION_REJECT:
			REDEBUG("EAP-AKA Peer Rejected AUTN");
			eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE);
			return RLM_MODULE_REJECT;

		case EAP_AKA_NOTIFICATION:
			goto notification;

		default:
			goto unexpected_subtype;
		}

	/*
	 *	Peer acked our failure
	 */
	case EAP_AKA_SERVER_FAILURE_NOTIFICATION:
		switch (subtype) {
		case EAP_AKA_NOTIFICATION:
			RDEBUG2("AKA-Notification ACKed, sending EAP-Failure");
			eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE);
			return RLM_MODULE_REJECT;

		default:
			goto unexpected_subtype;
		}

	/*
	 *	Something bad happened...
	 */
	default:
		rad_assert(0);
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_FAILURE_NOTIFICATION);
		return RLM_MODULE_HANDLED;				/* We need to process more packets */
	}
}

/** Initiate the EAP-SIM session by starting the state machine
 *
 */
static rlm_rcode_t mod_session_init(void *instance, eap_session_t *eap_session)
{
	REQUEST				*request = eap_session->request;
	eap_aka_session_t		*eap_aka_session;
	rlm_eap_aka_t			*inst = instance;
	fr_sim_id_type_t		type;
	fr_sim_method_hint_t		method;

	MEM(eap_aka_session = talloc_zero(eap_session, eap_aka_session_t));

	eap_session->opaque = eap_aka_session;

	/*
	 *	Set default configuration, we may allow these
	 *	to be toggled by attributes later.
	 */
	eap_aka_session->request_identity = inst->request_identity;
	eap_aka_session->send_result_ind = inst->protected_success;
	eap_aka_session->id_req = SIM_NO_ID_REQ;	/* Set the default */

	/*
	 *	This value doesn't have be strong, but it is
	 *	good if it is different now and then.
	 */
	eap_aka_session->aka_id = (fr_rand() & 0xff);

	/*
	 *	Process the identity that we received in the
	 *	EAP-Identity-Response and use it to determine
	 *	the initial request we send to the Supplicant.
	 */
	if (fr_sim_id_type(&type, &method,
			   eap_session->identity, talloc_array_length(eap_session->identity) - 1) < 0) {
		RDEBUG2("Failed parsing identity, continuing anyway: %s", fr_strerror());
	}

	/*
	 *	Unless AKA-Prime is explicitly disabled,
	 *	use it... It has stronger keying, and
	 *	binds authentication to the network.
	 */
	switch (eap_session->type) {
	case FR_EAP_AKA_PRIME:
	default:
		RDEBUG2("New EAP-AKA' session");
		eap_aka_session->type = FR_EAP_AKA_PRIME;
		eap_aka_session->kdf = FR_EAP_AKA_KDF_VALUE_EAP_AKA_PRIME_WITH_CK_PRIME_IK_PRIME;
		eap_aka_session->checkcode_md = eap_aka_session->mac_md = EVP_sha256();
		eap_aka_session->keys.network = (uint8_t *) talloc_bstrndup(eap_aka_session, inst->network_id,
									    talloc_array_length(inst->network_id) - 1);
		eap_aka_session->keys.network_len = talloc_array_length(eap_aka_session->keys.network) - 1;
		switch (method) {
		default:
			RWDEBUG("EAP-Identity-Response hints that EAP-%s should be started, but we're "
				"attempting EAP-AKA'", fr_int2str(sim_id_method_hint_table, method, "<INVALID>"));
			break;

		case SIM_METHOD_HINT_AKA_PRIME:
		case SIM_METHOD_HINT_UNKNOWN:
			break;
		}
		break;

	case FR_EAP_AKA:
		RDEBUG2("New EAP-AKA session");
		eap_aka_session->type = FR_EAP_AKA;
		eap_aka_session->kdf = 0;
		eap_aka_session->checkcode_md = eap_aka_session->mac_md = EVP_sha1();
		eap_aka_session->send_at_bidding = true;
		switch (method) {
		default:
			RWDEBUG("EAP-Identity-Response hints that EAP-%s should be started, but we're "
				"attempting EAP-AKA", fr_int2str(sim_id_method_hint_table, method, "<INVALID>"));
			break;

		case SIM_METHOD_HINT_AKA:
		case SIM_METHOD_HINT_UNKNOWN:
			break;
		}
		break;
	}
	eap_session->process = mod_process;

	/*
	 *	Admin wants us to always request an identity
	 *	initially.  The RFC says this is also the
	 *	better way to operate, as the supplicant
	 *	can 'decorate' the identity in the identity
	 *	response.
	 */
	if (inst->request_identity) {
	request_id:
		/*
		 *	We always start by requesting
		 *	any ID initially as we can
		 *	always negotiate down.
		 */
		eap_aka_session->id_req = SIM_ANY_ID_REQ;
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_IDENTITY);
		return RLM_MODULE_OK;
	}
	/*
	 *	Figure out what type of identity we have
	 *	and use it to determine the initial
	 *	request we send.
	 */
	switch (type) {
	/*
	 *	If there's no valid tag on the identity
	 *	then it's probably been decorated by the
	 *	supplicant.
	 *
	 *	Request the unmolested identity
	 */
	case SIM_ID_TYPE_UNKNOWN:
		RWDEBUG("Identity format unknown, sending Identity request");
		goto request_id;

	/*
	 *	These types need to be transformed into something
	 *	usable before we can do anything.
	 */
	case SIM_ID_TYPE_PSEUDONYM:
	case SIM_ID_TYPE_FASTAUTH:

	/*
	 *	Permanent ID means we can just send the challenge
	 */
	case SIM_ID_TYPE_PERMANENT:
		eap_aka_session->keys.identity_len = talloc_array_length(eap_session->identity) - 1;
		MEM(eap_aka_session->keys.identity = talloc_memdup(eap_aka_session, eap_session->identity,
								   eap_aka_session->keys.identity_len));
		eap_aka_state_enter(eap_session, EAP_AKA_SERVER_CHALLENGE);
		return RLM_MODULE_OK;
	}

	return RLM_MODULE_OK;
}

static int mod_load(void)
{
	dict_aka_root = fr_dict_attr_child_by_num(fr_dict_root(fr_dict_internal), FR_EAP_AKA_ROOT);
	if (!dict_aka_root) {
		ERROR("Missing EAP-AKA-Root attribute");
		return -1;
	}
	if (fr_sim_global_init() < 0) return -1;
	sim_xlat_register();

	return 0;
}

static void mod_unload(void)
{
	sim_xlat_unregister();
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 */
extern rlm_eap_submodule_t rlm_eap_aka;
rlm_eap_submodule_t rlm_eap_aka = {
	.name		= "eap_aka",
	.magic		= RLM_MODULE_INIT,

	.provides	= { FR_EAP_AKA, FR_EAP_AKA_PRIME },
	.inst_size	= sizeof(rlm_eap_aka_t),
	.config		= submodule_config,

	.load		= mod_load,
	.unload		= mod_unload,
	.session_init	= mod_session_init,	/* Initialise a new EAP session */
	.process	= mod_process,		/* Process next round of EAP method */
};
