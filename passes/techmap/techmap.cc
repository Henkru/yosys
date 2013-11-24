/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "passes/techmap/stdcells.inc"

static void apply_prefix(std::string prefix, std::string &id)
{
	if (id[0] == '\\')
		id = prefix + "." + id.substr(1);
	else
		id = "$techmap" + prefix + "." + id;
}

static void apply_prefix(std::string prefix, RTLIL::SigSpec &sig, RTLIL::Module *module)
{
	for (size_t i = 0; i < sig.chunks.size(); i++) {
		if (sig.chunks[i].wire == NULL)
			continue;
		std::string wire_name = sig.chunks[i].wire->name;
		apply_prefix(prefix, wire_name);
		assert(module->wires.count(wire_name) > 0);
		sig.chunks[i].wire = module->wires[wire_name];
	}
}

std::map<std::pair<RTLIL::IdString, std::map<RTLIL::IdString, RTLIL::Const>>, RTLIL::Module*> techmap_cache;
std::map<RTLIL::Module*, bool> techmap_do_cache;

struct TechmapWireData {
	RTLIL::Wire *wire;
	RTLIL::SigSpec value;
};

typedef std::map<std::string, std::vector<TechmapWireData>> TechmapWires;

static TechmapWires techmap_find_special_wires(RTLIL::Module *module)
{
	TechmapWires result;

	if (module == NULL)
		return result;

	for (auto &it : module->wires) {
		const char *p = it.first.c_str();
		if (*p == '$')
			continue;

		const char *q = strrchr(p+1, '.');
		p = q ? q : p+1;

		if (!strncmp(p, "_TECHMAP_", 9)) {
			TechmapWireData record;
			record.wire = it.second;
			record.value = it.second;
			result[p].push_back(record);
			it.second->attributes["\\keep"] = RTLIL::Const(1);
			it.second->attributes["\\_techmap_special_"] = RTLIL::Const(1);
		}
	}

	if (!result.empty()) {
		SigMap sigmap(module);
		for (auto &it1 : result)
		for (auto &it2 : it1.second)
			sigmap.apply(it2.value);
	}

	return result;
}

static void techmap_module_worker(RTLIL::Design *design, RTLIL::Module *module, RTLIL::Cell *cell, RTLIL::Module *tpl, bool flatten_mode)
{
	log("Mapping `%s.%s' using `%s'.\n", RTLIL::id2cstr(module->name), RTLIL::id2cstr(cell->name), RTLIL::id2cstr(tpl->name));

	if (tpl->memories.size() != 0)
		log_error("Technology map yielded memories -> this is not supported.\n");

	if (tpl->processes.size() != 0)
		log_error("Technology map yielded processes -> this is not supported.\n");

	std::map<RTLIL::IdString, RTLIL::IdString> positional_ports;

	for (auto &it : tpl->wires) {
		if (it.second->port_id > 0)
			positional_ports[stringf("$%d", it.second->port_id)] = it.first;
		RTLIL::Wire *w = new RTLIL::Wire(*it.second);
		apply_prefix(cell->name, w->name);
		w->port_input = false;
		w->port_output = false;
		w->port_id = 0;
		if (it.second->get_bool_attribute("\\_techmap_special_"))
			w->attributes.clear();
		module->wires[w->name] = w;
		design->select(module, w);
	}

	SigMap port_signal_map;

	for (auto &it : cell->connections) {
		RTLIL::IdString portname = it.first;
		if (positional_ports.count(portname) > 0)
			portname = positional_ports.at(portname);
		if (tpl->wires.count(portname) == 0 || tpl->wires.at(portname)->port_id == 0) {
			if (portname.substr(0, 1) == "$")
				log_error("Can't map port `%s' of cell `%s' to template `%s'!\n", portname.c_str(), cell->name.c_str(), tpl->name.c_str());
			continue;
		}
		RTLIL::Wire *w = tpl->wires.at(portname);
		RTLIL::SigSig c;
		if (w->port_output) {
			c.first = it.second;
			c.second = RTLIL::SigSpec(w);
			apply_prefix(cell->name, c.second, module);
		} else {
			c.first = RTLIL::SigSpec(w);
			c.second = it.second;
			apply_prefix(cell->name, c.first, module);
		}
		if (c.second.width > c.first.width)
			c.second.remove(c.first.width, c.second.width - c.first.width);
		if (c.second.width < c.first.width)
			c.second.append(RTLIL::SigSpec(RTLIL::State::S0, c.first.width - c.second.width));
		assert(c.first.width == c.second.width);
#if 0
		// more conservative approach:
		// connect internal and external wires
		module->connections.push_back(c);
#else
		// approach that yields nicer outputs:
		// replace internal wires that are connected to external wires
		if (w->port_output)
			port_signal_map.add(c.second, c.first);
		else
			port_signal_map.add(c.first, c.second);
#endif
	}

	for (auto &it : tpl->cells) {
		RTLIL::Cell *c = new RTLIL::Cell(*it.second);
		if (!flatten_mode && c->type.substr(0, 2) == "\\$")
			c->type = c->type.substr(1);
		apply_prefix(cell->name, c->name);
		for (auto &it2 : c->connections) {
			apply_prefix(cell->name, it2.second, module);
			port_signal_map.apply(it2.second);
		}
		module->cells[c->name] = c;
		design->select(module, c);
	}

	for (auto &it : tpl->connections) {
		RTLIL::SigSig c = it;
		apply_prefix(cell->name, c.first, module);
		apply_prefix(cell->name, c.second, module);
		port_signal_map.apply(c.first);
		port_signal_map.apply(c.second);
		module->connections.push_back(c);
	}

	module->cells.erase(cell->name);
	delete cell;
}

