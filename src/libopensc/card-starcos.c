/*
 * card-starcos.c: Support for STARCOS SPK 2.3 cards
 *
 * Copyright (C) 2003  J�rn Zukowski <zukowski@trustcenter.de> and 
 *                     Nils Larsch   <larsch@trustcenter.de>, TrustCenter AG
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "asn1.h"
#include "log.h"


/* TODO: - secure messaging 
 *       - Starcos ACs <-> OpenSC ACs
 *       - CREATE EF/MF/DF doesn't really work
 */

static const char *starcos_atrs[] = {
  "3B:B7:94:00:c0:24:31:fe:65:53:50:4b:32:33:90:00:b4",
  "3B:B7:94:00:81:31:fe:65:53:50:4b:32:33:90:00:d1",
  NULL
};

static struct sc_card_operations starcos_ops;
static struct sc_card_operations *iso_ops = NULL;

static struct sc_card_driver starcos_drv = {
	"driver for STARCOS SPK 2.3 cards",
	"starcos",
	&starcos_ops
};


const static struct sc_card_error starcos_errors[] = 
{
	{ 0x6600, SC_ERROR_INCORRECT_PARAMETERS, "Error setting the security env"},
	{ 0x66F0, SC_ERROR_INCORRECT_PARAMETERS, "No space left for padding"},
	{ 0x69F0, SC_ERROR_NOT_ALLOWED,          "Command not allowed"},
	{ 0x6A89, SC_ERROR_FILE_ALREADY_EXISTS,  "Files exists"},
	{ 0x6A8A, SC_ERROR_FILE_ALREADY_EXISTS,  "Application exists"},
	{ 0x6F01, SC_ERROR_CARD_CMD_FAILED, "public key not complete"},
	{ 0x6F02, SC_ERROR_CARD_CMD_FAILED, "data overflow"},
	{ 0x6F03, SC_ERROR_CARD_CMD_FAILED, "invalid command sequence"},
	{ 0x6F05, SC_ERROR_CARD_CMD_FAILED, "security enviroment invalid"},
	{ 0x6F07, SC_ERROR_FILE_NOT_FOUND, "key part not found"},
	{ 0x6F08, SC_ERROR_CARD_CMD_FAILED, "signature failed"},
	{ 0x6F0A, SC_ERROR_INCORRECT_PARAMETERS, "key format does not match key length"},
	{ 0x6F0B, SC_ERROR_INCORRECT_PARAMETERS, "length of key component inconsistent with algorithm"},
	{ 0x6F81, SC_ERROR_CARD_CMD_FAILED, "system error"}
};


typedef struct starcos_mse_state_st {
	int sec_ops;	/* the currently selected security operation,
			 * i.e. SC_SEC_OPERATION_AUTHENTICATE etc. */
	u8  buf[SC_MAX_APDU_BUFFER_SIZE]; /* apdu data */
	size_t buf_len;
	u8  p1, p2;	/* apdu parameters */
} starcos_mse_state;


static void process_fci(struct sc_context *ctx, struct sc_file *file,
		       const u8 *buf, size_t buflen)
{
	/* NOTE: According to the Starcos S 2.1 manual it's possible
	 *       that a SELECT DF returns as a FCI arbitrary data which
	 *       is stored in a object file (in the corresponding DF)
	 *       with the tag 0x6f.
	 */

	size_t taglen, len = buflen;
	const u8 *tag = NULL, *p = buf;
  
	if (ctx->debug >= 3)
		debug(ctx, "processing FCI bytes\n");

	/* defaults */
	file->type = SC_FILE_TYPE_WORKING_EF;
	file->ef_structure = SC_FILE_EF_UNKNOWN;
	file->shareable = 0;
	file->record_length = 0;
	file->size = 0;
  
	tag = sc_asn1_find_tag(ctx, p, len, 0x80, &taglen);
	if (tag != NULL && taglen >= 2) 
	{
		int bytes = (tag[0] << 8) + tag[1];
		if (ctx->debug >= 3)
			debug(ctx, "  bytes in file: %d\n", bytes);
		file->size = bytes;
	}

  	tag = sc_asn1_find_tag(ctx, p, len, 0x82, &taglen);
	if (tag != NULL) 
	{
		const char *type = "unknown";
		const char *structure = "unknown";

		if (taglen == 1 && tag[0] == 0x01) 
		{
			/* transparent EF */
			type = "working EF";
			structure = "transparent";
			file->type = SC_FILE_TYPE_WORKING_EF;
			file->ef_structure = SC_FILE_EF_TRANSPARENT;
		}
		else if (taglen == 1 && tag[0] == 0x11) 
		{
			/* object EF */
			type = "working EF";
			structure = "object";
			file->type = SC_FILE_TYPE_WORKING_EF;
			file->ef_structure = SC_FILE_EF_TRANSPARENT; /* TODO */
		}
		else if (taglen == 3 && tag[1] == 0x21)
		{
			type = "working EF";
			file->record_length = tag[2];
			file->type = SC_FILE_TYPE_WORKING_EF;
			/* linear fixed, cyclic or compute */
			switch ( tag[0] )
			{
				case 0x02:
					structure = "linear fixed";
					file->ef_structure = SC_FILE_EF_LINEAR_FIXED;
					break;
				case 0x07:
					structure = "cyclic";
					file->ef_structure = SC_FILE_EF_CYCLIC;
					break;
				case 0x17:
					structure = "compute";
					file->ef_structure = SC_FILE_EF_UNKNOWN;
					break;
				default:
					structure = "unknown";
					file->ef_structure = SC_FILE_EF_UNKNOWN;
					file->record_length = 0;
					break;
			}
		}

		if (ctx->debug >= 3) 
		{
	 		debug(ctx, "  type: %s\n", type);
			debug(ctx, "  EF structure: %s\n", structure);
		}
	}
	file->magic = SC_FILE_MAGIC;
}


