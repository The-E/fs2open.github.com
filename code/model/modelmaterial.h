#pragma once

#include "globalincs/pstypes.h"
#include "model/model.h"

// A texture material definition, read from a .mat file in the maps directory.
// The name of the .mat file (without extension) is the texture name that a model refers to.
// The definition holds only file names. The engine loads the images when a model uses them.
struct texture_material_def {
	SCP_string name;                  // lowercase name of the .mat file, without extension
	SCP_string maps[TM_NUM_TYPES];    // file names without extension; an empty string means "no map"
};

// Scan the maps directory for .mat files and parse them. Safe to call more than once.
void texture_material_init();

// Find the definition for a texture name. The name must be lowercase and without extension.
// Returns nullptr if no .mat file defines this name.
const texture_material_def* texture_material_find(const char* name);
