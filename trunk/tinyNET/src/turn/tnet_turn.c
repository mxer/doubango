/*
* Copyright (C) 2009 Mamadou Diop.
*
* Contact: Mamadou Diop <diopmamadou@yahoo.fr>
*	
* This file is part of Open Source Doubango Framework.
*
* DOUBANGO is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*	
* DOUBANGO is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*	
* You should have received a copy of the GNU General Public License
* along with DOUBANGO.
*
*/

/**@file tnet_turn.c
 * @brief Traversal Using Relays around NAT (TURN) implementation as per 'draft-ietf-behave-turn-16', 'draft-ietf-behave-turn-tcp-05'
 *		 and 'draft-ietf-behave-turn-ipv6'.
 *			http://tools.ietf.org/html/draft-ietf-behave-turn-16
 *			http://tools.ietf.org/html/draft-ietf-behave-turn-tcp-05
 *			http://tools.ietf.org/html/draft-ietf-behave-turn-ipv6-07
 *
 * @author Mamadou Diop <diopmamadou(at)yahoo.fr>
 *
 * @date Created: Sat Nov 8 16:54:58 2009 mdiop
 */
#include "tnet_turn.h"


#include "tsk_string.h"
#include "tsk_memory.h"

#include "../tnet_nat.h"
#include "../tnet_utils.h"

#include "tnet_turn_attribute.h"

#include "tsk_md5.h"
#include "tsk_debug.h"

#include <string.h>

/*
- IMPORTANT: 16.  Detailed Example
- It is suggested that the client refresh the allocation roughly 1 minute before it expires.
- If the client wishes to immediately delete an existing allocation, it includes a LIFETIME attribute with a value of 0.
*/

int tnet_turn_send_allocate(const tnet_nat_context_t* context, tnet_turn_allocation_t* allocation)
{
	tnet_stun_attribute_t* attribute;
	int ret = -1;

	tnet_stun_request_t *request = TNET_STUN_MESSAGE_CREATE(context->username, context->password);
	request->fingerprint = context->enable_fingerprint;
	request->integrity = context->enable_integrity;
	request->dontfrag = context->enable_dontfrag;
	request->realm = tsk_strdup(allocation->realm);
	request->nonce = tsk_strdup(allocation->nonce);

	if(request)
	{
		request->type = allocation->active ? stun_refresh_request : stun_allocate_request;

		{	/* Create random transaction id */
			tsk_istr_t random;
			tsk_md5digest_t digest;

			tsk_strrandom(&random);
			TSK_MD5_DIGEST_CALC(random, sizeof(random), digest);

			memcpy(request->transaction_id, digest, TNET_STUN_TRANSACID_SIZE);
		}

		/* Add software attribute */
		if(allocation->software)
		{
			attribute = TNET_STUN_ATTRIBUTE_SOFTWARE_CREATE(allocation->software, strlen(allocation->software));
			tnet_stun_message_add_attribute(request, &attribute);
		}

		/* Add Requested transport. */
		{
			attribute = TNET_TURN_ATTRIBUTE_REQTRANS_CREATE(TNET_SOCKET_TYPE_IS_DGRAM(allocation->socket_type) ? TNET_PROTO_UDP: TNET_PROTO_TCP);
			tnet_stun_message_add_attribute(request, &attribute);
		}

		/* Add lifetime */
		{
			attribute = TNET_TURN_ATTRIBUTE_LIFETIME_CREATE(ntohl(allocation->timeout));
			tnet_stun_message_add_attribute(request, &attribute);
		}

		/* Add Event Port */
		{
			attribute = TNET_TURN_ATTRIBUTE_EVEN_PORT_CREATE(context->enable_evenport);
			tnet_stun_message_add_attribute(request, &attribute);
		}

		if(TNET_SOCKET_TYPE_IS_DGRAM(allocation->socket_type))
		{
			tnet_stun_response_t *response = tnet_stun_send_unreliably(allocation->localFD, 500, 7, request, (struct sockaddr*)&allocation->server);
			if(response)
			{
				if(TNET_STUN_RESPONSE_IS_ERROR(response))
				{
					short code = tnet_stun_message_get_errorcode(response);
					const char* realm = tnet_stun_message_get_realm(response);
					const char* nonce = tnet_stun_message_get_nonce(response);

					if(code == 401 && realm && nonce)
					{
						if(!allocation->nonce)
						{	/* First time we get a nonce */
							tsk_strupdate(&allocation->nonce, nonce);
							tsk_strupdate(&allocation->realm, realm);
							
							/* Delete the message and response before retrying*/
							TSK_OBJECT_SAFE_FREE(response);
							TSK_OBJECT_SAFE_FREE(request);

							// Send again using new transaction identifier
							return tnet_turn_send_allocate(context, allocation);
						}
						else
						{
							ret = -3;
						}
					}
					else
					{
						TSK_DEBUG_ERROR("Server error code: %d", code);
						ret = -2;
					}
				}
				else
				{
					int32_t lifetime = tnet_stun_message_get_lifetime(response);
					if(lifetime>=0)
					{
						allocation->timeout = lifetime;
					}
					ret = 0;
				}
			}
			else
			{
				ret = -4;
			}
			TSK_OBJECT_SAFE_FREE(response);
		}
	}
	
	TSK_OBJECT_SAFE_FREE(request);
	return ret;
}