static int starcos_finish(struct sc_card *card)
{
	if (card->drv_data)
		free(card->drv_data);
	return 0;
}


static int starcos_match_card(struct sc_card *card)
{
	int i, match = -1;
  
	for (i = 0; starcos_atrs[i] != NULL; i++) 
	{
		u8 defatr[SC_MAX_ATR_SIZE];
		size_t len = sizeof(defatr);
		const char *atrp = starcos_atrs[i];
      
		if (sc_hex_to_bin(atrp, defatr, &len))
			continue;
		if (len != card->atr_len)
			continue;
		if (memcmp(card->atr, defatr, len) != 0)
			continue;
		match = i + 1;
		break;
    	}
	if (match == -1)
		return 0;
  
	return i;
}


static int starcos_init(struct sc_card *card)
{
	int type, flags;

	starcos_mse_state *state = malloc(sizeof(starcos_mse_state));
	if (!state)
		return SC_ERROR_OUT_OF_MEMORY;
	state->sec_ops = 0;

	card->name = "StarCOS";
	card->drv_data = state;
	card->cla = 0x00;

	/* set the supported algorithm */
	type = starcos_match_card(card);

	if (type == 1 || type == 2)
	{
		/* Starcos SPK 2.3 card */
		flags = SC_ALGORITHM_RSA_PAD_PKCS1 
			| SC_ALGORITHM_RSA_PAD_ISO9796
			| SC_ALGORITHM_RSA_HASH_NONE
			| SC_ALGORITHM_RSA_HASH_SHA1
			| SC_ALGORITHM_RSA_HASH_MD5
			| SC_ALGORITHM_RSA_HASH_RIPEMD160;

		_sc_card_add_rsa_alg(card, 512, flags, 0x10001);
		_sc_card_add_rsa_alg(card, 768, flags, 0x10001);
		_sc_card_add_rsa_alg(card,1024, flags, 0x10001);
	}
	else
		return SC_ERROR_INTERNAL;

	/* we need read_binary&friends with max 128 bytes per read */
	card->max_le = 0x80;

	return 0;
}


static int copy_path(sc_path_t *dest, const sc_path_t *src)
{
	if ( !dest || !src )
		return SC_ERROR_INVALID_ARGUMENTS;

	dest->type  = src->type;
	dest->len   = src->len;
	dest->index = src->index;
	memcpy(dest->value, src->value, src->len);

	return SC_NO_ERROR;
}


static int starcos_select_aid(struct sc_card *card,
			      u8 aid[16], size_t len,
			      struct sc_file **file_out)
{
	sc_apdu_t apdu;
	sc_file_t *file = NULL;
	int r;
	size_t i = 0;

