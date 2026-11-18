#include "grit_gdx_generator.h"

#include "grit_gdx_apply.h"
#include "grit_gdx_builder.h"
#include "grit_gdx_emitter.h"

#include "grit/compiler/grit_recording_code_generator.h"

#include "core/string/string_builder.h"
#include "core/object/class_db.h"
#include "grit/runtime/grit_runtime_access.h"

#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_cache.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/variant/variant.h"
#include "scene/main/scene_tree.h"

static String grit_gdx_platform_key() {
#if defined(MACOS_ENABLED)
	return "macos";
#elif defined(WINDOWS_ENABLED)
	return "windows";
#else
	return "linux";
#endif
}

static String grit_gdx_arch_key() {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
	return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
	return "x86_64";
#else
	return "unknown";
#endif
}

// The class icon of every generated class: the red GDScript gear, the same mark
// the archived `.gds` source uses.
static const char *const GRIT_GDX_CLASS_ICON_SVG = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\"><path fill=\"#ff5a5a\" d=\"m7 1-.565 2.258a5 5 0 0 0-.689.28L3.758 2.343 2.344 3.758l1.195 1.994a5 5 0 0 0-.285.685L1 7v2l2.258.564a5 5 0 0 0 .279.688l-1.193 1.99 1.414 1.414 1.994-1.195a5 5 0 0 0 .685.285L7 15h2l.564-2.258a5 5 0 0 0 .688-.28l1.99 1.194 1.414-1.414-1.195-1.994a5 5 0 0 0 .285-.685L15 9V7l-2.258-.564a5 5 0 0 0-.28-.688l1.194-1.99-1.414-1.414-1.994 1.195a5 5 0 0 0-.686-.285L9 1H7zm1 5a2 2 0 0 1 0 4 2 2 0 0 1 0-4z\"/></svg>\n";

String GritGdxGenerator::sanitize_identifier(const String &p_name) {
	String out;
	for (int i = 0; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
			out += String::chr(c);
		}
	}
	if (out.is_empty()) {
		return "Generated";
	}
	if (out[0] >= '0' && out[0] <= '9') {
		out = "_" + out;
	}
	return out;
}

String GritGdxGenerator::snake_case(const String &p_name) {
	// Mirrors godot-cpp's camel_to_snake(), which decides the generated header
	// names (node2d.hpp, a_star2d.hpp, http_request.hpp).
	String step1;
	for (int i = 0; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		if (i > 0 && c >= 'A' && c <= 'Z' && i + 1 < p_name.length() && p_name[i + 1] >= 'a' && p_name[i + 1] <= 'z') {
			step1 += "_";
		}
		step1 += String::chr(c);
	}
	String step2;
	for (int i = 0; i < step1.length(); i++) {
		const char32_t c = step1[i];
		if (i > 0 && c >= 'A' && c <= 'Z') {
			const char32_t previous = step1[i - 1];
			if ((previous >= 'a' && previous <= 'z') || (previous >= '0' && previous <= '9')) {
				step2 += "_";
			}
		}
		step2 += String::chr(c);
	}
	step2 = step2.replace("2_D", "2D").replace("3_D", "3D");
	return step2.to_lower();
}

String GritGdxGenerator::pascal_case(const String &p_text) {
	String out;
	bool capitalize = true;
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		const bool is_letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		const bool is_digit = c >= '0' && c <= '9';
		if (!is_letter && !is_digit) {
			capitalize = true;
			continue;
		}
		if (capitalize && c >= 'a' && c <= 'z') {
			out += String::chr(c - ('a' - 'A'));
		} else {
			out += String::chr(c);
		}
		capitalize = false;
	}
	return sanitize_identifier(out);
}

String GritGdxGenerator::suggested_class_name(const String &p_script_path) {
	return sanitize_identifier(pascal_case(p_script_path.get_file().get_basename()));
}

String GritGdxGenerator::class_name_for(const Ref<GDScript> &p_script) {
	String name = p_script->get_global_name();
	if (name.is_empty()) {
		name = p_script->get_gdextension_class_name();
	}
	return sanitize_identifier(name);
}

