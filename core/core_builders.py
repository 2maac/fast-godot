"""Functions used to generate source files during build time"""

from collections import OrderedDict
from io import TextIOWrapper

import hashlib
import os
import random

import methods


# Generate disabled classes
def disabled_class_builder(target, source, env):
    with methods.generated_wrapper(str(target[0])) as file:
        for c in source[0].read():
            if cs := c.strip():
                file.write(f"class {cs}; template <> struct is_class_enabled<{cs}> : std::false_type {{}};\n")


# Generate version info
def version_info_builder(target, source, env):
    with methods.generated_wrapper(str(target[0])) as file:
        file.write(
            """\
#define GODOT_VERSION_SHORT_NAME "{short_name}"
#define GODOT_VERSION_NAME "{name}"
#define GODOT_VERSION_MAJOR {major}
#define GODOT_VERSION_MINOR {minor}
#define GODOT_VERSION_PATCH {patch}
#define GODOT_VERSION_STATUS "{status}"
#define GODOT_VERSION_BUILD "{build}"
#define GODOT_VERSION_MODULE_CONFIG "{module_config}"
#define GODOT_VERSION_WEBSITE "{website}"
#define GODOT_VERSION_DOCS_BRANCH "{docs_branch}"
#define GODOT_VERSION_DOCS_URL "https://docs.godotengine.org/en/" GODOT_VERSION_DOCS_BRANCH
""".format(**source[0].read())
        )


def version_hash_builder(target, source, env):
    with methods.generated_wrapper(str(target[0])) as file:
        file.write(
            """\
#include "core/version.h"

const char *const GODOT_VERSION_HASH = "{git_hash}";
const unsigned long long GODOT_VERSION_TIMESTAMP = {git_timestamp};
""".format(**source[0].read())
        )



# --- White-box AES-256 for the PCK key unwrap -------------------------------
# A Chow-style table network: AES-256 encryption with the key baked into
# randomized lookup tables and random invertible GF(2) byte encodings between
# rounds, so the working key never exists as bytes in the binary or in code
# immediates. Only a 32-byte block is ever encrypted (the wrapped pack key), so
# the table lookups are far too few to matter at runtime.
_WB_SBOX = [
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
]
_WB_MC = ((2, 3, 1, 1), (1, 2, 3, 1), (1, 1, 2, 3), (3, 1, 1, 2))
_WB_ROUNDS = 13  # AES-256: 13 full rounds + the final round


def _wb_xtime(a):
    a <<= 1
    return (a ^ 0x11B) & 0xFF if a & 0x100 else a


def _wb_gmul(a, b):
    result = 0
    while b:
        if b & 1:
            result ^= a
        a = _wb_xtime(a)
        b >>= 1
    return result


def _wb_key_expand(key):
    words = [list(key[4 * i : 4 * i + 4]) for i in range(8)]
    rcon = 1
    for i in range(8, 60):
        temp = list(words[i - 1])
        if i % 8 == 0:
            temp = temp[1:] + temp[:1]
            temp = [_WB_SBOX[x] for x in temp]
            temp[0] ^= rcon
            rcon = _wb_xtime(rcon)
        elif i % 8 == 4:
            temp = [_WB_SBOX[x] for x in temp]
        words.append([words[i - 8][j] ^ temp[j] for j in range(4)])
    round_keys = []
    for r in range(15):
        rk = bytearray(16)
        for c in range(4):
            for row in range(4):
                rk[row + 4 * c] = words[4 * r + c][row]
        round_keys.append(bytes(rk))
    return round_keys


def _wb_rand_linear(rng):
    while True:
        rows = [rng.getrandbits(8) for _ in range(8)]
        basis = []
        independent = True
        for row in rows:
            x = row
            for b in basis:
                x = min(x, x ^ b)
            if x == 0:
                independent = False
                break
            basis.append(x)
            basis.sort(reverse=True)
        if independent:
            return rows


def _wb_apply_lin(rows, value):
    out = 0
    for bit in range(8):
        out |= (bin(rows[bit] & value).count("1") & 1) << bit
    return out