static bool techmap_module(RTLIL::Design *design, RTLIL::Module *module, RTLIL::Design *map, std::set<RTLIL::Cell*> &handled_cells,
		const std::map<RTLIL::IdString, std::set<RTLIL::IdString>> &celltypeMap, bool flatten_mode)
{
	if (!design->selected(module))
		return false;

	bool log_continue = false;
	bool did_something = false;
	std::vector<std::string> cell_names;

	for (auto &cell_it : module->cells)
		cell_names.push_back(cell_it.first);

	for (auto &cell_name : cell_names)
	{
		if (module->cells.count(cell_name) == 0)
			continue;

		RTLIL::Cell *cell = module->cells[cell_name];

		if (!design->selected(module, cell) || handled_cells.count(cell) > 0)
			continue;

		if (celltypeMap.count(cell->type) == 0)
			continue;

		for (auto &tpl_name : celltypeMap.at(cell->type))
		{
			std::string derived_name = tpl_name;
			RTLIL::Module *tpl = map->modules[tpl_name];
			std::map<RTLIL::IdString, RTLIL::Const> parameters = cell->parameters;

			if (!flatten_mode) {
				for (auto conn : cell->connections) {
					if (conn.first.substr(0, 1) == "$")
						continue;
					if (tpl->wires.count(conn.first) > 0 && tpl->wires.at(conn.first)->port_id > 0)
						continue;
					if (!conn.second.is_fully_const() || parameters.count(conn.first) > 0)
						goto next_tpl;
					parameters[conn.first] = conn.second.as_const();
				}

				if (0) {
		next_tpl:
					continue;
				}
			}

			std::pair<RTLIL::IdString, std::map<RTLIL::IdString, RTLIL::Const>> key(tpl_name, parameters);
			if (techmap_cache.count(key) > 0) {
				tpl = techmap_cache[key];
			} else {
				if (cell->parameters.size() != 0) {
					derived_name = tpl->derive(map, parameters, cell->signed_parameters);
					tpl = map->modules[derived_name];
					log_continue = true;
				}
				techmap_cache[key] = tpl;
			}

			if (flatten_mode)
				techmap_do_cache[tpl] = true;

			if (techmap_do_cache.count(tpl) == 0)
			{
				bool keep_running = true;
				techmap_do_cache[tpl] = true;

				while (keep_running)
				{
					TechmapWires twd = techmap_find_special_wires(tpl);
					keep_running = false;

					for (auto &it : twd["_TECHMAP_FAIL_"]) {
						RTLIL::SigSpec value = it.value;
						if (value.is_fully_const() && value.as_bool()) {
							log("Not using module `%s' from techmap as it contains a %s marker wire with non-zero value %s.\n",
									derived_name.c_str(), RTLIL::id2cstr(it.wire->name), log_signal(value));
							techmap_do_cache[tpl] = false;
						}
					}

					if (!techmap_do_cache[tpl])
						break;

					for (auto &it : twd)
					{
						if (it.first.substr(0, 12) != "_TECHMAP_DO_" || it.second.empty())
							continue;

						auto &data = it.second.front();

						if (!data.value.is_fully_const())
							log_error("Techmap yielded config wire %s with non-const value %s.\n", RTLIL::id2cstr(data.wire->name), log_signal(data.value));

						tpl->wires.erase(data.wire->name);
						const char *p = data.wire->name.c_str();
						const char *q = strrchr(p+1, '.');
						q = q ? q : p+1;

						assert(!strncmp(q, "_TECHMAP_DO_", 12));
						std::string new_name = data.wire->name.substr(0, q-p) + "_TECHMAP_DONE_" + data.wire->name.substr(q-p+12);
						while (tpl->wires.count(new_name))
							new_name += "_";
						data.wire->name = new_name;
						tpl->add(data.wire);

						std::string cmd_string;
						std::vector<char> cmd_string_chars;
						std::vector<RTLIL::State> bits = data.value.as_const().bits;
						for (int i = 0; i < int(bits.size()); i += 8) {
							char ch = 0;
							for (int j = 0; j < 8 && i+j < int(bits.size()); j++)
								if (bits[i+j] == RTLIL::State::S1)
									ch |= 1 << j;
							if (ch != 0)
								cmd_string_chars.push_back(ch);
						}
						for (int i = int(cmd_string_chars.size())-1; i >= 0; i--)
							cmd_string += cmd_string_chars[i];

						RTLIL::Selection tpl_mod_sel(false);
						tpl_mod_sel.select(tpl);
						map->selection_stack.push_back(tpl_mod_sel);
						Pass::call(map, cmd_string);
						map->selection_stack.pop_back();

						keep_running = true;
						break;
					}
				}

				TechmapWires twd = techmap_find_special_wires(tpl);
				for (auto &it : twd) {
					if (it.first != "_TECHMAP_FAIL_" && it.first.substr(0, 12) != "_TECHMAP_DO_" && it.first.substr(0, 14) != "_TECHMAP_DONE_")
						log_error("Techmap yielded unknown config wire %s.\n", it.first.c_str());
					if (techmap_do_cache[tpl])
						for (auto &it2 : it.second)
							if (!it2.value.is_fully_const())
								log_error("Techmap yielded config wire %s with non-const value %s.\n", RTLIL::id2cstr(it2.wire->name), log_signal(it2.value));
				}
			}

			if (techmap_do_cache.at(tpl) == false)
				continue;

			if (log_continue) {
				log_header("Continuing TECHMAP pass.\n");
				log_continue = false;
			}

			techmap_module_worker(design, module, cell, tpl, flatten_mode);
			did_something = true;
			cell = NULL;
			break;
		}

		handled_cells.insert(cell);
	}

	if (log_continue) {
		log_header("Continuing TECHMAP pass.\n");
		log_continue = false;
	}

	return did_something;
}

