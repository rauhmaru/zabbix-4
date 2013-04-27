/*
** Zabbix
** Copyright (C) 2000-2013 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "lld.h"
#include "db.h"
#include "log.h"
#include "zbxalgo.h"
#include "zbxserver.h"

typedef struct
{
	zbx_uint64_t	hostmacroid;
	char		*macro;
	char		*value;
}
zbx_lld_hostmacro_t;

static void	DBlld_hostmacro_free(zbx_lld_hostmacro_t *hostmacro)
{
	zbx_free(hostmacro->macro);
	zbx_free(hostmacro->value);
	zbx_free(hostmacro);
}

static void	DBlld_hostmacros_free(zbx_vector_ptr_t *hostmacros)
{
	while (0 != hostmacros->values_num)
		DBlld_hostmacro_free((zbx_lld_hostmacro_t *)hostmacros->values[--hostmacros->values_num]);
}

typedef struct
{
	zbx_uint64_t		hostid;
	zbx_vector_uint64_t	new_groupids;		/* host groups which should be added */
	zbx_vector_uint64_t	lnk_templateids;	/* templates which should be linked */
	zbx_vector_uint64_t	del_templateids;	/* templates which should be unlinked */
	zbx_vector_ptr_t	new_hostmacros;		/* host macros whic should be added */
	char			*host_proto;
	char			*host;
	char			*host_orig;
	char			*name;
	char			*name_orig;
	int			lastcheck;
	int			ts_delete;
#define ZBX_FLAG_LLD_HOST_DISCOVERED		0x01	/* hosts which should be updated or added */
#define ZBX_FLAG_LLD_HOST_UPDATE_HOST		0x02	/* hosts.host and host_discovery.host fields should be updated  */
#define ZBX_FLAG_LLD_HOST_UPDATE_NAME		0x04	/* hosts.name field should be updated */
#define ZBX_FLAG_LLD_HOST_UPDATE_PROXY		0x08	/* hosts.proxy_hostid field should be updated */
#define ZBX_FLAG_LLD_HOST_UPDATE_IPMI_AUTH	0x10	/* hosts.ipmi_authtype field should be updated */
#define ZBX_FLAG_LLD_HOST_UPDATE_IPMI_PRIV	0x20	/* hosts.ipmi_privilege field should be updated */
#define ZBX_FLAG_LLD_HOST_UPDATE_IPMI_USER	0x40	/* hosts.ipmi_username field should be updated */
#define ZBX_FLAG_LLD_HOST_UPDATE_IPMI_PASS	0x80	/* hosts.ipmi_password field should be updated */
#define ZBX_FLAG_LLD_HOST_UPDATE								\
		(ZBX_FLAG_LLD_HOST_UPDATE_HOST | ZBX_FLAG_LLD_HOST_UPDATE_NAME |		\
		ZBX_FLAG_LLD_HOST_UPDATE_PROXY | ZBX_FLAG_LLD_HOST_UPDATE_IPMI_AUTH |		\
		ZBX_FLAG_LLD_HOST_UPDATE_IPMI_PRIV | ZBX_FLAG_LLD_HOST_UPDATE_IPMI_USER |	\
		ZBX_FLAG_LLD_HOST_UPDATE_IPMI_PASS)
	unsigned char		flags;
	char			inventory_mode;
}
zbx_lld_host_t;

static void	DBlld_hosts_free(zbx_vector_ptr_t *hosts)
{
	zbx_lld_host_t	*host;

	while (0 != hosts->values_num)
	{
		host = (zbx_lld_host_t *)hosts->values[--hosts->values_num];

		zbx_vector_uint64_destroy(&host->new_groupids);
		zbx_vector_uint64_destroy(&host->lnk_templateids);
		zbx_vector_uint64_destroy(&host->del_templateids);
		DBlld_hostmacros_free(&host->new_hostmacros);
		zbx_vector_ptr_destroy(&host->new_hostmacros);
		zbx_free(host->host_proto);
		zbx_free(host->host);
		zbx_free(host->host_orig);
		zbx_free(host->name);
		zbx_free(host->name_orig);
		zbx_free(host);
	}
}

typedef struct
{
	char		*ip_esc;
	char		*dns_esc;
	char		*port_esc;
	unsigned char	main;
	unsigned char	type;
	unsigned char	useip;
}
zbx_lld_interface_t;

