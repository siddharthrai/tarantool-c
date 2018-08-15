/* -*- C -*- */
#ifdef _WIN32
#include <tnt_winsup.h>
#else
#include <sys/time.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <odbcinst.h>
#include <sqlext.h>
#ifndef _WIN32
#include <odbcinstext.h>
#endif

#include <tarantool/tarantool.h>
#include <tarantool/tnt_net.h>
#include <tarantool/tnt_fetch.h>

#include "driver.h"


static int realloc_params(int num,int *old_num, tnt_bind_t **params);


SQLRETURN
stmt_prepare(SQLHSTMT    stmth, SQLCHAR     *query, SQLINTEGER  query_len)
{
	odbc_stmt *stmt = (odbc_stmt *) stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;

	LOG_TRACE(stmt, "Prepare(sql='%s')\n", query);

	free_stmt(stmth, SQL_CLOSE);

	if (query_len == SQL_NTS)
		query_len = (SQLINTEGER)strlen((char *)query);
	stmt->tnt_statement = tnt_prepare(stmt->connect->tnt_hndl,(char *)query, query_len);

	if (stmt->tnt_statement) {
		stmt->state = PREPARED;
		return SQL_SUCCESS;
	} else {
		/* Prepare is just copying query string so only memory error could be here. */
		set_stmt_error(stmt,ODBC_MEM_ERROR,"Unable to allocate memory", "Prepare");
		return SQL_ERROR;
	}
}

SQLRETURN
set_out_bind_params(odbc_stmt *stmt, const char *fname)
{
	if (stmt->outbind_params) {
		if (tnt_number_of_cols(stmt->tnt_statement) > stmt->outbind_items &&
		    realloc_params(tnt_number_of_cols(stmt->tnt_statement),
				   &(stmt->outbind_items),&(stmt->outbind_params)) == FAIL) {
			set_stmt_error(stmt,ODBC_MEM_ERROR,"Unable to allocate memory for parameters", fname);
			return SQL_ERROR;
		}
		tnt_bind_result(stmt->tnt_statement,stmt->outbind_params,stmt->outbind_items);
	}
	return SQL_SUCCESS;
}


SQLRETURN
stmt_execute(SQLHSTMT stmth)
{
	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;

	if (stmt->state!=PREPARED) {
		set_stmt_error(stmt, ODBC_24000_ERROR, "Invalid cursor state", "Execute");
		return SQL_ERROR;
	}
	if (!stmt->tnt_statement) {
		set_stmt_error(stmt,ODBC_EMPTY_STATEMENT,"ODBC statement without query/prepare","Execute");
		return SQL_ERROR;
	}
	if (stmt->inbind_params)
		tnt_bind_query(stmt->tnt_statement,stmt->inbind_params,stmt->inbind_items);


	if (tnt_stmt_execute(stmt->tnt_statement)!=OK) {
		size_t sz=0;
		const char* error = tnt_stmt_error(stmt->tnt_statement,&sz);
		if (!error) {
			error = "Unknown error state";
		}
		set_stmt_error_len(stmt,tnt2odbc_error
				   (tnt_stmt_code(stmt->tnt_statement)),
				   error, (int)sz, "Execute");
		return SQL_ERROR;
	}
	/*
	if (set_out_bind_params(stmt, "Execute") != SQL_SUCCESS)
		return SQL_ERROR;
	*/

	stmt->state = EXECUTED;
	LOG_INFO(stmt, "Execute(%s) = OK  %s %" PRIu64 " rows so far\n",
		 (stmt->tnt_statement->qtype == DML)?"DML/DDL":"SELECT",
		 (stmt->tnt_statement->qtype == DML)?"affected":"prefetched",
		 (stmt->tnt_statement->qtype == DML)?tnt_affected_rows(stmt->tnt_statement):
		 stmt->tnt_statement->nrows);
	return SQL_SUCCESS;
}