	if (!card )
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
	if (file_out)
		*file_out = NULL;
  
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xA4, 0x04, 0x0C);
	apdu.lc = len;
	apdu.data = (u8*)aid;
	apdu.datalen = len;
	apdu.resplen = 0;
	apdu.le = 0;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");

	/* check return value */
	if (!(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) && apdu.sw1 != 0x61 )
    		SC_FUNC_RETURN(card->ctx, 2, sc_check_sw(card, apdu.sw1, apdu.sw2));
  
	/* update cache */
	card->cache.current_path.type = SC_PATH_TYPE_DF_NAME;
	card->cache.current_path.len = len;
	memcpy(card->cache.current_path.value, aid, len);

	if (file_out)
	{
		file = sc_file_new();
		if (file == NULL)
			SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_OUT_OF_MEMORY);
		file->type = SC_FILE_TYPE_DF;
		file->ef_structure = SC_FILE_EF_UNKNOWN;
		file->path.len = 0;
		file->size = 0;
		/* AID */
		for (i = 0; i < len; i++)  
			file->name[i] = aid[i];
		file->namelen = len;
		file->id = 0x0000;
		file->magic = SC_FILE_MAGIC;
		*file_out = file;
	}
	SC_FUNC_RETURN(card->ctx, 2, SC_SUCCESS);
}

static int starcos_select_fid(struct sc_card *card,
			      u8 id_hi, u8 id_lo,
			      struct sc_file **file_out)
{
	sc_apdu_t apdu;
	u8 data[] = {id_hi, id_lo};
	u8 resp[SC_MAX_APDU_BUFFER_SIZE];
	int bIsDF = 0, r;

	if (!card )
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);

	if (file_out)
		*file_out = NULL;

	/* request FCI to distinguish between EFs and DFs */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0xA4, 0x00, 0x00);
	apdu.resp = (u8*)resp;
	apdu.resplen = SC_MAX_APDU_BUFFER_SIZE;
	apdu.le = 256;
	apdu.lc = 2;
	apdu.data = (u8*)data;
	apdu.datalen = 2;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");

	if (apdu.p2 == 0x00 && apdu.sw1 == 0x62 && apdu.sw2 == 0x84 )
	{
		/* no FCI => we have a DF (see comment in process_fci()) */
		bIsDF = 1;
		apdu.p2 = 0x0C;
		apdu.cse = SC_APDU_CASE_3_SHORT;
		apdu.resplen = 0;
		apdu.le = 0;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU re-transmit failed");
    	}
	else if (apdu.sw1 == 0x61 || (apdu.sw1 == 0x90 && apdu.sw2 == 0x00))
	{
		/* SELECT returned some data (possible FCI) =>
		 * try a READ BINARY to see if a EF is selected */
		sc_apdu_t apdu2;
		u8 resp2[2];
		sc_format_apdu(card, &apdu2, SC_APDU_CASE_2_SHORT, 0xB0, 0, 0);
		apdu2.resp = (u8*)resp2;
		apdu2.resplen = 2;
		apdu2.le = 1;
		apdu2.lc = 0;
		r = sc_transmit_apdu(card, &apdu2);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu2.sw1 == 0x69 && apdu2.sw2 == 0x86)
			/* no current EF is selected => we have a DF */
			bIsDF = 1;
	}

	if (apdu.sw1 != 0x61 && (apdu.sw1 != 0x90 || apdu.sw2 != 0x00))
		SC_FUNC_RETURN(card->ctx, 2, sc_check_sw(card, apdu.sw1, apdu.sw2));

	/* update cache */
	if (bIsDF)
	{
		card->cache.current_path.type = SC_PATH_TYPE_PATH;
		card->cache.current_path.value[0] = 0x3f;
		card->cache.current_path.value[1] = 0x00;
		if (id_hi == 0x3f && id_lo == 0x00)
			card->cache.current_path.len = 2;
		else
		{
			card->cache.current_path.len = 4;
			card->cache.current_path.value[2] = id_hi;
			card->cache.current_path.value[3] = id_lo;
		}
	}

	if (file_out)
	{
		sc_file_t *file = sc_file_new();
		if (!file)
			SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_OUT_OF_MEMORY);
		file->id = (id_hi << 8) + id_lo;
		copy_path(&file->path, &card->cache.current_path);

		if (bIsDF)
		{
			/* we have a DF */
			file->type = SC_FILE_TYPE_DF;
			file->ef_structure = SC_FILE_EF_UNKNOWN;
			file->size = 0;
			file->namelen = 0;
			file->magic = SC_FILE_MAGIC;
			*file_out = file;
		}
		else /* bIsDF == 0 */
		{
			/* ok, assume we have a EF */
			if (apdu.resp[0] != 0x6F)
			{
				/* missing tag */
				free(file);
				SC_FUNC_RETURN(card->ctx, 2,
				       SC_ERROR_UNKNOWN_DATA_RECEIVED);
			}
			/* check length of the FCI data */
			if (apdu.resp[1] <= apdu.resplen-2)
				process_fci(card->ctx,file,apdu.resp+2, apdu.resp[1]);
			*file_out = file;
		}
	}
	else if (file_out)
		*file_out = NULL;

	SC_FUNC_RETURN(card->ctx, 2, SC_SUCCESS);
}