static void	DBlld_interfaces_free(zbx_vector_ptr_t *interfaces)
{
	zbx_lld_interface_t	*interface;

	while (0 != interfaces->values_num)
	{
		interface = (zbx_lld_interface_t *)interfaces->values[--interfaces->values_num];

		zbx_free(interface->port_esc);
		zbx_free(interface->dns_esc);
		zbx_free(interface->ip_esc);
		zbx_free(interface);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_hosts_get                                                  *
 *                                                                            *
 * Purpose: retrieves existing hosts for the specified host prototype         *
 *                                                                            *
 * Parameters: parent_hostid - [IN] host prototype identificator              *
 *             hosts         - [OUT] list of hosts                            *
 *                                                                            *
 ******************************************************************************/
static void	DBlld_hosts_get(zbx_uint64_t parent_hostid, zbx_vector_ptr_t *hosts, zbx_uint64_t proxy_hostid,
		char ipmi_authtype, unsigned char ipmi_privilege, const char *ipmi_username, const char *ipmi_password,
		char inventory_mode)
{
	const char	*__function_name = "DBlld_hosts_get";

	DB_RESULT	result;
	DB_ROW		row;
	zbx_lld_host_t	*host;
	zbx_uint64_t	db_proxy_hostid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	result = DBselect(
			"select hd.hostid,hd.host,hd.lastcheck,hd.ts_delete,h.host,h.name,h.proxy_hostid,"
				"h.ipmi_authtype,h.ipmi_privilege,h.ipmi_username,h.ipmi_password,hi.inventory_mode"
			" from host_discovery hd"
				" join hosts h"
					" on hd.hostid=h.hostid"
				" left join host_inventory hi"
					" on hd.hostid=hi.hostid"
			" where hd.parent_hostid=" ZBX_FS_UI64,
			parent_hostid);

	while (NULL != (row = DBfetch(result)))
	{
		host = zbx_malloc(NULL, sizeof(zbx_lld_host_t));

		ZBX_STR2UINT64(host->hostid, row[0]);
		host->host_proto = zbx_strdup(NULL, row[1]);
		host->lastcheck = atoi(row[2]);
		host->ts_delete = atoi(row[3]);
		host->host = zbx_strdup(NULL, row[4]);
		host->host_orig = NULL;
		host->name = zbx_strdup(NULL, row[5]);
		host->name_orig = NULL;
		host->flags = 0x00;

		ZBX_DBROW2UINT64(db_proxy_hostid, row[6]);
		if (db_proxy_hostid != proxy_hostid)
			host->flags |= ZBX_FLAG_LLD_HOST_UPDATE_PROXY;

		if ((char)atoi(row[7]) != ipmi_authtype)
			host->flags |= ZBX_FLAG_LLD_HOST_UPDATE_IPMI_AUTH;

		if ((unsigned char)atoi(row[8]) != ipmi_privilege)
			host->flags |= ZBX_FLAG_LLD_HOST_UPDATE_IPMI_PRIV;

		if (0 != strcmp(row[9], ipmi_username))
			host->flags |= ZBX_FLAG_LLD_HOST_UPDATE_IPMI_USER;

		if (0 != strcmp(row[10], ipmi_password))
			host->flags |= ZBX_FLAG_LLD_HOST_UPDATE_IPMI_PASS;

		if (SUCCEED == DBis_null(row[11]))
			host->inventory_mode = HOST_INVENTORY_DISABLED;
		else
			host->inventory_mode = (char)atoi(row[11]);

		zbx_vector_uint64_create(&host->new_groupids);
		zbx_vector_uint64_create(&host->lnk_templateids);
		zbx_vector_uint64_create(&host->del_templateids);
		zbx_vector_ptr_create(&host->new_hostmacros);

		zbx_vector_ptr_append(hosts, host);
	}
	DBfree_result(result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_hosts_validate                                             *
 *                                                                            *
 * Parameters: hosts - [IN] list of hosts; should be sorted by hostid         *
 *                                                                            *
 ******************************************************************************/
void	DBlld_hosts_validate(zbx_vector_ptr_t *hosts, char **error)
{
	const char		*__function_name = "DBlld_hosts_validate";

	char			*tnames = NULL, *vnames = NULL, *host_esc, *name_esc;
	size_t			tnames_alloc = 256, tnames_offset = 0,
				vnames_alloc = 256, vnames_offset = 0;
	DB_RESULT		result;
	DB_ROW			row;
	int			i, j;
	zbx_lld_host_t		*host, *host_b;
	zbx_vector_uint64_t	hostids;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_uint64_create(&hostids);

	tnames = zbx_malloc(tnames, tnames_alloc);	/* list of technical host names */
	vnames = zbx_malloc(vnames, vnames_alloc);	/* list of visible host names */

	/* checking a host name validity */
	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		/* only new hosts or hosts with a new host name will be validated */
		if (0 != host->hostid && 0 == (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_HOST))
			continue;

		/* host name is valid? */
		if (SUCCEED == zbx_check_hostname(host->host))
			continue;

		*error = zbx_strdcatf(*error, "Cannot %s host: invalid host name \"%s\".\n",
				(0 != host->hostid ? "update" : "create"), host->host);

		if (0 != host->hostid)
		{
			/* return an original host name and drop the correspond flag */
			zbx_free(host->host);
			host->host = host->host_orig;
			host->host_orig = NULL;
			host->flags &= ~ZBX_FLAG_LLD_HOST_UPDATE_HOST;
		}
		else
			host->flags &= ~ZBX_FLAG_LLD_HOST_DISCOVERED;
	}

	/* checking a visible host name validity */
	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		/* only new hosts or hosts with a new visible host name will be validated */
		if (0 != host->hostid && 0 == (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_NAME))
			continue;

		/* visible host name is valid utf8 sequence and has a valid length */
		if (SUCCEED == zbx_is_utf8(host->name) && '\0' != *host->name &&
				HOST_NAME_LEN >= zbx_strlen_utf8(host->name))
		{
			continue;
		}

		zbx_replace_invalid_utf8(host->name);
		*error = zbx_strdcatf(*error, "Cannot %s host: invalid visible host name \"%s\".\n",
				(0 != host->hostid ? "update" : "create"), host->name);

		if (0 != host->hostid)
		{
			/* return an original visible host name and drop the correspond flag */
			zbx_free(host->name);
			host->name = host->name_orig;
			host->name_orig = NULL;
			host->flags &= ~ZBX_FLAG_LLD_HOST_UPDATE_NAME;
		}
		else
			host->flags &= ~ZBX_FLAG_LLD_HOST_DISCOVERED;
	}

	/* checking duplicated host names */
	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		/* only new hosts or hosts with a new host name will be validated */
		if (0 != host->hostid && 0 == (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_HOST))
			continue;

		for (j = 0; j < hosts->values_num; j++)
		{
			host_b = (zbx_lld_host_t *)hosts->values[j];

			if (0 == host_b->flags || i == j)
				continue;

			if (0 != strcmp(host->host, host_b->host))
				continue;

			*error = zbx_strdcatf(*error, "Cannot %s host:"
						" host with the same name \"%s\" already exists.\n",
						(0 != host->hostid ? "update" : "create"), host->host);

			if (0 != host->hostid)
			{
				/* return an original host name and drop the correspond flag */
				zbx_free(host->host);
				host->host = host->host_orig;
				host->host_orig = NULL;
				host->flags &= ~ZBX_FLAG_LLD_HOST_UPDATE_HOST;
			}
			else
				host->flags &= ~ZBX_FLAG_LLD_HOST_DISCOVERED;
		}
	}

	/* checking duplicated visible host names */
	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		/* only new hosts or hosts with a new visible host name will be validated */
		if (0 != host->hostid && 0 == (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_NAME))
			continue;

		for (j = 0; j < hosts->values_num; j++)
		{
			host_b = (zbx_lld_host_t *)hosts->values[j];

			if (0 == host_b->flags || i == j)
				continue;

			if (0 != strcmp(host->name, host_b->name))
				continue;

			*error = zbx_strdcatf(*error, "Cannot %s host:"
					" host with the same visible name \"%s\" already exists.\n",
					(0 != host->hostid ? "update" : "create"), host->name);

			if (0 != host->hostid)
			{
				/* return an original visible host name and drop the correspond flag */
				zbx_free(host->name);
				host->name = host->name_orig;
				host->name_orig = NULL;
				host->flags &= ~ZBX_FLAG_LLD_HOST_UPDATE_NAME;
			}
			else
				host->flags &= ~ZBX_FLAG_LLD_HOST_DISCOVERED;
		}
	}

	/* checking duplicated host names and visible host names in DB */

	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		if (0 != host->hostid)
			zbx_vector_uint64_append(&hostids, host->hostid);

		if (0 == host->hostid || 0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_HOST))
		{
			host_esc = DBdyn_escape_string(host->host);

			if (0 != tnames_offset)
				zbx_chrcpy_alloc(&tnames, &tnames_alloc, &tnames_offset, ',');
			zbx_chrcpy_alloc(&tnames, &tnames_alloc, &tnames_offset, '\'');
			zbx_strcpy_alloc(&tnames, &tnames_alloc, &tnames_offset, host_esc);
			zbx_chrcpy_alloc(&tnames, &tnames_alloc, &tnames_offset, '\'');

			zbx_free(host_esc);
		}

		if (0 == host->hostid || 0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_NAME))
		{
			name_esc = DBdyn_escape_string(host->name);

			if (0 != vnames_offset)
				zbx_chrcpy_alloc(&vnames, &vnames_alloc, &vnames_offset, ',');
			zbx_chrcpy_alloc(&vnames, &vnames_alloc, &vnames_offset, '\'');
			zbx_strcpy_alloc(&vnames, &vnames_alloc, &vnames_offset, name_esc);
			zbx_chrcpy_alloc(&vnames, &vnames_alloc, &vnames_offset, '\'');

			zbx_free(name_esc);
		}
	}

	if (0 != tnames_offset || 0 != vnames_offset)
	{
		char	*sql = NULL;
		size_t	sql_alloc = 256, sql_offset = 0;

		sql = zbx_malloc(sql, sql_alloc);

		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
				"select host,name"
				" from hosts"
				" where status in (%d,%d,%d)"
					" and flags<>%d"
					" and ",
				HOST_STATUS_MONITORED, HOST_STATUS_NOT_MONITORED, HOST_STATUS_TEMPLATE,
				ZBX_FLAG_DISCOVERY_PROTOTYPE);
		if (0 != tnames_offset && 0 != vnames_offset)
			zbx_chrcpy_alloc(&sql, &sql_alloc, &sql_offset, '(');
		if (0 != tnames_offset)
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, "host in (%s)", tnames);
		if (0 != tnames_offset && 0 != vnames_offset)
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, " or ");
		if (0 != vnames_offset)
			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, "name in (%s)", vnames);
		if (0 != tnames_offset && 0 != vnames_offset)
			zbx_chrcpy_alloc(&sql, &sql_alloc, &sql_offset, ')');
		if (0 != hostids.values_num)
		{
			zbx_vector_uint64_sort(&hostids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
			zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, " and not");
			DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "hostid",
					hostids.values, hostids.values_num);
		}

		result = DBselect("%s", sql);

		while (NULL != (row = DBfetch(result)))
		{
			for (i = 0; i < hosts->values_num; i++)
			{
				host = (zbx_lld_host_t *)hosts->values[i];

				if (0 == host->flags)
					continue;

				if (0 == strcmp(host->host, row[0]))
				{
					*error = zbx_strdcatf(*error, "Cannot %s host:"
							" host with the same name \"%s\" already exists.\n",
							(0 != host->hostid ? "update" : "create"), host->host);

					if (0 != host->hostid)
					{
						/* return an original host name and drop the correspond flag */
						zbx_free(host->host);
						host->host = host->host_orig;
						host->host_orig = NULL;
						host->flags &= ~ZBX_FLAG_LLD_HOST_UPDATE_HOST;
					}
					else
						host->flags &= ~ZBX_FLAG_LLD_HOST_DISCOVERED;

					continue;
				}

				if (0 == strcmp(host->name, row[1]))
				{
					*error = zbx_strdcatf(*error, "Cannot %s host:"
							" host with the same visible name \"%s\" already exists.\n",
							(0 != host->hostid ? "update" : "create"), host->name);

					if (0 != host->hostid)
					{
						/* return an original visible host name and drop the correspond flag */
						zbx_free(host->name);
						host->name = host->name_orig;
						host->name_orig = NULL;
						host->flags &= ~ZBX_FLAG_LLD_HOST_UPDATE_NAME;
					}
					else
						host->flags &= ~ZBX_FLAG_LLD_HOST_DISCOVERED;
				}
			}
		}
		DBfree_result(result);

		zbx_free(sql);
	}

	zbx_free(vnames);
	zbx_free(tnames);

	zbx_vector_uint64_destroy(&hostids);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