int
odbc_types_covert(SQLSMALLINT ctype)
{
	switch (ctype) {
	case SQL_C_CHAR:
		return TNTC_CHAR;
	case SQL_C_BINARY:
		return TNTC_BIN;
	case SQL_C_DOUBLE:
		return TNTC_DOUBLE;
	case SQL_C_FLOAT:
		return TNTC_FLOAT;
	case SQL_C_SBIGINT:
		return TNTC_SBIGINT;
	case SQL_C_UBIGINT:
		return TNTC_UBIGINT;
	case SQL_C_SSHORT:
		return TNTC_SSHORT;
	case SQL_C_USHORT:
		return TNTC_USHORT;
	case SQL_C_SHORT:
		return TNTC_SHORT;
	case SQL_C_LONG:
		return TNTC_LONG;
	case SQL_C_SLONG:
		return TNTC_SLONG;
	case SQL_C_ULONG:
		return TNTC_ULONG;

	default:
		return -1;
	}
}


static int
realloc_params(int num,int *old_num, tnt_bind_t **params)
{
	if (num>*old_num || *params == NULL) {
		tnt_bind_t *npar = (tnt_bind_t *)malloc(sizeof(tnt_bind_t)*num);
		if (!npar) {
			return FAIL;
		}
		memset(npar, 0, sizeof(tnt_bind_t)*num);
		for(int i=0;i<*old_num;++i) {
			npar[i] = (*params)[i];
		}
		free(*params);
		*params = npar;
		*old_num = num;
	}
	return OK;
}

SQLRETURN
stmt_in_bind(SQLHSTMT stmth, SQLUSMALLINT parnum, SQLSMALLINT ptype,
	     SQLSMALLINT ctype, SQLSMALLINT sqltype,
	     SQLULEN col_len, SQLSMALLINT scale, SQLPOINTER buf,
	     SQLLEN buf_len, SQLLEN *len_ind)
{
	(void) ptype;
	(void) sqltype;
	(void) col_len;
	(void) scale;

	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;

	if (parnum < 1 ) {
		set_stmt_error(stmt,ODBC_07009_ERROR,
			       "ODBC bind parameter invalid index number",
			       "SQLBindParameter");
		return SQL_ERROR;
	}

	int in_type = odbc_types_covert(ctype);
	if (in_type == -1) {
		set_stmt_error(stmt,ODBC_HY003_ERROR,
			       "Invalid application buffer type",
			       "SQLBindParameter");
		return SQL_ERROR;
	}

	if (realloc_params(parnum,&(stmt->inbind_items),&(stmt->inbind_params))==FAIL) {
		set_stmt_error(stmt,ODBC_MEM_ERROR,
			       "Unable to allocate memory",
			       "SQLBindParameter");
		return SQL_ERROR;
	}
	--parnum;

	if (!len_ind || *len_ind != SQL_NULL_DATA) {
		stmt->inbind_params[parnum].type = in_type;
		stmt->inbind_params[parnum].buffer = (void *)buf;
		if (stmt->inbind_params[parnum].type == MP_STR && buf_len == SQL_NTS)
			stmt->inbind_params[parnum].in_len = strlen ((char *)stmt->inbind_params[parnum].buffer);
		else
			stmt->inbind_params[parnum].in_len = buf_len;
	} else {
		stmt->inbind_params[parnum].type = MP_NIL;
	}
	return SQL_SUCCESS;
}

