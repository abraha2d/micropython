/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>

#include "py/runtime.h"
#include "py/mperrno.h"
#include "extmod/vfs.h"
#include "mphalport.h"
#include "modesp32.h"
#include "esp_ota_ops.h"

// esp_partition_read and esp_partition_write can operate on arbitrary bytes
// but esp_partition_erase_range operates on 4k blocks.  The default block size
// for a Partition object is therefore 4k, to make writes efficient, and also
// make it work well with filesystems like littlefs.  The Partition object also
// supports smaller block sizes, in which case a cache is used and writes may
// be less efficient.
#define NATIVE_BLOCK_SIZE_BYTES (4096)

enum {
    ESP32_PARTITION_BOOT,
    ESP32_PARTITION_RUNNING,
};

typedef struct _esp32_partition_obj_t {
    mp_obj_base_t base;
    const esp_partition_t *part;
    uint8_t *cache;
    uint16_t block_size;
} esp32_partition_obj_t;

STATIC esp32_partition_obj_t *esp32_partition_new(const esp_partition_t *part, uint16_t block_size) {
    if (part == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }
    esp32_partition_obj_t *self = mp_obj_malloc(esp32_partition_obj_t, &esp32_partition_type);
    self->part = part;
    self->block_size = block_size;
    if (self->block_size < NATIVE_BLOCK_SIZE_BYTES) {
        self->cache = m_new(uint8_t, NATIVE_BLOCK_SIZE_BYTES);
    } else {
        self->cache = NULL;
    }
    return self;
}

STATIC void esp32_partition_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp32_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<Partition type=%u, subtype=%u, address=%u, size=%u, label=%s, encrypted=%u>",
        self->part->type, self->part->subtype,
        self->part->address, self->part->size,
        &self->part->label[0], self->part->encrypted
        );
}

STATIC mp_obj_t esp32_partition_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    // Check args
    mp_arg_check_num(n_args, n_kw, 1, 2, false);

    // Get requested partition
    const esp_partition_t *part;
    if (mp_obj_is_int(all_args[0])) {
        // Integer given, get that particular partition
        switch (mp_obj_get_int(all_args[0])) {
            case ESP32_PARTITION_BOOT:
                part = esp_ota_get_boot_partition();
                break;
            case ESP32_PARTITION_RUNNING:
                part = esp_ota_get_running_partition();
                break;
            default:
                mp_raise_ValueError(NULL);
        }
    } else {
        // String given, search for partition with that label
        const char *label = mp_obj_str_get_str(all_args[0]);
        part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
        if (part == NULL) {
            part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
        }
    }

    // Get block size if given
    uint16_t block_size = NATIVE_BLOCK_SIZE_BYTES;
    if (n_args == 2) {
        block_size = mp_obj_get_int(all_args[1]);
    }

    // Return new object
    return MP_OBJ_FROM_PTR(esp32_partition_new(part, block_size));
}

STATIC mp_obj_t esp32_partition_find(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    // Parse args
    enum { ARG_type, ARG_subtype, ARG_label, ARG_block_size };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_type, MP_ARG_INT, {.u_int = ESP_PARTITION_TYPE_APP} },
        { MP_QSTR_subtype, MP_ARG_INT, {.u_int = ESP_PARTITION_SUBTYPE_ANY} },
        { MP_QSTR_label, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_block_size, MP_ARG_INT, {.u_int = NATIVE_BLOCK_SIZE_BYTES} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // Get optional label string
    const char *label = NULL;
    if (args[ARG_label].u_obj != mp_const_none) {
        label = mp_obj_str_get_str(args[ARG_label].u_obj);
    }

    // Get block size
    uint16_t block_size = args[ARG_block_size].u_int;

    // Build list of matching partitions
    mp_obj_t list = mp_obj_new_list(0, NULL);
    esp_partition_iterator_t iter = esp_partition_find(args[ARG_type].u_int, args[ARG_subtype].u_int, label);
    while (iter != NULL) {
        mp_obj_list_append(list, MP_OBJ_FROM_PTR(esp32_partition_new(esp_partition_get(iter), block_size)));
        iter = esp_partition_next(iter);
    }
    esp_partition_iterator_release(iter);

    return list;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(esp32_partition_find_fun_obj, 0, esp32_partition_find);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(esp32_partition_find_obj, MP_ROM_PTR(&esp32_partition_find_fun_obj));

STATIC mp_obj_t esp32_partition_info(mp_obj_t self_in) {
    esp32_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t tuple[] = {
        MP_OBJ_NEW_SMALL_INT(self->part->type),
        MP_OBJ_NEW_SMALL_INT(self->part->subtype),
        mp_obj_new_int_from_uint(self->part->address),
        mp_obj_new_int_from_uint(self->part->size),
        mp_obj_new_str(&self->part->label[0], strlen(&self->part->label[0])),
        mp_obj_new_bool(self->part->encrypted),
    };
    return mp_obj_new_tuple(6, tuple);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp32_partition_info_obj, esp32_partition_info);

STATIC mp_obj_t esp32_partition_readblocks(size_t n_args, const mp_obj_t *args) {
    esp32_partition_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint32_t offset = mp_obj_get_int(args[1]) * self->block_size;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[2], &bufinfo, MP_BUFFER_WRITE);
    if (n_args == 4) {
        offset += mp_obj_get_int(args[3]);
    }
    check_esp_err(esp_partition_read(self->part, offset, bufinfo.buf, bufinfo.len));
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esp32_partition_readblocks_obj, 3, 4, esp32_partition_readblocks);