static void	DBlld_host_make(zbx_vector_ptr_t *hosts, const char *host_proto, const char *name_proto,
		struct zbx_json_parse *jp_row)
{
	const char	*__function_name = "DBlld_host_make";

	char		*buffer = NULL;
	int		i;
	zbx_lld_host_t	*host;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 != (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		buffer = zbx_strdup(buffer, host->host_proto);
		substitute_discovery_macros(&buffer, jp_row, ZBX_MACRO_ANY, NULL, 0);
		zbx_lrtrim(buffer, ZBX_WHITESPACE);

		if (0 == strcmp(host->host, buffer))
			break;
	}

	if (i == hosts->values_num)	/* no host found */
	{
		host = zbx_malloc(NULL, sizeof(zbx_lld_host_t));

		host->hostid = 0;
		host->host_proto = NULL;
		host->lastcheck = 0;
		host->ts_delete = 0;
		host->host = zbx_strdup(NULL, host_proto);
		host->host_orig = NULL;
		substitute_discovery_macros(&host->host, jp_row, ZBX_MACRO_ANY, NULL, 0);
		zbx_lrtrim(host->host, ZBX_WHITESPACE);
		host->name = zbx_strdup(NULL, name_proto);
		substitute_discovery_macros(&host->name, jp_row, ZBX_MACRO_ANY, NULL, 0);
		zbx_lrtrim(host->name, ZBX_WHITESPACE);
		host->name_orig = NULL;
		zbx_vector_uint64_create(&host->new_groupids);
		zbx_vector_uint64_create(&host->lnk_templateids);
		zbx_vector_uint64_create(&host->del_templateids);
		zbx_vector_ptr_create(&host->new_hostmacros);
		host->flags = ZBX_FLAG_LLD_HOST_DISCOVERED;

		zbx_vector_ptr_append(hosts, host);
	}
	else
	{
		/* host technical name */
		if (0 != strcmp(host->host_proto, host_proto))	/* the new host prototype differs */
		{
			host->host_orig = host->host;
			host->host = zbx_strdup(NULL, host_proto);
			substitute_discovery_macros(&host->host, jp_row, ZBX_MACRO_ANY, NULL, 0);
			zbx_lrtrim(host->host, ZBX_WHITESPACE);
			host->flags |= ZBX_FLAG_LLD_HOST_UPDATE_HOST;
		}

		/* host visible name */
		buffer = zbx_strdup(buffer, name_proto);
		substitute_discovery_macros(&buffer, jp_row, ZBX_MACRO_ANY, NULL, 0);
		zbx_lrtrim(buffer, ZBX_WHITESPACE);
		if (0 != strcmp(host->name, buffer))
		{
			host->name_orig = host->name;
			host->name = buffer;
			buffer = NULL;
			host->flags |= ZBX_FLAG_LLD_HOST_UPDATE_NAME;
		}

		host->flags |= ZBX_FLAG_LLD_HOST_DISCOVERED;
	}