SQLRETURN
stmt_out_bind(SQLHSTMT stmth, SQLUSMALLINT colnum, SQLSMALLINT ctype, SQLPOINTER val, SQLLEN in_len, SQLLEN *out_len)
{
	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;

	if (colnum < 1 ) {
		set_stmt_error(stmt,ODBC_HYC00_ERROR,
			       "Optional feature not implemented",
			       "SQLBindCol");
		return SQL_ERROR;
	}

	if (in_len<0) {
		set_stmt_error(stmt, ODBC_HY090_ERROR,
			       "Invalid string or buffer length",
			       "SQLBindCol");
		return SQL_ERROR;
	}

	int num_of_cols = colnum;
	if (stmt->state == EXECUTED) {
		num_of_cols = tnt_number_of_cols(stmt->tnt_statement);
		if (num_of_cols < colnum) {
			set_stmt_error(stmt,ODBC_07009_ERROR,
				       "Invalid descriptor index",
				       "SQLBindCol");
			return SQL_ERROR;
		}
	}

	if (realloc_params(num_of_cols, &(stmt->outbind_items), &(stmt->outbind_params))==FAIL) {
		set_stmt_error(stmt,ODBC_MEM_ERROR,
			       "Unable to allocate memory",
			       "SQLBindCol");
		return SQL_ERROR;
	}

	int in_type = odbc_types_covert(ctype);
	if (in_type == -1) {
		if (ctype == SQL_C_DEFAULT) {
			/* Here we don't know the exact types of result set.
			*  In order to work properly odbc needs to know
			*  result set after prepare phase.
			*/
			in_type = TNTC_CHAR;
		}
		else {
			set_stmt_error(stmt, ODBC_HY003_ERROR,
				"Invalid application buffer type",
				"SQLBindCol");
			return SQL_ERROR;
		}
	}

	--colnum;
	stmt->outbind_params[colnum].type = in_type;
	stmt->outbind_params[colnum].buffer = (void *)val;
	stmt->outbind_params[colnum].in_len = in_len;
	stmt->outbind_params[colnum].out_len = out_len;

	tnt_bind_result(stmt->tnt_statement,stmt->outbind_params,
			stmt->outbind_items);
	return SQL_SUCCESS;
}

SQLRETURN
stmt_fetch(SQLHSTMT stmth)
{
	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;

	/* drop last col SQLGetData offset */
	stmt->last_col = 0;
	stmt->last_col_sofar = 0;

	if (!stmt->tnt_statement || stmt->state!=EXECUTED) {
		set_stmt_error(stmt,ODBC_24000_ERROR,"Invalid cursor state", "SQLFetch");
		return SQL_ERROR;
	}

	if (set_out_bind_params(stmt, "Fetch") != SQL_SUCCESS)
		return SQL_ERROR;

	int retcode = tnt_fetch(stmt->tnt_statement);


	if (retcode==OK) {
		LOG_INFO(stmt, "SQLFetch(OK) %d columns\n",
			 tnt_number_of_cols(stmt->tnt_statement));
		return SQL_SUCCESS;
	} else if (retcode==NODATA) {
		LOG_INFO(stmt, "SQLFetch(NO_DATA) = %s\n", "END_OF_DATA");
		return SQL_NO_DATA;
	} else {
		LOG_INFO(stmt, "SQLFetch(%s)\n", "FAIL");
		return SQL_ERROR;
	}
}



SQLRETURN
stmt_fetch_scroll(SQLHSTMT stmth, SQLSMALLINT orientation, SQLLEN offset)
{
	(void) offset;
	if (orientation != SQL_FETCH_NEXT) {
		set_stmt_error((odbc_stmt *)stmth, ODBC_HY106_ERROR,
			       "Unsupported fetch orientation",
			       "SQLFetchScroll");
		return SQL_ERROR;
	}
	return stmt_fetch(stmth);
}


void
recalculate_types(tnt_bind_t *p)
{

	switch (p->type) {
	case MP_INT:
	case MP_UINT:
		if ((size_t)p->in_len < sizeof(int64_t)) {
			switch (p->in_len) {
				case sizeof(int32_t):
					p->type = (p->type == MP_UINT) ? TNTC_UINT : TNTC_INT;
					break;
				case sizeof(int16_t):
					p->type = (p->type == MP_UINT) ? TNTC_USHORT : TNTC_SHORT;
					break;
				case sizeof(int8_t):
					p->type = (p->type == MP_UINT) ? TNTC_UTINY : TNTC_TINY;
					break;
			}
		}
		break;
	case MP_DOUBLE:
	case MP_FLOAT:
		if (p->in_len == sizeof(double))
			p->type = MP_DOUBLE;
		if (p->in_len == sizeof(float))
			p->type = MP_FLOAT;
		else if ((size_t)p->in_len < sizeof(float)) {
			p->type = MP_NIL;
			p->in_len = 0;
		}
	break;
	}
}


