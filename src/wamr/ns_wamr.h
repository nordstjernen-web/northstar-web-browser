/* Northstar — narrow accessors into WAMR internals for the WebAssembly JS API. */

#ifndef NS_WAMR_H
#define NS_WAMR_H

#include <stdbool.h>
#include <stdint.h>

#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_WAMR_NULL_REF 0xFFFFFFFFu

bool ns_wamr_table_size(wasm_module_inst_t inst, const char *export_name,
                        uint32_t *out_size, uint32_t *out_max);
bool ns_wamr_table_grow(wasm_module_inst_t inst, const char *export_name,
                        uint32_t delta, uint32_t *out_old_size);
bool ns_wamr_table_get_ref(wasm_module_inst_t inst, const char *export_name,
                           uint32_t idx, uint32_t *out_ref);
bool ns_wamr_table_set_ref(wasm_module_inst_t inst, const char *export_name,
                           uint32_t idx, uint32_t ref);
void ns_wamr_externref_reclaim(wasm_module_inst_t inst);

void ns_wamr_set_host_calls_ctors(bool enabled);
/* Lets a module import its memory and table under any module name, not
 * only "env": the host provides them. */
void ns_wamr_set_host_links_imports(bool enabled);

/* Exception handling: exported tags, tag signatures, and moving a thrown
 * wasm exception (tag index plus payload cells) between an instance and
 * the host. */
#define NS_WAMR_EH_MAX_CELLS 16
uint32_t ns_wamr_export_tag_count(wasm_module_t module);
bool ns_wamr_export_tag(wasm_module_t module, uint32_t nth,
                        const char **out_name, uint32_t *out_tag_index);
uint32_t ns_wamr_tag_param_count(wasm_module_t module, uint32_t tag_index);
/* WASM_EXTERNREF for a type the host cannot carry. */
wasm_valkind_t ns_wamr_tag_param_kind(wasm_module_t module, uint32_t tag_index,
                                      uint32_t param);
/* Takes and clears the wasm exception leaving the instance, if any. */
bool ns_wamr_take_exception(wasm_module_inst_t inst, uint32_t *out_tag_index,
                            uint32_t *out_cells, uint32_t max_cells,
                            uint32_t *out_cell_num);
/* Raises a wasm exception in the instance; catchable by its handlers once
 * the current host call returns. */
bool ns_wamr_throw_exception(wasm_module_inst_t inst, uint32_t tag_index,
                             const uint32_t *cells, uint32_t cell_num);

/* Shared memory created on its own, for WebAssembly.Memory({shared: true}):
 * reserved at max_pages so it never moves, reference counted, and used as
 * the imported memory of the next instantiation on this thread once
 * provided (pass NULL afterwards). */
wasm_memory_inst_t ns_wamr_shared_memory_new(uint32_t init_pages,
                                             uint32_t max_pages);
void ns_wamr_shared_memory_retain(wasm_memory_inst_t memory);
void ns_wamr_shared_memory_release(wasm_memory_inst_t memory);
void ns_wamr_provide_shared_memory_for_next_instance(wasm_memory_inst_t memory);

void wasm_runtime_set_enlarge_mem_success_callback(
    void (*callback)(wasm_module_inst_t module_inst, void *user_data),
    void *user_data);

#ifdef __cplusplus
}
#endif

#endif