STATIC mp_obj_t esp32_partition_writeblocks(size_t n_args, const mp_obj_t *args) {
    esp32_partition_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint32_t offset = mp_obj_get_int(args[1]) * self->block_size;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[2], &bufinfo, MP_BUFFER_READ);
    if (n_args == 3) {
        // A simple write, which requires erasing first.
        if (self->block_size >= NATIVE_BLOCK_SIZE_BYTES) {
            // Block size is at least native erase-page size, so do an efficient erase.
            check_esp_err(esp_partition_erase_range(self->part, offset, bufinfo.len));
        } else {
            // Block size is less than native erase-page size, so do erase in sections.
            uint32_t addr = (offset / NATIVE_BLOCK_SIZE_BYTES) * NATIVE_BLOCK_SIZE_BYTES;
            uint32_t o = offset % NATIVE_BLOCK_SIZE_BYTES;
            uint32_t top_addr = offset + bufinfo.len;
            while (addr < top_addr) {
                if (o > 0 || top_addr < addr + NATIVE_BLOCK_SIZE_BYTES) {
                    check_esp_err(esp_partition_read(self->part, addr, self->cache, NATIVE_BLOCK_SIZE_BYTES));
                }
                check_esp_err(esp_partition_erase_range(self->part, addr, NATIVE_BLOCK_SIZE_BYTES));
                if (o > 0) {
                    check_esp_err(esp_partition_write(self->part, addr, self->cache, o));
                }
                if (top_addr < addr + NATIVE_BLOCK_SIZE_BYTES) {
                    check_esp_err(esp_partition_write(self->part, top_addr, self->cache, addr + NATIVE_BLOCK_SIZE_BYTES - top_addr));
                }
                o = 0;
                addr += NATIVE_BLOCK_SIZE_BYTES;
            }
        }
    } else {
        // An extended write, erasing must have been done explicitly before this write.
        offset += mp_obj_get_int(args[3]);
    }
    check_esp_err(esp_partition_write(self->part, offset, bufinfo.buf, bufinfo.len));
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esp32_partition_writeblocks_obj, 3, 4, esp32_partition_writeblocks);

