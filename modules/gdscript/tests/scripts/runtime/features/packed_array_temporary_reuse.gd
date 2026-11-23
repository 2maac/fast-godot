# The native compiler reuses a Variant temporary as the result slot of a builtin
# call. A packed-array value that has been stored in a local must not be aliased
# by that reuse (packed arrays share a refcounted payload inside Variants).

func decode_bytes(text: String) -> String:
	var out := PackedByteArray()
	for i in text.length():
		out.append_array(text[i].to_utf8_buffer())
	return out.get_string_from_utf8()

func test():
	print(decode_bytes("/a.js"))
	print(decode_bytes("abcd"))

	# Assigning a packed array to a local still shares the payload; the fix must
	# not change that user-visible behavior.
	var shared := PackedByteArray([1, 2])
	var copy := shared
	@warning_ignore("return_value_discarded")
	copy.append(3)
	print(shared)
	print(copy)