static int starcos_select_file(struct sc_card *card,
			       const struct sc_path *in_path,
			       struct sc_file **file_out)
{
	u8 pathbuf[SC_MAX_PATH_SIZE], *path = pathbuf;
	int    r;
	size_t i, pathlen;

	if ( !card || !in_path )
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);

	if (card->ctx->debug >= 4)
	{
		char buf[128], *p_buf = buf;
		for (i = 0; i < card->cache.current_path.len; i++) 
		{
			sprintf(p_buf, "%02X", card->cache.current_path.value[i]);
			p_buf += 2;
		}
		p_buf[0] = 0x00;
		debug(card->ctx, "current path (%s, %s): %s (len: %u)\n",
			(card->cache.current_path.type==SC_PATH_TYPE_DF_NAME?"aid":"path"),
			(card->cache_valid?"valid":"invalid"),
			buf, card->cache.current_path.len);
	}
  
	memcpy(path, in_path->value, in_path->len);
	pathlen = in_path->len;

	if (in_path->type == SC_PATH_TYPE_FILE_ID)
	{	/* SELECT EF/DF with ID */
		/* Select with 2byte File-ID */
		if (pathlen != 2)
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
		/* check if we are already in the right DF */
		if (card->cache_valid 
		    && card->cache.current_path.type == SC_PATH_TYPE_PATH
		    && card->cache.current_path.len >= 2
		    && card->cache.current_path.value[card->cache.current_path.len-2] == path[0]
		    && card->cache.current_path.value[card->cache.current_path.len-1] == path[1] )
		{
			if (card->ctx->debug >= 4)
				debug(card->ctx, "cache hit\n");
			SC_FUNC_RETURN(card->ctx, 2, SC_SUCCESS);
		}
		else
			return starcos_select_fid(card, path[0], path[1], file_out);
	}
	else if (in_path->type == SC_PATH_TYPE_DF_NAME)
      	{	/* SELECT DF with AID */
		/* Select with 1-16byte Application-ID */
		if (card->cache_valid 
		    && card->cache.current_path.type == SC_PATH_TYPE_DF_NAME
		    && card->cache.current_path.len == pathlen
		    && memcmp(card->cache.current_path.value, pathbuf, pathlen) == 0 )
		{
			if (card->ctx->debug >= 4)
				debug(card->ctx, "cache hit\n");
			SC_FUNC_RETURN(card->ctx, 2, SC_SUCCESS);
		}
		else
			return starcos_select_aid(card, pathbuf, pathlen, file_out);
	}
	else if (in_path->type == SC_PATH_TYPE_PATH)
	{
		u8 n_pathbuf[SC_MAX_PATH_SIZE];
		int bMatch = -1;

		/* Select with path (sequence of File-IDs) */
		/* Starcos (S 2.1 and SPK 2.3) only supports one
		 * level of subdirectories, therefore a path is
		 * at most 3 FID long (the last one being the FID
		 * of a EF) => pathlen must be even and less than 6
		 */
		if (pathlen%2 != 0 || pathlen > 6 || pathlen <= 0)
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
		/* if pathlen == 6 then the first FID must be MF (== 3F00) */
		if (pathlen == 6 && ( path[0] != 0x3f || path[1] != 0x00 ))
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);

		/* unify path (the first FID should be MF) */
		if (path[0] != 0x3f || path[1] != 0x00)
		{
			n_pathbuf[0] = 0x3f;
			n_pathbuf[1] = 0x00;
			for (i=0; i< pathlen; i++)
				n_pathbuf[i+2] = pathbuf[i];
			path = n_pathbuf;
			pathlen += 2; 
		}
	
		/* check current working directory */
		if (card->cache_valid 
		    && card->cache.current_path.type == SC_PATH_TYPE_PATH
		    && card->cache.current_path.len >= 2
		    && card->cache.current_path.len <= pathlen )
		{
			bMatch = 0;
			for (i=0; i < card->cache.current_path.len; i+=2)
				if (card->cache.current_path.value[i] == path[i] 
				    && card->cache.current_path.value[i+1] == path[i+1] )
					bMatch += 2;
		}

		if ( card->cache_valid && bMatch >= 0 )
		{
			if ( pathlen - bMatch == 2 )
				/* we are in the rigth directory */
				return starcos_select_fid(card, path[bMatch], path[bMatch+1], file_out);
			else if ( pathlen - bMatch > 2 )
			{
				/* two more steps to go */
				sc_path_t new_path;
	
				/* first step: change directory */
				r = starcos_select_fid(card, path[bMatch], path[bMatch+1], NULL);
				SC_TEST_RET(card->ctx, r, "SELECT FILE (DF-ID) failed");
		
				new_path.type = SC_PATH_TYPE_PATH;
				new_path.len  = pathlen - bMatch-2;
				memcpy(new_path.value, &(path[bMatch+2]), new_path.len);
				/* final step: select file */
				return starcos_select_file(card, &new_path, file_out);
      			}
			else /* if (bMatch - pathlen == 0) */
			{
				/* done: we are already in the
				 * requested directory */
				if ( card->ctx->debug >= 4 )
					debug(card->ctx, "cache hit\n");
				/* TODO: Should SELECT DF be called again ? 
				 *       (Calling SELECT DF resets the status 
				 *       of the current DF).
				 */
#if 0
				/* SELECT the DF again */
				return starcos_select_fid(card, path[pathlen-2],
						path[pathlen-1], file_out);
#else
				/* copy file info (if necessary) */
				if (file_out)
				{
					sc_file_t *file = sc_file_new();
					if (!file)
						SC_FUNC_RETURN(card->ctx, 0,
							SC_ERROR_OUT_OF_MEMORY);
					file->id = (path[pathlen-2] << 8) +
						   path[pathlen-1];
					copy_path(&file->path,
						&card->cache.current_path);
					file->type = SC_FILE_TYPE_DF;
					file->ef_structure = SC_FILE_EF_UNKNOWN;
					file->size = 0;
					file->namelen = 0;
					file->magic = SC_FILE_MAGIC;
					*file_out = file;
				}
				/* nothing left to do */
				return SC_SUCCESS;
#endif 
			}
		}
		else
		{
			/* no usable cache */
			for ( i=0; i<pathlen-2; i+=2 )
			{
				r = starcos_select_fid(card, path[i], path[i+1], NULL);
				SC_TEST_RET(card->ctx, r, "SELECT FILE (DF-ID) failed");
			}
			return starcos_select_fid(card, path[pathlen-2], path[pathlen-1], file_out);
		}
	}
	else
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
  
	SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INTERNAL);
}