SQLRETURN
get_data(SQLHSTMT stmth, SQLUSMALLINT num, SQLSMALLINT type,
	SQLPOINTER val_ptr, SQLLEN in_len, SQLLEN *out_len)
{
	odbc_stmt *stmt = (odbc_stmt *)stmth;

	if (!stmt)
		return SQL_INVALID_HANDLE;
	if (!stmt->tnt_statement || stmt->state != EXECUTED) {
		set_stmt_error(stmt, ODBC_HY010_ERROR,
			"Function sequence error", "SQLGetData");
		return SQL_ERROR;
	}
	/* Don't do bookmarks for now */

	if (num == 0 || num >= stmt->tnt_statement->ncols + 1) {
		set_stmt_error(stmt, ODBC_07009_ERROR,
			"Invalid descriptor index", "SQLGetData");
		return SQL_ERROR;
	}

	--num;

	if (stmt->tnt_statement->nrows < 0) {
		set_stmt_error(stmt, ODBC_07009_ERROR,
			"No data or row in current row", "SQLGetData");
		return SQL_ERROR;
	}
	if (tnt_col_is_null(stmt->tnt_statement, num)) {
		if (out_len) {
			*out_len = SQL_NULL_DATA;
			return SQL_SUCCESS;
		}
		else {
			set_stmt_error(stmt, ODBC_22002_ERROR,
				"Indicator variable required but "
				"not supplied", "SQLGetData");
			return SQL_ERROR;
		}
	}
	if (in_len < 0) {
		set_stmt_error(stmt, ODBC_HY090_ERROR,
			"Invalid string or buffer length", "SQLGetData");
		return SQL_ERROR;
	}

	int in_type = odbc_types_covert(type);
	if (in_type == FAIL) {
		if (type == SQL_C_DEFAULT) {
			/* Tarantool has a 1:1 mapping from DB types to C language. */
			in_type = tnt_col_type(stmt->tnt_statement, num);
		} else {
			set_stmt_error(stmt, ODBC_HY090_ERROR,
				"Invalid string or buffer length",
				"SQLGetData");
			return SQL_ERROR;
		}
	}


	tnt_bind_t par;
	memset(&par, 0 , sizeof(tnt_bind_t));
	par.type = in_type;
	par.buffer = val_ptr;
	par.in_len = in_len;
	par.out_len = out_len;
	par.is_null = NULL;

	/* this function corrects types according to size.
	 * This is happened due to SQL_C_DEFAULT ODBC type
	 * and due to tnt sql layer do not checks sizes for
	 * integral types.
	 */
	recalculate_types(&par);

	int error = 0;
	par.error = &error;

	if (stmt->last_col != num || !out_len) {
		/* Flush chunked offset */
		stmt->last_col_sofar = 0;
		stmt->last_col = num;
	}

	if (stmt->last_col_sofar && (stmt->last_col_sofar >=
				     tnt_col_len(stmt->tnt_statement,num))) {
		return SQL_NO_DATA;
	}

	store_conv_bind_var(stmt->tnt_statement, num , &par,
			    stmt->last_col_sofar);

	/* If application don't provide the out_len or it's not
	 *  a string or a binary data
	 * chuncked get_data is not provided.
	 */
	if (!out_len || (tnt_col_type(stmt->tnt_statement,num)!=MP_STR &&
			 tnt_col_type(stmt->tnt_statement,num)!=MP_BIN)) {
		return SQL_SUCCESS;
	}

	stmt->last_col_sofar += (int)*out_len;
	if (stmt->last_col_sofar >= tnt_col_len(stmt->tnt_statement,num)) {
		return SQL_SUCCESS;
	} else {
		set_stmt_error(stmt,ODBC_01004_ERROR,
			       "String data, right truncated", "SQLGetData");
		return SQL_SUCCESS_WITH_INFO;
	}
}

int
tnt2odbc(int t)
{
	switch (t) {
	case MP_INT:
	case MP_UINT:
		return SQL_BIGINT;
	case MP_STR:
		return SQL_VARCHAR; /* Or SQL_VARCHAR? */
	case MP_FLOAT:
		return SQL_REAL;
	case MP_DOUBLE:
		return SQL_DOUBLE;
	case MP_BIN:
		return SQL_BINARY;
	default:
		return SQL_VARCHAR; /* Shouldn't be */
	}
}

