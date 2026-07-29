/*
 * Custom os module for Jumperless
 * Provides filesystem functions using FatFS bridge
 * 
 * This is needed because MICROPY_VFS is disabled, so the standard os module
 * doesn't have filesystem functions. We implement them using our custom FatFS bridge.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mphal.h"
#include "py/mperrno.h"

#if MICROPY_JL_CUSTOM_OS_BRIDGE && MICROPY_PY_OS || 1

// Forward declarations for C functions implemented in JumperlessMicroPythonAPI.cpp
extern int jl_fs_exists(const char* path);
extern char* jl_fs_listdir(const char* path);
extern int jl_fs_mkdir(const char* path);
extern int jl_fs_rmdir(const char* path);
extern int jl_fs_remove(const char* path);
extern int jl_fs_rename(const char* pathFrom, const char* pathTo);
extern char* jl_fs_get_current_dir(void);
extern int jl_fs_stat_size(const char* path);
extern int jl_fs_stat_isdir(const char* path);

// Static current directory (simplified implementation)
static char os_cwd[256] = "/";

// os.getcwd() - Get current working directory
static mp_obj_t mp_os_getcwd(void) {
    return mp_obj_new_str(os_cwd, strlen(os_cwd));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_os_getcwd_obj, mp_os_getcwd);

// os.chdir(path) - Change current working directory
static mp_obj_t mp_os_chdir(mp_obj_t path_obj) {
    const char* path = mp_obj_str_get_str(path_obj);
    
    // Normalize the path
    if (path[0] == '/') {
        // Absolute path
        strncpy(os_cwd, path, sizeof(os_cwd) - 1);
        os_cwd[sizeof(os_cwd) - 1] = '\0';
    } else {
        // Relative path - append to current directory
        size_t cwd_len = strlen(os_cwd);
        if (cwd_len > 1 && os_cwd[cwd_len - 1] != '/') {
            strncat(os_cwd, "/", sizeof(os_cwd) - cwd_len - 1);
        }
        strncat(os_cwd, path, sizeof(os_cwd) - strlen(os_cwd) - 1);
    }
    
    // Remove trailing slash unless it's root
    size_t len = strlen(os_cwd);
    if (len > 1 && os_cwd[len - 1] == '/') {
        os_cwd[len - 1] = '\0';
    }
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_chdir_obj, mp_os_chdir);

// os.listdir(path) - List directory contents
// Returns a list of strings
static mp_obj_t mp_os_listdir(size_t n_args, const mp_obj_t *args) {
    const char* path;
    if (n_args == 0) {
        path = os_cwd;
    } else {
        path = mp_obj_str_get_str(args[0]);
        if (path[0] == '\0') {
            path = os_cwd;
        }
    }
    
    char* result = jl_fs_listdir(path);
    if (result == NULL) {
        mp_raise_OSError(MP_ENOENT); // directory doesn't exist
    }
    
    // Entries are '\n'-separated ('\n' is illegal in FAT filenames; the old ','
    // separator split names like "a,b.txt" into phantom entries). Parse straight
    // out of the producer's static buffer - no copy, no strtok, and nothing
    // leaks if mp_obj_new_str raises UnicodeError.
    mp_obj_t list_obj = mp_obj_new_list(0, NULL);
    for (const char* p = result; *p;) {
        const char* nl = strchr(p, '\n');
        size_t entry_len = nl ? (size_t)(nl - p) : strlen(p);
        // Normalize: strip trailing slash to match standard os.listdir
        size_t name_len = entry_len;
        if (name_len > 0 && p[name_len - 1] == '/') {
            name_len -= 1;
        }
        if (name_len > 0) {
            mp_obj_list_append(list_obj, mp_obj_new_str(p, name_len));
        }
        p += entry_len + (nl ? 1 : 0);
    }
    
    return list_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_os_listdir_obj, 0, 1, mp_os_listdir);

// os.ilistdir(path) - Iterator version of listdir
// For compatibility, we just return listdir result as a list (not a true iterator)
static mp_obj_t mp_os_ilistdir(size_t n_args, const mp_obj_t *args) {
    const char* path;
    if (n_args == 0) {
        path = os_cwd;
    } else {
        path = mp_obj_str_get_str(args[0]);
        if (path[0] == '\0') {
            path = os_cwd;
        }
    }
    
    char* result = jl_fs_listdir(path);
    if (result == NULL) {
        mp_raise_OSError(MP_ENOENT); // directory doesn't exist
    }
    
    // Parse '\n'-separated entries into ilistdir tuples: (name, type, inode, size)
    // type: 0x4000 for directory, 0x8000 for file (see mp_os_listdir for why '\n')
    mp_obj_t list_obj = mp_obj_new_list(0, NULL);
    for (const char* p = result; *p;) {
        const char* nl = strchr(p, '\n');
        size_t entry_len = nl ? (size_t)(nl - p) : strlen(p);
        // Normalize: strip trailing slash to match standard os.ilistdir
        // (stat on the stripped name - the old code stat'ed "name/" for dirs)
        size_t name_len = entry_len;
        if (name_len > 0 && p[name_len - 1] == '/') {
            name_len -= 1;
        }
        if (name_len > 0) {
            // Build full path for stat
            char fullpath[256];
            if (path[0] == '/' && path[1] == '\0') {
                snprintf(fullpath, sizeof(fullpath), "/%.*s", (int)name_len, p);
            } else {
                snprintf(fullpath, sizeof(fullpath), "%s/%.*s", path, (int)name_len, p);
            }

            int isdir = jl_fs_stat_isdir(fullpath);
            int size = jl_fs_stat_size(fullpath);
            
            // Create tuple: (name, type, inode, size)
            mp_obj_t items[4] = {
                mp_obj_new_str(p, name_len),
                MP_OBJ_NEW_SMALL_INT(isdir ? 0x4000 : 0x8000),
                MP_OBJ_NEW_SMALL_INT(0),  // inode (not supported)
                MP_OBJ_NEW_SMALL_INT(size < 0 ? 0 : size)
            };
            mp_obj_list_append(list_obj, mp_obj_new_tuple(4, items));
        }
        p += entry_len + (nl ? 1 : 0);
    }
    
    return list_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_os_ilistdir_obj, 0, 1, mp_os_ilistdir);

// os.stat(path) - Get file/directory status
// Returns tuple: (mode, ino, dev, nlink, uid, gid, size, atime, mtime, ctime)
static mp_obj_t mp_os_stat(mp_obj_t path_obj) {
    const char* path = mp_obj_str_get_str(path_obj);
    
    int size = jl_fs_stat_size(path);
    int isdir = jl_fs_stat_isdir(path);
    
    if (size < 0 && !isdir) {
        // File/directory doesn't exist
        mp_raise_OSError(ENOENT);
    }
    
    // mode: 0x4000 for directory, 0x8000 for regular file, with permissions
    int mode = isdir ? (0x4000 | 0x1FF) : (0x8000 | 0x1FF);  // drwxrwxrwx or -rwxrwxrwx
    
    // Create stat result tuple (10 elements)
    mp_obj_t items[10] = {
        MP_OBJ_NEW_SMALL_INT(mode),     // st_mode
        MP_OBJ_NEW_SMALL_INT(0),        // st_ino
        MP_OBJ_NEW_SMALL_INT(0),        // st_dev
        MP_OBJ_NEW_SMALL_INT(1),        // st_nlink
        MP_OBJ_NEW_SMALL_INT(0),        // st_uid
        MP_OBJ_NEW_SMALL_INT(0),        // st_gid
        MP_OBJ_NEW_SMALL_INT(size < 0 ? 0 : size),  // st_size
        MP_OBJ_NEW_SMALL_INT(0),        // st_atime
        MP_OBJ_NEW_SMALL_INT(0),        // st_mtime
        MP_OBJ_NEW_SMALL_INT(0),        // st_ctime
    };
    
    return mp_obj_new_tuple(10, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_stat_obj, mp_os_stat);

// os.mkdir(path) - Create directory
// jl_fs_mkdir returns 0 on success, negative errno on failure
static mp_obj_t mp_os_mkdir(mp_obj_t path_obj) {
    const char* path = mp_obj_str_get_str(path_obj);
    int result = jl_fs_mkdir(path);
    if (result < 0) {
        // jl_fs_mkdir returns negative errno on failure
        // Convert to positive errno for mp_raise_OSError
        mp_raise_OSError(-result);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_mkdir_obj, mp_os_mkdir);

// os.rmdir(path) - Remove directory
static mp_obj_t mp_os_rmdir(mp_obj_t path_obj) {
    const char* path = mp_obj_str_get_str(path_obj);
    int result = jl_fs_rmdir(path);
    if (!result) {
        mp_raise_OSError(EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_rmdir_obj, mp_os_rmdir);

// os.remove(path) - Remove file
static mp_obj_t mp_os_remove(mp_obj_t path_obj) {
    const char* path = mp_obj_str_get_str(path_obj);
    int result = jl_fs_remove(path);
    if (!result) {
        mp_raise_OSError(EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_remove_obj, mp_os_remove);

// os.rename(old, new) - Rename file or directory
static mp_obj_t mp_os_rename(mp_obj_t old_obj, mp_obj_t new_obj) {
    const char* old_path = mp_obj_str_get_str(old_obj);
    const char* new_path = mp_obj_str_get_str(new_obj);
    int result = jl_fs_rename(old_path, new_path);
    if (!result) {
        mp_raise_OSError(EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp_os_rename_obj, mp_os_rename);

// os.uname() - Return system information
#if MICROPY_PY_OS_UNAME
#include "genhdr/mpversion.h"

static const qstr mp_os_uname_info_fields[] = {
    MP_QSTR_sysname,
    MP_QSTR_nodename,
    MP_QSTR_release,
    MP_QSTR_version,
    MP_QSTR_machine
};
static const MP_DEFINE_STR_OBJ(mp_os_uname_info_sysname_obj, MICROPY_PY_SYS_PLATFORM);
static const MP_DEFINE_STR_OBJ(mp_os_uname_info_nodename_obj, MICROPY_PY_SYS_PLATFORM);
static const MP_DEFINE_STR_OBJ(mp_os_uname_info_release_obj, MICROPY_VERSION_STRING);
static const MP_DEFINE_STR_OBJ(mp_os_uname_info_version_obj, MICROPY_GIT_TAG " on " MICROPY_BUILD_DATE);
static const MP_DEFINE_STR_OBJ(mp_os_uname_info_machine_obj, MICROPY_HW_BOARD_NAME " with " MICROPY_HW_MCU_NAME);

static MP_DEFINE_ATTRTUPLE(
    mp_os_uname_info_obj,
    mp_os_uname_info_fields,
    5,
    MP_ROM_PTR(&mp_os_uname_info_sysname_obj),
    MP_ROM_PTR(&mp_os_uname_info_nodename_obj),
    MP_ROM_PTR(&mp_os_uname_info_release_obj),
    MP_ROM_PTR(&mp_os_uname_info_version_obj),
    MP_ROM_PTR(&mp_os_uname_info_machine_obj)
);

static mp_obj_t mp_os_uname(void) {
    return MP_OBJ_FROM_PTR(&mp_os_uname_info_obj);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_os_uname_obj, mp_os_uname);
#endif

// Module globals table
static const mp_rom_map_elem_t os_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_os) },
    
    // Path separator
    { MP_ROM_QSTR(MP_QSTR_sep), MP_ROM_QSTR(MP_QSTR__slash_) },
    
    // Directory operations
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&mp_os_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&mp_os_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&mp_os_listdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_ilistdir), MP_ROM_PTR(&mp_os_ilistdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&mp_os_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&mp_os_rmdir_obj) },
    
    // File operations
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&mp_os_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&mp_os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_unlink), MP_ROM_PTR(&mp_os_remove_obj) },  // alias
    { MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&mp_os_rename_obj) },
    
    #if MICROPY_PY_OS_UNAME
    { MP_ROM_QSTR(MP_QSTR_uname), MP_ROM_PTR(&mp_os_uname_obj) },
    #endif
};
static MP_DEFINE_CONST_DICT(os_module_globals, os_module_globals_table);

// Module definition
const mp_obj_module_t mp_module_os = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&os_module_globals,
};

// Register the module
#endif // MICROPY_JL_CUSTOM_OS_BRIDGE && MICROPY_PY_OS