Error GritGdxGenerator::write_file(const String &p_path, const String &p_content) {
	// Preserve unchanged files: Build GDE must not force a full rebuild (SCons
	// keys off mtimes) when the generated sources are the same.
	if (FileAccess::exists(p_path)) {
		Error read_error = OK;
		const String existing = FileAccess::get_file_as_string(p_path, &read_error);
		if (read_error == OK && existing == p_content) {
			return OK;
		}
	}
	const Error error = DirAccess::make_dir_recursive_absolute(p_path.get_base_dir());
	ERR_FAIL_COND_V_MSG(error != OK, error, vformat("Grit: could not create \"%s\".", p_path.get_base_dir()));
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_CANT_CREATE, vformat("Grit: could not write \"%s\".", p_path));
	file->store_string(p_content);
	return OK;
}

Error GritGdxGenerator::collect(const String &p_script_path, const String &p_extension_directory, GritGdxClass &r_class, String &r_error, const String &p_record_path) {
	Error error = OK;
	const String path = p_script_path.begins_with("res://") || p_script_path.begins_with("user://") ? p_script_path : ProjectSettings::get_singleton()->localize_path(p_script_path);
	const String record_path = p_record_path.is_empty() ? path : p_record_path;
	Ref<GDScript> script = GDScriptCache::get_full_script(path, error);
	if (error != OK || script.is_null()) {
		r_error = vformat("could not load \"%s\"", record_path);
		return error != OK ? error : ERR_CANT_OPEN;
	}

	// Never compile or otherwise touch the resource the editor has open: build a
	// private copy under a synthetic path (still ending in `.gde` so the class
	// name exemptions apply) and record that.
	Ref<GDScript> build_script;
	build_script.instantiate();
	build_script->set_source_code(script->get_source_code());
	build_script->set_path(path + ".__grit_build.gde");
	GritScriptRecording recording(build_script.ptr());
	error = build_script->reload(true);
	if (error != OK) {
		r_error = vformat("could not compile \"%s\"", record_path);
		return error;
	}

	// The generated class always takes the script's name. A `.gde` source must
	// declare class_name; legacy `.gd` sources may still carry the annotation.
	const bool is_gdextension_source = record_path.ends_with(".gde");
	const String global_name = build_script->get_global_name();
	const String class_name = !global_name.is_empty() ? String(global_name) : (is_gdextension_source ? String() : build_script->get_gdextension_class_name());
	if (class_name.is_empty()) {
		r_error = is_gdextension_source ? vformat("\"%s\" has no class_name; the generated class always takes the script's class_name.", record_path) : vformat("\"%s\" has no class_name; add one (the generated class takes that name).", record_path);
		return ERR_INVALID_PARAMETER;
	}
	if (!class_name.is_valid_identifier()) {
		r_error = vformat("the class name \"%s\" in \"%s\" is not a valid identifier.", class_name, record_path);
		return ERR_INVALID_PARAMETER;
	}
	if (GritGdxEmitter::is_reserved_class_name(class_name)) {
		r_error = vformat("the class name \"%s\" in \"%s\" cannot be a generated C++ class (C++ keyword, the godot namespace, or an engine virtual name); choose another", class_name, record_path);
		return ERR_INVALID_PARAMETER;
	}
	if (ClassDB::class_exists(class_name)) {
		const ClassDB::APIType api = ClassDB::get_api_type(class_name);
		if (api != ClassDB::API_EXTENSION && api != ClassDB::API_EDITOR_EXTENSION) {
			r_error = vformat("the class name \"%s\" in \"%s\" hides the native class \"%s\"; choose another name.", class_name, record_path, class_name);
			return ERR_INVALID_PARAMETER;
		}
	}

	r_class = GritGdxClass();
	r_class.script_path = record_path;
	r_class.class_name = class_name_for(build_script);
	r_class.base_type = build_script->get_instance_base_type();
	r_class.is_tool = build_script->is_tool();
	build_script->set_path(String());
	if (r_class.base_type == StringName()) {
		r_error = "the script has no native base class (does it extend another script?)";
		return ERR_INVALID_PARAMETER;
	}
	if (!ClassDB::class_exists(r_class.base_type)) {
		r_error = vformat("\"%s\" is not a native class", r_class.base_type);
		return ERR_INVALID_PARAMETER;
	}
	// A script whose parent is another script would lose the parent's methods:
	// the generated class derives from the native base, not from the parent.
	const Ref<Script> base_script = script->get_base_script();
	if (base_script.is_valid()) {
		r_error = vformat("\"%s\" extends the script \"%s\"; a generated class can only derive from a native class, so convert a script whose parent is native (or fold the parent's methods into it)", path, base_script->get_path());
		return ERR_INVALID_PARAMETER;
	}

	// A script already extending a generated class is the migration story, not a
	// new class: attaching it to a node of that class keeps the native code.
	const ClassDB::APIType base_api = ClassDB::get_api_type(r_class.base_type);
	if (base_api == ClassDB::API_EXTENSION || base_api == ClassDB::API_EDITOR_EXTENSION) {
		r_error = vformat("\"%s\" already extends the GDExtension class \"%s\"; attach it to a %s node instead of converting it again", path, r_class.base_type, r_class.base_type);
		return ERR_INVALID_PARAMETER;
	}

	// Script variables become native fields.
	const HashMap<StringName, GritRuntimeAccess::MemberInfo> &members = GritRuntimeAccess::get_member_indices(script.ptr());
	for (const KeyValue<StringName, GritRuntimeAccess::MemberInfo> &entry : members) {
		GritGdxProperty property;
		property.name = String(entry.key);
		property.field = "member_" + sanitize_identifier(property.name);
		property.getter = "get_" + sanitize_identifier(property.name);
		property.setter = "set_" + sanitize_identifier(property.name);
		property.info = entry.value.property_info;
		property.type = entry.value.data_type;
		property.is_export = (property.info.usage & PROPERTY_USAGE_EDITOR) != 0;
		if (entry.value.data_type.kind == GDScriptDataType::BUILTIN && grit_is_unboxed_type(entry.value.data_type.builtin_type)) {
			property.storage = entry.value.data_type.builtin_type;
		}
		Variant default_value;
		if (script->get_property_default_value(entry.key, default_value)) {
			property.default_value = default_value;
			property.has_default = true;
		}
		const int property_index = r_class.properties.size();
		r_class.properties.push_back(property);
		r_class.property_by_member_index.insert(entry.value.index, property_index);
	}

	// Signals become ClassDB signals.
	List<MethodInfo> signal_list;
	script->get_script_signal_list(&signal_list);
	for (const MethodInfo &method : signal_list) {
		GritGdxSignal signal;
		signal.name = method.name;
		for (const PropertyInfo &argument : method.arguments) {
			signal.arguments.push_back(argument);
		}
		r_class.signals.push_back(signal);
	}

	// Defaults of non-exported members live in the implicit constructor.
	for (const GritFunction &initializer : recording.functions) {
		if (initializer.name != StringName("@implicit_new") || initializer.blocks.is_empty()) {
			continue;
		}
		const GritBlock &block = initializer.blocks[0];
		HashMap<int, Variant> literals;
		for (const GritStatement &statement : block.statements) {
			const auto operand_value = [&](const GritOperand &p_operand, Variant &r_value) {
				if (p_operand.kind == GritOperand::Kind::CONSTANT) {
					r_value = initializer.constant_values[p_operand.index];
					return true;
				}
				if (p_operand.kind == GritOperand::Kind::VARIABLE) {
					const Variant *literal = literals.getptr(p_operand.index);
					if (literal) {
						r_value = *literal;
						return true;
					}
				}
				return false;
			};

			switch (statement.opcode) {
				case GritOpcode::CONSTRUCT_ARRAY:
				case GritOpcode::CONSTRUCT_DICTIONARY: {
					if (statement.target.kind != GritOperand::Kind::VARIABLE) {
						break;
					}
					Array values;
					bool is_constant = true;
					for (const GritOperand &operand : statement.operands) {
						Variant value;
						if (!operand_value(operand, value)) {
							is_constant = false;
							break;
						}
						values.push_back(value);
					}
					if (!is_constant) {
						break;
					}
					if (statement.opcode == GritOpcode::CONSTRUCT_ARRAY) {
						literals.insert(statement.target.index, values);
					} else {
						Dictionary dictionary;
						for (int i = 0; i + 1 < values.size(); i += 2) {
							dictionary[values[i]] = values[i + 1];
						}
						literals.insert(statement.target.index, dictionary);
					}
				} break;
				case GritOpcode::ASSIGN:
				case GritOpcode::ASSIGN_TYPED: {
					if (statement.target.kind != GritOperand::Kind::MEMBER || statement.operands.is_empty()) {
						break;
					}
					Variant value;
					if (!operand_value(statement.operands[0], value)) {
						break;
					}
					const int *property_index = r_class.property_by_member_index.getptr(statement.target.index);
					if (!property_index) {
						break;
					}
					GritGdxProperty &property = r_class.properties[*property_index];
					if (!property.has_default) {
						property.default_value = value;
						property.has_default = true;
					}
				} break;
				default:
					break;
			}
		}
	}

	// Functions the emitter can handle. Lambdas are kept too: a call site creates
	// a callable for them, so they are emitted as internal methods rather than
	// bound as script methods (see GritGdxEmitter).
	for (const GritFunction &function : recording.functions) {
		if (String(function.name).begins_with("@")) {
			GritGdxSkipped skipped;
			skipped.name = String(function.name);
			skipped.reason = "implicit GDScript accessor";
			r_class.skipped.push_back(skipped);
			continue;
		}
		if (function.registry_key.contains(">")) {
			// A lambda is not a script method: an unsupported one is reported under
			// an internal name so no stub is generated for it, and the creation site
			// reports the method that uses it.
			if (!function.is_supported()) {
				GritGdxSkipped skipped;
				skipped.name = vformat("@lambda:%d", function.initial_line);
				skipped.reason = function.unsupported_reason;
				r_class.skipped.push_back(skipped);
			} else {
				r_class.functions.push_back(function);
			}
			continue;
		}
		if (!function.is_supported()) {
			GritGdxSkipped skipped;
			skipped.name = String(function.name);
			skipped.reason = function.unsupported_reason;
			r_class.skipped.push_back(skipped);
			continue;
		}
		r_class.functions.push_back(function);
	}

	r_error = String();
	return OK;
}