def _wb_invert_lin(rows):
    table = [_wb_apply_lin(rows, x) for x in range(256)]
    inverse = [0] * 256
    for x, y in enumerate(table):
        inverse[y] = x
    return inverse


_WB_DECOYS = 64
_WB_POOL = 512


def _wb_make_tables(key, seed, wrapped):
    rng = random.Random(seed)
    round_keys = _wb_key_expand(key)
    enc = [[_wb_rand_linear(rng) for _ in range(16)] for _ in range(_WB_ROUNDS)]
    enc_inv = [[_wb_invert_lin(enc[r][p]) for p in range(16)] for r in range(_WB_ROUNDS)]
    tables = []
    enc_keys = []
    for r in range(1, _WB_ROUNDS + 1):
        rk_lut = [_wb_apply_lin(enc[r - 1][p], round_keys[r][p]) for p in range(16)]
        enc_keys.append(rk_lut)
        round_tables = []
        for out_pos in range(16):
            out_row, out_col = out_pos % 4, out_pos // 4
            pos_tables = []
            for k in range(4):
                in_pos = k + 4 * ((out_col + k) % 4)
                table = bytearray(256)
                for idx in range(256):
                    if r == 1:
                        real_in = idx ^ round_keys[0][in_pos]
                    else:
                        real_in = enc_inv[r - 2][in_pos][idx]
                    contribution = _wb_gmul(_WB_MC[out_row][k], _WB_SBOX[real_in])
                    table[idx] = _wb_apply_lin(enc[r - 1][out_pos], contribution)
                pos_tables.append(bytes(table))
            round_tables.append(pos_tables)
        tables.append(round_tables)
    final_tables = []
    for out_pos in range(16):
        out_row, out_col = out_pos % 4, out_pos // 4
        in_pos = out_row + 4 * ((out_col + out_row) % 4)
        table = bytearray(256)
        for idx in range(256):
            table[idx] = _WB_SBOX[enc_inv[_WB_ROUNDS - 1][in_pos][idx]] ^ round_keys[14][out_pos]
        final_tables.append(bytes(table))

    # The tables are no longer stored as recognizable permutations: every entry is
    # XORed with its own random mask, the real tables are shuffled into a larger
    # blob together with random decoy tables, and the wrapped key bytes are spread
    # over a random pool. Recovering the key from the binary now needs the lookup
    # structures and the XOR masks, not just a pattern match on the table bytes.
    n_real = _WB_ROUNDS * 16 * 4
    n_final = 16
    total = n_real + _WB_DECOYS + n_final
    slots = list(range(total))
    rng.shuffle(slots)
    real_slots = slots[:n_real]
    final_slots = slots[n_real + _WB_DECOYS:]
    blob = [bytearray(rng.getrandbits(8) for _ in range(256)) for _ in range(total)]
    masks = [bytearray(rng.getrandbits(8) for _ in range(256)) for _ in range(total)]
    flat = 0
    for r in range(_WB_ROUNDS):
        for out_pos in range(16):
            for k in range(4):
                slot = real_slots[flat]
                flat += 1
                table = tables[r][out_pos][k]
                for i in range(256):
                    blob[slot][i] = table[i] ^ masks[slot][i]
    for i, table in enumerate(final_tables):
        slot = final_slots[i]
        for j in range(256):
            blob[slot][j] = table[j] ^ masks[slot][j]

    wrap_mask = bytearray(rng.getrandbits(8) for _ in range(32))
    wrap_pool = bytearray(rng.getrandbits(8) for _ in range(_WB_POOL))
    wrap_index = rng.sample(range(_WB_POOL), 32)
    for i in range(32):
        wrap_pool[wrap_index[i]] = wrapped[i] ^ wrap_mask[i]

    return {
        "round_keys": enc_keys,
        "blob": blob,
        "masks": masks,
        "real_slots": real_slots,
        "final_slots": final_slots,
        "wrap_pool": wrap_pool,
        "wrap_index": wrap_index,
        "wrap_mask": wrap_mask,
    }