struct TechmapPass : public Pass {
	TechmapPass() : Pass("techmap", "simple technology mapper") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    techmap [-map filename] [selection]\n");
		log("\n");
		log("This pass implements a very simple technology mapper that replaces cells in\n");
		log("the design with implementations given in form of a verilog or ilang source\n");
		log("file.\n");
		log("\n");
		log("    -map filename\n");
		log("        the library of cell implementations to be used.\n");
		log("        without this parameter a builtin library is used that\n");
		log("        transforms the internal RTL cells to the internal gate\n");
		log("        library.\n");
		log("\n");
		log("When a module in the map file has the 'techmap_celltype' attribute set, it will\n");
		log("match cells with a type that match the text value of this attribute.\n");
		log("\n");
		log("All wires in the modules from the map file matching the pattern _TECHMAP_*\n");
		log("or *._TECHMAP_* are special wires that are used to pass instructions from\n");
		log("the mapping module to the techmap command. At the moment the following spoecial\n");
		log("wires are supported:\n");
		log("\n");
		log("    _TECHMAP_FAIL_\n");
		log("        When this wire is set to a non-zero constant value, techmap will not\n");
		log("        use this module and instead try the next module with a matching\n");
		log("        'techmap_celltype' attribute.\n");
		log("\n");
		log("        When such a wire exists but does not have a constant value after all\n");
		log("        _TECHMAP_DO_* commands have been executed, an error is generated.\n");
		log("\n");
		log("    _TECHMAP_DO_*\n");
		log("        This wires are evaluated in alphabetical order. The constant text value\n");
		log("        of this wire is a yosys command (or sequence of commands) that is run\n");
		log("        by techmap on the module. A common use case is to run 'proc' on modules\n");
		log("        that are written using always-statements.\n");
		log("\n");
		log("        When such a wire has a non-constant value at the time it is to be\n");
		log("        evaluated, an error is produced. That means it is possible for such a\n");
		log("        wire to start out as non-constant and evaluate to a constant value\n");
		log("        during processing of other _TECHMAP_DO_* commands.\n");
		log("\n");
		log("When a module in the map file has a parameter where the according cell in the\n");
		log("design has a port, the module from the map file is only used if the port in\n");
		log("the design is connected to a constant value. The parameter is then set to the\n");
		log("constant value.\n");
		log("\n");
		log("See 'help extract' for a pass that does the opposite thing.\n");
		log("\n");
		log("See 'help flatten' for a pass that does flatten the design (which is\n");
		log("esentially techmap but using the design itself as map library).\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header("Executing TECHMAP pass (map to technology primitives).\n");
		log_push();

		std::vector<std::string> map_files;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-map" && argidx+1 < args.size()) {
				map_files.push_back(args[++argidx]);
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		RTLIL::Design *map = new RTLIL::Design;
		if (map_files.empty()) {
			FILE *f = fmemopen(stdcells_code, strlen(stdcells_code), "rt");
			Frontend::frontend_call(map, f, "<stdcells.v>", "verilog");
			fclose(f);
		} else
			for (auto &fn : map_files) {
				FILE *f = fopen(fn.c_str(), "rt");
				if (f == NULL)
					log_cmd_error("Can't open map file `%s'\n", fn.c_str());
				Frontend::frontend_call(map, f, fn, (fn.size() > 3 && fn.substr(fn.size()-3) == ".il") ? "ilang" : "verilog");
				fclose(f);
			}

		std::map<RTLIL::IdString, RTLIL::Module*> modules_new;
		for (auto &it : map->modules) {
			if (it.first.substr(0, 2) == "\\$")
				it.second->name = it.first.substr(1);
			modules_new[it.second->name] = it.second;
		}
		map->modules.swap(modules_new);

		std::map<RTLIL::IdString, std::set<RTLIL::IdString>> celltypeMap;
		for (auto &it : map->modules) {
			if (it.second->attributes.count("\\techmap_celltype") && !it.second->attributes.at("\\techmap_celltype").str.empty()) {
				celltypeMap[RTLIL::escape_id(it.second->attributes.at("\\techmap_celltype").str)].insert(it.first);
			} else
				celltypeMap[it.first].insert(it.first);
		}

		bool did_something = true;
		std::set<RTLIL::Cell*> handled_cells;
		while (did_something) {
			did_something = false;
			for (auto &mod_it : design->modules)
				if (techmap_module(design, mod_it.second, map, handled_cells, celltypeMap, false))
					did_something = true;
			if (did_something)
				design->check();
		}

		log("No more expansions possible.\n");
		techmap_cache.clear();
		techmap_do_cache.clear();
		delete map;
		log_pop();
	}
} TechmapPass;
 
