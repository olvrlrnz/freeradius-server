/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
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
 *
 * @file unlang/call_env.c
 * @brief Call environment parsing functions
 *
 * @copyright 2023 Network RADIUS SAS (legal@networkradius.com)
 */
RCSID("$Id$")

#include <freeradius-devel/server/log.h>
#include <freeradius-devel/unlang/tmpl.h>
#include <freeradius-devel/unlang/function.h>
#include <freeradius-devel/unlang/interpret.h>
#include <talloc.h>
#include "call_env.h"


/** Parse the result of call_env tmpl expansion
 */
static inline CC_HINT(always_inline)
call_env_result_t call_env_value_parse(TALLOC_CTX *ctx, request_t *request, void *out,
				       void **tmpl_out, call_env_parsed_t const *env,
				       fr_value_box_list_t *tmpl_expanded)
{
	fr_value_box_t	*vb;

	if (tmpl_out) *tmpl_out = env->tmpl;
	if (env->tmpl_only) return CALL_ENV_SUCCESS;

	vb = fr_value_box_list_head(tmpl_expanded);
	if (!vb) {
		if (!env->rule->pair.nullable) {
			RPEDEBUG("Failed to evaluate required module option %s = %s", env->rule->name, env->tmpl->name);
			return CALL_ENV_MISSING;
		}
		return CALL_ENV_SUCCESS;
	}

	/*
	 *	Concatenate multiple boxes if needed
	 */
	if (env->rule->pair.concat &&
	    fr_value_box_list_concat_in_place(vb, vb, tmpl_expanded, env->rule->type,
					      FR_VALUE_BOX_LIST_FREE, true, SIZE_MAX) < 0 ) {
		RPEDEBUG("Failed concatenating values for %s", env->rule->name);
		return CALL_ENV_INVALID;
	}

	if (env->rule->pair.single && (fr_value_box_list_num_elements(tmpl_expanded) > 1)) {
		RPEDEBUG("%d values found for %s.  Only one is allowed",
			 fr_value_box_list_num_elements(tmpl_expanded), env->rule->name);
		return CALL_ENV_INVALID;
	}

	while ((vb = fr_value_box_list_pop_head(tmpl_expanded))) {
		switch (env->rule->pair.type) {
		case CALL_ENV_TYPE_VALUE_BOX:
			fr_value_box_copy_shallow(ctx, (fr_value_box_t *)(out), vb);
			break;

		case CALL_ENV_TYPE_VALUE_BOX_LIST:
			if (!fr_value_box_list_initialised((fr_value_box_list_t *)out)) fr_value_box_list_init((fr_value_box_list_t *)out);
			fr_value_box_list_insert_tail((fr_value_box_list_t *)out, vb);
			break;

		case CALL_ENV_TYPE_TMPL_ONLY:
			fr_assert(0);
			break;
		}
	}

	return CALL_ENV_SUCCESS;
}

/** Context to keep track of expansion of call environments
 *
 */
typedef struct {
	call_env_result_t			*result;		//!< Where to write the return code of callenv expansion.
	call_env_t const			*call_env;		//!< Call env being expanded.
	call_env_parsed_t const			*last_expanded;		//!< The last expanded tmpl.
	fr_value_box_list_t			tmpl_expanded;		//!< List to write value boxes to as tmpls are expanded.
	void					**data;			//!< Final destination structure for value boxes.
} call_env_rctx_t;

static unlang_action_t call_env_expand_repeat(rlm_rcode_t *p_result, int *priority, request_t *request, void *uctx);

/** Start the expansion of a call environment tmpl.
 *
 */
