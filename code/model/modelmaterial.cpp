#include "model/modelmaterial.h"

#include "bmpman/bmpman.h"
#include "cfile/cfile.h"
#include "parse/parselo.h"

#include <algorithm>

namespace {

struct mat_keyword {
	int type;
	const char* keyword;
};

// One entry for each texture slot. The parser reads the keywords in this order.
const mat_keyword Mat_keywords[] = {
	{ TM_BASE_TYPE,       "$Diffuse:" },
	{ TM_GLOW_TYPE,       "$Glow:" },
	{ TM_SPECULAR_TYPE,   "$Specular:" },
	{ TM_NORMAL_TYPE,     "$Normal:" },
	{ TM_HEIGHT_TYPE,     "$Height:" },
	{ TM_MISC_TYPE,       "$Misc:" },
	{ TM_SPEC_GLOSS_TYPE, "$Reflect:" },
	{ TM_AMBIENT_TYPE,    "$AO:" },
};

SCP_unordered_map<SCP_string, texture_material_def> Texture_materials;

// Read the type name and the keys of a scalar. The function reads all keys of the type, also when one is not valid.
// If a key is not valid, the scalar stays at None.
void parse_scalar(texture_scalar& scalar, const char* keyword, const char* filename)
{
	SCP_string type_name;
	stuff_string(type_name, F_NAME);

	texture_scalar result;
	bool valid = true;

	auto read_period = [&](const char* key, float& value) {
		value = 0.0f;
		if (optional_string(key)) {
			stuff_float(&value);
		}
		if (value <= 0.0f) {
			error_display(1, "ScalarType %s of %s in '%s' needs %s greater than zero. The scalar is ignored.", type_name.c_str(), keyword, filename, key);
			valid = false;
		}
	};

	if (!stricmp(type_name.c_str(), "Sine") || !stricmp(type_name.c_str(), "Cosine")) {
		result.type = !stricmp(type_name.c_str(), "Sine") ? texture_scalar::Type::Sine : texture_scalar::Type::Cosine;
		read_period("+Period:", result.period);
	} else if (!stricmp(type_name.c_str(), "RampUp") || !stricmp(type_name.c_str(), "RampDown")) {
		result.type = !stricmp(type_name.c_str(), "RampUp") ? texture_scalar::Type::RampUp : texture_scalar::Type::RampDown;
		read_period("+Period:", result.period);
		if (optional_string("+Start:")) {
			stuff_float(&result.start);
		}
		if (optional_string("+End:")) {
			stuff_float(&result.end);
		}
		if (optional_string("+Repeats:")) {
			stuff_boolean(&result.repeats);
		}
	} else if (!stricmp(type_name.c_str(), "Blink")) {
		result.type = texture_scalar::Type::Blink;
		read_period("+OnPeriod:", result.on_period);
		read_period("+OffPeriod:", result.off_period);
	} else if (!stricmp(type_name.c_str(), "Quake")) {
		result.type = texture_scalar::Type::Quake;

		if (optional_string("+Pattern:")) {
			stuff_string(result.pattern, F_NAME);
		}
		if (optional_string("+Speed:")) {
			stuff_float(&result.speed);
		}

		std::transform(result.pattern.begin(), result.pattern.end(), result.pattern.begin(), [](unsigned char c) { return (char)tolower(c); });
		bool letters = std::all_of(result.pattern.begin(), result.pattern.end(), [](char c) { return c >= 'a' && c <= 'z'; });

		if (result.pattern.empty() || !letters) {
			error_display(1, "ScalarType Quake of %s in '%s' needs a +Pattern of the letters a to z. The scalar is ignored.", keyword, filename);
			valid = false;
		}
		if (result.speed <= 0.0f) {
			error_display(1, "ScalarType Quake of %s in '%s' needs a +Speed greater than zero. The scalar is ignored.", keyword, filename);
			valid = false;
		}
	} else {
		error_display(1, "Unknown ScalarType '%s' of %s in '%s'. The scalar is ignored.", type_name.c_str(), keyword, filename);
		valid = false;
	}

	if (valid) {
		scalar = std::move(result);
	}
}

void parse_texture_material(const char* filename)
{
	try {
		if (!cf_exists_full(filename, CF_TYPE_MAPS)) {
			return;
		}

		read_file_text(filename, CF_TYPE_MAPS);
		reset_parse();

		texture_material_def def;

		// the file name without the extension is the texture name
		def.name = filename;
		auto dot = def.name.rfind('.');
		if (dot != SCP_string::npos) {
			def.name.resize(dot);
		}
		std::transform(def.name.begin(), def.name.end(), def.name.begin(), [](unsigned char c) { return (char)tolower(c); });

		for (const auto& kw : Mat_keywords) {
			if (!optional_string(kw.keyword)) {
				continue;
			}

			texture_material_slot slot;

			// a slot has a file name on the keyword line, or a +Color value on the next line, but not both
			if (!check_for_eoln()) {
				stuff_string(slot.file, F_NAME);

				// the engine adds the extension, so remove one if the author wrote it
				auto ext = slot.file.rfind('.');
				if (ext != SCP_string::npos) {
					slot.file.resize(ext);
				}
				std::transform(slot.file.begin(), slot.file.end(), slot.file.begin(), [](unsigned char c) { return (char)tolower(c); });
			}

			bool has_color_value = false;
			vec3d color = vmd_zero_vector;
			if (optional_string("+Color:")) {
				has_color_value = true;
				stuff_parenthesized_vec3d(&color);
			}

			// an optional scalar changes the slot with time; the keys that follow +ScalarType belong to its type
			if (optional_string("+ScalarType:")) {
				parse_scalar(slot.scalar, kw.keyword, filename);

				if (slot.scalar.type != texture_scalar::Type::None && kw.type == TM_HEIGHT_TYPE) {
					Warning(LOCATION, "ScalarType of %s in '%s' has no effect. The model shader does not use a height map.", kw.keyword, filename);
				}
			}

			// the glow slot has two more settings; they stay at the defaults if the file does not set them
			if (kw.type == TM_GLOW_TYPE) {
				if (optional_string("+Intensity:")) {
					stuff_float(&slot.glow.intensity);
					if (slot.glow.intensity < 0.0f) {
						Warning(LOCATION, "+Intensity of %s in '%s' must not be negative. The value is set to zero.", kw.keyword, filename);
						slot.glow.intensity = 0.0f;
					}
				}
				if (optional_string("+SrgbMultiplier:")) {
					stuff_float(&slot.glow.srgb_multiplier);
					if (slot.glow.srgb_multiplier < 0.0f) {
						Warning(LOCATION, "+SrgbMultiplier of %s in '%s' must not be negative. The value is set to zero.", kw.keyword, filename);
						slot.glow.srgb_multiplier = 0.0f;
					}
				}
			}

			if (has_color_value && !slot.file.empty()) {
				error_display(1, "%s in '%s' has both a file name and a +Color value. Use only one. The slot is ignored.", kw.keyword, filename);
				continue;
			}

			if (has_color_value) {
				if (color.xyz.x < 0.0f || color.xyz.x > 1.0f || color.xyz.y < 0.0f || color.xyz.y > 1.0f || color.xyz.z < 0.0f || color.xyz.z > 1.0f) {
					Warning(LOCATION, "%s +Color in '%s' must be in the range 0 to 1. The value is clamped.", kw.keyword, filename);
					CLAMP(color.xyz.x, 0.0f, 1.0f);
					CLAMP(color.xyz.y, 0.0f, 1.0f);
					CLAMP(color.xyz.z, 0.0f, 1.0f);
				}

				slot.has_color = true;
				slot.color = color;
			} else if (slot.file.empty()) {
				error_display(1, "%s in '%s' has no file name and no +Color value. The slot is ignored.", kw.keyword, filename);
				continue;
			}

			// texture_info::LoadTexture rejects names of this length or more
			if (slot.file.size() + 4 >= NAME_LENGTH) {
				Warning(LOCATION, "Texture name '%s' in '%s' is too long and will be ignored.", slot.file.c_str(), filename);
				continue;
			}

			def.maps[kw.type] = std::move(slot);
		}

		Texture_materials.insert_or_assign(def.name, std::move(def));
	} catch (const parse::ParseException& e) {
		mprintf(("TEXTURE MATERIAL: Unable to parse '%s'! Error message = %s.\n", filename, e.what()));
	}
}

} // namespace

