/*
Copyright (C) 2013-2018 Draios Inc dba Sysdig.

This file is part of sysdig.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#include "sinsp.h"
#include "sinsp_int.h"
#include "filter.h"
#include "filterchecks.h"
#include "scap_source_interface.h"
#include "source_plugin.h"

extern sinsp_filter_check_list g_filterlist;

///////////////////////////////////////////////////////////////////////////////
// sinsp_source_plugin implementation
///////////////////////////////////////////////////////////////////////////////

sinsp_filter_check_plugin::sinsp_filter_check_plugin()
{
	m_info.m_name = "plugin";
	m_info.m_fields = NULL;
	m_info.m_nfields = 0;
	m_info.m_flags = filter_check_info::FL_NONE;
	m_cnt = 0;
}

void sinsp_filter_check_plugin::set_name(string name)
{
	m_info.m_name = name;
}

void sinsp_filter_check_plugin::set_fields(filtercheck_field_info* fields, uint32_t nfields)
{
	m_info.m_fields = fields;
	m_info.m_nfields = nfields;
}

sinsp_filter_check* sinsp_filter_check_plugin::allocate_new()
{
	sinsp_filter_check_plugin* np = new sinsp_filter_check_plugin();
	np->set_fields((filtercheck_field_info*)m_info.m_fields, m_info.m_nfields);
	np->set_name(m_info.m_name);
	np->m_id = m_id;
	np->m_source_info = m_source_info;

	return (sinsp_filter_check*)np;
}

uint8_t* sinsp_filter_check_plugin::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	//
	// Reject any event that is not generated by a plugin
	//
	if(evt->get_type() != PPME_PLUGINEVENT_E)
	{
		return NULL;
	}

	//
	// Reject events that have not generated by this plugin specifically
	//
	sinsp_evt_param *parinfo = evt->get_param(0);
	ASSERT(parinfo->m_len == sizeof(int32_t));
	uint32_t pgid = *(int32_t *)parinfo->m_val;
	if(pgid != m_id)
	{
		return NULL;
	}

	parinfo = evt->get_param(1);
	*len = 0;

	ppm_param_type type = m_info.m_fields[m_field_id].m_type;
	switch(type)
	{
	case PT_CHARBUF:
	{
		char* pret = m_source_info->extract_as_string(m_field_id, (uint8_t*)parinfo->m_val, parinfo->m_len);
		*len = strlen(pret);
		return (uint8_t*)pret;
	}
	default:
		ASSERT(false);
		throw sinsp_exception("plugin extract error unsupported field type " + to_string(type));
		break;
	}

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_source_plugin implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_source_plugin::sinsp_source_plugin(sinsp* inspector)
{
	m_inspector = inspector;
}

sinsp_source_plugin::~sinsp_source_plugin()
{
	if(m_source_info.destroy != NULL)
	{
		m_source_info.destroy(m_source_info.state);
	}
}

void sinsp_source_plugin::configure(source_plugin_info* plugin_info, char* config)
{
	char error[SCAP_LASTERR_SIZE];
	int init_res;

	ASSERT(m_inspector != NULL);
	ASSERT(plugin_info != NULL);

	m_source_info = *plugin_info;

	if(m_source_info.get_id == NULL)
	{
		throw sinsp_exception("invalid source plugin: 'get_id' method missing");
	}

	if(m_source_info.open == NULL)
	{
		throw sinsp_exception("invalid source plugin: 'open' method missing");
	}

	if(m_source_info.close == NULL)
	{
		throw sinsp_exception("invalid source plugin: 'close' method missing");
	}

	if(m_source_info.next == NULL)
	{
		throw sinsp_exception("invalid source plugin: 'next' method missing");
	}

	if(m_source_info.event_to_string == NULL)
	{
		throw sinsp_exception("invalid source plugin: 'event_to_string' method missing");
	}

	if(m_source_info.get_name == NULL)
	{
		throw sinsp_exception("invalid source plugin: 'get_name' method missing");
	}

	if(m_source_info.get_fields == NULL)
	{
		throw sinsp_exception("invalid source plugin: 'get_name' method missing");
	}

	if(m_source_info.extract_as_string == NULL)
	{
		throw sinsp_exception("invalid source plugin: 'extract_as_string' method missing");
	}

	//
	// Initialize the plugin
	//
	if(m_source_info.init != NULL)
	{
		m_source_info.state = m_source_info.init(config, error, &init_res);
		if(init_res != SCAP_SUCCESS)
		{
			throw sinsp_exception(error);
		}
	}

	m_id = m_source_info.get_id();
	m_source_info.id = m_id;

	//
	// Get JSON with the fields exported by the plugin, parse it and created our
	// list of fields.
	//
	std::string json(m_source_info.get_fields());
	SINSP_DEBUG("Parsing Container JSON=%s", json.c_str());
	Json::Value root;
	if(Json::Reader().parse(json, root) == false)
	{
		throw sinsp_exception(string("error in plugin ") + m_source_info.get_name() + ": get_fields returned an invalid JSON");
	}

	for(Json::Value::ArrayIndex j = 0; j != root.size(); j++)
	{
		filtercheck_field_info tf;
		tf.m_flags = EPF_NONE;

		const Json::Value &jvtype = root[j]["type"];
		string ftype = jvtype.asString();
		if(ftype == "")
		{
			throw sinsp_exception(string("error in plugin ") + m_source_info.get_name() + ": field JSON entry has no type");
		}
		const Json::Value &jvname = root[j]["name"];
		string fname = jvname.asString();
		if(fname == "")
		{
			throw sinsp_exception(string("error in plugin ") + m_source_info.get_name() + ": field JSON entry has no name");
		}
		const Json::Value &jvdesc = root[j]["desc"];
		string fdesc = jvdesc.asString();
		if(fdesc == "")
		{
			throw sinsp_exception(string("error in plugin ") + m_source_info.get_name() + ": field JSON entry has no desc");
		}

		strncpy(tf.m_name, fname.c_str(), sizeof(tf.m_name));
		strncpy(tf.m_description, fdesc.c_str(), sizeof(tf.m_description));
		tf.m_print_format = PF_DEC;
		if(ftype == "string")
		{
			tf.m_type = PT_CHARBUF;
		}
		else if(ftype == "integer")
		{
			tf.m_type = PT_INT64;
		}
		else if(ftype == "float")
		{
			tf.m_type = PT_DOUBLE;
		}
		else
		{
			throw sinsp_exception(string("error in plugin ") + m_source_info.get_name() + ": invalid field type " + ftype);
		}

		m_fields.push_back(tf);
	}

	sinsp_filter_check_plugin* fc = new sinsp_filter_check_plugin();
	fc->set_name(string("plugin_") + m_source_info.get_name());
	fc->set_fields((filtercheck_field_info*)&m_fields[0], 
		m_fields.size());
	fc->m_id = m_id;
	fc->m_source_info = &m_source_info;

	g_filterlist.add_filter_check(fc);
}

uint32_t sinsp_source_plugin::get_id()
{
	return m_id;
}