def _wb_emit_tables(bundle):
    total = len(bundle["blob"])
    parts = ["static const uint8_t grit_wb_blob[%d][256] = {" % total]
    for table in bundle["blob"]:
        parts.append("{" + ",".join("0x%02x" % b for b in table) + "},")
    parts.append("};")
    parts.append("static const uint8_t grit_wb_masks[%d][256] = {" % total)
    for table in bundle["masks"]:
        parts.append("{" + ",".join("0x%02x" % b for b in table) + "},")
    parts.append("};")
    parts.append("static const uint16_t grit_wb_slots[%d] = {" % (_WB_ROUNDS * 16 * 4))
    parts.append(",".join(str(s) for s in bundle["real_slots"]))
    parts.append("};")
    parts.append("static const uint16_t grit_wb_final_slots[16] = {")
    parts.append(",".join(str(s) for s in bundle["final_slots"]))
    parts.append("};")
    parts.append("static const uint8_t grit_wb_round_keys[%d][16] = {" % _WB_ROUNDS)
    for rk in bundle["round_keys"]:
        parts.append("{" + ",".join("0x%02x" % b for b in rk) + "},")
    parts.append("};")
    # volatile: otherwise the optimizer constant-folds the wrap reconstruction and
    # emits the plain wrapped key instead of the pool/mask arrays.
    parts.append("static const volatile uint8_t grit_wrap_pool[%d] = {" % len(bundle["wrap_pool"]))
    parts.append(",".join("0x%02x" % b for b in bundle["wrap_pool"]))
    parts.append("};")
    parts.append("static const volatile uint16_t grit_wrap_index[32] = {" + ",".join(str(i) for i in bundle["wrap_index"]) + "};")
    parts.append("static const volatile uint8_t grit_wrap_mask[32] = {" + ",".join("0x%02x" % b for b in bundle["wrap_mask"]) + "};")
    return "\n".join(parts)


def _wb_source_value(source, index):
    if len(source) > index:
        try:
            value = source[index].read()
        except (AttributeError, IndexError):
            value = ""
        if value:
            return str(value)
    return ""