String GritGdxGenerator::class_file_stem(const String &p_class_name) {
	return snake_case(p_class_name);
}

LocalVector<GritGdxSkipped> GritGdxGenerator::collect_skipped(const GritGdxClass &p_class) {
	LocalVector<GritGdxSkipped> skipped;
	for (const GritGdxSkipped &entry : GritGdxEmitter::skipped_functions(p_class)) {
		skipped.push_back(entry);
	}
	for (const GritGdxSkipped &entry : p_class.skipped) {
		// "@implicit_new" and implicit accessors are GDScript internals.
		if (String(entry.name).begins_with("@")) {
			continue;
		}
		skipped.push_back(entry);
	}
	return skipped;
}

Error GritGdxGenerator::write_registry(const String &p_extension_directory, const GritGdxManifest &p_manifest) {
	const String name = GritGdxManifest::extension_name(p_extension_directory);
	StringBuilder registry;
	for (const GritGdxManifestEntry &entry : p_manifest.classes) {
		registry.append("#include \"" + class_file_stem(entry.class_name) + ".h\"\n");
	}
	registry.append("\n#include <godot_cpp/core/class_db.hpp>\n");
	registry.append("#include <godot_cpp/godot.hpp>\n\n");
	registry.append("using namespace godot;\n\n");
	registry.append("static void " + name + "_initialize(ModuleInitializationLevel p_level) {\n");
	registry.append("\tif (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {\n");
	registry.append("\t\treturn;\n");
	registry.append("\t}\n");
	for (const GritGdxManifestEntry &entry : p_manifest.classes) {
		registry.append("\tGDREGISTER_CLASS(" + entry.class_name + ");\n");
	}
	registry.append("}\n\n");
	registry.append("static void " + name + "_terminate(ModuleInitializationLevel p_level) {\n");
	registry.append("\tif (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {\n");
	registry.append("\t\treturn;\n");
	registry.append("\t}\n");
	registry.append("}\n\n");
	registry.append("extern \"C\" {\n");
	registry.append("GDExtensionBool GDE_EXPORT " + name + "_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {\n");
	registry.append("\tGDExtensionBinding::InitObject init_object(p_get_proc_address, p_library, r_initialization);\n");
	registry.append("\tinit_object.register_initializer(" + name + "_initialize);\n");
	registry.append("\tinit_object.register_terminator(" + name + "_terminate);\n");
	registry.append("\tinit_object.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);\n");
	registry.append("\treturn init_object.init();\n");
	registry.append("}\n");
	registry.append("}\n");
	return write_file(p_extension_directory.path_join("src").path_join("register_classes.gen.cpp"), registry.as_string());
}

