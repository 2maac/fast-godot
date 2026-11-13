#include "cffi_compat.hpp"

#include "cffi_type_parser.hpp"

namespace cffi {

namespace {
// The character classes the old regexes used. PCRE's \s without UCP is ASCII
// whitespace, and the identifier rule was [a-zA-Z_][a-zA-Z0-9_]*.
inline bool is_space(char32_t c) {
	return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == U'\f' || c == U'\v';
}

inline bool is_digit(char32_t c) {
	return c >= U'0' && c <= U'9';
}

inline bool is_ident_start(char32_t c) {
	return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c == U'_';
}

inline bool is_ident_char(char32_t c) {
	return is_ident_start(c) || is_digit(c);
}

// `const` and `volatile` are matched as bare prefixes, without a word boundary,
// because that is what (?:const|volatile)? did. It is why "constant" parses as
// the identifier "ant" rather than failing. Preserved rather than corrected:
// this parser's quirks are what callers already resolve names against.
inline int qualifier_length(const char32_t *s, int at, int len) {
	static const char *const KEYWORDS[] = { "const", "volatile" };
	static const int LENGTHS[] = { 5, 8 };
	for (int k = 0; k < 2; k++) {
		if (at + LENGTHS[k] > len) {
			continue;
		}
		bool match = true;
		for (int c = 0; c < LENGTHS[k]; c++) {
			if (s[at + c] != (char32_t) KEYWORDS[k][c]) {
				match = false;
				break;
			}
		}
		if (match) {
			return LENGTHS[k];
		}
	}
	return 0;
}
}

bool CFFITypeParser::is_valid() const {
	return !name.is_empty();
}

void CFFITypeParser::clear() {
	name = "";
	array_levels.clear();
}

const String& CFFITypeParser::get_base_name() const {
	return name;
}

const LocalVector<int>& CFFITypeParser::get_array_levels() const {
	return array_levels;
}

String CFFITypeParser::get_full_name() const {
	String full_name = name;
	for (int level : array_levels) {
		if (level < 0) {
			full_name += "*";
		}
		else {
			full_name += "[";
			full_name += String::num_int64(level);
			full_name += "]";
		}
	}
	return full_name;
}

// Scanned by hand rather than with RegEx.
//
// This used to build TWO RegEx objects with create_from_string on every call -
// once for the identifier and once for the loop over pointer/array levels - and
// compiling a pattern costs far more than using it. Measured on an M-series Mac:
// the two compilations come to ~1699 ns, against ~2278 ns for a whole
// `CFFI["int32_t"]` lookup, so roughly 75% of resolving a type by name was
// recompiling two constant patterns. It is now a single pass with no allocation
// beyond the name and the levels that were already being produced.
//
// The grammar is unchanged, including the parts the doc comment above gets
// wrong. What it actually accepts, verified against the regex version over a
// 64-name corpus:
//
//   "int", "int *", "int **", "int[4]", "int[3][4]", "int[ 4 ]", "int[]"
//   "const int", "volatile int *", "const char **", " int ", "int  *  "
//
//   - at most ONE qualifier, and only before the base name: "const volatile
//     int" fails, and so does a trailing "int *const";
//   - multi-word C types are NOT supported: "unsigned int" fails, because the
//     identifier stops at the first word and " int" is not a pointer or array;
//   - "int[]" is a POINTER, not a zero-length array: the empty capture means
//     level -1. A flexible array member has to be written "int[0]";
//   - text between levels is skipped, not rejected, because the old loop used
//     `search` rather than an anchored match. "int !!! *" parses as "int *".
//
// The last one is a wart, and it is deliberately kept: a name that resolved
// before still resolves now. Tightening it would be a separate change with its
// own compatibility question.
bool CFFITypeParser::parse(const String &full_name) {
	clear();

	const char32_t *s = full_name.ptr();
	const int len = full_name.length();
	int i = 0;

	auto skip_space = [&]() {
		while (i < len && is_space(s[i])) {
			i++;
		}
	};

	// ^\s*(?:const|volatile)?\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*
	skip_space();
	i += qualifier_length(s, i, len);
	skip_space();
	if (i >= len || !is_ident_start(s[i])) {
		return false;
	}
	const int name_start = i;
	while (i < len && is_ident_char(s[i])) {
		i++;
	}
	name = full_name.substr(name_start, i - name_start);
	skip_space();

	// The old loop used RegEx::search from here, so the leftmost match of
	// `(?:const|volatile)?\s*\*` or `\[\s*(\d*)\s*\]` wins, and anything before
	// it is skipped.
	int offset = i;
	while (offset < len) {
		bool matched = false;
		for (int pos = offset; pos < len && !matched; pos++) {
			// Alternative 1: an optional qualifier and a "*".
			int j = pos;
			while (j < len && is_space(s[j])) {
				j++;
			}
			j += qualifier_length(s, j, len);
			while (j < len && is_space(s[j])) {
				j++;
			}
			if (j < len && s[j] == U'*') {
				j++;
				while (j < len && is_space(s[j])) {
					j++;
				}
				array_levels.push_back(-1);
				offset = j;
				matched = true;
				break;
			}

			// Alternative 2: "[", optional digits, "]". No qualifier allowed.
			j = pos;
			while (j < len && is_space(s[j])) {
				j++;
			}
			if (j < len && s[j] == U'[') {
				j++;
				while (j < len && is_space(s[j])) {
					j++;
				}
				int level = 0;
				bool has_digits = false;
				while (j < len && is_digit(s[j])) {
					level = level * 10 + (int) (s[j] - U'0');
					has_digits = true;
					j++;
				}
				while (j < len && is_space(s[j])) {
					j++;
				}
				if (j < len && s[j] == U']') {
					j++;
					while (j < len && is_space(s[j])) {
						j++;
					}
					// An empty dimension is a pointer level, as above.
					array_levels.push_back(has_digits ? level : -1);
					offset = j;
					matched = true;
					break;
				}
			}
		}
		if (!matched) {
			return false;
		}
	}
	return true;
}

}