static int starcos_create_file(struct sc_card *card, struct sc_file *file)
{
	int r, i;
	size_t len;
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];
	struct sc_apdu apdu;

	len = SC_MAX_APDU_BUFFER_SIZE;

	if (file->type == SC_FILE_TYPE_WORKING_EF)
	{
		/* create a EF */
		/* FIXME: use variable AC etc. */
		/* set the FID */
		sbuf[0] = (file->id & 0xffff) >> 8;
		sbuf[1] = (file->id & 0x00ff);
		/* set ACs */
		for (i=0; i<9; i++)
			sbuf[2+i] = 0x00;
		/* set SM byte (not supported) */
		sbuf[11] = 0x00;
		/* set SID */
		sbuf[12] = 0x00;
		/* set EF-INFO and EF descriptor */
		switch (file->ef_structure)
		{
		case SC_FILE_EF_LINEAR_FIXED:
			sbuf[13] = 0x82;
			sbuf[14] = file->record_count & 0xff;
			sbuf[15] = file->record_length & 0xff;
			break;
		case SC_FILE_EF_CYCLIC:
			sbuf[13] = 0x84;
			sbuf[14] = file->record_count & 0xff;
			sbuf[15] = file->record_length & 0xff;
			break;
		case SC_FILE_EF_TRANSPARENT:
			sbuf[13] = 0x81;
			sbuf[14] = (file->size & 0xffff) >> 8;
			sbuf[15] = (file->size & 0x00ff);
			break;
#if 0
		case SC_FILE_EF_OBJECT:
		case SC_FILE_EF_COMPUTE:
#endif
		default:
			return SC_ERROR_INVALID_ARGUMENTS;
		}
		sc_format_apdu(card,&apdu,SC_APDU_CASE_3_SHORT,0xE0,0x03,0x00);
		len = 16;
	}
	else if (file->type == SC_FILE_TYPE_DF)
	{
		size_t namelen = file->namelen;
		/* create a DF */

		/* first step: REGISTER DF to allocate the required memory */
		sc_format_apdu(card,&apdu,SC_APDU_CASE_3_SHORT,0x52,
			       (file->size & 0xffff) >> 8, file->size & 0xff);
		sbuf[0] = (file->id & 0xffff) >> 8;
		sbuf[1] = file->id & 0xff;
		if (namelen)
		{
			sbuf[2] = namelen & 0xff;
			memcpy(sbuf+3, file->name, namelen);
		}
		else
		{	/* Starcos seems to need a AID name */
			sbuf[2] = 2;
			sbuf[3] = sbuf[0];
			sbuf[4] = sbuf[1];
			namelen = 2;
		}
		apdu.cla    |= 0x80;
		apdu.lc      = 3 + namelen;
		apdu.datalen = 3 + namelen;
		apdu.data    = sbuf;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (!(apdu.sw1 == 0x90 && apdu.sw2 == 0x00))
			SC_FUNC_RETURN(card->ctx, 4, 
				       sc_check_sw(card, apdu.sw1, apdu.sw2));

		/* second step: create the DF */
		/* FIXME: use variable parameters */
		/* set the ISF space */
		sbuf[19] = 0x00;
		sbuf[20] = 0x80;
		/* set AC CREATE EF */
		sbuf[21] = 0x00;
		/* set AC CREATE KEY */
		sbuf[22] = 0x00;
		/* set SM byte CR */
		sbuf[23] = 0x00;
		/* set SM byte ISF */
		sbuf[24] = 0x00;

		sc_format_apdu(card,&apdu,SC_APDU_CASE_3_SHORT,0xE0,0x01,0x00);
		len = 25;
	}
	
	apdu.cla |= 0x80;  /* this is an proprietary extension */
	apdu.lc = len;
	apdu.datalen = len;
	apdu.data = sbuf;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
}