Error GritGdxGenerator::write_extension_files(const String &p_extension_directory, const String &p_extra_class) {
	const String directory = p_extension_directory.ends_with("/") ? p_extension_directory.left(-1) : p_extension_directory;
	const String name = GritGdxManifest::extension_name(directory);
	GritGdxManifest manifest;
	const Error load_error = manifest.load(directory);
	ERR_FAIL_COND_V_MSG(load_error != OK, load_error, "Grit: could not read the extension manifest.");

	Error write_error = write_registry(directory, manifest);
	ERR_FAIL_COND_V_MSG(write_error != OK, write_error, "Grit: could not write the class registry.");

	// Build script: a real GDExtension library per platform/config.
	StringBuilder build;
	build.append("#!/usr/bin/env python\n\n");
	build.append("import os\n\n");
	build.append("godot_cpp = ARGUMENTS.get(\"godot_cpp\") or os.environ.get(\"GRIT_GDX_GODOT_CPP\", \"godot-cpp\")\n");
	build.append("env = SConscript(os.path.join(godot_cpp, \"SConstruct\"))\n");
	build.append("env.Append(CPPPATH=[\"src/\"])\n");
	build.append("env[\"SHLIBPREFIX\"] = \"\"\n\n");
	build.append("sources = Glob(\"src/*.cpp\")\n\n");
	build.append("name = \"" + name + "\"\n");
	build.append("arch = env[\"arch\"]\n");
	build.append("config = \"debug\" if env[\"target\"] in (\"debug\", \"template_debug\", \"editor\") else \"release\"\n");
	build.append("extension = {\"macos\": \".dylib\", \"windows\": \".dll\"}.get(env[\"platform\"], \".so\")\n");
	// SCons keeps a target whose suffix matches $SHLIBSUFFIX and refuses one that
	// does not, so the target keeps the full artifact name. Windows only accepts
	// this when SHLIBSUFFIX is set (clearing it broke the build there).
	build.append("env[\"SHLIBSUFFIX\"] = extension\n");
	build.append("artifact = \"bin/lib%s.%s.%s.%s%s\" % (name, env[\"platform\"], arch, config, extension)\n\n");
	build.append("library = env.SharedLibrary(target=artifact, source=sources)\n");
	build.append("env.Default(library)\n");
	write_error = write_file(directory.path_join("SConstruct"), build.as_string());
	ERR_FAIL_COND_V_MSG(write_error != OK, write_error, "Grit: could not write the build script.");

	const String platform = grit_gdx_platform_key();
	const String arch = grit_gdx_arch_key();
	const String extension = platform == "macos" ? ".dylib" : (platform == "windows" ? ".dll" : ".so");
	String relative_output = directory;
	if (relative_output.begins_with("res://")) {
		relative_output = relative_output.substr(6);
	} else {
		const String project_root = ProjectSettings::get_singleton()->get_resource_path();
		if (relative_output.begins_with(project_root)) {
			relative_output = relative_output.substr(project_root.length());
		}
	}
	relative_output = relative_output.strip_edges();
	if (relative_output.begins_with("/")) {
		relative_output = relative_output.substr(1);
	}

	StringBuilder descriptor;
	descriptor.append("[configuration]\n\n");
	descriptor.append("entry_symbol = \"" + name + "_library_init\"\n");
	// Lets the editor pick up a rebuilt library (adding a class to an existing
	// extension) without restarting; ignored outside editor builds.
	descriptor.append("reloadable = true\n");
	descriptor.append("compatibility_minimum = \"4.7\"\n\n");
	descriptor.append("[libraries]\n\n");
	descriptor.append(platform + ".debug = \"res://" + relative_output + "/bin/lib" + name + "." + platform + "." + arch + ".debug" + extension + "\"\n");
	descriptor.append(platform + ".release = \"res://" + relative_output + "/bin/lib" + name + "." + platform + "." + arch + ".release" + extension + "\"\n");

	// The generated class is the node's type, so its class icon is the red script:
	// the editor reads [icons] from the descriptor. The manifest lists the classes
	// of the extension; the class being generated may not be saved there yet.
	GritGdxManifest icon_manifest;
	Vector<String> icon_classes;
	if (icon_manifest.load(directory) == OK) {
		for (const GritGdxManifestEntry &entry : icon_manifest.classes) {
			if (!entry.class_name.is_empty()) {
				icon_classes.push_back(entry.class_name);
			}
		}
	}
	if (!p_extra_class.is_empty() && !icon_classes.has(p_extra_class)) {
		icon_classes.push_back(p_extra_class);
	}
	if (!icon_classes.is_empty()) {
		descriptor.append("\n[icons]\n\n");
		for (const String &class_name : icon_classes) {
			descriptor.append(class_name + " = \"icon.svg\"\n");
		}
		const Error icon_error = write_file(directory.path_join("icon.svg"), GRIT_GDX_CLASS_ICON_SVG);
		ERR_FAIL_COND_V_MSG(icon_error != OK, icon_error, "Grit: could not write the extension icon.");
	}
	// Publish the descriptor only once its library exists. The editor's file scan
	// would otherwise try to load the not-yet-built library and log "Can't open
	// dynamic library" during the first build.
	const String descriptor_path = directory.path_join(name + ".gdextension");
	const String library = GritGdxBuilder::library_path(directory, name, "editor");
	if (!FileAccess::exists(library)) {
		if (FileAccess::exists(descriptor_path)) {
			DirAccess::remove_absolute(descriptor_path);
		}
		return OK;
	}
	return write_file(descriptor_path, descriptor.as_string());
}