def encryption_key_builder(target, source, env):
    src = source[0].read() or "0" * 64
    try:
        buffer = bytes.fromhex(src)
        if len(buffer) != 32:
            raise ValueError
    except ValueError:
        methods.print_error(
            f'Invalid AES256 encryption key, not 64 hexadecimal characters: "{src}".\n'
            "Unset `SCRIPT_AES256_ENCRYPTION_KEY` in your environment "
            "or make sure that it contains exactly 64 hexadecimal characters."
        )
        raise

    wrapped_hex = (_wb_source_value(source, 1) or os.environ.get("GRIT_WRAPPED_KEY") or "").strip().lower()
    wrapped = None
    if wrapped_hex:
        try:
            wrapped = bytes.fromhex(wrapped_hex)
            if len(wrapped) != 32:
                raise ValueError
        except ValueError:
            methods.print_error(
                f'Invalid wrapped PCK data key (expected 64 hexadecimal characters): "{wrapped_hex}".'
            )
            raise

    if wrapped is not None:
        # Hybrid white-box mode: the exported pack is encrypted with a random data
        # key, and the template only carries that data key wrapped with the user's
        # key. A Chow-style table network for AES-256 with the user key baked into
        # randomized tables recomputes the data key at pack-open; neither key
        # exists as bytes or immediates in the binary.
        wb_seed = _wb_source_value(source, 2) or os.environ.get("GRIT_WB_SEED") or ""
        seed_material = wb_seed + buffer.hex() + wrapped_hex
        seed = int.from_bytes(hashlib.sha256(b"grit-wb-aes256:" + seed_material.encode("utf-8")).digest(), "big")
        table_source = _wb_emit_tables(_wb_make_tables(buffer, seed, wrapped))
        with methods.generated_wrapper(str(target[0])) as file:
            file.write(
                f"""\
#include <cstdint>
#include <cstring>

#include "core/os/secure_memory.h"

// A debugger attached to the game could read the unwrapped pack key from memory.
// The encrypted template refuses to run under one: the pack key comes out zeroed
// and the pack cannot be opened.
#if defined(MACOS_ENABLED)
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
static bool grit_debugger_attached() {{
	ptrace(PT_DENY_ATTACH, 0, nullptr, 0);
	int mib[4] = {{ CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid() }};
	struct kinfo_proc info;
	size_t size = sizeof(info);
	memset(&info, 0, sizeof(info));
	if (sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && (info.kp_proc.p_flag & P_TRACED) != 0) {{
		return true;
	}}
	return false;
}}
#elif defined(WINDOWS_ENABLED)
#include <windows.h>
static bool grit_debugger_attached() {{
	return IsDebuggerPresent() != 0;
}}
#elif defined(LINUXBSD_ENABLED) && !defined(WEB_ENABLED)
#include <cstdio>
#include <cstdlib>
static bool grit_debugger_attached() {{
	FILE *status = fopen("/proc/self/status", "r");
	if (status == nullptr) {{
		return false;
	}}
	char line[256];
	bool traced = false;
	while (fgets(line, sizeof(line), status)) {{
		if (strncmp(line, "TracerPid:", 10) == 0) {{
			traced = atoi(line + 10) != 0;
			break;
		}}
	}}
	fclose(status);
	return traced;
}}
#else
static bool grit_debugger_attached() {{
	return false;
}}
#endif

{table_source}

static void grit_wb_encrypt_block(const uint8_t p_in[16], uint8_t r_out[16]) {{
	uint8_t s[16];
	uint8_t o[16];
	memcpy(s, p_in, 16);
	for (int round = 0; round < {_WB_ROUNDS}; round++) {{
		for (int out_pos = 0; out_pos < 16; out_pos++) {{
			const int out_col = out_pos / 4;
			uint8_t value = grit_wb_round_keys[round][out_pos];
			for (int k = 0; k < 4; k++) {{
				const int in_pos = k + 4 * ((out_col + k) % 4);
				const unsigned slot = grit_wb_slots[(round * 16 + out_pos) * 4 + k];
				value ^= grit_wb_blob[slot][s[in_pos]] ^ grit_wb_masks[slot][s[in_pos]];
			}}
			o[out_pos] = value;
		}}
		memcpy(s, o, 16);
	}}
	for (int out_pos = 0; out_pos < 16; out_pos++) {{
		const int out_row = out_pos % 4;
		const int in_pos = out_row + 4 * ((out_pos / 4 + out_row) % 4);
		const unsigned slot = grit_wb_final_slots[out_pos];
		o[out_pos] = grit_wb_blob[slot][s[in_pos]] ^ grit_wb_masks[slot][s[in_pos]];
	}}
	memcpy(r_out, o, 16);
}}

// Rebuilds the key the exported pack was encrypted with. The user key lives only
// inside the white-box tables above, and the pack key only in wrapped form.
void script_encryption_key_get(uint8_t *p_key) {{
	if (grit_debugger_attached()) {{
		memset(p_key, 0, 32);
		return;
	}}
	uint8_t wrapped[32];
	for (int i = 0; i < 32; i++) {{
		wrapped[i] = grit_wrap_pool[grit_wrap_index[i]] ^ grit_wrap_mask[i];
	}}
	uint8_t data_key[32];
	grit_wb_encrypt_block(wrapped, data_key);
	grit_wb_encrypt_block(wrapped + 16, data_key + 16);
	memcpy(p_key, data_key, 32);
	SecureMemory::zero(data_key, sizeof(data_key));
	SecureMemory::zero(wrapped, sizeof(wrapped));
}}
"""
            )
        return

    # Builds without an exported pack (editor builds) only need the split-share
    # form: no plaintext key array, and no table network to generate.
    seed = hashlib.sha256(b"grit-encryption-key-shares-v1" + buffer).digest()
    rng = random.Random(int.from_bytes(seed, "big"))
    share1 = bytes(rng.randrange(256) for _ in range(32))
    share2 = bytes(rng.randrange(256) for _ in range(32))
    share0 = bytes(a ^ b ^ c for a, b, c in zip(buffer, share1, share2))
    order = list(range(32))
    rng.shuffle(order)
    stores = []
    for index in order:
        stores.append(
            "\tp[%d] = uint8_t(0x%02x ^ 0x%02x ^ 0x%02x);\n"
            % (index, share0[index], share1[index], share2[index])
        )
    with methods.generated_wrapper(str(target[0])) as file:
        file.write(
            f"""\
#include <cstdint>

// Rebuilds the script/PCK encryption key compiled into this build. The key is
// split into XOR shares written byte by byte in a build-specific order through
// a volatile pointer, so no contiguous key exists in the binary's data.
// Called only when a pack is opened.
void script_encryption_key_get(uint8_t *p_key) {{
	volatile uint8_t *p = p_key;
{''.join(stores)}}}

"""
        )


