# BuildingGenerator

Unreal Engine 5.5 PCG building generation sample project.

This repository is a reduced export of the original project and currently includes:

- `Content/PCGBuilding`
- `Content/PCGGenerator`
- `Source`
- `BuildingGenerator.uproject`

## Requirements

- Unreal Engine `5.5`
- Enabled UE plugins:
  - `PCG`
  - `PCGExternalDataInterop`
  - `PCGGeometryScriptInterop`

The `.uproject` in this export also references `MetaHumanSDK`, `MetaHumanRuntime`, and `NNERuntimeORT`.
If those plugins are not installed in your local engine setup, disable them in `BuildingGenerator.uproject` before opening the project.

## Project Structure

- `Content/PCGBuilding`
  - Building-related PCG assets, subgraphs, data assets, and sample content.
- `Content/PCGGenerator`
  - Reusable PCG graphs and spline-based generation assets.
- `Source/BuildingGenerator`
  - Runtime module implementation for custom building generation and PCG nodes.
  - Includes custom nodes such as roof assembly, door grammar assignment, overlap pruning, and rotated bounds splitting.
- `Source/BuildingGenerator/ThirdParty/Clipper2`
  - Embedded Clipper2 headers used by the runtime module.

## Build Notes

The main runtime module declares these core dependencies:

- `Core`
- `CoreUObject`
- `Engine`
- `InputCore`
- `PCG`

It also adds the bundled `Clipper2` headers through `PublicSystemIncludePaths`.

## Included Assets

Example assets in this export include:

- `Content/PCGBuilding/PCG_BuildingSample.uasset`
- `Content/PCGBuilding/SubGraph/PCG_DataDrivenBuilding.uasset`
- `Content/PCGGenerator/PCGGraph/PCGB_BuildingMainGraph.uasset`
- `Content/PCGGenerator/PCGSplineBuildingGraph/PCG_BuildingSample.uasset`

## Notes

- This is not the full original Unreal project.
- Large demo environments and unrelated content were intentionally excluded from this repository.