Error GritGdxGenerator::generate_class(const String &p_extension_directory, const String &p_script_path, GritGdxClass &r_class, String &r_report, const String &p_record_path) {
	const String directory = p_extension_directory.ends_with("/") ? p_extension_directory.left(-1) : p_extension_directory;
	String error_message;
	const Error error = collect(p_script_path, directory, r_class, error_message, p_record_path);
	if (error != OK) {
		r_report = "Grit: " + error_message + ".";
		return error;
	}

	const String source_directory = directory.path_join("src");
	const String stem = class_file_stem(r_class.class_name);
	Error write_error = write_file(source_directory.path_join(stem + ".h"), GritGdxEmitter::emit_header(r_class));
	ERR_FAIL_COND_V_MSG(write_error != OK, write_error, "Grit: could not write the generated header.");
	write_error = write_file(source_directory.path_join(stem + ".cpp"), GritGdxEmitter::emit_source(r_class));
	ERR_FAIL_COND_V_MSG(write_error != OK, write_error, "Grit: could not write the generated class.");
	write_error = write_file(source_directory.path_join(GritGdxEmitter::helpers_file_name()), GritGdxEmitter::emit_helpers(r_class));
	ERR_FAIL_COND_V_MSG(write_error != OK, write_error, "Grit: could not write the generated helpers.");

	GritGdxManifest manifest;
	const Error load_error = manifest.load(directory);
	ERR_FAIL_COND_V_MSG(load_error != OK, load_error, "Grit: could not read the extension manifest.");
	// The source's class_name is authoritative: never rename it. Another script
	// using the same name is an error; an entry left over from a previous
	// class_name of this same source is dropped.
	for (int i = manifest.classes.size() - 1; i >= 0; i--) {
		const GritGdxManifestEntry &existing = manifest.classes[i];
		if (existing.script_path != r_class.script_path) {
			if (existing.class_name == r_class.class_name) {
				r_report = vformat("Grit: class \"%s\" is already generated from \"%s\"; class names must be unique.", r_class.class_name, existing.script_path);
				return ERR_INVALID_PARAMETER;
			}
			continue;
		}
		if (existing.class_name != r_class.class_name) {
			manifest.classes.remove_at(i);
		}
	}
	GritGdxManifestEntry entry;
	entry.class_name = r_class.class_name;
	entry.script_path = r_class.script_path;
	entry.base_type = String(r_class.base_type);
	entry.is_tool = r_class.is_tool;
	// Keep the archive recorded by a previous conversion of the same script.
	if (const GritGdxManifestEntry *existing = manifest.find_script(r_class.script_path)) {
		entry.archived_script = existing->archived_script;
	}
	manifest.upsert(entry);
	const Error save_error = manifest.save(directory);
	ERR_FAIL_COND_V_MSG(save_error != OK, save_error, "Grit: could not write the extension manifest.");

	// Remove sources of classes that are no longer in the manifest.
	{
		Vector<String> keep;
		for (const GritGdxManifestEntry &known : manifest.classes) {
			keep.push_back(class_file_stem(known.class_name) + ".h");
			keep.push_back(class_file_stem(known.class_name) + ".cpp");
		}
		keep.push_back(GritGdxEmitter::helpers_file_name());
		keep.push_back(String("register_classes.gen.cpp"));
		Ref<DirAccess> sources = DirAccess::open(source_directory);
		if (sources.is_valid()) {
			for (const String &file : sources->get_files()) {
				const String file_extension = file.get_extension();
				if ((file_extension == "h" || file_extension == "cpp") && !keep.has(file)) {
					sources->remove(file);
				}
			}
		}
	}

	write_error = write_extension_files(directory, r_class.class_name);
	ERR_FAIL_COND_V_MSG(write_error != OK, write_error, "Grit: could not write the extension files.");

	const LocalVector<GritGdxSkipped> skipped = collect_skipped(r_class);
	int exported_count = 0;
	for (const GritGdxProperty &property : r_class.properties) {
		if (property.is_export) {
			exported_count++;
		}
	}
	StringBuilder report;
	report.append(vformat("Grit: generated GDExtension class \"%s\" in \"%s\" from \"%s\"\n", r_class.class_name, directory, r_class.script_path));
	report.append(vformat("  base class: %s\n", r_class.base_type));
	report.append(vformat("  properties: %d (%d exported)\n", r_class.properties.size(), exported_count));
	report.append(vformat("  signals: %d\n", r_class.signals.size()));
	report.append(vformat("  methods: %d converted, %d skipped\n", r_class.functions.size() - GritGdxEmitter::skipped_functions(r_class).size(), skipped.size()));
	if (!skipped.is_empty()) {
		report.append("  not converted:\n");
		for (const GritGdxSkipped &entry : skipped) {
			report.append(vformat("    %s: %s\n", entry.name, entry.reason));
		}
	}
	report.append(vformat("  extension: %s (%d class(es))\n", directory, manifest.classes.size()));
	r_report = report.as_string();
	return OK;
}

