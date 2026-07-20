# GenerateSecrets.cmake — configure-time generation of BuiltinSecrets.h in the BUILD TREE.
#
# Reads native/secrets/screenscraper.secrets (two lines: devid=... / devpassword=...), obfuscates
# the concatenated bytes with a rolling XOR (best-effort ONLY — NOT cryptography; it merely keeps the
# plaintext creds out of a `strings` scan of the exe), splits the obfuscated blob across two arrays,
# and writes them + the lengths into a generated header via configure_file.
#
# Absent/incomplete file  ->  empty arrays (a single 0 byte, all lengths 0) + one loud STATUS line;
#                             the addon then falls back to the user's addon settings.
#
# Inputs (set by the caller before include()):
#   MMV_SECRETS_FILE      absolute path to screenscraper.secrets
#   MMV_SECRETS_TEMPLATE  absolute path to BuiltinSecrets.h.in
#   MMV_SECRETS_GEN_DIR   build-tree dir to emit BuiltinSecrets.h into
#
# The XOR scheme (key + formula) MUST stay in lockstep with AddonContext::builtinCredential().
#   obfuscated[i] = plain[i] XOR KEY[i % keylen] XOR (i AND 0xFF)

# Fixed rolling key — mirrored byte-for-byte in native/src/addons/AddonContext.cpp.
set(_mmv_xor_key 90 195 23 158 66 189 47 113)
list(LENGTH _mmv_xor_key _mmv_keylen)

# Re-run CMake configure (which re-runs this script) whenever the secrets file appears, changes, or
# disappears — this is how the generated header stays in sync with the secrets file.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MMV_SECRETS_FILE}")

set(_mmv_have_secrets FALSE)
set(_mmv_devid "")
set(_mmv_devpw "")

if(EXISTS "${MMV_SECRETS_FILE}")
    file(STRINGS "${MMV_SECRETS_FILE}" _mmv_lines)
    foreach(_line IN LISTS _mmv_lines)
        if(_line MATCHES "^devid=(.*)$")
            set(_mmv_devid "${CMAKE_MATCH_1}")
        elseif(_line MATCHES "^devpassword=(.*)$")
            set(_mmv_devpw "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(_mmv_devid AND _mmv_devpw)
        set(_mmv_have_secrets TRUE)
    endif()
endif()

if(_mmv_have_secrets)
    # Byte-accurate lengths via hex (string(LENGTH) would miscount any multibyte input).
    string(HEX "${_mmv_devid}" _devid_hex)
    string(HEX "${_mmv_devpw}"  _devpw_hex)
    set(_blob_hex "${_devid_hex}${_devpw_hex}")

    string(LENGTH "${_devid_hex}" _dh)
    math(EXPR _devid_len "${_dh} / 2")
    string(LENGTH "${_devpw_hex}" _ph)
    math(EXPR _devpw_len "${_ph} / 2")
    string(LENGTH "${_blob_hex}" _bh)
    math(EXPR _blob_len "${_bh} / 2")

    # Obfuscate every byte with the rolling XOR.
    set(_obf "")
    math(EXPR _last "${_blob_len} - 1")
    foreach(_i RANGE ${_last})
        math(EXPR _pos "${_i} * 2")
        string(SUBSTRING "${_blob_hex}" ${_pos} 2 _hh)
        math(EXPR _plain "0x${_hh}")
        math(EXPR _ki "${_i} % ${_mmv_keylen}")
        list(GET _mmv_xor_key ${_ki} _kb)
        math(EXPR _idxb "${_i} % 256")
        math(EXPR _ob "(${_plain} ^ ${_kb}) ^ ${_idxb}")
        list(APPEND _obf ${_ob})
    endforeach()

    # Split the obfuscated blob in half across two arrays.
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
    string(REPLACE ";" ", " MMV_SS_ARRAY_A "${_arrA}")
    string(REPLACE ";" ", " MMV_SS_ARRAY_B "${_arrB}")
    set(MMV_SS_LEN_A "${_lenA}")
    set(MMV_SS_LEN_B "${_lenB}")
    set(MMV_SS_DEVID_LEN "${_devid_len}")
    set(MMV_SS_DEVPASSWORD_LEN "${_devpw_len}")
    # Do NOT print any credential material here. Lengths only, and only at debug verbosity.
    message(STATUS "ScreenScraper builtin credentials embedded (obfuscated; ${_blob_len} bytes across 2 arrays).")
else()
    # Zero-size arrays are ill-formed in standard C++, so emit a single dummy byte with length 0.
    set(MMV_SS_ARRAY_A "0")
    set(MMV_SS_ARRAY_B "0")
    set(MMV_SS_LEN_A "0")
    set(MMV_SS_LEN_B "0")
    set(MMV_SS_DEVID_LEN "0")
    set(MMV_SS_DEVPASSWORD_LEN "0")
    message(STATUS "ScreenScraper builtin credentials NOT embedded — secrets file missing; addon falls back to user settings")
endif()

file(MAKE_DIRECTORY "${MMV_SECRETS_GEN_DIR}")
configure_file("${MMV_SECRETS_TEMPLATE}" "${MMV_SECRETS_GEN_DIR}/BuiltinSecrets.h" @ONLY)