const char *
sqltypestr(int t)
{
	switch (t) {
	case MP_INT:
		return "BIGINT";
	case MP_UINT:
		/* There aren't exactly such thing as a usigned type. */
		return "BIGINT";
	case MP_STR:
		return "VARCHAR"; /* Or SQL_VARCHAR? */
	case MP_FLOAT:
		return "REAL";
	case MP_DOUBLE:
		return "DOUBLE PRECISION";
	case MP_BIN:
		return "BINARY";
	default:
		return "VARCHAR";
	}
}


SQLRETURN
column_info(SQLHSTMT stmth, SQLUSMALLINT ncol, SQLCHAR *colname,
	    SQLSMALLINT maxname, SQLSMALLINT *name_len,
	    SQLSMALLINT *type, SQLULEN *colsz, SQLSMALLINT *scale,
	    SQLSMALLINT *isnull)
{
	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;
	if (!stmt->tnt_statement || !stmt->tnt_statement->reply) {
		set_stmt_error(stmt,ODBC_HY010_ERROR,
			       "Function sequence error", "SQLDescribeCol");
		return SQL_ERROR;
	}
	if (!stmt->tnt_statement->field_names) {
		set_stmt_error(stmt,ODBC_07005_ERROR,
			       "Prepared statement not a cursor-specification",
			       "SQLDescribeCol");
		return SQL_ERROR;
	}

	if (ncol == 0 || ncol >= tnt_number_of_cols(stmt->tnt_statement) + 1) {
		set_stmt_error(stmt,ODBC_07009_ERROR,
			       "Invalid descriptor index", "SQLDescribeCol");
		return SQL_ERROR;
	}

	ncol--;

	if (isnull)
		*isnull = SQL_NULLABLE_UNKNOWN;
	if (scale)
		*scale = 0;
	if (colsz)
		*colsz = 0;
	int status = SQL_SUCCESS;
	if (colname) {
		strncpy((char*)colname,tnt_col_name(stmt->tnt_statement,ncol),
			maxname);
		if (maxname <= (int) strlen(tnt_col_name(stmt->tnt_statement,ncol))) {
			status = SQL_SUCCESS_WITH_INFO;
			set_stmt_error(stmt,ODBC_01004_ERROR,
				       "String data, right truncated",
				       "SQLDescribeCol");
		}
	}
	if (name_len) {
		*name_len = (SQLSMALLINT)strlen(tnt_col_name(stmt->tnt_statement,ncol));
	}
	if (type) {
		*type = tnt2odbc(tnt_col_type(stmt->tnt_statement,ncol));
	}
	return status;
}

SQLRETURN
num_cols(SQLHSTMT stmth, SQLSMALLINT *ncols)
{
	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;
	if (!ncols) {
		set_stmt_error(stmt, ODBC_HY009_ERROR,
			       "Invalid use of null pointer",
			       "SQLNumResultCols");
		return SQL_ERROR;
	}
	if (!stmt->tnt_statement || stmt->state!=EXECUTED) {
		set_stmt_error(stmt, ODBC_HY010_ERROR,
			       "Function sequence error", "SQLNumResultCols");
		return SQL_ERROR;
	}

	*ncols = tnt_number_of_cols(stmt->tnt_statement);

	LOG_INFO(stmt, "SQLNumResultCols(OK) %d columns\n", (int) *ncols);
	return SQL_SUCCESS;
}

SQLRETURN
affected_rows(SQLHSTMT stmth,SQLLEN *cnt)
{
	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;
	if (!cnt) {
		set_stmt_error(stmt,ODBC_HY009_ERROR,
			       "Invalid use of null pointer", "SQLRowCount");
		return SQL_ERROR;
	}
	if (stmt->tnt_statement->qtype == DML) {
		*cnt = tnt_affected_rows(stmt->tnt_statement);
	} else {
		*cnt = -1;
	}
	return SQL_SUCCESS;
}

void
len_strncpy(SQLPOINTER char_p, const char *d, SQLSMALLINT buflen,
	    SQLSMALLINT *out_len)
{
	if (char_p) {
		strncpy((char*)char_p, d, buflen );
		if (out_len)
			*out_len = (SQLSMALLINT)strlen((char *)char_p);
	}
}