def make_certs_header(target, source, env):
    buffer = methods.get_buffer(str(source[0]))
    decomp_size = len(buffer)
    buffer = methods.compress_buffer(buffer)

    with methods.generated_wrapper(str(target[0])) as file:
        # System certs path. Editor will use them if defined. (for package maintainers)
        file.write('#define _SYSTEM_CERTS_PATH "{}"\n'.format(source[2].read() or ""))
        if source[1].read():
            # Defined here and not in env so changing it does not trigger a full rebuild.
            file.write(f"""\
#define BUILTIN_CERTS_ENABLED

inline constexpr int _certs_compressed_size = {len(buffer)};
inline constexpr int _certs_uncompressed_size = {decomp_size};
inline constexpr unsigned char _certs_compressed[] = {{
{methods.format_buffer(buffer, 1)}
}};
""")


def make_authors_header(target, source, env):
    SECTIONS = {
        "Project Founders": "AUTHORS_FOUNDERS",
        "Lead Developer": "AUTHORS_LEAD_DEVELOPERS",
        "Project Manager": "AUTHORS_PROJECT_MANAGERS",
        "Developers": "AUTHORS_DEVELOPERS",
    }
    buffer = methods.get_buffer(str(source[0]))
    reading = False

    with methods.generated_wrapper(str(target[0])) as file:

        def close_section():
            file.write("\tnullptr,\n};\n\n")

        for line in buffer.decode().splitlines():
            if line.startswith("    ") and reading:
                file.write(f'\t"{methods.to_escaped_cstring(line).strip()}",\n')
            elif line.startswith("## "):
                if reading:
                    close_section()
                    reading = False
                section = SECTIONS[line[3:].strip()]
                if section:
                    file.write(f"inline constexpr const char *{section}[] = {{\n")
                    reading = True

        if reading:
            close_section()


def make_donors_header(target, source, env):
    SECTIONS = {
        "Patrons": "DONORS_PATRONS",
        "Platinum sponsors": "DONORS_SPONSORS_PLATINUM",
        "Gold sponsors": "DONORS_SPONSORS_GOLD",
        "Silver sponsors": "DONORS_SPONSORS_SILVER",
        "Diamond members": "DONORS_MEMBERS_DIAMOND",
        "Titanium members": "DONORS_MEMBERS_TITANIUM",
        "Platinum members": "DONORS_MEMBERS_PLATINUM",
        "Gold members": "DONORS_MEMBERS_GOLD",
    }
    buffer = methods.get_buffer(str(source[0]))
    reading = False

    with methods.generated_wrapper(str(target[0])) as file:

        def close_section():
            file.write("\tnullptr,\n};\n\n")

        for line in buffer.decode().splitlines():
            if line.startswith("    ") and reading:
                file.write(f'\t"{methods.to_escaped_cstring(line).strip()}",\n')
            elif line.startswith("## "):
                if reading:
                    close_section()
                    reading = False
                section = SECTIONS.get(line[3:].strip())
                if section:
                    file.write(f"inline constexpr const char *{section}[] = {{\n")
                    reading = True

        if reading:
            close_section()


