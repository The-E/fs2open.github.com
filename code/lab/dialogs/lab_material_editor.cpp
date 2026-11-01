#include "lab/dialogs/lab_material_editor.h"

#include "lab/labv2_internal.h"
#include "model/modelmaterial.h"

namespace {

struct slot_label {
	int type;
	const char* label;
};

// One entry for each texture slot, in the order of the editor
const slot_label Slot_labels[] = {
	{ TM_BASE_TYPE,       "Diffuse" },
	{ TM_GLOW_TYPE,       "Glow" },
	{ TM_SPECULAR_TYPE,   "Specular" },
	{ TM_NORMAL_TYPE,     "Normal" },
	{ TM_HEIGHT_TYPE,     "Height" },
	{ TM_MISC_TYPE,       "Misc" },
	{ TM_SPEC_GLOSS_TYPE, "Reflect" },
	{ TM_AMBIENT_TYPE,    "AO" },
};

enum class slot_source : int { None, File, Color };

const char* const Source_names[] = { "None", "File", "Color" };
const char* const Scalar_names[] = { "None", "Sine" };

// The edit state of one slot. The text buffer holds the file name while the user types.
struct slot_edit {
	char file_buffer[NAME_LENGTH] = "";
};

// The edit state of one material of the model
struct material_edit {
	bool initialized = false;
	bool modified = false;
	texture_material_def original;               // the material that the model loaded with
	texture_material_def current;                // the material with the edits of the user
	slot_edit slots[TM_NUM_TYPES];

	// what the replacement table held before the first edit, so that Reset can restore it
	int saved_handles[TM_NUM_TYPES];
	texture_scalar saved_scalars[TM_NUM_TYPES];

	// bitmaps that the current material made for its color slots
	SCP_vector<int> owned_color_bitmaps;
};

// the state belongs to one model instance of the lab
int Edited_instance = -1;
SCP_vector<material_edit> Edits;

void release_owned_bitmaps(material_edit& edit)
{
	for (int handle : edit.owned_color_bitmaps) {
		model_release_color_bitmap(handle);
	}
	edit.owned_color_bitmaps.clear();
}

slot_source get_source(const texture_material_slot& slot)
{
	if (slot.has_color)
		return slot_source::Color;
	return slot.file.empty() ? slot_source::None : slot_source::File;
}

void init_edit(material_edit& edit, polymodel* pm, int tmap_index)
{
	const auto& tmap = pm->maps[tmap_index];

	// the material that the loader used for this texture
	auto def = texture_material_find(tmap.name.c_str());
	edit.original = (def != nullptr) ? *def : texture_material_make_default(tmap.name.c_str());
	edit.original.pseudo = (def != nullptr) && def->pseudo;

	// a file that the loader did not find is not part of the material that the model shows
	for (int type = 0; type < TM_NUM_TYPES; type++) {
		auto& slot = edit.original.maps[type];
		if (!slot.has_color && tmap.textures[type].GetOriginalTexture() < 0) {
			slot.file.clear();
		}
	}

	edit.current = edit.original;
	for (int type = 0; type < TM_NUM_TYPES; type++) {
		strcpy_s(edit.slots[type].file_buffer, edit.current.maps[type].file.c_str());
	}
	edit.initialized = true;
}

// Make the model instance show the current material of the edit
void apply_edit(material_edit& edit, polymodel_instance* pmi, int tmap_index)
{
	if (pmi->texture_replace == nullptr) {
		pmi->texture_replace = std::make_shared<model_texture_replace>();
	}
	auto& replace = *pmi->texture_replace;

	if (!edit.modified) {
		for (int type = 0; type < TM_NUM_TYPES; type++) {
			edit.saved_handles[type] = replace[tmap_index * TM_NUM_TYPES + type];
			edit.saved_scalars[type] = replace.scalars[tmap_index * TM_NUM_TYPES + type];
		}
		edit.modified = true;
	}

	// the new bitmaps replace the old ones, and then nothing uses the old ones
	SCP_vector<int> old_bitmaps = std::move(edit.owned_color_bitmaps);
	edit.owned_color_bitmaps.clear();

	texture_material_apply_def(replace, tmap_index, edit.current, &edit.owned_color_bitmaps);

	for (int handle : old_bitmaps) {
		model_release_color_bitmap(handle);
	}
}

void reset_edit(material_edit& edit, polymodel_instance* pmi, int tmap_index)
{
	if (edit.modified && pmi->texture_replace != nullptr) {
		auto& replace = *pmi->texture_replace;
		for (int type = 0; type < TM_NUM_TYPES; type++) {
			replace[tmap_index * TM_NUM_TYPES + type] = edit.saved_handles[type];
			replace.scalars[tmap_index * TM_NUM_TYPES + type] = edit.saved_scalars[type];
		}
	}

	release_owned_bitmaps(edit);
	edit.modified = false;
	edit.current = edit.original;
	for (int type = 0; type < TM_NUM_TYPES; type++) {
		strcpy_s(edit.slots[type].file_buffer, edit.current.maps[type].file.c_str());
	}
}

// Show the controls of one slot. Returns true if the user changed the slot.
bool build_slot_editor(texture_material_slot& slot, slot_edit& state, const char* id_prefix)
{
	bool changed = false;
	ImGui::PushID(id_prefix);

	int source = static_cast<int>(get_source(slot));
	if (ImGui::Combo("Source", &source, Source_names, IM_ARRAYSIZE(Source_names))) {
		switch (static_cast<slot_source>(source)) {
		case slot_source::None:
			slot.file.clear();
			slot.has_color = false;
			break;
		case slot_source::File:
			slot.has_color = false;
			slot.file = state.file_buffer;
			break;
		case slot_source::Color:
			slot.file.clear();
			slot.has_color = true;
			break;
		}
		changed = true;
	}

	if (slot.has_color) {
		float rgb[3] = { slot.color.xyz.x, slot.color.xyz.y, slot.color.xyz.z };
		if (ImGui::ColorEdit3("Color", rgb)) {
			slot.color.xyz.x = rgb[0];
			slot.color.xyz.y = rgb[1];
			slot.color.xyz.z = rgb[2];
			changed = true;
		}
	} else if (!slot.file.empty() || get_source(slot) == slot_source::File) {
		// the file is loaded when the user leaves the field, not after every key
		ImGui::InputText("File", state.file_buffer, IM_ARRAYSIZE(state.file_buffer));
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			SCP_string name = state.file_buffer;
			auto ext = name.rfind('.');
			if (ext != SCP_string::npos)
				name.resize(ext);
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)tolower(c); });

			slot.file = name;
			changed = true;
		}
	}

	int scalar_type = static_cast<int>(slot.scalar.type);
	if (ImGui::Combo("Scalar type", &scalar_type, Scalar_names, IM_ARRAYSIZE(Scalar_names))) {
		slot.scalar.type = static_cast<texture_scalar::Type>(scalar_type);
		changed = true;
	}

	if (slot.scalar.type == texture_scalar::Type::Sine) {
		if (ImGui::DragFloat("Period (seconds)", &slot.scalar.period, 0.1f, 0.01f, 600.0f)) {
			slot.scalar.period = std::max(slot.scalar.period, 0.01f);
			changed = true;
		}
	}

	ImGui::PopID();
	return changed;
}

} // namespace

