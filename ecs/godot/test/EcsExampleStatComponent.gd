@tool
class_name EcsExampleStatComponent
extends EcsComponent

## Base for a generic depletable/regenerating float stat - hp, mp, sp, etc. Packed
## layout: 4x float32 (current, max, regen_per_sec, decay_per_sec), 16 bytes.
##
## Subclass per stat with just class_name + extends (see EcsExampleHp/Mp/Sp below) -
## each concrete stat needs its own class_name so it gets its own native component
## id/column; the packing logic here is shared since the layout is identical.

static func byte_size() -> int:
	return 16

static func to_bytes(data: Dictionary) -> PackedByteArray:
	var b := PackedByteArray()
	b.resize(16)
	b.encode_float(0, data.get("current", 100.0))
	b.encode_float(4, data.get("max", 100.0))
	b.encode_float(8, data.get("regen_per_sec", 0.0))
	b.encode_float(12, data.get("decay_per_sec", 0.0))
	return b

static func from_bytes(bytes: PackedByteArray): # Dictionary at runtime - base declares untyped (Variant), override must match exactly
	return {
		"current": bytes.decode_float(0),
		"max": bytes.decode_float(4),
		"regen_per_sec": bytes.decode_float(8),
		"decay_per_sec": bytes.decode_float(12),
	}