// Build a material from a texture name and the map files that exist for it
static texture_material_def make_material(const char* name, const SCP_unordered_set<SCP_string>* existing_files)
{
	texture_material_def def;
	def.name = name;
	def.maps[TM_BASE_TYPE].file = name;

	for (const auto& suffix : MODEL_TEXTURE_SUFFIXES) {
		SCP_string map_name = def.name + suffix.second;

		// without a list of files, assume that every map can exist
		if (existing_files == nullptr || existing_files->count(map_name) > 0) {
			def.maps[suffix.first].file = std::move(map_name);
		}
	}

	return def;
}

// Build a pseudomaterial for each texture file that the real materials do not cover
static void build_pseudo_materials()
{
	// the extensions of CF_TYPE_MAPS, see cfile.cpp; .mat files are not images
	static const char* const filters[] = { "*.pcx", "*.ani", "*.eff", "*.tga", "*.jpg", "*.png", "*.dds", "*.ktx" };

	// the real materials exclude the files that they name from this pass
	SCP_unordered_set<SCP_string> referenced;
	for (const auto& entry : Texture_materials) {
		for (const auto& slot : entry.second.maps) {
			if (!slot.file.empty()) {
				referenced.insert(slot.file);
			}
		}
	}

	SCP_unordered_set<SCP_string> files;
	for (const char* filter : filters) {
		SCP_vector<SCP_string> list;
		cf_get_file_list(list, CF_TYPE_MAPS, filter, CF_SORT_NONE);

		for (auto& name : list) {
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)tolower(c); });
			files.insert(std::move(name));
		}
	}

	size_t count = 0;
	for (const auto& name : files) {
		// a real material of this name replaces the files, and a real material that names the file uses it
		if (Texture_materials.count(name) > 0 || referenced.count(name) > 0) {
			continue;
		}

		// texture_info::LoadTexture rejects names of this length or more
		if (name.size() + model_texture_longest_suffix().size() + 4 >= NAME_LENGTH) {
			continue;
		}

		// a map file of another texture is part of that texture's material
		bool is_map_file = false;
		for (const auto& suffix : MODEL_TEXTURE_SUFFIXES) {
			if (name.size() > suffix.second.size() && name.compare(name.size() - suffix.second.size(), SCP_string::npos, suffix.second) == 0
				&& files.count(name.substr(0, name.size() - suffix.second.size())) > 0) {
				is_map_file = true;
				break;
			}
		}
		if (is_map_file) {
			continue;
		}

		auto def = make_material(name.c_str(), &files);
		def.pseudo = true;
		Texture_materials.emplace(name, std::move(def));
		count++;
	}

	mprintf(("TEXTURE MATERIAL: Built %zu pseudomaterials from %zu texture files.\n", count, files.size()));
}