tnet_turn_allocation_id_t tnet_turn_allocate(const void* nat_context, const tnet_fd_t localFD, tnet_socket_type_t socket_type)
{
	tnet_turn_allocation_id_t id = TNET_TURN_INVALID_ALLOCATION_ID;
	const tnet_nat_context_t* context = nat_context;

	if(context)
	{
		int ret;
		tnet_turn_allocation_t* allocation = TNET_TURN_ALLOCATION_CREATE(localFD, context->socket_type, context->server_address, context->server_port, context->username, context->password);
		allocation->software = tsk_strdup(context->software);
		
		if((ret = tnet_turn_send_allocate(nat_context, allocation)))
		{
			TSK_DEBUG_ERROR("TURN allocation failed with error code:%d.", ret);
			TSK_OBJECT_SAFE_FREE(allocation);
		}
		else
		{
			id = allocation->id;
			allocation->active = 1;
			tsk_list_push_back_data(context->allocations, (void**)&allocation);
		}
	}

	return id;
}

int tnet_turn_unallocate(const void* nat_context, tnet_turn_allocation_t *allocation)
{
	const tnet_nat_context_t* context = nat_context;

	if(context && allocation)
	{
		int ret;
		uint32_t timeout = allocation->timeout; /* Save timeout. */
		allocation->timeout = 0;

		if((ret = tnet_turn_send_allocate(nat_context, allocation)))
		{
			TSK_DEBUG_ERROR("TURN unallocation failed with error code:%d.", ret);

			allocation->timeout = timeout; /* Restore timeout */
			return -1;
		}
		else
		{
			tsk_list_remove_item_by_data(context->allocations, allocation);
			return 0;
		}
	}
	return -1;
}


//========================================================
//	TURN CONTEXT object definition
//
static void* tnet_turn_context_create(void * self, va_list * app)
{
	tnet_turn_context_t *context = self;
	if(context)
	{
		context->server_address = tsk_strdup(va_arg(*app, const char*));
		context->server_port = va_arg(*app, tnet_port_t);

		context->username = tsk_strdup(va_arg(*app, const char*));
		context->password = tsk_strdup(va_arg(*app, const char*));

		context->software = tsk_strdup(TNET_SOFTWARE);
		context->timeout = 600;

		context->allocations = TSK_LIST_CREATE();
	}
	return self;
}

static void* tnet_turn_context_destroy(void * self)
{ 
	tnet_turn_context_t *context = self;
	if(context)
	{
		TSK_FREE(context->username);
		TSK_FREE(context->password);

		TSK_FREE(context->server_address);

		TSK_FREE(context->software);

		TSK_OBJECT_SAFE_FREE(context->allocations);
	}

	return self;
}

static const tsk_object_def_t tnet_turn_context_def_s = 
{
	sizeof(tnet_turn_context_t),
	tnet_turn_context_create, 
	tnet_turn_context_destroy,
	0, 
};
const void *tnet_turn_context_def_t = &tnet_turn_context_def_s;



//========================================================
//	TURN ALLOCATION object definition
//
static void* tnet_turn_allocation_create(void * self, va_list * app)
{
	tnet_turn_allocation_t *allocation = self;
	if(allocation)
	{
		static tnet_turn_allocation_id_t __unique_id = 0;
		
		const char* server_address;
		tnet_port_t server_port;

		allocation->id = ++__unique_id;

		allocation->localFD = va_arg(*app, tnet_fd_t);
		allocation->socket_type = va_arg(*app, tnet_socket_type_t);
	
		server_address = va_arg(*app, const char*);
		server_port = va_arg(*app, tnet_port_t);

		allocation->username = tsk_strdup(va_arg(*app, const char*));
		allocation->password = tsk_strdup(va_arg(*app, const char*));

		tnet_sockaddr_init(server_address, server_port, allocation->socket_type, &allocation->server);
		allocation->timeout = 600;
	}
	return self;
}

static void* tnet_turn_allocation_destroy(void * self)
{ 
	tnet_turn_allocation_t *allocation = self;
	if(allocation)
	{
		TSK_FREE(allocation->relay_address);

		TSK_FREE(allocation->username);
		TSK_FREE(allocation->password);
		TSK_FREE(allocation->realm);
		TSK_FREE(allocation->nonce);

		TSK_FREE(allocation->software);
	}
	
	return self;
}

static const tsk_object_def_t tnet_turn_allocation_def_s = 
{
	sizeof(tnet_turn_allocation_t),
	tnet_turn_allocation_create, 
	tnet_turn_allocation_destroy,
	0, 
};
const void *tnet_turn_allocation_def_t = &tnet_turn_allocation_def_s;