struct FlattenPass : public Pass {
	FlattenPass() : Pass("flatten", "flatten design") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    flatten [selection]\n");
		log("\n");
		log("This pass flattens the design by replacing cells by their implementation. This\n");
		log("pass is very simmilar to the 'techmap' pass. The only difference is that this\n");
		log("pass is using the current design as mapping library.\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header("Executing FLATTEN pass (flatten design).\n");
		log_push();

		extra_args(args, 1, design);

		std::map<RTLIL::IdString, std::set<RTLIL::IdString>> celltypeMap;
		for (auto &it : design->modules)
			celltypeMap[it.first].insert(it.first);

		RTLIL::Module *top_mod = NULL;
		if (design->full_selection())
			for (auto &mod_it : design->modules)
				if (mod_it.second->get_bool_attribute("\\top"))
					top_mod = mod_it.second;

		bool did_something = true;
		std::set<RTLIL::Cell*> handled_cells;
		while (did_something) {
			did_something = false;
			if (top_mod != NULL) {
				if (techmap_module(design, top_mod, design, handled_cells, celltypeMap, true))
					did_something = true;
			} else {
				for (auto &mod_it : design->modules)
					if (techmap_module(design, mod_it.second, design, handled_cells, celltypeMap, true))
						did_something = true;
			}
		}

		log("No more expansions possible.\n");

		if (top_mod != NULL) {
			std::map<RTLIL::IdString, RTLIL::Module*> new_modules;
			for (auto &mod_it : design->modules)
				if (mod_it.second == top_mod) {
					new_modules[mod_it.first] = mod_it.second;
				} else {
					log("Deleting now unused module %s.\n", RTLIL::id2cstr(mod_it.first));
					delete mod_it.second;
				}
			design->modules.swap(new_modules);
		}

		techmap_cache.clear();
		techmap_do_cache.clear();
		log_pop();
	}
} FlattenPass;

