# Structure layouts

Layouts of game structures the fork reads, from the Skate 2 debug build's PDB
(`tools/re/sk2_types.py show NAME`), checked against Skate 3 (TU3). Skate 3
changed some of them, so a layout is only listed here with what TU3 is known
to do.

- **In TU3:**
  - `confirmed`: our code reads this offset and works (the source is named);
  - `moved`: TU3 has the field at another offset, given;
  - `likely`: not checked in TU3 yet, but the neighbouring fields match.
- **Checking:** `tools/re/sk2_types.py check` checks every table row's offset
  and field name against the Skate 2 PDB, so the Skate 2 side can't drift.
  The TU3 column comes from probes, not the PDB.
- **Adding a class:** `sk2_types.py show NAME --flat` gives the Skate 2
  layout. List only the fields we use or have confirmed, and say how TU3 was
  checked.

### `Sk8::Render::cModelInstance`

One model instance: the model, its per-mesh contexts and the packed bone
palette. Skate 2 sizeof 0x28; TU3 0x28 (`confirmed`: the stride of
`UpdateBoneTransforms`' part array, [characters.md](characters.md)).

| Offset | Field | Type | In TU3 |
|---|---|---|---|
| +0x00 | `m_model` | `pegasus::tRModelData*` | confirmed (`skate3_native_lw.cpp`: model; mesh count = u16 at model+0x32) |
| +0x04 | `m_arrMeshContext` | `xcore::xvector<MeshContext>` (begin +0x04, end +0x08) | confirmed (`skate3_native_palette.cpp`) |
| +0x14 | `m_matrices` | `Matrix44*`: the packed palette, 3x4 rows | confirmed (palette snapshots) |
| +0x18 | `m_MatrixCount` | `unsigned`: bones in the palette | confirmed (palette snapshots) |
| +0x1C | `m_meshRemapDepth` | `int*` | likely |
| +0x20 | `m_meshRemapTables` | per-mesh bone remap tables | likely (the remaps `UpdateBoneTransforms` takes) |
| +0x24 | `m_bSkinned` | `bool` | likely |
| +0x25 | `m_bMultiParted` | `bool` | likely |

### `Sk8::Render::MeshContext`

One drawn mesh of an instance. **Skate 3 grew it:** Skate 2 sizeof 0x34,
TU3 0x50 (`confirmed`: the stride `skate3_native_lw.cpp` walks).

| Offset | Field | Type | In TU3 |
|---|---|---|---|
| +0x00 | `m_pMesh` | `pegasus::tRModelData::tMeshData*` | confirmed (RE-5: a UBT part's per-frame copy and the instance's persistent context point at the same mesh) |
| +0x04 | `m_pEffectList` | `Sk8::ShaderSystem::cEffectList*` (pass count at +0x44) | confirmed |
| +0x14 | `m_PassCount` | `unsigned` | not checked |
| +0x30 | `m_islandDrawList` | `renderengine::DrawIndexedParameters*` | moved: the draw list is at +0x48 in TU3 |

### `pegasus::tRModelData` and its mesh data

What `Sk8::UpdateBoneTransforms` reads (TU3 disassembly, RE-5). The model
matches Skate 2; the mesh data does not.

| Structure | Offset | Field | In TU3 |
|---|---|---|---|
| `tRModelData` | +0x24 | `m_pMeshTable` (8-byte entries, mesh data pointer first) | confirmed |
| | +0x32 | `m_iNumMeshes` | confirmed |
| | +0x34 | `m_iNumBones` (UBT skips models above 80) | confirmed |
| mesh data (`tRMeshData`) | +0x48 | the mesh's bone count (palette length) | confirmed; Skate 2 has `m_uiNumBoneMats` at +0x40 and `m_pBlendShapeTable` here |
| | +0x4C | u16 list: mesh bone → model bone | confirmed; Skate 2 has `m_szBlendShapeNames` here |

### `Sk8::Render::cModelInstance` as a UBT part

| Offset | Field | In TU3 |
|---|---|---|
| +0x00 | `m_model` | confirmed (RE-5) |
| +0x04 | `m_arrMeshContext` (one context per UBT part) | confirmed |
| +0x14 | `m_matrices`: UBT writes whole 64-byte Matrix44s here | confirmed |

### `Sk8::SkaterPresEntity`

The player skater's presentation entity (Skate 2 sizeof 0x950).

| Offset | Field | Type | In TU3 |
|---|---|---|---|
| +0x160 | `m_MatLtoW` | `Matrix44`: local to world | confirmed (the harness's skater position, `kEntityMatLtoW = 352`) |
| +0x1A0 | `m_MatLtoWTrans` | `Matrix44` | likely |

### `Sk8::cLivingWorldPresEntity`

A pedestrian or other LivingWorld character. **Skate 3 grew it:** Skate 2
sizeof 0x240; TU3 reads +0x240 and beyond.

| Offset | Field | Type | In TU3 |
|---|---|---|---|
| +0x10 | `m_uiCurrLODLvl` | `unsigned`: current LOD | confirmed (`skate3_native_lw.cpp`: idx = [e+16]) |
| +0x14 | `m_uiLODNumParts` | `unsigned[3]`: part count per LOD | confirmed (count = [e + (idx+5)*4]) |
| +0x20 | `m_vLODMdlInstances` | `cModelInstance*[3]` per LOD | confirmed (array = [e + (idx+8)*4]) |
| +0x210 | `mSpawnTransparency` | `float` | confirmed as the opacity: TU3 keeps a Vector4 here, x = alpha. TU3 adds the spawn ramp at +0x240 and the "is fading" byte at +0x244 (not in Skate 2) |
