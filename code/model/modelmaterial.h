#pragma once

#include "globalincs/pstypes.h"
#include "model/model.h"

// The definition of one texture slot. A slot holds a file name, a single color, or nothing.
struct texture_material_slot {
	SCP_string file;               // file name without extension; empty if the slot has no file
	bool has_color = false;        // true if the slot is a single color
	vec3d color = vmd_zero_vector; // red, green and blue in the range 0 to 1
	texture_scalar scalar;         // optional change over time
	texture_glow_params glow;      // only the glow slot uses it

	bool empty() const { return file.empty() && !has_color; }
};

// A texture material definition, read from a .mat file in the maps directory.
// The name of the .mat file (without extension) is the texture name that a model refers to.
// The definition holds only file names. The engine loads the images when a model uses them.
struct texture_material_def {
	SCP_string name;                  // lowercase name of the material, without extension
	bool pseudo = false;              // true if the engine built it from the image files, not from a .mat file
	texture_material_slot maps[TM_NUM_TYPES];
};

// Scan the maps directory. Parse the .mat files, then build a pseudomaterial for each texture that no .mat file
// defines or references. Both kinds go into the same table. Safe to call more than once.
void texture_material_init();

// Find the definition for a texture name. The name must be lowercase and without extension.
// Returns nullptr if no .mat file defines this name.
const texture_material_def* texture_material_find(const char* name);

// Build a material for a texture name that the table does not have, for example because the image files
// were not on disk at startup. It names the image file and the files with the standard suffixes.
// The caller owns it, and the table does not keep it.
texture_material_def texture_material_make_default(const char* name);

// Find a material that a .mat file defines. A pseudomaterial does not count. Returns nullptr if there is none.
const texture_material_def* texture_material_find_real(const char* name);

// Put a replacement texture into a model_texture_replace.
//   dest        the replacement table of a model instance
//   tmap_index  index of the model texture, that is the index into polymodel::maps
//   slot        the slot that the old texture name matched (a TM_* value)
//   new_name    name of the new texture, without extension
//   new_id      the bitmap that the caller loaded for new_name, REPLACE_WITH_INVISIBLE, or -1
// If the old name matched the diffuse slot and new_name is a material from a .mat file, the new material replaces all
// maps of the model texture: a map that the material does not define is removed, and a color or scalar of the
// material applies. In every other case only the matched slot changes, to new_id.
void texture_material_apply_replacement(model_texture_replace& dest, int tmap_index, int slot, const char* new_name, int new_id);

// Put every map of a material into a model_texture_replace, as texture_material_apply_replacement does for a real
// material. The material does not have to be in the table of materials.
// If owned_color_bitmaps is not null, a color slot gets its own bitmap, and the handle is added to the list.
// The caller must release these with model_release_color_bitmap when nothing uses them. Otherwise the colors share bitmaps.
void texture_material_apply_def(model_texture_replace& dest, int tmap_index, const texture_material_def& def, SCP_vector<int>* owned_color_bitmaps = nullptr);