void GritGdxGenerator::run_from_command_line() {
	const List<String> arguments = OS::get_singleton()->get_cmdline_args();
	String generate_script;
	bool migrate = false;
	bool build = false;
	bool project_check = false;
	for (const List<String>::Element *element = arguments.front(); element; element = element->next()) {
		const String &argument = element->get();
		const auto next_value = [&]() -> String {
			const List<String>::Element *next = element->next();
			if (next) {
				element = next;
				return next->get();
			}
			return String();
		};
		if (argument == "--grit-gdx-generate") {
			generate_script = next_value();
		} else if (argument == "--grit-gdx-build") {
			build = true;
		} else if (argument == "--grit-gdx-migrate") {
			migrate = true;
		} else if (argument == "--grit-gdx-project-check") {
			project_check = true;
		}
	}

	String report;
	Error error = OK;
	if (project_check) {
		String refresh;
		error = GritGdxApply::refresh_project_extension(refresh);
		String consolidation;
		GritGdxApply::consolidate_project_extension(consolidation);
		report = refresh + consolidation;
	} else if (!generate_script.is_empty()) {
		error = GritGdxApply::refresh_project_extension(report);
		if (error == OK && build) {
			const String directory = GritGdxApply::project_extension_directory();
			String library;
			String build_error;
			error = GritGdxApply::build(directory, "editor", GritGdxApply::Progress(), library, build_error);
			report += error == OK ? vformat("  library: %s\n", library) : "  " + build_error;
			if (error == OK) {
				String consolidation;
				GritGdxApply::consolidate_project_extension(consolidation);
				report += consolidation;
			}
		}
	} else if (migrate) {
		Vector<String> migrated;
		error = GritGdxApply::migrate_legacy(migrated, report);
	} else {
		return;
	}
	print_line(report);
	OS::get_singleton()->set_exit_code(error == OK ? 0 : 1);
	SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
	if (tree) {
		tree->quit(error == OK ? 0 : 1);
	}
}