SQLRETURN
col_attribute(SQLHSTMT stmth, SQLUSMALLINT ncol, SQLUSMALLINT id,
	      SQLPOINTER char_p, SQLSMALLINT buflen, SQLSMALLINT *out_len,
	      SQLLEN *num_p)
{
	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;

	LOG_INFO (stmt, "SQLColAttribute(Attribute=%hu, ColNumber=%hu)\n", id, ncol);



	if (!stmt->tnt_statement) {
		set_stmt_error(stmt, ODBC_HY010_ERROR,
			       "Function sequence error", "SQLColAttribute");
		return SQL_ERROR;
	}
	if (ncol == 0 || ncol >= tnt_number_of_cols(stmt->tnt_statement) + 1) {
		set_stmt_error(stmt, ODBC_07009_ERROR,
			       "Invalid descriptor index", "SQLColAttribute");
		return SQL_ERROR;
	}

	--ncol;
	int val = 0;

	switch (id) {
	case SQL_DESC_AUTO_UNIQUE_VALUE:
		val = -1;
		break;
	case SQL_DESC_BASE_COLUMN_NAME:
		len_strncpy( char_p, "" , buflen, out_len);
		break;
	case SQL_DESC_BASE_TABLE_NAME:
		len_strncpy( char_p, "" , buflen, out_len);
		break;
	case SQL_DESC_CASE_SENSITIVE:
		val  = -1;
		break;
	case SQL_DESC_CATALOG_NAME:
		len_strncpy( char_p, "" , buflen, out_len);
		break;
	case SQL_DESC_CONCISE_TYPE:
		val = tnt2odbc(tnt_col_type(stmt->tnt_statement,ncol));;
		break;
	case SQL_DESC_COUNT:
		val = tnt_number_of_cols(stmt->tnt_statement);
		break;
	case SQL_DESC_DISPLAY_SIZE:
		val = -1;
		break;
	case SQL_DESC_FIXED_PREC_SCALE:
		val = -1;
		break;
	case SQL_DESC_LABEL:
		len_strncpy( char_p, "" , buflen, out_len);
		break;
	case SQL_DESC_LENGTH:
		val = (int)tnt_col_len(stmt->tnt_statement,ncol);
		break;
	case SQL_DESC_LITERAL_PREFIX:
		len_strncpy( char_p, "" , buflen, out_len);
		break;
	case SQL_DESC_LITERAL_SUFFIX:
		len_strncpy( char_p, "" , buflen, out_len);
		break;
	case SQL_DESC_LOCAL_TYPE_NAME:
		len_strncpy( char_p, "" , buflen, out_len);
		break;
	case SQL_DESC_NAME:
		len_strncpy( char_p, tnt_col_name(stmt->tnt_statement,ncol),
			     buflen, out_len);
		break;
	case SQL_DESC_NULLABLE:
		val = SQL_NULLABLE_UNKNOWN;
		break;
	case SQL_DESC_NUM_PREC_RADIX:
		val = -1;
		break;
	case SQL_DESC_OCTET_LENGTH:
		val = -1;
		break;
	case SQL_DESC_PRECISION:
		val = -1;
		break;
	case SQL_DESC_SCALE:
		val = -1;
		break;
	case SQL_DESC_SCHEMA_NAME:
		len_strncpy( char_p, "" , buflen, out_len);
		break;
	case SQL_DESC_SEARCHABLE:
		val = -1;
		break;
	case SQL_DESC_TABLE_NAME:
		len_strncpy( char_p, "" , buflen, out_len);
		break;
	case SQL_DESC_TYPE:
		val = tnt2odbc(tnt_col_type(stmt->tnt_statement,ncol));
		break;
	case SQL_DESC_TYPE_NAME:
		len_strncpy( char_p,
			     sqltypestr(tnt2odbc(tnt_col_type(
			     stmt->tnt_statement,ncol))) , buflen, out_len);
		break;
	case SQL_DESC_UNNAMED:
		val = -1;
		break;
	case SQL_DESC_UNSIGNED:
		val = tnt_col_type(stmt->tnt_statement,ncol)==MP_INT?SQL_FALSE:
		SQL_TRUE;
		break;
	case SQL_DESC_UPDATABLE:
		val = -1;
		break;
	default:
		break;
	}
	if (num_p)
		*num_p = val;
	return SQL_SUCCESS;
}

