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

			SCP_string value;
			stuff_string(value, F_NAME);

			// the engine adds the extension, so remove one if the author wrote it
			dot = value.rfind('.');
			if (dot != SCP_string::npos) {
				value.resize(dot);
			}
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)tolower(c); });

			// texture_info::LoadTexture rejects names of this length or more
			if (value.size() + 4 >= NAME_LENGTH) {
				Warning(LOCATION, "Texture name '%s' in '%s' is too long and will be ignored.", value.c_str(), filename);
				continue;
			}

			def.maps[kw.type] = std::move(value);
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