	zbx_free(buffer);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_groups_get                                                 *
 *                                                                            *
 * Purpose: retrieve list of host groups which should be present on the each  *
 *          discovered host                                                   *
 *                                                                            *
 * Parameters: parent_hostid - [IN] host prototype identificator              *
 *             groupids      - [OUT] sorted list of host groups               *
 *                                                                            *
 ******************************************************************************/
static void	DBlld_groups_get(zbx_uint64_t parent_hostid, zbx_vector_uint64_t *groupids)
{
	DB_RESULT	result;
	DB_ROW		row;
	zbx_uint64_t	groupid;

	result = DBselect(
			"select groupid"
			" from group_prototype"
			" where groupid is not null"
				" and hostid=" ZBX_FS_UI64,
			parent_hostid);

	while (NULL != (row = DBfetch(result)))
	{
		ZBX_STR2UINT64(groupid, row[0]);
		zbx_vector_uint64_append(groupids, groupid);
	}
	DBfree_result(result);

	zbx_vector_uint64_sort(groupids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_groups_make                                                *
 *                                                                            *
 * Parameters: groupids         - [IN] sorted list of host groups which       *
 *                                     should be present on the each          *
 *                                     discovered host                        *
 *             hosts            - [IN/OUT] list of hosts                      *
 *                                         should be sorted by hostid         *
 *             del_hostgroupids - [OUT] list of host groups which should be   *
 *                                      deleted                               *
 *                                                                            *
 ******************************************************************************/
static void	DBlld_groups_make(const zbx_vector_uint64_t *groupids, zbx_vector_ptr_t *hosts,
		zbx_vector_uint64_t *del_hostgroupids)
{
	const char		*__function_name = "DBlld_groups_make";

	DB_RESULT		result;
	DB_ROW			row;
	int			i, j;
	zbx_vector_uint64_t	hostids;
	zbx_uint64_t		hostgroupid, hostid, groupid;
	zbx_lld_host_t		*host;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_uint64_create(&hostids);

	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		zbx_vector_uint64_reserve(&host->new_groupids, groupids->values_num);
		for (j = 0; j < groupids->values_num; j++)
			zbx_vector_uint64_append(&host->new_groupids, groupids->values[j]);

		if (0 != host->hostid)
			zbx_vector_uint64_append(&hostids, host->hostid);
	}

	if (0 != hostids.values_num)
	{
		char	*sql = NULL;
		size_t	sql_alloc = 256, sql_offset = 0;

		sql = zbx_malloc(sql, sql_alloc);

		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
				"select hg.hostid,hg.groupid,hg.hostgroupid"
				" from hosts_groups hg"
					" left join group_discovery gd"
						" on hg.groupid=gd.groupid"
				" where gd.groupid is null"
					" and");
		DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "hg.hostid", hostids.values, hostids.values_num);

		result = DBselect("%s", sql);

		zbx_free(sql);

		while (NULL != (row = DBfetch(result)))
		{
			ZBX_STR2UINT64(hostid, row[0]);
			ZBX_STR2UINT64(groupid, row[1]);

			if (FAIL == (i = zbx_vector_ptr_bsearch(hosts, &hostid, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC)))
			{
				THIS_SHOULD_NEVER_HAPPEN;
				continue;
			}

			host = (zbx_lld_host_t *)hosts->values[i];

			if (FAIL == (i = zbx_vector_uint64_bsearch(&host->new_groupids, groupid,
					ZBX_DEFAULT_UINT64_COMPARE_FUNC)))
			{
				/* host groups which should be deleted */
				ZBX_STR2UINT64(hostgroupid, row[2]);
				zbx_vector_uint64_append(del_hostgroupids, hostgroupid);
			}
			else
			{
				/* host groups which are already added */
				zbx_vector_uint64_remove(&host->new_groupids, i);
			}
		}
		DBfree_result(result);

		zbx_vector_uint64_sort(del_hostgroupids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	}

	zbx_vector_uint64_destroy(&hostids);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_hostmacros_get                                             *
 *                                                                            *
 * Purpose: retrieve list of host macros which should be present on the each  *
 *          discovered host                                                   *
 *                                                                            *
 * Parameters: hostmacros - [OUT] list of host macros                         *
 *                                                                            *
 ******************************************************************************/
static void	DBlld_hostmacros_get(zbx_uint64_t lld_ruleid, zbx_vector_ptr_t *hostmacros)
{
	const char		*__function_name = "DBlld_hostgroups_get";

	DB_RESULT		result;
	DB_ROW			row;
	zbx_lld_hostmacro_t	*hostmacro;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	result = DBselect(
			"select hm.macro,hm.value"
			" from hostmacro hm,items i"
			" where hm.hostid=i.hostid"
				" and i.itemid=" ZBX_FS_UI64,
			lld_ruleid);

	while (NULL != (row = DBfetch(result)))
	{
		hostmacro = zbx_malloc(NULL, sizeof(zbx_lld_hostmacro_t));

		hostmacro->macro = zbx_strdup(NULL, row[0]);
		hostmacro->value = zbx_strdup(NULL, row[1]);

		zbx_vector_ptr_append(hostmacros, hostmacro);
	}
	DBfree_result(result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_hostmacros_make                                            *
 *                                                                            *
 * Parameters: hostmacros       - [IN] list of host macros which              *
 *                                     should be present on the each          *
 *                                     discovered host                        *
 *             hosts            - [IN/OUT] list of hosts                      *
 *                                         should be sorted by hostid         *
 *             del_hostmacroids - [OUT] list of host macros which should be   *
 *                                      deleted                               *
 *                                                                            *
 ******************************************************************************/
static void	DBlld_hostmacros_make(const zbx_vector_ptr_t *hostmacros, zbx_vector_ptr_t *hosts,
		zbx_vector_uint64_t *del_hostmacroids)
{
	const char		*__function_name = "DBlld_hostmacros_make";

	DB_RESULT		result;
	DB_ROW			row;
	int			i, j;
	zbx_vector_uint64_t	hostids;
	zbx_uint64_t		hostmacroid, hostid;
	zbx_lld_host_t		*host;
	zbx_lld_hostmacro_t	*hostmacro = NULL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_uint64_create(&hostids);

	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		zbx_vector_ptr_reserve(&host->new_hostmacros, hostmacros->values_num);
		for (j = 0; j < hostmacros->values_num; j++)
		{
			hostmacro = zbx_malloc(NULL, sizeof(zbx_lld_hostmacro_t));

			hostmacro->hostmacroid = 0;
			hostmacro->macro = zbx_strdup(NULL, ((zbx_lld_hostmacro_t *)hostmacros->values[j])->macro);
			hostmacro->value = zbx_strdup(NULL, ((zbx_lld_hostmacro_t *)hostmacros->values[j])->value);

			zbx_vector_ptr_append(&host->new_hostmacros, hostmacro);
		}

		if (0 != host->hostid)
			zbx_vector_uint64_append(&hostids, host->hostid);
	}

	if (0 != hostids.values_num)
	{
		char	*sql = NULL;
		size_t	sql_alloc = 256, sql_offset = 0;

		sql = zbx_malloc(sql, sql_alloc);

		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
				"select hostmacroid,hostid,macro,value"
				" from hostmacro"
				" where");
		DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "hostid", hostids.values, hostids.values_num);

		result = DBselect("%s", sql);

		zbx_free(sql);

		while (NULL != (row = DBfetch(result)))
		{
			ZBX_STR2UINT64(hostid, row[1]);

			if (FAIL == (i = zbx_vector_ptr_bsearch(hosts, &hostid, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC)))
			{
				THIS_SHOULD_NEVER_HAPPEN;
				continue;
			}

			host = (zbx_lld_host_t *)hosts->values[i];

			for (i = 0; i < host->new_hostmacros.values_num; i++)
			{
				hostmacro = (zbx_lld_hostmacro_t *)host->new_hostmacros.values[i];

				if (0 == strcmp(hostmacro->macro, row[2]))
					break;
			}

			if (i == host->new_hostmacros.values_num)
			{
				/* host macros which should be deleted */
				ZBX_STR2UINT64(hostmacroid, row[0]);
				zbx_vector_uint64_append(del_hostmacroids, hostmacroid);
			}
			else
			{
				/* host macros which are already added */
				if (0 == strcmp(hostmacro->value, row[3]))	/* value doesn't changed */
				{
					DBlld_hostmacro_free(hostmacro);
					zbx_vector_ptr_remove(&host->new_hostmacros, i);
				}
				else
					ZBX_STR2UINT64(hostmacro->hostmacroid, row[0]);
			}
		}
		DBfree_result(result);

		zbx_vector_uint64_sort(del_hostmacroids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	}

	zbx_vector_uint64_destroy(&hostids);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_templates_make                                             *
 *                                                                            *
 * Purpose: gets templates from a host prototype                              *
 *                                                                            *
 * Parameters: parent_hostid - [IN] host prototype identificator              *
 *             hosts         - [IN/OUT] list of hosts                         *
 *                                      should be sorted by hostid            *
 *                                                                            *
 ******************************************************************************/
static void	DBlld_templates_make(zbx_uint64_t parent_hostid, zbx_vector_ptr_t *hosts)
{
	const char		*__function_name = "DBlld_templates_make";

	DB_RESULT		result;
	DB_ROW			row;
	zbx_vector_uint64_t	templateids, hostids;
	zbx_uint64_t		templateid, hostid;
	zbx_lld_host_t		*host;
	int			i, j;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_uint64_create(&templateids);
	zbx_vector_uint64_create(&hostids);

	/* select templates which should be linked */

	result = DBselect("select templateid from hosts_templates where hostid=" ZBX_FS_UI64, parent_hostid);

	while (NULL != (row = DBfetch(result)))
	{
		ZBX_STR2UINT64(templateid, row[0]);
		zbx_vector_uint64_append(&templateids, templateid);
	}
	DBfree_result(result);

	zbx_vector_uint64_sort(&templateids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	/* select list of already created hosts */

	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		zbx_vector_uint64_reserve(&host->lnk_templateids, templateids.values_num);
		for (j = 0; j < templateids.values_num; j++)
			zbx_vector_uint64_append(&host->lnk_templateids, templateids.values[j]);

		if (0 != host->hostid)
			zbx_vector_uint64_append(&hostids, host->hostid);
	}

	if (0 != hostids.values_num)
	{
		char	*sql = NULL;
		size_t	sql_alloc = 256, sql_offset = 0;

		sql = zbx_malloc(sql, sql_alloc);

		/* select already linked temlates */

		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
				"select hostid,templateid"
				" from hosts_templates"
				" where");
		DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "hostid", hostids.values, hostids.values_num);

		result = DBselect("%s", sql);

		zbx_free(sql);

		while (NULL != (row = DBfetch(result)))
		{
			ZBX_STR2UINT64(hostid, row[0]);
			ZBX_STR2UINT64(templateid, row[1]);

			if (FAIL == (i = zbx_vector_ptr_bsearch(hosts, &hostid, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC)))
			{
				THIS_SHOULD_NEVER_HAPPEN;
				continue;
			}

			host = (zbx_lld_host_t *)hosts->values[i];

			if (FAIL == (i = zbx_vector_uint64_bsearch(&host->lnk_templateids, templateid,
					ZBX_DEFAULT_UINT64_COMPARE_FUNC)))
			{
				/* templates which should be unlinked */
				zbx_vector_uint64_append(&host->del_templateids, templateid);
			}
			else
			{
				/* templates which are already linked */
				zbx_vector_uint64_remove(&host->lnk_templateids, i);
			}
		}
		DBfree_result(result);

		for (i = 0; i < hosts->values_num; i++)
		{
			host = (zbx_lld_host_t *)hosts->values[i];

			if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
				continue;

			zbx_vector_uint64_sort(&host->del_templateids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
		}
	}

	zbx_vector_uint64_destroy(&hostids);
	zbx_vector_uint64_destroy(&templateids);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_hosts_save                                                 *
 *                                                                            *
 * Parameters: hosts            - [IN] list of hosts;                         *
 *                                     should be sorted by hostid             *
 *             status           - [IN] initial host satatus                   *
 *             del_hostgroupids - [IN] host groups which should be deleted    *
 *             del_hostmacroids - [IN] host macros which should be deleted    *
 *                                                                            *
 ******************************************************************************/
static void	DBlld_hosts_save(zbx_uint64_t parent_hostid, zbx_vector_ptr_t *hosts, const char *host_proto,
		zbx_uint64_t proxy_hostid, char ipmi_authtype, unsigned char ipmi_privilege, const char *ipmi_username,
		const char *ipmi_password, unsigned char status, char inventory_mode,
		zbx_vector_uint64_t *del_hostgroupids, zbx_vector_uint64_t *del_hostmacroids,
		zbx_vector_ptr_t *interfaces)
{
	const char		*__function_name = "DBlld_hosts_save";

	int			i, j, new_hosts = 0, new_host_inventories = 0, upd_hosts = 0, new_hostgroups = 0,
				new_hostmacros = 0, upd_hostmacros = 0, new_interfaces;
	zbx_lld_host_t		*host;
	zbx_lld_hostmacro_t	*hostmacro;
	zbx_lld_interface_t	*interface;
	zbx_vector_uint64_t	upd_host_inventory_hostids, del_host_inventory_hostids;
	zbx_uint64_t		hostid = 0, hostgroupid = 0, hostmacroid = 0, interfaceid = 0;
	char			*sql1 = NULL, *sql2 = NULL, *sql3 = NULL, *sql4 = NULL, *sql5 = NULL, *sql6 = NULL,
				*sql7 = NULL, *sql8 = NULL, *host_esc, *name_esc, *host_proto_esc,
				*ipmi_username_esc, *ipmi_password_esc, *macro_esc, *value_esc;
	size_t			sql1_alloc = 8 * ZBX_KIBIBYTE, sql1_offset = 0,
				sql2_alloc = 2 * ZBX_KIBIBYTE, sql2_offset = 0,
				sql3_alloc = 2 * ZBX_KIBIBYTE, sql3_offset = 0,
				sql4_alloc = 8 * ZBX_KIBIBYTE, sql4_offset = 0,
				sql5_alloc = 2 * ZBX_KIBIBYTE, sql5_offset = 0,
				sql6_alloc = 2 * ZBX_KIBIBYTE, sql6_offset = 0,
				sql7_alloc = 256, sql7_offset = 0,
				sql8_alloc = 2 * ZBX_KIBIBYTE, sql8_offset = 0;
	const char		*ins_hosts_sql =
				"insert into hosts (hostid,host,name,proxy_hostid,ipmi_authtype,ipmi_privilege,"
					"ipmi_username,ipmi_password,status,flags)"
				" values ";
	const char		*ins_host_discovery_sql =
				"insert into host_discovery (hostid,parent_hostid,host) values ";
	const char		*ins_host_inventory_sql = "insert into host_inventory (hostid,inventory_mode) values ";
	const char		*ins_hosts_groups_sql = "insert into hosts_groups (hostgroupid,hostid,groupid) values ";
	const char		*ins_hostmacro_sql = "insert into hostmacro (hostmacroid,hostid,macro,value) values ";
	const char		*ins_interface_sql =
				"insert into interface (interfaceid,hostid,type,main,useip,ip,dns,port) values ";

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_uint64_create(&upd_host_inventory_hostids);
	zbx_vector_uint64_create(&del_host_inventory_hostids);

	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		if (0 == host->hostid)
		{
			new_hosts++;
			if (HOST_INVENTORY_DISABLED != inventory_mode)
				new_host_inventories++;
		}
		else
		{
			if (0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE))
				upd_hosts++;

			if (host->inventory_mode != inventory_mode)
			{
				if (HOST_INVENTORY_DISABLED == inventory_mode)
					zbx_vector_uint64_append(&del_host_inventory_hostids, host->hostid);
				else if (HOST_INVENTORY_DISABLED == host->inventory_mode)
					new_host_inventories++;
				else
					zbx_vector_uint64_append(&upd_host_inventory_hostids, host->hostid);
			}
		}

		new_hostgroups += host->new_groupids.values_num;

		for (j = 0; j < host->new_hostmacros.values_num; j++)
		{
			hostmacro = (zbx_lld_hostmacro_t *)host->new_hostmacros.values[j];

			if (0 == hostmacro->hostmacroid)
				new_hostmacros++;
			else
				upd_hostmacros++;
		}
	}

	if (0 != new_hosts)
	{
		hostid = DBget_maxid_num("hosts", new_hosts);

		sql1 = zbx_malloc(sql1, sql1_alloc);
		sql2 = zbx_malloc(sql2, sql2_alloc);
		DBbegin_multiple_update(&sql1, &sql1_alloc, &sql1_offset);
		DBbegin_multiple_update(&sql2, &sql2_alloc, &sql2_offset);
#ifdef HAVE_MULTIROW_INSERT
		zbx_strcpy_alloc(&sql1, &sql1_alloc, &sql1_offset, ins_hosts_sql);
		zbx_strcpy_alloc(&sql2, &sql2_alloc, &sql2_offset, ins_host_discovery_sql);
#endif
	}

	if (0 != new_host_inventories)
	{
		sql3 = zbx_malloc(sql3, sql3_alloc);
		DBbegin_multiple_update(&sql3, &sql3_alloc, &sql3_offset);
#ifdef HAVE_MULTIROW_INSERT
		zbx_strcpy_alloc(&sql3, &sql3_alloc, &sql3_offset, ins_host_inventory_sql);
#endif
	}

	if (0 != upd_hosts || 0 != upd_hostmacros)
	{
		sql4 = zbx_malloc(sql4, sql4_alloc);
		DBbegin_multiple_update(&sql4, &sql4_alloc, &sql4_offset);
	}

	if (0 != new_hostgroups)
	{
		hostgroupid = DBget_maxid_num("hosts_groups", new_hostgroups);

		sql5 = zbx_malloc(sql5, sql5_alloc);
		DBbegin_multiple_update(&sql5, &sql5_alloc, &sql5_offset);
#ifdef HAVE_MULTIROW_INSERT
		zbx_strcpy_alloc(&sql5, &sql5_alloc, &sql5_offset, ins_hosts_groups_sql);
#endif
	}

	if (0 != new_hostmacros)
	{
		hostmacroid = DBget_maxid_num("hostmacro", new_hostmacros);

		sql6 = zbx_malloc(sql6, sql6_alloc);
		DBbegin_multiple_update(&sql6, &sql6_alloc, &sql6_offset);
#ifdef HAVE_MULTIROW_INSERT
		zbx_strcpy_alloc(&sql6, &sql6_alloc, &sql6_offset, ins_hostmacro_sql);
#endif
	}

	if (0 != (new_interfaces = new_hosts * interfaces->values_num))
	{
		interfaceid = DBget_maxid_num("interface", new_interfaces);

		sql8 = zbx_malloc(sql8, sql8_alloc);
		DBbegin_multiple_update(&sql8, &sql8_alloc, &sql8_offset);
#ifdef HAVE_MULTIROW_INSERT
		zbx_strcpy_alloc(&sql8, &sql8_alloc, &sql8_offset, ins_interface_sql);
#endif
	}

	host_proto_esc = DBdyn_escape_string(host_proto);
	ipmi_username_esc = DBdyn_escape_string(ipmi_username);
	ipmi_password_esc = DBdyn_escape_string(ipmi_password);

	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		host_esc = DBdyn_escape_string(host->host);
		name_esc = DBdyn_escape_string(host->name);

		if (0 == host->hostid)
		{
			host->hostid = hostid++;
#ifndef HAVE_MULTIROW_INSERT
			zbx_strcpy_alloc(&sql1, &sql1_alloc, &sql1_offset, ins_hosts_sql);
#endif
			zbx_snprintf_alloc(&sql1, &sql1_alloc, &sql1_offset,
					"(" ZBX_FS_UI64 ",'%s','%s',%s,%d,%d,'%s','%s',%d,%d)" ZBX_ROW_DL,
					host->hostid, host_esc, name_esc, DBsql_id_ins(proxy_hostid),
					(int)ipmi_authtype, (int)ipmi_privilege, ipmi_username_esc, ipmi_password_esc,
					(int)status, ZBX_FLAG_DISCOVERY_CREATED);

#ifndef HAVE_MULTIROW_INSERT
			zbx_strcpy_alloc(&sql2, &sql2_alloc, &sql2_offset, ins_host_discovery_sql);
#endif
			zbx_snprintf_alloc(&sql2, &sql2_alloc, &sql2_offset,
					"(" ZBX_FS_UI64 "," ZBX_FS_UI64 ",'%s')" ZBX_ROW_DL,
					host->hostid, parent_hostid, host_proto_esc);

			if (HOST_INVENTORY_DISABLED != inventory_mode)
			{
#ifndef HAVE_MULTIROW_INSERT
				zbx_strcpy_alloc(&sql3, &sql3_alloc, &sql3_offset, ins_host_inventory_sql);
#endif
				zbx_snprintf_alloc(&sql3, &sql3_alloc, &sql3_offset, "(" ZBX_FS_UI64 ",%d)" ZBX_ROW_DL,
						host->hostid, (int)inventory_mode);
			}

			for (j = 0; j < interfaces->values_num; j++)
			{
				interface = (zbx_lld_interface_t *)interfaces->values[j];
#ifndef HAVE_MULTIROW_INSERT
				zbx_strcpy_alloc(&sql8, &sql8_alloc, &sql8_offset, ins_interface_sql);
#endif
				zbx_snprintf_alloc(&sql8, &sql8_alloc, &sql8_offset,
						"(" ZBX_FS_UI64 "," ZBX_FS_UI64 ",%d,%d,%d,'%s','%s','%s')" ZBX_ROW_DL,
						interfaceid++, host->hostid, (int)interface->type, (int)interface->main,
						(int)interface->useip, interface->ip_esc, interface->dns_esc,
						interface->port_esc);
			}
		}
		else
		{
			if (0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE))
			{
				const char	*d = "";

				zbx_strcpy_alloc(&sql4, &sql4_alloc, &sql4_offset, "update hosts set ");
				if (0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_HOST))
				{
					zbx_snprintf_alloc(&sql4, &sql4_alloc, &sql4_offset, "host='%s'", host_esc);
					d = ",";
				}
				if (0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_NAME))
				{
					zbx_snprintf_alloc(&sql4, &sql4_alloc, &sql4_offset,
							"%sname='%s'", d, name_esc);
					d = ",";
				}
				if (0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_PROXY))
				{
					zbx_snprintf_alloc(&sql4, &sql4_alloc, &sql4_offset,
							"%sproxy_hostid=%s", d, DBsql_id_ins(proxy_hostid));
					d = ",";
				}
				if (0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_IPMI_AUTH))
				{
					zbx_snprintf_alloc(&sql4, &sql4_alloc, &sql4_offset,
							"%sipmi_authtype=%d", d, (int)ipmi_authtype);
					d = ",";
				}
				if (0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_IPMI_PRIV))
				{
					zbx_snprintf_alloc(&sql4, &sql4_alloc, &sql4_offset,
							"%sipmi_privilege=%d", d, (int)ipmi_privilege);
					d = ",";
				}
				if (0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_IPMI_USER))
				{
					zbx_snprintf_alloc(&sql4, &sql4_alloc, &sql4_offset,
							"%sipmi_username='%s'", d, ipmi_username_esc);
					d = ",";
				}
				if (0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_IPMI_PASS))
				{
					zbx_snprintf_alloc(&sql4, &sql4_alloc, &sql4_offset,
							"%sipmi_password='%s'", d, ipmi_password_esc);
				}
				zbx_snprintf_alloc(&sql4, &sql4_alloc, &sql4_offset, " where hostid=" ZBX_FS_UI64 ";\n",
						host->hostid);
			}

			if (host->inventory_mode != inventory_mode && HOST_INVENTORY_DISABLED == host->inventory_mode)
			{
#ifndef HAVE_MULTIROW_INSERT
				zbx_strcpy_alloc(&sql3, &sql3_alloc, &sql3_offset, ins_host_inventory_sql);
#endif
				zbx_snprintf_alloc(&sql3, &sql3_alloc, &sql3_offset, "(" ZBX_FS_UI64 ",%d)" ZBX_ROW_DL,
						host->hostid, (int)inventory_mode);
			}

			if (0 != (host->flags & ZBX_FLAG_LLD_HOST_UPDATE_HOST))
			{
				zbx_snprintf_alloc(&sql4, &sql4_alloc, &sql4_offset,
						"update host_discovery"
						" set host='%s'"
						" where hostid=" ZBX_FS_UI64 ";\n",
						host_proto_esc, host->hostid);
			}
		}

		for (j = 0; j < host->new_groupids.values_num; j++)
		{
#ifndef HAVE_MULTIROW_INSERT
			zbx_strcpy_alloc(&sql5, &sql5_alloc, &sql5_offset, ins_hosts_groups_sql);
#endif
			zbx_snprintf_alloc(&sql5, &sql5_alloc, &sql5_offset,
					"(" ZBX_FS_UI64 "," ZBX_FS_UI64 "," ZBX_FS_UI64 ")" ZBX_ROW_DL,
					hostgroupid++, host->hostid, host->new_groupids.values[j]);
		}

		for (j = 0; j < host->new_hostmacros.values_num; j++)
		{
			hostmacro = (zbx_lld_hostmacro_t *)host->new_hostmacros.values[j];

			value_esc = DBdyn_escape_string(hostmacro->value);

			if (0 == hostmacro->hostmacroid)
			{
#ifndef HAVE_MULTIROW_INSERT
				zbx_strcpy_alloc(&sql6, &sql6_alloc, &sql6_offset, ins_hostmacro_sql);
#endif
				macro_esc = DBdyn_escape_string(hostmacro->macro);

				zbx_snprintf_alloc(&sql6, &sql6_alloc, &sql6_offset,
						"(" ZBX_FS_UI64 "," ZBX_FS_UI64 ",'%s','%s')" ZBX_ROW_DL,
						hostmacroid++, host->hostid, macro_esc, value_esc);

				zbx_free(macro_esc);
			}
			else
			{
				zbx_snprintf_alloc(&sql4, &sql4_alloc, &sql4_offset,
						"update hostmacro"
						" set value='%s'"
						" where hostmacroid=" ZBX_FS_UI64 ";\n",
						value_esc, hostmacro->hostmacroid);
			}

			zbx_free(value_esc);
		}

		zbx_free(name_esc);
		zbx_free(host_esc);
	}

	zbx_free(ipmi_password_esc);
	zbx_free(ipmi_username_esc);
	zbx_free(host_proto_esc);

	if (0 != new_hosts)
	{
#ifdef HAVE_MULTIROW_INSERT
		sql1_offset--;
		sql2_offset--;
		zbx_strcpy_alloc(&sql1, &sql1_alloc, &sql1_offset, ";\n");
		zbx_strcpy_alloc(&sql2, &sql2_alloc, &sql2_offset, ";\n");
#endif
		DBend_multiple_update(&sql1, &sql1_alloc, &sql1_offset);
		DBend_multiple_update(&sql2, &sql2_alloc, &sql2_offset);
		DBexecute("%s", sql1);
		DBexecute("%s", sql2);
		zbx_free(sql1);
		zbx_free(sql2);
	}

	if (0 != new_host_inventories)
	{
#ifdef HAVE_MULTIROW_INSERT
		sql3_offset--;
		zbx_strcpy_alloc(&sql3, &sql3_alloc, &sql3_offset, ";\n");
#endif
		DBend_multiple_update(&sql3, &sql3_alloc, &sql3_offset);
		DBexecute("%s", sql3);
		zbx_free(sql3);
	}

	if (0 != upd_hosts || 0 != upd_hostmacros)
	{
		DBend_multiple_update(&sql4, &sql4_alloc, &sql4_offset);
		DBexecute("%s", sql4);
		zbx_free(sql4);
	}

	if (0 != new_hostgroups)
	{
#ifdef HAVE_MULTIROW_INSERT
		sql5_offset--;
		zbx_strcpy_alloc(&sql5, &sql5_alloc, &sql5_offset, ";\n");
#endif
		DBend_multiple_update(&sql5, &sql5_alloc, &sql5_offset);
		DBexecute("%s", sql5);
		zbx_free(sql5);
	}

	if (0 != new_hostmacros)
	{
#ifdef HAVE_MULTIROW_INSERT
		sql6_offset--;
		zbx_strcpy_alloc(&sql6, &sql6_alloc, &sql6_offset, ";\n");
#endif
		DBend_multiple_update(&sql6, &sql6_alloc, &sql6_offset);
		DBexecute("%s", sql6);
		zbx_free(sql6);
	}

	if (0 != del_hostgroupids->values_num || 0 != del_hostmacroids->values_num ||
			0 != upd_host_inventory_hostids.values_num || 0 != del_host_inventory_hostids.values_num)
	{
		sql7 = zbx_malloc(sql7, sql7_alloc);
		DBbegin_multiple_update(&sql7, &sql7_alloc, &sql7_offset);

		if (0 != del_hostgroupids->values_num)
		{
			zbx_strcpy_alloc(&sql7, &sql7_alloc, &sql7_offset, "delete from hosts_groups where");
			DBadd_condition_alloc(&sql7, &sql7_alloc, &sql7_offset, "hostgroupid",
					del_hostgroupids->values, del_hostgroupids->values_num);
			zbx_strcpy_alloc(&sql7, &sql7_alloc, &sql7_offset, ";\n");
		}

		if (0 != del_hostmacroids->values_num)
		{
			zbx_strcpy_alloc(&sql7, &sql7_alloc, &sql7_offset, "delete from hostmacro where");
			DBadd_condition_alloc(&sql7, &sql7_alloc, &sql7_offset, "hostmacroid",
					del_hostmacroids->values, del_hostmacroids->values_num);
			zbx_strcpy_alloc(&sql7, &sql7_alloc, &sql7_offset, ";\n");
		}

		if (0 != upd_host_inventory_hostids.values_num)
		{
			zbx_snprintf_alloc(&sql7, &sql7_alloc, &sql7_offset,
					"update host_inventory set inventory_mode=%d where", (int)inventory_mode);
			DBadd_condition_alloc(&sql7, &sql7_alloc, &sql7_offset, "hostid",
					upd_host_inventory_hostids.values, upd_host_inventory_hostids.values_num);
			zbx_strcpy_alloc(&sql7, &sql7_alloc, &sql7_offset, ";\n");
		}

		if (0 != del_host_inventory_hostids.values_num)
		{
			zbx_strcpy_alloc(&sql7, &sql7_alloc, &sql7_offset, "delete from host_inventory where");
			DBadd_condition_alloc(&sql7, &sql7_alloc, &sql7_offset, "hostid",
					del_host_inventory_hostids.values, del_host_inventory_hostids.values_num);
			zbx_strcpy_alloc(&sql7, &sql7_alloc, &sql7_offset, ";\n");
		}

		DBend_multiple_update(&sql7, &sql7_alloc, &sql7_offset);
		DBexecute("%s", sql7);
		zbx_free(sql7);
	}

	if (0 != new_interfaces)
	{
#ifdef HAVE_MULTIROW_INSERT
		sql8_offset--;
		zbx_strcpy_alloc(&sql8, &sql8_alloc, &sql8_offset, ";\n");
#endif
		DBend_multiple_update(&sql8, &sql8_alloc, &sql8_offset);
		DBexecute("%s", sql8);
		zbx_free(sql8);
	}

	zbx_vector_uint64_destroy(&del_host_inventory_hostids);
	zbx_vector_uint64_destroy(&upd_host_inventory_hostids);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_templates_link                                             *
 *                                                                            *
 ******************************************************************************/