SQLRETURN
num_params(SQLHSTMT stmth, SQLSMALLINT *cnt)
{
	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;
	if (cnt) {
		if (stmt->tnt_statement && stmt->tnt_statement->query) {
			*cnt = get_query_num(stmt->tnt_statement->query,
					     stmt->tnt_statement->query_len);
		} else {
			if (!stmt->tnt_statement) {
				set_stmt_error(stmt,ODBC_HY010_ERROR,
					       "Function sequence error",
					       "SQLNumParams");
				return SQL_ERROR;
			}
		}
	}
	return SQL_SUCCESS;
}

SQLRETURN
param_info(SQLHSTMT stmth, SQLUSMALLINT pnum, SQLSMALLINT *type_ptr,
	   SQLULEN *out_len, SQLSMALLINT *out_dnum,
		 SQLSMALLINT *is_null)
{
	(void) pnum;
	(void) type_ptr;
	(void) out_len;
	(void) out_dnum;
	(void) is_null;

	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;
	/* Driver needs parsed statement in order to implement this */
	set_stmt_error(stmt, ODBC_IM001_ERROR,
		       "Driver does not support this function",
		       "SQLDescribeParam");
	return SQL_ERROR;
}



SQLRETURN
stmt_set_attr(SQLHSTMT stmth, SQLINTEGER att, SQLPOINTER val,
	SQLINTEGER vallen)
{
	(void) val;
	(void) vallen;
	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;

	LOG_INFO(stmt, "SQLSetStmtAttr(att=%d)\n", att);

	switch (att) {
	case SQL_ATTR_NOSCAN:
		/* TODO */
		return SQL_SUCCESS;
	case SQL_ATTR_METADATA_ID:
		/* TODO */
		return SQL_SUCCESS;
	case SQL_ATTR_APP_ROW_DESC:
	case SQL_ATTR_APP_PARAM_DESC:
	case SQL_ATTR_CURSOR_SCROLLABLE:
	case SQL_ATTR_CURSOR_SENSITIVITY:
	case SQL_ATTR_ASYNC_ENABLE:
	case SQL_ATTR_CONCURRENCY:
	case SQL_ATTR_CURSOR_TYPE:
	case SQL_ATTR_ENABLE_AUTO_IPD:
	case SQL_ATTR_FETCH_BOOKMARK_PTR:
	case SQL_ATTR_KEYSET_SIZE:
	case SQL_ATTR_MAX_LENGTH:
	case SQL_ATTR_MAX_ROWS:
	case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
	case SQL_ATTR_PARAM_BIND_TYPE:
	case SQL_ATTR_PARAM_OPERATION_PTR:
	case SQL_ATTR_PARAM_STATUS_PTR:
	case SQL_ATTR_PARAMS_PROCESSED_PTR:
	case SQL_ATTR_PARAMSET_SIZE:
	case SQL_ATTR_QUERY_TIMEOUT:
	case SQL_ATTR_RETRIEVE_DATA:
	case SQL_ATTR_ROW_NUMBER:
	case SQL_ATTR_ROW_OPERATION_PTR:
	case SQL_ATTR_ROW_STATUS_PTR:
	case SQL_ATTR_ROWS_FETCHED_PTR:
	case SQL_ATTR_ROW_ARRAY_SIZE:
	case SQL_ATTR_SIMULATE_CURSOR:
	case SQL_ATTR_USE_BOOKMARKS:
		return SQL_SUCCESS;

	case SQL_ATTR_IMP_ROW_DESC:
	case SQL_ATTR_IMP_PARAM_DESC:
		return SQL_ERROR;

	case SQL_ATTR_ROW_BIND_OFFSET_PTR:
	case SQL_ATTR_ROW_BIND_TYPE:
	default:
		set_stmt_error(stmt, ODBC_IM001_ERROR,
			"Driver does not support the statment attribute",
			"SQLSetStmtAttr");
		return SQL_ERROR;
	}
}