/* DELETE works only for the MF (<=> clearing the whole filesystem)
 * (and only with test cards) */
static int starcos_delete_file(struct sc_card *card, const struct sc_path *path)
{
	int r;
	u8 sbuf[2];
	struct sc_apdu apdu;

	SC_FUNC_CALLED(card->ctx, 1);
	if (path->type != SC_PATH_TYPE_FILE_ID && path->len != 2)
	{
		error(card->ctx, "File type has to be SC_PATH_TYPE_FILE_ID\n");
		SC_FUNC_RETURN(card->ctx, 1, SC_ERROR_INVALID_ARGUMENTS);
	}
	sbuf[0] = path->value[0];
	sbuf[1] = path->value[1];
	if (sbuf[0] != 0x3f || sbuf[1] != 0x00)
	{
		error(card->ctx, "Only the MF can be deleted\n");
		SC_FUNC_RETURN(card->ctx, 1, SC_ERROR_INVALID_ARGUMENTS);
	}

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE4, 0x00, 0x00);
	apdu.cla |= 0x80;
	apdu.lc   = 2;
	apdu.datalen = 2;
	apdu.data = sbuf;
	
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
}


static int starcos_set_security_env(struct sc_card *card,
				    const struct sc_security_env *env,
				    int se_num)
{
	/* NOTE: starcos_set_security_env() does not call MSE!
	 *       MSE is called immediately before the corresponding 
	 *       crypto operation by the corresponding function.
	 *       starcos_set_security_env() evaluates the sc_security_env
	 *       argument and inserts the information in the
	 *       starcos_mse_state structure.
	 */
	starcos_mse_state *mse;
	u8 *p;

	assert(card != NULL && env != NULL);

	mse = (starcos_mse_state *)card->drv_data;
	p   = mse->buf;

	switch (env->operation)
	{
	case SC_SEC_OPERATION_DECIPHER:
		mse->sec_ops = SC_SEC_OPERATION_DECIPHER;
		mse->p1 = 0x81;
		mse->p2 = 0xB8;
		break;
	case SC_SEC_OPERATION_SIGN:
		mse->sec_ops = SC_SEC_OPERATION_SIGN;
		mse->p1 = 0x41;
		mse->p2 = 0xB6;
		break;
	case SC_SEC_OPERATION_AUTHENTICATE:
		mse->sec_ops = SC_SEC_OPERATION_AUTHENTICATE;
		mse->p1 = 0x41;
		mse->p2 = 0xa4;
		break;
	default:
		return SC_ERROR_INVALID_ARGUMENTS;
	}

	if (env->flags & SC_SEC_ENV_ALG_REF_PRESENT)
	{
		*p++ = 0x80;
		*p++ = 0x01;
		*p++ = env->algorithm_ref & 0xFF;
	}
	else if (env->flags & SC_SEC_ENV_ALG_PRESENT &&
		 env->algorithm == SC_ALGORITHM_RSA  &&
		 env->algorithm_flags & SC_ALGORITHM_RSA_PAD_PKCS1)
	{
		if (env->operation == SC_SEC_OPERATION_DECIPHER)
		{
			/* XXX set a default value for the deciphering 
			 * operations (PKCS#1 BT 2) */
			*p++ = 0x80;
			*p++ = 0x01;
			*p++ = 0x02;	
		}
		else if (env->operation == SC_SEC_OPERATION_SIGN ||
			 env->operation == SC_SEC_OPERATION_AUTHENTICATE)
		{
			/* default value for signing PKCS#1 BT 1 */
			*p++ = 0x80;
			*p++ = 0x01;
			*p++ = 0x12;
		}
	}

	if (env->flags & SC_SEC_ENV_KEY_REF_PRESENT)
	{
		if (env->flags & SC_SEC_ENV_KEY_REF_ASYMMETRIC)
			*p++ = 0x83;
		else
			*p++ = 0x84;
		*p++ = env->key_ref_len;
		memcpy(p, env->key_ref, env->key_ref_len);
		p += env->key_ref_len;
	}

	mse->buf_len = p - mse->buf;

	return SC_SUCCESS;
}


