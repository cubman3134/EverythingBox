# GenerateSecrets.cmake — configure-time generation of BuiltinSecrets.h in the BUILD TREE.
#
# Reads each git-ignored native/secrets/<provider>.secrets file (lines of key=value), obfuscates every
# embedded credential VALUE with a rolling XOR (best-effort ONLY — NOT cryptography; it merely keeps the
# plaintext creds out of a `strings` scan of the exe), splits each obfuscated value across two arrays, and
# writes the arrays + their lengths into a generated header via configure_file.
#
# Absent/blank value  ->  a single 0 byte + both lengths 0; the addon then falls back to the user's addon
#                         settings, and absent those too, stays dormant (no request with an empty key).
#
# Inputs (set by the caller before include()):
#   EB_SECRETS_DIR       absolute path to native/secrets (holds the per-provider .secrets files)
#   EB_SECRETS_TEMPLATE  absolute path to BuiltinSecrets.h.in
#   EB_SECRETS_GEN_DIR   build-tree dir to emit BuiltinSecrets.h into
#
# The XOR scheme (key + formula) MUST stay in lockstep with AddonContext.cpp's de-obfuscation:
#   obfuscated[i] = plain[i] XOR KEY[i % keylen] XOR (i AND 0xFF)   (i is the index WITHIN one value's blob)

# Fixed rolling key — mirrored byte-for-byte in native/src/addons/AddonContext.cpp.
set(_eb_xor_key 90 195 23 158 66 189 47 113)
list(LENGTH _eb_xor_key _eb_keylen)

