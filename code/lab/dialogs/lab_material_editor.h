#pragma once

#include "model/model.h"

// Show an editor for each material of the model, under the model information of the lab.
// The edits only change the texture replacement of the model instance that the lab shows.
// No material of the table and no other object changes.
void lab_material_editor_build(polymodel* pm, polymodel_instance* pmi);

// Discard all edits and give back the resources that they hold. Call it when the lab shows another object.
void lab_material_editor_reset();