void texture_material_init()
{
	Texture_materials.clear();
	parse_modular_table(NOX("*.mat"), parse_texture_material, CF_TYPE_MAPS);
	build_pseudo_materials();
}

const texture_material_def* texture_material_find_real(const char* name)
{
	if (name == nullptr) {
		return nullptr;
	}

	SCP_string lower_name = name;
	std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return (char)tolower(c); });

	auto def = texture_material_find(lower_name.c_str());
	return (def != nullptr && !def->pseudo) ? def : nullptr;
}

void texture_material_apply_replacement(model_texture_replace& dest, int tmap_index, int slot, const char* new_name, int new_id)
{
	const int base_index = tmap_index * TM_NUM_TYPES;

	const texture_material_def* def = (new_id == REPLACE_WITH_INVISIBLE || slot != TM_BASE_TYPE) ? nullptr : texture_material_find_real(new_name);
	if (def == nullptr) {
		dest[base_index + slot] = new_id;
		dest.scalars[base_index + slot] = texture_scalar();
		dest.glow_params[base_index + slot] = texture_glow_params();
		return;
	}

	texture_material_apply_def(dest, tmap_index, *def, nullptr);
}

void texture_material_apply_def(model_texture_replace& dest, int tmap_index, const texture_material_def& def, SCP_vector<int>* owned_color_bitmaps)
{
	const int base_index = tmap_index * TM_NUM_TYPES;

	for (int type = 0; type < TM_NUM_TYPES; type++) {
		const auto& map = def.maps[type];
		int handle;

		if (map.has_color) {
			if (owned_color_bitmaps != nullptr) {
				handle = model_create_color_bitmap(map.color);
				if (handle >= 0) {
					owned_color_bitmaps->push_back(handle);
				}
			} else {
				handle = model_get_shared_color_bitmap(map.color);
			}
		} else if (!map.file.empty()) {
			handle = bm_load_either(map.file.c_str());
		} else {
			// the new material does not define this map, so the old one must not show through
			handle = (type == TM_BASE_TYPE) ? REPLACE_WITH_INVISIBLE : REPLACE_WITH_NO_MAP;
		}

		if (handle < 0 && handle != REPLACE_WITH_INVISIBLE && handle != REPLACE_WITH_NO_MAP) {
			mprintf(("Could not load map %d of replacement material %s\n", type, def.name.c_str()));
			handle = (type == TM_BASE_TYPE) ? -1 : REPLACE_WITH_NO_MAP;
		}

		dest[base_index + type] = handle;
		dest.scalars[base_index + type] = map.scalar;
		dest.glow_params[base_index + type] = map.glow;
	}
}

texture_material_def texture_material_make_default(const char* name)
{
	return make_material(name, nullptr);
}

const texture_material_def* texture_material_find(const char* name)
{
	if (name == nullptr || Texture_materials.empty()) {
		return nullptr;
	}

	auto it = Texture_materials.find(name);
	return it != Texture_materials.end() ? &it->second : nullptr;
}