static unlang_action_t call_env_expand_start(UNUSED rlm_rcode_t *p_result, UNUSED int *priority, request_t *request, void *uctx)
{
	call_env_rctx_t	*call_env_rctx = talloc_get_type_abort(uctx, call_env_rctx_t);
	TALLOC_CTX	*ctx;
	call_env_parsed_t const	*env = NULL;
	void		**out;

	while ((call_env_rctx->last_expanded = call_env_parsed_next(&call_env_rctx->call_env->parsed, call_env_rctx->last_expanded))) {
		env = call_env_rctx->last_expanded;
		fr_assert(env != NULL);

		if (!env->tmpl_only) break;

		/*
		 *	If we only need the tmpl, just set the pointer and move the next.
		 */
		out = (void **)((uint8_t *)*call_env_rctx->data + env->rule->pair.tmpl_offset);
		*out = env->tmpl;
	}

	if (!call_env_rctx->last_expanded) {	/* No more! */
		if (call_env_rctx->result) *call_env_rctx->result = CALL_ENV_SUCCESS;
		return UNLANG_ACTION_CALCULATE_RESULT;
	}

	ctx = *call_env_rctx->data;

	fr_assert(env != NULL);

	/*
	 *	Multi pair options should allocate boxes in the context of the array
	 */
	if (env->rule->pair.multi) {
		out = (void **)((uint8_t *)(*call_env_rctx->data) + env->rule->offset);

		/*
		 *	For multi pair options, allocate the array before expanding the first entry.
		 */
		if (env->multi_index == 0) {
			void *array;
			MEM(array = _talloc_zero_array((*call_env_rctx->data), env->rule->pair.size,
						       env->count, env->rule->pair.type_name));
			*out = array;
		}
		ctx = *out;
	}

	if (unlang_tmpl_push(ctx, &call_env_rctx->tmpl_expanded, request, call_env_rctx->last_expanded->tmpl,
			     NULL) < 0) return UNLANG_ACTION_FAIL;

	return UNLANG_ACTION_PUSHED_CHILD;
}

/** Extract expanded call environment tmpl and store in env_data
 *
 * If there are more tmpls to expand, push the next expansion.
 */
static unlang_action_t call_env_expand_repeat(UNUSED rlm_rcode_t *p_result, UNUSED int *priority,
					      request_t *request, void *uctx)
{
	void			*out = NULL, *tmpl_out = NULL;
	call_env_rctx_t		*call_env_rctx = talloc_get_type_abort(uctx, call_env_rctx_t);
	call_env_parsed_t	const *env;
	call_env_result_t	result;

	env = call_env_rctx->last_expanded;
	if (!env) return UNLANG_ACTION_CALCULATE_RESULT;

	if (env->tmpl_only) goto tmpl_only;
	/*
	 *	Find the location of the output
	 */
	out = ((uint8_t*)(*call_env_rctx->data)) + env->rule->offset;

	/*
	 *	If this is a multi pair option, the output is an array.
	 *	Find the correct offset in the array
	 */
	if (env->rule->pair.multi) {
		void *array = *(void **)out;
		out = ((uint8_t *)array) + env->rule->pair.size * env->multi_index;
	}

tmpl_only:
	if (env->rule->pair.tmpl_offset >= 0) tmpl_out = ((uint8_t *)*call_env_rctx->data) + env->rule->pair.tmpl_offset;

	result = call_env_value_parse(*call_env_rctx->data, request, out, tmpl_out, env, &call_env_rctx->tmpl_expanded);
	if (result != CALL_ENV_SUCCESS) {
		if (call_env_rctx->result) *call_env_rctx->result = result;
		return UNLANG_ACTION_FAIL;
	}

	if (!call_env_parsed_next(&call_env_rctx->call_env->parsed, env)) {
		if (call_env_rctx->result) *call_env_rctx->result = CALL_ENV_SUCCESS;
		return UNLANG_ACTION_CALCULATE_RESULT;
	}

	return unlang_function_push(request, call_env_expand_start, call_env_expand_repeat, NULL, 0, UNLANG_SUB_FRAME,
				    call_env_rctx);
}

/** Initialise the expansion of a call environment
 *
 * @param[in] ctx		in which to allocate destination structure for resulting value boxes.
 * @param[in] request		Current request.
 * @param[out] env_result	Where to write the result of the callenv expansion.  May be NULL
 * @param[in,out] env_data	Where the destination structure should be created.
 * @param[in] call_env		Call environment being expanded.
 */
unlang_action_t call_env_expand(TALLOC_CTX *ctx, request_t *request, call_env_result_t *env_result, void **env_data,
				call_env_t const *call_env)
{
	call_env_rctx_t	*call_env_rctx;

	MEM(call_env_rctx = talloc_zero(ctx, call_env_rctx_t));
	MEM(*env_data = talloc_zero_array(ctx, uint8_t, call_env->method->inst_size));
	talloc_set_name_const(*env_data, call_env->method->inst_type);
	call_env_rctx->result = env_result;
	if (env_result) *env_result = CALL_ENV_INVALID;	/* Make sure we ran to completion*/
	call_env_rctx->data = env_data;
	call_env_rctx->call_env = call_env;
	fr_value_box_list_init(&call_env_rctx->tmpl_expanded);

	return unlang_function_push(request, call_env_expand_start, call_env_expand_repeat, NULL, 0, UNLANG_SUB_FRAME,
				    call_env_rctx);
}

