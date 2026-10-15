#pragma once

#include "globalincs/pstypes.h"
#include "model/model.h"

// The definition of one texture slot. A slot holds a file name, a single color, or nothing.
struct texture_material_slot {
	SCP_string file;               // file name without extension; empty if the slot has no file
	bool has_color = false;        // true if the slot is a single color
	vec3d color = vmd_zero_vector; // red, green and blue in the range 0 to 1
	texture_scalar scalar;         // optional change over time; only the glow slot uses it

	bool empty() const { return file.empty() && !has_color; }
};

// A texture material definition, read from a .mat file in the maps directory.
// The name of the .mat file (without extension) is the texture name that a model refers to.
// The definition holds only file names. The engine loads the images when a model uses them.
struct texture_material_def {
	SCP_string name;                  // lowercase name of the .mat file, without extension
	texture_material_slot maps[TM_NUM_TYPES];
};

// Scan the maps directory for .mat files and parse them. Safe to call more than once.
void texture_material_init();

// Find the definition for a texture name. The name must be lowercase and without extension.
// Returns nullptr if no .mat file defines this name.
const texture_material_def* texture_material_find(const char* name);