static int starcos_compute_signature(struct sc_card *card,
				     const u8 * data, size_t datalen,
				     u8 * out, size_t outlen)
{
	/* NOTE: data should point to a hash value */
	int r;
	struct sc_apdu apdu;
	u8 rbuf[SC_MAX_APDU_BUFFER_SIZE];
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];
	starcos_mse_state *mse;

	assert(card != NULL && data != NULL && out != NULL);
	if (datalen > 20)
		SC_FUNC_RETURN(card->ctx, 4, SC_ERROR_INVALID_ARGUMENTS);

	mse = (starcos_mse_state *)card->drv_data;

	/* first step: MSE */
	if (mse->sec_ops == 0)
		SC_FUNC_RETURN(card->ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, mse->p1,
		       mse->p2);
	apdu.data    = mse->buf;
	apdu.datalen = mse->buf_len;
	apdu.lc      = mse->buf_len;
	apdu.le      = 0;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
		SC_FUNC_RETURN(card->ctx, 4, sc_check_sw(card, apdu.sw1, apdu.sw2));

	/* the second step depends on the signature method used:
	 * INTERNAL AUTHENTICATE or COMPUTE SIGNATURE */

	if (mse->sec_ops == SC_SEC_OPERATION_SIGN)
	{
		/* second step: set the hash value */
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x2A,
			       0x90, 0x81);

		apdu.resp = rbuf;
		apdu.resplen = sizeof(rbuf);
		apdu.le = 0;
		memcpy(sbuf, data, datalen);
		apdu.data = sbuf;
		apdu.lc = datalen;
		apdu.datalen = datalen;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
			SC_FUNC_RETURN(card->ctx, 4, 
				       sc_check_sw(card, apdu.sw1, apdu.sw2));

		/* third and final step: calculate the signature */
		sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0x2A,
			       0x9E, 0x9A);
		apdu.resp = rbuf;
		apdu.resplen = sizeof(rbuf);
		apdu.le = 256;

		apdu.lc = 0;
		apdu.datalen = 0;
		apdu.sensitive = 1;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 == 0x90 && apdu.sw2 == 0x00)
		{
			int len = apdu.resplen > outlen ? outlen : apdu.resplen;

			memcpy(out, apdu.resp, len);
			SC_FUNC_RETURN(card->ctx, 4, len);
		}
	}
	else if (mse->sec_ops == SC_SEC_OPERATION_AUTHENTICATE)
	{
		/* second and final step: compute the signature */
		sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x88,
			       0x10, 0x00);
		memcpy(sbuf, data, datalen);
		apdu.data = sbuf;
		apdu.lc = datalen;
		apdu.datalen = datalen;
		apdu.resp = rbuf;
		apdu.resplen = sizeof(rbuf);
		apdu.le = 256;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		{
			int len = apdu.resplen > outlen ? outlen : apdu.resplen;

			memcpy(out, apdu.resp, len);
			SC_FUNC_RETURN(card->ctx, 4, len);
		}
	}
	else
		SC_FUNC_RETURN(card->ctx, 4, SC_ERROR_INVALID_ARGUMENTS);

	/* clear the old mse state */
	memset(mse, 0, sizeof(starcos_mse_state));

	SC_FUNC_RETURN(card->ctx, 4, sc_check_sw(card, apdu.sw1, apdu.sw2));
}