/** Parse per call env
 *
 * Used for config options which must be parsed in the context in which
 * the module is being called.
 *
 * @param[in] ctx		To allocate parsed environment in.
 * @param[out] parsed		Where to write parsed environment.
 * @param[in] name		Module name for error messages.
 * @param[in] dict_def		Default dictionary to use when tokenizing tmpls.
 * @param[in] cs		Module config.
 * @param[in] call_env		to parse.
 * @return
 *	- 0 on success;
 *	- <0 on failure;
 */
static int call_env_parse(TALLOC_CTX *ctx, call_env_parsed_head_t *parsed, char const *name, fr_dict_t const *dict_def,
			  CONF_SECTION const *cs, call_env_parser_t const *call_env) {
	CONF_PAIR const		*cp, *next;
	call_env_parsed_t	*call_env_parsed;
	ssize_t			len, count, multi_index;
	char const		*value;
	fr_token_t		quote;
	fr_type_t		type;

	while (call_env->name) {
		if (call_env->flags & CONF_FLAG_SUBSECTION) {
			CONF_SECTION const *subcs;
			subcs = cf_section_find(cs, call_env->name, call_env->section.ident2);
			if (!subcs) {
				if (!call_env->section.required) goto next;
				cf_log_err(cs, "Module %s missing required section %s", name, call_env->name);
				return -1;
			}

			if (call_env_parse(ctx, parsed, name, dict_def, subcs, call_env->section.subcs) < 0) return -1;
			goto next;
		}

		cp = cf_pair_find(cs, call_env->name);

		if (!cp && !call_env->dflt) {
			if (!call_env->pair.required) goto next;

			cf_log_err(cs, "Module %s missing required option %s", name, call_env->name);
			return -1;
		}

		/*
		 *	Check for additional conf pairs and error
		 *	if there is one and multi is not allowed.
		 */
		if (!call_env->pair.multi && ((next = cf_pair_find_next(cs, cp, call_env->name)))) {
			cf_log_err(cf_pair_to_item(next), "Invalid duplicate configuration item '%s'", call_env->name);
			return -1;
		}

		count = cf_pair_count(cs, call_env->name);
		if (count == 0) count = 1;

		for (multi_index = 0; multi_index < count; multi_index ++) {
			MEM(call_env_parsed = talloc_zero(ctx, call_env_parsed_t));
			call_env_parsed->rule = call_env;
			call_env_parsed->count = count;
			call_env_parsed->multi_index = multi_index;
			if (call_env->pair.type == CALL_ENV_TYPE_TMPL_ONLY) call_env_parsed->tmpl_only = true;

			if (cp) {
				value = cf_pair_value(cp);
				len = talloc_array_length(value) - 1;
				quote = call_env->pair.force_quote ? call_env->dflt_quote : cf_pair_value_quote(cp);
			} else {
				value = call_env->dflt;
				len = strlen(value);
				quote = call_env->dflt_quote;
			}

			type = call_env->type;
			if (tmpl_afrom_substr(call_env_parsed, &call_env_parsed->tmpl, &FR_SBUFF_IN(value, len),
					      quote, NULL, &(tmpl_rules_t){
							.cast = (type == FR_TYPE_VOID ? FR_TYPE_NULL : type),
							.attr = {
								.list_def = request_attr_request,
								.dict_def = dict_def
							}
						}) < 0) {
			error:
				talloc_free(call_env_parsed);
				cf_log_perr(cp, "Failed to parse configuration item '%s = %s'", call_env->name, value);
				return -1;
			}

			/*
			 *	Ensure only valid TMPL types are produced.
			 */
			switch (call_env_parsed->tmpl->type) {
			case TMPL_TYPE_DATA:
			case TMPL_TYPE_EXEC:
			case TMPL_TYPE_XLAT:
				if (call_env->type & CONF_FLAG_ATTRIBUTE) {
					cf_log_perr(cp, "'%s' expands to %s - attribute reference required", value,
					   	    fr_table_str_by_value(tmpl_type_table, call_env_parsed->tmpl->type,
						    			  "<INVALID>"));
					goto error;
				}
				FALL_THROUGH;

			case TMPL_TYPE_ATTR:
				break;

			default:
				cf_log_err(cp, "'%s' expands to invalid tmpl type %s", value,
					   fr_table_str_by_value(tmpl_type_table, call_env_parsed->tmpl->type, "<INVALID>"));
				goto error;
			}

			call_env_parsed_insert_tail(parsed, call_env_parsed);

			cp = cf_pair_find_next(cs, cp, call_env->name);
		}
	next:
		call_env++;
	}

	return 0;
}