STATIC mp_obj_t esp32_partition_ioctl(mp_obj_t self_in, mp_obj_t cmd_in, mp_obj_t arg_in) {
    esp32_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t cmd = mp_obj_get_int(cmd_in);
    switch (cmd) {
        case MP_BLOCKDEV_IOCTL_INIT:
            return MP_OBJ_NEW_SMALL_INT(0);
        case MP_BLOCKDEV_IOCTL_DEINIT:
            return MP_OBJ_NEW_SMALL_INT(0);
        case MP_BLOCKDEV_IOCTL_SYNC:
            return MP_OBJ_NEW_SMALL_INT(0);
        case MP_BLOCKDEV_IOCTL_BLOCK_COUNT:
            return MP_OBJ_NEW_SMALL_INT(self->part->size / self->block_size);
        case MP_BLOCKDEV_IOCTL_BLOCK_SIZE:
            return MP_OBJ_NEW_SMALL_INT(self->block_size);
        case MP_BLOCKDEV_IOCTL_BLOCK_ERASE: {
            if (self->block_size != NATIVE_BLOCK_SIZE_BYTES) {
                return MP_OBJ_NEW_SMALL_INT(-MP_EINVAL);
            }
            uint32_t offset = mp_obj_get_int(arg_in) * NATIVE_BLOCK_SIZE_BYTES;
            check_esp_err(esp_partition_erase_range(self->part, offset, NATIVE_BLOCK_SIZE_BYTES));
            return MP_OBJ_NEW_SMALL_INT(0);
        }
        default:
            return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(esp32_partition_ioctl_obj, esp32_partition_ioctl);

STATIC mp_obj_t esp32_partition_set_boot(mp_obj_t self_in) {
    esp32_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_esp_err(esp_ota_set_boot_partition(self->part));
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp32_partition_set_boot_obj, esp32_partition_set_boot);

STATIC mp_obj_t esp32_partition_get_next_update(mp_obj_t self_in) {
    esp32_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_FROM_PTR(esp32_partition_new(esp_ota_get_next_update_partition(self->part), NATIVE_BLOCK_SIZE_BYTES));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp32_partition_get_next_update_obj, esp32_partition_get_next_update);

STATIC mp_obj_t esp32_partition_mark_app_valid_cancel_rollback(mp_obj_t cls_in) {
    check_esp_err(esp_ota_mark_app_valid_cancel_rollback());
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp32_partition_mark_app_valid_cancel_rollback_fun_obj,
    esp32_partition_mark_app_valid_cancel_rollback);
STATIC MP_DEFINE_CONST_CLASSMETHOD_OBJ(esp32_partition_mark_app_valid_cancel_rollback_obj,
    MP_ROM_PTR(&esp32_partition_mark_app_valid_cancel_rollback_fun_obj));

STATIC mp_obj_t esp32_partition_mark_app_invalid_rollback_and_reboot(mp_obj_t cls_in) {
    check_esp_err(esp_ota_mark_app_invalid_rollback_and_reboot());
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp32_partition_mark_app_invalid_rollback_and_reboot_fun_obj,
    esp32_partition_mark_app_invalid_rollback_and_reboot);
STATIC MP_DEFINE_CONST_CLASSMETHOD_OBJ(esp32_mark_app_invalid_rollback_and_reboot_obj,
    MP_ROM_PTR(&esp32_partition_mark_app_invalid_rollback_and_reboot_fun_obj));

STATIC mp_obj_t esp32_check_rollback_is_possible(mp_obj_t cls_in) {
    return mp_obj_new_bool(esp_ota_check_rollback_is_possible());
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp32_check_rollback_is_possible_fun_obj, esp32_check_rollback_is_possible);
STATIC MP_DEFINE_CONST_CLASSMETHOD_OBJ(esp32_check_rollback_is_possible_obj, MP_ROM_PTR(&esp32_check_rollback_is_possible_fun_obj));

STATIC mp_obj_t esp32_app_description(mp_obj_t self_in) {
    esp32_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    esp_app_desc_t app;

    check_esp_err(esp_ota_get_partition_description(self->part, &app));

    mp_obj_t tuple[] = {
        mp_obj_new_int_from_uint(app.secure_version),
        mp_obj_new_str(app.version, strlen(app.version)),
        mp_obj_new_str(app.project_name, strlen(app.project_name)),
        mp_obj_new_str(app.time, strlen(app.time)),
        mp_obj_new_str(app.date, strlen(app.date)),
        mp_obj_new_str(app.idf_ver, strlen(app.idf_ver)),
        mp_obj_new_bytes(app.app_elf_sha256, 32)
    };
    return mp_obj_new_tuple(7, tuple);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp32_app_description_obj, esp32_app_description);

STATIC mp_obj_t esp32_app_get_state(mp_obj_t self_in) {
    esp32_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char *ret = NULL;
    esp_ota_img_states_t state;

    check_esp_err(esp_ota_get_state_partition(self->part, &state));

    switch (state) {
        // Monitor the first boot. In bootloader this state is changed to ESP_OTA_IMG_PENDING_VERIFY.
        case ESP_OTA_IMG_NEW:
            ret = "new";
            break;
        // First boot for this app. If this state persists during second boot, then it will be changed to ABORTED.
        case ESP_OTA_IMG_PENDING_VERIFY:
            ret = "verify";
            break;
        // App was confirmed as workable. App can boot and work without limits.
        case ESP_OTA_IMG_VALID:
            ret = "valid";
            break;
        // App was confirmed as non-workable. This app will not be selected to boot at all.
        case ESP_OTA_IMG_INVALID:
            ret = "invalid";
            break;
        // App could not confirmed as workable or non-workable. In bootloader IMG_PENDING_VERIFY state will be changed to IMG_ABORTED. This app will not be selected to boot at all.
        case ESP_OTA_IMG_ABORTED:
            ret = "aborted";
            break;
        // App can boot and work without limits.
        default:
            ret = "undefined";
    }
    return mp_obj_new_str(ret, strlen(ret));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp32_app_get_state_obj, esp32_app_get_state);

STATIC mp_obj_t esp32_ota_begin(size_t n_args, const mp_obj_t *args) {
    esp32_partition_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    esp_ota_handle_t handle;
    size_t image_size = 0;

    if (n_args == 2) {
        image_size = mp_obj_get_int(args[1]);
    }
    check_esp_err(esp_ota_begin(self->part, image_size, &handle));
    return mp_obj_new_int_from_uint(handle);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esp32_ota_begin_obj, 1, 2, esp32_ota_begin);

STATIC mp_obj_t esp32_ota_write(mp_obj_t self_in, const mp_obj_t handle_in, const mp_obj_t data_in) {
    const esp_ota_handle_t handle = mp_obj_get_int(handle_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);

    check_esp_err(esp_ota_write(handle, bufinfo.buf, bufinfo.len));
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(esp32_ota_write_obj, esp32_ota_write);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 2, 0)
STATIC mp_obj_t esp32_ota_write_with_offset(size_t n_args, const mp_obj_t *args) {
    esp_ota_handle_t handle = mp_obj_get_int(args[1]);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[2], &bufinfo, MP_BUFFER_READ);
    const uint32_t offset = mp_obj_get_int(args[3]);

    check_esp_err(esp_ota_write_with_offset(handle, bufinfo.buf, bufinfo.len, offset));
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esp32_ota_write_with_offset_obj, 4, 4, esp32_ota_write_with_offset);
#endif

STATIC mp_obj_t esp32_ota_end(mp_obj_t self_in, const mp_obj_t handle_in) {
    const esp_ota_handle_t handle = mp_obj_get_int(handle_in);

    check_esp_err(esp_ota_end(handle));
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(esp32_ota_end_obj, esp32_ota_end);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 3, 0)
STATIC mp_obj_t esp32_ota_abort(mp_obj_t self_in, const mp_obj_t handle_in) {
    esp_ota_handle_t handle = mp_obj_get_int(handle_in);

    check_esp_err(esp_ota_abort(handle));
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(esp32_ota_abort_obj, esp32_ota_abort);
#endif

STATIC const mp_rom_map_elem_t esp32_partition_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_find), MP_ROM_PTR(&esp32_partition_find_obj) },

    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&esp32_partition_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_readblocks), MP_ROM_PTR(&esp32_partition_readblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks), MP_ROM_PTR(&esp32_partition_writeblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&esp32_partition_ioctl_obj) },

    { MP_ROM_QSTR(MP_QSTR_set_boot), MP_ROM_PTR(&esp32_partition_set_boot_obj) },
    { MP_ROM_QSTR(MP_QSTR_mark_app_valid_cancel_rollback), MP_ROM_PTR(&esp32_partition_mark_app_valid_cancel_rollback_obj) },
    { MP_ROM_QSTR(MP_QSTR_mark_app_invalid_rollback_and_reboot), MP_ROM_PTR(&esp32_mark_app_invalid_rollback_and_reboot_obj) },
    { MP_ROM_QSTR(MP_QSTR_check_rollback_is_possible), MP_ROM_PTR(&esp32_check_rollback_is_possible_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_next_update), MP_ROM_PTR(&esp32_partition_get_next_update_obj) },

    { MP_ROM_QSTR(MP_QSTR_app_description), MP_ROM_PTR(&esp32_app_description_obj) },
    { MP_ROM_QSTR(MP_QSTR_app_state), MP_ROM_PTR(&esp32_app_get_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_ota_begin), MP_ROM_PTR(&esp32_ota_begin_obj) },
    { MP_ROM_QSTR(MP_QSTR_ota_write), MP_ROM_PTR(&esp32_ota_write_obj) },
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 2, 0)
    { MP_ROM_QSTR(MP_QSTR_ota_write_with_offset), MP_ROM_PTR(&esp32_ota_write_with_offset_obj) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_ota_end), MP_ROM_PTR(&esp32_ota_end_obj) },
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 3, 0)
    { MP_ROM_QSTR(MP_QSTR_ota_abort), MP_ROM_PTR(&esp32_ota_abort_obj) },
    #endif

    { MP_ROM_QSTR(MP_QSTR_BOOT), MP_ROM_INT(ESP32_PARTITION_BOOT) },
    { MP_ROM_QSTR(MP_QSTR_RUNNING), MP_ROM_INT(ESP32_PARTITION_RUNNING) },
    { MP_ROM_QSTR(MP_QSTR_TYPE_APP), MP_ROM_INT(ESP_PARTITION_TYPE_APP) },
    { MP_ROM_QSTR(MP_QSTR_TYPE_DATA), MP_ROM_INT(ESP_PARTITION_TYPE_DATA) },
};
STATIC MP_DEFINE_CONST_DICT(esp32_partition_locals_dict, esp32_partition_locals_dict_table);

const mp_obj_type_t esp32_partition_type = {
    { &mp_type_type },
    .name = MP_QSTR_Partition,
    .print = esp32_partition_print,
    .make_new = esp32_partition_make_new,
    .locals_dict = (mp_obj_dict_t *)&esp32_partition_locals_dict,
};