static void	DBlld_templates_link(const zbx_vector_ptr_t *hosts)
{
	const char	*__function_name = "DBlld_templates_link";

	int		i;
	zbx_lld_host_t	*host;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
			continue;

		if (0 != host->lnk_templateids.values_num)
			DBcopy_template_elements(host->hostid, &host->lnk_templateids);

		if (0 != host->del_templateids.values_num)
			DBdelete_template_elements(host->hostid, &host->del_templateids);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_remove_lost_resources                                      *
 *                                                                            *
 * Purpose: updates host_discovery.lastcheck and host_discovery.ts_delete     *
 *          fields; removes lost resources                                    *
 *                                                                            *
 ******************************************************************************/
static void	DBlld_remove_lost_resources(zbx_vector_ptr_t *hosts, unsigned short lifetime, int lastcheck)
{
	char			*sql = NULL;
	size_t			sql_alloc = 256, sql_offset = 0;
	zbx_lld_host_t		*host;
	zbx_vector_uint64_t	del_hostids, lc_hostids, ts_hostids;
	int			i, lifetime_sec;

	if (0 == hosts->values_num)
		return;

	lifetime_sec = lifetime * SEC_PER_DAY;

	zbx_vector_uint64_create(&del_hostids);
	zbx_vector_uint64_create(&lc_hostids);
	zbx_vector_uint64_create(&ts_hostids);

	sql = zbx_malloc(sql, sql_alloc);

	DBbegin_multiple_update(&sql, &sql_alloc, &sql_offset);

	for (i = 0; i < hosts->values_num; i++)
	{
		host = (zbx_lld_host_t *)hosts->values[i];

		if (0 == host->hostid)
			continue;

		if (0 == (host->flags & ZBX_FLAG_LLD_HOST_DISCOVERED))
		{
			if (host->lastcheck < lastcheck - lifetime_sec)
			{
				zbx_vector_uint64_append(&del_hostids, host->hostid);
			}
			else if (host->ts_delete != host->lastcheck + lifetime_sec)
			{
				zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
						"update host_discovery"
						" set ts_delete=%d"
						" where hostid=" ZBX_FS_UI64 ";\n",
						host->lastcheck + lifetime_sec, host->hostid);
			}
		}
		else
		{
			zbx_vector_uint64_append(&lc_hostids, host->hostid);
			if (0 != host->ts_delete)
				zbx_vector_uint64_append(&ts_hostids, host->hostid);
		}
	}

	if (0 != lc_hostids.values_num)
	{
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, "update host_discovery set lastcheck=%d where",
				lastcheck);
		DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "hostid",
				lc_hostids.values, lc_hostids.values_num);
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
	}

	if (0 != ts_hostids.values_num)
	{
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, "update host_discovery set ts_delete=0 where");
		DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "hostid",
				ts_hostids.values, ts_hostids.values_num);
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
	}

	DBend_multiple_update(&sql, &sql_alloc, &sql_offset);

	if (16 < sql_offset)	/* in ORACLE always present begin..end; */
		DBexecute("%s", sql);

	zbx_free(sql);

	zbx_vector_uint64_sort(&del_hostids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	if (0 != del_hostids.values_num)
		DBdelete_hosts(&del_hostids);

	zbx_vector_uint64_destroy(&ts_hostids);
	zbx_vector_uint64_destroy(&lc_hostids);
	zbx_vector_uint64_destroy(&del_hostids);
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_interfaces_get                                             *
 *                                                                            *
 * Purpose: retrieves list of interfaces from the lld rule's host             *
 *                                                                            *
 ******************************************************************************/
static void	DBlld_interfaces_get(zbx_uint64_t lld_ruleid, zbx_vector_ptr_t *interfaces)
{
	DB_RESULT		result;
	DB_ROW			row;
	zbx_lld_interface_t	*interface;

	result = DBselect(
			"select hi.type,hi.main,hi.useip,hi.ip,hi.dns,hi.port"
			" from interface hi,items i"
			" where hi.hostid=i.hostid"
				" and i.itemid=" ZBX_FS_UI64,
			lld_ruleid);

	while (NULL != (row = DBfetch(result)))
	{
		interface = zbx_malloc(NULL, sizeof(zbx_lld_interface_t));

		interface->type = (unsigned char)atoi(row[0]);
		interface->main = (unsigned char)atoi(row[1]);
		interface->useip = (unsigned char)atoi(row[2]);
		interface->ip_esc = DBdyn_escape_string(row[3]);
		interface->dns_esc = DBdyn_escape_string(row[4]);
		interface->port_esc = DBdyn_escape_string(row[5]);

		zbx_vector_ptr_append(interfaces, interface);
	}
	DBfree_result(result);
}

/******************************************************************************
 *                                                                            *
 * Function: DBlld_update_hosts                                               *
 *                                                                            *
 * Purpose: add or update low-level discovered hosts                          *
 *                                                                            *
 ******************************************************************************/
void	DBlld_update_hosts(zbx_uint64_t lld_ruleid, struct zbx_json_parse *jp_data, char **error,
		const char *f_macro, const char *f_regexp, ZBX_REGEXP *regexps, int regexps_num,
		unsigned short lifetime, int lastcheck)
{
	const char		*__function_name = "DBlld_update_hosts";

	struct zbx_json_parse	jp_row;
	const char		*p;
	DB_RESULT		result;
	DB_ROW			row;
	zbx_vector_ptr_t	hosts, interfaces, hostmacros;
	zbx_vector_uint64_t	groupids;		/* list of host groups which should be added */
	zbx_vector_uint64_t	del_hostgroupids;	/* list of host groups which should be deleted */
	zbx_vector_uint64_t	del_hostmacroids;	/* list of host macros which should be deleted */
	zbx_uint64_t		proxy_hostid;
	char			*ipmi_username = NULL, *ipmi_password;
	char			ipmi_authtype, inventory_mode;
	unsigned char		ipmi_privilege;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	result = DBselect(
			"select h.proxy_hostid,h.ipmi_authtype,h.ipmi_privilege,h.ipmi_username,h.ipmi_password"
			" from hosts h,items i"
			" where h.hostid=i.hostid"
				" and i.itemid=" ZBX_FS_UI64,
			lld_ruleid);

	if (NULL != (row = DBfetch(result)))
	{
		ZBX_DBROW2UINT64(proxy_hostid, row[0]);
		ipmi_authtype = (char)atoi(row[1]);
		ipmi_privilege = (unsigned char)atoi(row[2]);
		ipmi_username = zbx_strdup(NULL, row[3]);
		ipmi_password = zbx_strdup(NULL, row[4]);
	}
	DBfree_result(result);

	if (NULL == ipmi_username)
	{
		*error = zbx_strdcatf(*error, "Cannot process host prototypes: a parent host not found.\n");
		return;
	}

	zbx_vector_ptr_create(&hosts);
	zbx_vector_uint64_create(&groupids);
	zbx_vector_uint64_create(&del_hostgroupids);
	zbx_vector_uint64_create(&del_hostmacroids);
	zbx_vector_ptr_create(&interfaces);
	zbx_vector_ptr_create(&hostmacros);

	DBlld_interfaces_get(lld_ruleid, &interfaces);
	DBlld_hostmacros_get(lld_ruleid, &hostmacros);

	result = DBselect(
			"select h.hostid,h.host,h.name,h.status,hi.inventory_mode"
			" from hosts h,host_discovery hd"
				" left join host_inventory hi"
					" on hd.hostid=hi.hostid"
			" where h.hostid=hd.hostid"
				" and hd.parent_itemid=" ZBX_FS_UI64,
			lld_ruleid);

	while (NULL != (row = DBfetch(result)))
	{
		zbx_uint64_t	parent_hostid;
		const char	*host_proto, *name_proto;
		unsigned char	status;

		ZBX_STR2UINT64(parent_hostid, row[0]);
		host_proto = row[1];
		name_proto = row[2];
		status = (unsigned char)atoi(row[3]);
		if (SUCCEED == DBis_null(row[4]))
			inventory_mode = HOST_INVENTORY_DISABLED;
		else
			inventory_mode = (char)atoi(row[4]);

		DBlld_groups_get(parent_hostid, &groupids);

		DBlld_hosts_get(parent_hostid, &hosts, proxy_hostid, ipmi_authtype, ipmi_privilege, ipmi_username,
				ipmi_password, inventory_mode);

		p = NULL;
		/* {"data":[{"{#VMNAME}":"vm_001"},{"{#VMNAME}":"vm_002"},...]} */
		/*          ^                                                   */
		while (NULL != (p = zbx_json_next(jp_data, p)))
		{
			/* {"data":[{"{#VMNAME}":"vm_001"},{"{#VMNAME}":"vm_002"},...]} */
			/*          ^--------------------^                              */
			if (FAIL == zbx_json_brackets_open(p, &jp_row))
				continue;

			if (SUCCEED != lld_check_record(&jp_row, f_macro, f_regexp, regexps, regexps_num))
				continue;

			DBlld_host_make(&hosts, host_proto, name_proto, &jp_row);
		}

		zbx_vector_ptr_sort(&hosts, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

		DBlld_hosts_validate(&hosts, error);

		DBlld_groups_make(&groupids, &hosts, &del_hostgroupids);
		DBlld_templates_make(parent_hostid, &hosts);
		DBlld_hostmacros_make(&hostmacros, &hosts, &del_hostmacroids);

		DBlld_hosts_save(parent_hostid, &hosts, host_proto, proxy_hostid, ipmi_authtype, ipmi_privilege,
				ipmi_username, ipmi_password, status, inventory_mode, &del_hostgroupids,
				&del_hostmacroids, &interfaces);

		/* linking of the templates */
		DBlld_templates_link(&hosts);

		DBlld_remove_lost_resources(&hosts, lifetime, lastcheck);

		DBlld_hosts_free(&hosts);

		groupids.values_num = 0;
		del_hostgroupids.values_num = 0;
		del_hostmacroids.values_num = 0;
	}
	DBfree_result(result);

	DBlld_hostmacros_free(&hostmacros);
	DBlld_interfaces_free(&interfaces);

	zbx_vector_ptr_destroy(&hostmacros);
	zbx_vector_ptr_destroy(&interfaces);
	zbx_vector_uint64_destroy(&del_hostmacroids);
	zbx_vector_uint64_destroy(&del_hostgroupids);
	zbx_vector_uint64_destroy(&groupids);
	zbx_vector_ptr_destroy(&hosts);

	zbx_free(ipmi_password);
	zbx_free(ipmi_username);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}