SQLRETURN
stmt_get_attr(SQLHSTMT stmth, SQLINTEGER att, SQLPOINTER ptr,
	SQLINTEGER buflen, SQLINTEGER *olen)
{
	(void) buflen;
	odbc_stmt *stmt = (odbc_stmt *)stmth;
	if (!stmt)
		return SQL_INVALID_HANDLE;
	LOG_INFO(stmt, "SQLGetStmtAttr(att=%d)\n", att);

	*(void**)ptr = 0;
	switch (att) {
	case SQL_ATTR_APP_ROW_DESC:             /* 10010 */
		*(void**)ptr = (SQLPOINTER)stmt->ard;
		break;
	case SQL_ATTR_APP_PARAM_DESC:   /* 10011 */
		*(void**)ptr = (SQLPOINTER)stmt->apd;
		break;
	case SQL_ATTR_IMP_ROW_DESC:             /* 10012 */
		*(void**)ptr = (SQLPOINTER)stmt->ird;
		break;
	case SQL_ATTR_IMP_PARAM_DESC:   /* 10013 */
		*(void**)ptr = (SQLPOINTER)stmt->ipd;
		break;
	}
	if (*(void**)ptr) {
		if (olen)
			*olen = sizeof(SQLPOINTER);
		return SQL_SUCCESS;
	}

#define SET_LEN(t) do {if (olen) *olen = sizeof(t);} while(0)

	switch (att) {
	case SQL_ATTR_CURSOR_SCROLLABLE:
		*(SQLULEN*)ptr = (SQLULEN) SQL_NONSCROLLABLE;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_CURSOR_SENSITIVITY:
		*(SQLULEN*)ptr = (SQLULEN)SQL_INSENSITIVE;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_ASYNC_ENABLE:
		*(SQLULEN*)ptr = (SQLULEN)SQL_ASYNC_ENABLE_OFF;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_CONCURRENCY:
		*(SQLULEN*)ptr = (SQLULEN)SQL_CONCUR_READ_ONLY;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_CURSOR_TYPE:
		*(SQLULEN*)ptr = (SQLULEN)SQL_CURSOR_FORWARD_ONLY;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_ENABLE_AUTO_IPD:
		*(SQLULEN*)ptr = (SQLULEN)SQL_FALSE;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_MAX_LENGTH:
		*(SQLULEN*)ptr = (SQLULEN)0;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_MAX_ROWS:
		*(SQLULEN*)ptr = (SQLULEN)0;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_METADATA_ID:
		*(SQLULEN*)ptr = (SQLULEN)SQL_FALSE;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_NOSCAN:
		*(SQLULEN*)ptr = (SQLULEN)SQL_NOSCAN_ON;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_QUERY_TIMEOUT:
		*(SQLULEN*)ptr = (SQLULEN)0;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_RETRIEVE_DATA:
		*(SQLULEN*)ptr = (SQLULEN)SQL_RD_ON;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_ROW_NUMBER:
		/* XXX: It's a place for row number in a result set.*/
		*(SQLULEN*)ptr = (SQLULEN)0;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_USE_BOOKMARKS:
		*(SQLULEN*)ptr = (SQLULEN)SQL_UB_OFF;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_ROW_BIND_TYPE:
		*(SQLULEN*)ptr = (SQLULEN)SQL_BIND_TYPE_DEFAULT;
		SET_LEN(SQLULEN);
		break;
	case SQL_ATTR_FETCH_BOOKMARK_PTR:
	case SQL_ATTR_KEYSET_SIZE:
	case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
	case SQL_ATTR_PARAM_BIND_TYPE:
	case SQL_ATTR_PARAM_OPERATION_PTR:
	case SQL_ATTR_PARAM_STATUS_PTR:
	case SQL_ATTR_PARAMS_PROCESSED_PTR:
	case SQL_ATTR_PARAMSET_SIZE:
	case SQL_ATTR_ROW_BIND_OFFSET_PTR:
	case SQL_ATTR_ROW_OPERATION_PTR:
	case SQL_ATTR_ROW_STATUS_PTR:
	case SQL_ATTR_ROWS_FETCHED_PTR:
	case SQL_ATTR_ROW_ARRAY_SIZE:
	case SQL_ATTR_SIMULATE_CURSOR:
	default:
		set_stmt_error(stmt, ODBC_IM001_ERROR,
			"Driver does not support the statment attribute",
			"SQLGetStmtAttr");
		return SQL_ERROR;
	}
	return SQL_SUCCESS;
}