void lab_material_editor_reset()
{
	for (auto& edit : Edits) {
		release_owned_bitmaps(edit);
	}
	Edits.clear();
	Edited_instance = -1;
}

void lab_material_editor_build(polymodel* pm, polymodel_instance* pmi)
{
	if (pm == nullptr || pmi == nullptr)
		return;

	// a different model instance has different edits
	if (Edited_instance != pmi->id) {
		lab_material_editor_reset();
		Edited_instance = pmi->id;
	}
	if (Edits.size() != static_cast<size_t>(pm->n_textures)) {
		lab_material_editor_reset();
		Edited_instance = pmi->id;
		Edits.resize(pm->n_textures);
	}

	with_TreeNode("Materials")
	{
		ImGui::TextWrapped("Changes here only affect the object that the lab shows. They are lost when the object changes.");

		for (int i = 0; i < pm->n_textures; i++) {
			auto& edit = Edits[i];
			if (!edit.initialized)
				init_edit(edit, pm, i);

			SCP_string label;
			sprintf(label, "%s%s###material%d", pm->maps[i].name.c_str(), edit.modified ? " (edited)" : "", i);

			with_TreeNode(label.c_str())
			{
				const char* kind = edit.original.pseudo ? "built from the texture files" : (texture_material_find_real(pm->maps[i].name.c_str()) != nullptr ? "defined by a .mat file" : "default");
				ImGui::Text("Material '%s', %s", pm->maps[i].name.c_str(), kind);

				bool changed = false;
				for (const auto& entry : Slot_labels) {
					SCP_string slot_id;
					sprintf(slot_id, "slot%d", entry.type);

					ImGui::SeparatorText(entry.label);
					changed |= build_slot_editor(edit.current.maps[entry.type], edit.slots[entry.type], slot_id.c_str());
				}

				if (changed) {
					apply_edit(edit, pmi, i);
				}

				ImGui::BeginDisabled(!edit.modified);
				if (ImGui::Button("Reset material")) {
					reset_edit(edit, pmi, i);
				}
				ImGui::EndDisabled();
			}
		}
	}
}