/**  Perform a quick assessment of how many parsed call env will be produced.
 *
 * @param[in,out] names_len	Where to write the sum of bytes required to represent
 *				the strings which will be parsed as tmpls.  This is required
 *				to pre-allocate space for the tmpl name buffers.
 * @param[in] cs		Conf section to search for pairs.
 * @param[in] call_env		to parse.
 * @return Number of parsed_call_env expected to be required.
 */
static size_t call_env_count(size_t *names_len, CONF_SECTION const *cs, call_env_parser_t const *call_env)
{
	size_t	pair_count, tmpl_count = 0;
	CONF_PAIR const	*cp;

	*names_len = 0;

	while (call_env->name) {
		if (call_env->flags & CONF_FLAG_SUBSECTION) {
			CONF_SECTION const *subcs;
			subcs = cf_section_find(cs, call_env->name, call_env->section.ident2);
			if (!subcs) goto next;

			tmpl_count += call_env_count(names_len, subcs, call_env->section.subcs);
			goto next;
		}
		pair_count = 0;
		cp = NULL;
		while ((cp = cf_pair_find_next(cs, cp, call_env->name))) {
			pair_count++;
			*names_len += talloc_array_length(cf_pair_value(cp));
		}
		if (!pair_count && call_env->dflt) {
			pair_count = 1;
			*names_len += strlen(call_env->dflt);
		}
		tmpl_count += pair_count;
	next:
		call_env++;
	}

	return tmpl_count;
}

/** Given a call_env_method, parse all call_env_pair_t in the context of a specific call to an xlat or module method
 *
 * @param[in] ctx		to allocate the call_env_t in.
 * @param[in] name		Module name for error messages.
 * @param[in] call_env_method	containing the call_env_pair_t to evaluate against the specified CONF_SECTION.
 * @param[in] namespace		to pass to the tmpl tokenizer when parsing pairs from the specified CONF_SECTION.
 * @param[in] cs		to parse in the context of the call.
 * @return
 *	- A new call_env_t on success.
 * 	- NULL on failure.
 */
call_env_t *call_env_alloc(TALLOC_CTX *ctx, char const *name, call_env_method_t const *call_env_method,
			   fr_dict_t const *namespace, CONF_SECTION *cs)
{
	unsigned int	count;
	size_t		names_len;
	call_env_t	*call_env;

	/*
	 *	Only used if caller doesn't use a more specific assert
	 */
	fr_assert_msg(call_env_method->inst_size, "inst_size 0 for %s, method_env (%p)", name, call_env_method);

	/*
	 *	Firstly assess how many parsed env there will be and create a talloc pool to hold them.
	 *	The pool size is a rough estimate based on each tmpl also allocating at least two children,
	 *	for which we allow twice the length of the value to be parsed.
	 */
	count = call_env_count(&names_len, cs, call_env_method->env);

	/*
	 *  Pre-allocated headers:
	 *	1 header for the call_env_pair_parsed_t, 1 header for the tmpl_t, 1 header for the name,
	 *	one header for the value.
	 *
	 *  Pre-allocated memory:
	 *	((sizeof(call_env_pair_parsed_t) + sizeof(tmpl_t)) * count) + (names of tmpls * 2)... Not sure what
	 *	the * 2 is for, maybe for slop?
	 */
	MEM(call_env = talloc_pooled_object(ctx, call_env_t, count * 4, (sizeof(call_env_parser_t) + sizeof(tmpl_t)) * count + names_len * 2));
	call_env->method = call_env_method;
	call_env_parsed_init(&call_env->parsed);
	if (call_env_parse(call_env, &call_env->parsed, name, namespace, cs, call_env_method->env) < 0) {
		talloc_free(call_env);
		return NULL;
	}

	return call_env;
}
