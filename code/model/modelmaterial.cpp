#include "model/modelmaterial.h"

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
				SCP_string type_name;
				stuff_string(type_name, F_NAME);

				if (!stricmp(type_name.c_str(), "Sine")) {
					float period = 0.0f;
					if (optional_string("+Period:")) {
						stuff_float(&period);
					}

					if (period > 0.0f) {
						slot.scalar.type = texture_scalar::Type::Sine;
						slot.scalar.period = period;
					} else {
						error_display(1, "ScalarType Sine of %s in '%s' needs a +Period greater than zero. The scalar is ignored.", kw.keyword, filename);
					}
				} else {
					error_display(1, "Unknown ScalarType '%s' of %s in '%s'. The scalar is ignored.", type_name.c_str(), kw.keyword, filename);
				}

				if (slot.scalar.type != texture_scalar::Type::None && kw.type == TM_HEIGHT_TYPE) {
					Warning(LOCATION, "ScalarType of %s in '%s' has no effect. The model shader does not use a height map.", kw.keyword, filename);
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

void texture_material_init()
{
	Texture_materials.clear();
	parse_modular_table(NOX("*.mat"), parse_texture_material, CF_TYPE_MAPS);
}

const texture_material_def* texture_material_find(const char* name)
{
	if (name == nullptr || Texture_materials.empty()) {
		return nullptr;
	}

	auto it = Texture_materials.find(name);
	return it != Texture_materials.end() ? &it->second : nullptr;
}