static int starcos_decipher(struct sc_card *card,
			    const u8 * crgram, size_t crgram_len,
			    u8 * out, size_t outlen)
{
	int r;
	struct sc_apdu apdu;
	u8 rbuf[SC_MAX_APDU_BUFFER_SIZE];
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];
	starcos_mse_state *mse;

	assert(card != NULL && crgram != NULL && out != NULL);
	SC_FUNC_CALLED(card->ctx, 2);
	if (crgram_len > 255)
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);

	/* MSE */
	mse = (starcos_mse_state *)card->drv_data;

	if (mse->sec_ops == 0)
		SC_FUNC_RETURN(card->ctx, 4, SC_ERROR_INVALID_ARGUMENTS);
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, mse->p1,
		       mse->p2);
	apdu.data    = mse->buf;
	apdu.datalen = mse->buf_len;
	apdu.lc      = mse->buf_len;
	apdu.le      = 0;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
		SC_FUNC_RETURN(card->ctx, 4, sc_check_sw(card, apdu.sw1, apdu.sw2));

	/* INS: 0x2A  PERFORM SECURITY OPERATION
	 * P1:  0x80  Resp: Plain value
	 * P2:  0x86  Cmd: Padding indicator byte followed by cryptogram */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x2A, 0x80, 0x86);
	apdu.resp = rbuf;
	apdu.resplen = sizeof(rbuf); /* FIXME */
	apdu.sensitive = 1;
	
	sbuf[0] = 0; /* padding indicator byte, 0x00 = No further indication */
	memcpy(sbuf + 1, crgram, crgram_len);
	apdu.data = sbuf;
	apdu.lc = crgram_len + 1;
	apdu.datalen = crgram_len + 1;
	apdu.le = 256;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if (apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		int len = apdu.resplen > outlen ? outlen : apdu.resplen;

		memcpy(out, apdu.resp, len);
		SC_FUNC_RETURN(card->ctx, 2, len);
	}

	/* clear the old mse state */
	memset(mse, 0, sizeof(starcos_mse_state));

	SC_FUNC_RETURN(card->ctx, 2, sc_check_sw(card, apdu.sw1, apdu.sw2));
}



static int starcos_check_sw(struct sc_card *card, int sw1, int sw2)
{
	const int err_count = sizeof(starcos_errors)/sizeof(starcos_errors[0]);
	int i;

	if (card->ctx->debug >= 3)
		debug(card->ctx, "sw1 = 0x%02x, sw2 = 0x%02x\n", sw1, sw2);
  
	if (sw1 == 0x90)
		return SC_NO_ERROR;
	if (sw1 == 0x63 && (sw2 & ~0x0f) == 0xc0 )
	{
		error(card->ctx, "Verification failed (remaining tries: %d)\n",
		(sw2 & 0x0f));
		return SC_ERROR_PIN_CODE_INCORRECT;
	}
  
	/* check starcos error messages */
	for (i = 0; i < err_count; i++)
		if (starcos_errors[i].SWs == ((sw1 << 8) | sw2))
		{
			error(card->ctx, "%s\n", starcos_errors[i].errorstr);
			return starcos_errors[i].errorno;
		}
  
	/* iso error */
	return iso_ops->check_sw(card, sw1, sw2);
}

static struct sc_card_driver * sc_get_driver(void)
{
	struct sc_card_driver *iso_drv = sc_get_iso7816_driver();
	if (iso_ops == NULL)
		iso_ops = iso_drv->ops;
  
	starcos_ops = *iso_drv->ops;
	starcos_ops.match_card = starcos_match_card;
	starcos_ops.init   = starcos_init;
	starcos_ops.finish = starcos_finish;
	starcos_ops.select_file = starcos_select_file;
	starcos_ops.check_sw    = starcos_check_sw;
	starcos_ops.create_file = starcos_create_file;
	starcos_ops.delete_file = starcos_delete_file;
	starcos_ops.set_security_env  = starcos_set_security_env;
	starcos_ops.compute_signature = starcos_compute_signature;
	starcos_ops.decipher    = starcos_decipher;
  
	return &starcos_drv;
}

struct sc_card_driver * sc_get_starcos_driver(void)
{
	return sc_get_driver();
}
