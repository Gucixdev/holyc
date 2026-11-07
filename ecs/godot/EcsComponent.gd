@tool
class_name EcsComponent
extends RefCounted

## Pure-data component DEFINITION for the native ECS (EcsWorld/EcsView, GDExtension
## at addons/toolbox/tools/ecs/bin). Replaces the old EzComponent Resource: there's no
## per-entity Resource instance anymore, storage is a contiguous byte column inside the
## native world, so a component type is just this - a name/size plus a pack/unpack pair.
##
## Subclass and override byte_size()/to_bytes()/from_bytes(). A tag (zero-size, pure
## presence/absence - e.g. "Dead") only needs byte_size() left at the default 0.

static func byte_size() -> int:
	return 0

## `data` is whatever shape from_bytes() returns (commonly a Dictionary of field values).
static func to_bytes(_data) -> PackedByteArray:
	return PackedByteArray()

static func from_bytes(_bytes: PackedByteArray):
	return null