# Read one key=value line out of a .secrets file (empty string when the file or key is absent).
function(_eb_read_secret _file _key _out)
    set(${_out} "" PARENT_SCOPE)
    if(EXISTS "${_file}")
        file(STRINGS "${_file}" _lines)
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "^${_key}=(.*)$")
                set(${_out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
            endif()
        endforeach()
    endif()
endfunction()

# Obfuscate one credential value into two comma-separated byte arrays + their two lengths. A blank value
# yields A="0" B="0" with both lengths 0 (a valid, never-indexed C array). Sets the four named PARENT_SCOPE
# output variables. Zero-size arrays are ill-formed in standard C++, so an empty half is emitted as "0".
function(_eb_obf_value _value _out_a _out_b _out_lena _out_lenb)
    if(_value STREQUAL "")
        set(${_out_a}    "0" PARENT_SCOPE)
        set(${_out_b}    "0" PARENT_SCOPE)
        set(${_out_lena} "0" PARENT_SCOPE)
        set(${_out_lenb} "0" PARENT_SCOPE)
        return()
    endif()

    # Byte-accurate via hex (string(LENGTH) would miscount any multibyte input).
    string(HEX "${_value}" _hex)
    string(LENGTH "${_hex}" _hlen)
    math(EXPR _blob_len "${_hlen} / 2")

    set(_obf "")
    math(EXPR _last "${_blob_len} - 1")
    foreach(_i RANGE ${_last})
        math(EXPR _pos "${_i} * 2")
        string(SUBSTRING "${_hex}" ${_pos} 2 _hh)
        math(EXPR _plain "0x${_hh}")
        math(EXPR _ki "${_i} % ${_eb_keylen}")
        list(GET _eb_xor_key ${_ki} _kb)
        math(EXPR _idxb "${_i} % 256")
        math(EXPR _ob "(${_plain} ^ ${_kb}) ^ ${_idxb}")
        list(APPEND _obf ${_ob})
    endforeach()

    # Split the obfuscated blob in half across the two arrays.
    math(EXPR _half "(${_blob_len} + 1) / 2")
    set(_arrA "")
    set(_arrB "")
    set(_j 0)
    foreach(_byte IN LISTS _obf)
        if(_j LESS _half)
            list(APPEND _arrA ${_byte})
        else()
            list(APPEND _arrB ${_byte})
        endif()
        math(EXPR _j "${_j} + 1")
    endforeach()
    list(LENGTH _arrA _lenA)
    list(LENGTH _arrB _lenB)
    string(REPLACE ";" ", " _sA "${_arrA}")
    string(REPLACE ";" ", " _sB "${_arrB}")
    if(_sA STREQUAL "")
        set(_sA "0")   # e.g. a 0-byte half; length stays the real (0) count
    endif()
    if(_sB STREQUAL "")
        set(_sB "0")   # e.g. a 1-byte value puts everything in A, leaving B empty
    endif()

    set(${_out_a}    "${_sA}"   PARENT_SCOPE)
    set(${_out_b}    "${_sB}"   PARENT_SCOPE)
    set(${_out_lena} "${_lenA}" PARENT_SCOPE)
    set(${_out_lenb} "${_lenB}" PARENT_SCOPE)
endfunction()

# Re-run configure (which re-runs this script) whenever any secrets file appears, changes, or disappears —
# this is how the generated header stays in sync with the secrets files.
set(_eb_secret_files
    "${EB_SECRETS_DIR}/screenscraper.secrets"
    "${EB_SECRETS_DIR}/thegamesdb.secrets"
    "${EB_SECRETS_DIR}/igdb.secrets"
    "${EB_SECRETS_DIR}/steamgriddb.secrets"
    "${EB_SECRETS_DIR}/lastfm.secrets")
foreach(_f IN LISTS _eb_secret_files)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_f}")
endforeach()

# Read each provider credential value (empty when its file/key is absent or blank).
_eb_read_secret("${EB_SECRETS_DIR}/screenscraper.secrets" "devid"        _v_ss_devid)
_eb_read_secret("${EB_SECRETS_DIR}/screenscraper.secrets" "devpassword"  _v_ss_devpw)
_eb_read_secret("${EB_SECRETS_DIR}/thegamesdb.secrets"    "apikey"       _v_tgdb_key)
_eb_read_secret("${EB_SECRETS_DIR}/igdb.secrets"          "clientId"     _v_igdb_id)
_eb_read_secret("${EB_SECRETS_DIR}/igdb.secrets"          "clientSecret" _v_igdb_secret)
_eb_read_secret("${EB_SECRETS_DIR}/steamgriddb.secrets"   "apikey"       _v_sgdb_key)
# Last.fm (#192): the APPLICATION identity, not a user credential. Both halves are required — a key with no
# secret cannot sign a single call, so the provider treats "either missing" as "not available in this build".
_eb_read_secret("${EB_SECRETS_DIR}/lastfm.secrets"       "apikey"       _v_lastfm_key)
_eb_read_secret("${EB_SECRETS_DIR}/lastfm.secrets"       "secret"       _v_lastfm_secret)

# Obfuscate each into the template's @VARS@.
_eb_obf_value("${_v_ss_devid}"    EB_SS_DEVID_ARRAY_A    EB_SS_DEVID_ARRAY_B    EB_SS_DEVID_LEN_A    EB_SS_DEVID_LEN_B)
_eb_obf_value("${_v_ss_devpw}"    EB_SS_DEVPW_ARRAY_A    EB_SS_DEVPW_ARRAY_B    EB_SS_DEVPW_LEN_A    EB_SS_DEVPW_LEN_B)
_eb_obf_value("${_v_tgdb_key}"    EB_TGDB_KEY_ARRAY_A    EB_TGDB_KEY_ARRAY_B    EB_TGDB_KEY_LEN_A    EB_TGDB_KEY_LEN_B)
_eb_obf_value("${_v_igdb_id}"     EB_IGDB_ID_ARRAY_A     EB_IGDB_ID_ARRAY_B     EB_IGDB_ID_LEN_A     EB_IGDB_ID_LEN_B)
_eb_obf_value("${_v_igdb_secret}" EB_IGDB_SECRET_ARRAY_A EB_IGDB_SECRET_ARRAY_B EB_IGDB_SECRET_LEN_A EB_IGDB_SECRET_LEN_B)
_eb_obf_value("${_v_sgdb_key}"    EB_SGDB_KEY_ARRAY_A    EB_SGDB_KEY_ARRAY_B    EB_SGDB_KEY_LEN_A    EB_SGDB_KEY_LEN_B)
_eb_obf_value("${_v_lastfm_key}"    EB_LASTFM_KEY_ARRAY_A    EB_LASTFM_KEY_ARRAY_B    EB_LASTFM_KEY_LEN_A    EB_LASTFM_KEY_LEN_B)
_eb_obf_value("${_v_lastfm_secret}" EB_LASTFM_SECRET_ARRAY_A EB_LASTFM_SECRET_ARRAY_B EB_LASTFM_SECRET_LEN_A EB_LASTFM_SECRET_LEN_B)

# Count embedded slots for a loud-but-secret-free STATUS line. NEVER print any credential material.
set(_eb_embedded 0)
foreach(_v "${_v_ss_devid}" "${_v_ss_devpw}" "${_v_tgdb_key}" "${_v_igdb_id}" "${_v_igdb_secret}" "${_v_sgdb_key}"
           "${_v_lastfm_key}" "${_v_lastfm_secret}")
    if(NOT _v STREQUAL "")
        math(EXPR _eb_embedded "${_eb_embedded} + 1")
    endif()
endforeach()
if(_eb_embedded GREATER 0)
    message(STATUS "Builtin provider credentials embedded (obfuscated): ${_eb_embedded} of 8 slots filled.")
else()
    message(STATUS "Builtin provider credentials NOT embedded — no secrets files; providers fall back to user settings.")
endif()

file(MAKE_DIRECTORY "${EB_SECRETS_GEN_DIR}")
configure_file("${EB_SECRETS_TEMPLATE}" "${EB_SECRETS_GEN_DIR}/BuiltinSecrets.h" @ONLY)