def make_license_header(target, source, env):
    src_copyright = str(source[0])
    src_license = str(source[1])

    class LicenseReader:
        def __init__(self, license_file: TextIOWrapper):
            self._license_file = license_file
            self.line_num = 0
            self.current = self.next_line()

        def next_line(self):
            line = self._license_file.readline()
            self.line_num += 1
            while line.startswith("#"):
                line = self._license_file.readline()
                self.line_num += 1
            self.current = line
            return line

        def next_tag(self):
            if ":" not in self.current:
                return ("", [])
            tag, line = self.current.split(":", 1)
            lines = [line.strip()]
            while self.next_line() and self.current.startswith(" "):
                lines.append(self.current.strip())
            return (tag, lines)

    projects = OrderedDict()
    license_list = []

    with open(src_copyright, "r", encoding="utf-8") as copyright_file:
        reader = LicenseReader(copyright_file)
        part = {}
        while reader.current:
            tag, content = reader.next_tag()
            if tag in ("Files", "Copyright", "License"):
                part[tag] = content[:]
            elif tag == "Comment" and part:
                # attach non-empty part to named project
                projects[content[0]] = projects.get(content[0], []) + [part]

            if not tag or not reader.current:
                # end of a paragraph start a new part
                if "License" in part and "Files" not in part:
                    # no Files tag in this one, so assume standalone license
                    license_list.append(part["License"])
                part = {}
                reader.next_line()

    data_list = []
    for project in iter(projects.values()):
        for part in project:
            part["file_index"] = len(data_list)
            data_list += part["Files"]
            part["copyright_index"] = len(data_list)
            data_list += part["Copyright"]

    with open(src_license, "r", encoding="utf-8") as file:
        license_text = file.read()

    with methods.generated_wrapper(str(target[0])) as file:
        file.write(f"""\
inline constexpr const char *GODOT_LICENSE_TEXT = {{
{methods.to_raw_cstring(license_text)}
}};

struct ComponentCopyrightPart {{
	const char *license;
	const char *const *files;
	const char *const *copyright_statements;
	int file_count;
	int copyright_count;
}};

struct ComponentCopyright {{
	const char *name;
	const ComponentCopyrightPart *parts;
	int part_count;
}};

""")

        file.write("inline constexpr const char *COPYRIGHT_INFO_DATA[] = {\n")
        for line in data_list:
            file.write(f'\t"{methods.to_escaped_cstring(line)}",\n')
        file.write("};\n\n")

        file.write("inline constexpr ComponentCopyrightPart COPYRIGHT_PROJECT_PARTS[] = {\n")
        part_index = 0
        part_indexes = {}
        for project_name, project in iter(projects.items()):
            part_indexes[project_name] = part_index
            for part in project:
                file.write(
                    f'\t{{ "{methods.to_escaped_cstring(part["License"][0])}", '
                    + f"&COPYRIGHT_INFO_DATA[{part['file_index']}], "
                    + f"&COPYRIGHT_INFO_DATA[{part['copyright_index']}], "
                    + f"{len(part['Files'])}, {len(part['Copyright'])} }},\n"
                )
                part_index += 1
        file.write("};\n\n")

        file.write(f"inline constexpr int COPYRIGHT_INFO_COUNT = {len(projects)};\n")

        file.write("inline constexpr ComponentCopyright COPYRIGHT_INFO[] = {\n")
        for project_name, project in iter(projects.items()):
            file.write(
                f'\t{{ "{methods.to_escaped_cstring(project_name)}", '
                + f"&COPYRIGHT_PROJECT_PARTS[{part_indexes[project_name]}], "
                + f"{len(project)} }},\n"
            )
        file.write("};\n\n")

        file.write(f"inline constexpr int LICENSE_COUNT = {len(license_list)};\n")

        file.write("inline constexpr const char *LICENSE_NAMES[] = {\n")
        for license in license_list:
            file.write(f'\t"{methods.to_escaped_cstring(license[0])}",\n')
        file.write("};\n\n")

        file.write("inline constexpr const char *LICENSE_BODIES[] = {\n\n")
        for license in license_list:
            to_raw = []
            for line in license[1:]:
                if line == ".":
                    to_raw += [""]
                else:
                    to_raw += [line]
            file.write(f"{methods.to_raw_cstring(to_raw)},\n\n")
        file.write("};\n\n")
