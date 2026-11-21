/* Northstar — narrow accessors into WAMR internals for the WebAssembly JS API. */

#include "ns_wamr.h"

#include "wasm_runtime_common.h"
#include "../interpreter/wasm_runtime.h"

static WASMTableInstance *
ns_wamr_find_export_table(wasm_module_inst_t inst, const char *export_name,
                          uint32 *out_table_idx)
{
    WASMModuleInstance *module_inst = (WASMModuleInstance *)inst;
    WASMModule *module;
    uint32 i;

    if (!module_inst || module_inst->module_type != Wasm_Module_Bytecode)
        return NULL;
    if (!export_name)
        return NULL;

    module = module_inst->module;
    for (i = 0; i < module->export_count; i++) {
        WASMExport *export = module->exports + i;
        if (export->kind == EXPORT_KIND_TABLE
            && !strcmp(export->name, export_name)) {
            if (out_table_idx)
                *out_table_idx = export->index;
            return wasm_get_table_inst(module_inst, export->index);
        }
    }
    return NULL;
}

bool
ns_wamr_table_size(wasm_module_inst_t inst, const char *export_name,
                   uint32_t *out_size, uint32_t *out_max)
{
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         NULL);
    if (!table)
        return false;
    if (out_size)
        *out_size = table->cur_size;
    if (out_max)
        *out_max = table->max_size;
    return true;
}

bool
ns_wamr_table_grow(wasm_module_inst_t inst, const char *export_name,
                   uint32_t delta, uint32_t *out_old_size)
{
    uint32 table_idx = 0;
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         &table_idx);
    if (!table)
        return false;
    if (out_old_size)
        *out_old_size = table->cur_size;
    return wasm_enlarge_table((WASMModuleInstance *)inst, table_idx, delta,
                              NULL_REF);
}

bool
ns_wamr_table_get_ref(wasm_module_inst_t inst, const char *export_name,
                      uint32_t idx, uint32_t *out_ref)
{
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         NULL);
    if (!table || idx >= table->cur_size)
        return false;
    *out_ref = (uint32)table->elems[idx];
    return true;
}

bool
ns_wamr_table_set_ref(wasm_module_inst_t inst, const char *export_name,
                      uint32_t idx, uint32_t ref)
{
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         NULL);
    if (!table || idx >= table->cur_size)
        return false;
    table->elems[idx] = ref;
    return true;
}

void
ns_wamr_externref_reclaim(wasm_module_inst_t inst)
{
    if (inst)
        wasm_externref_reclaim(inst);
}

extern bool ns_wamr_host_calls_ctors;

void
ns_wamr_set_host_calls_ctors(bool enabled)
{
    ns_wamr_host_calls_ctors = enabled;
}

extern bool ns_wamr_host_links_imports;

void
ns_wamr_set_host_links_imports(bool enabled)
{
    ns_wamr_host_links_imports = enabled;
}

void
wasm_trap_delete(void *trap)
{
    (void)trap;
}

#if WASM_ENABLE_EXCE_HANDLING != 0 && WASM_ENABLE_FAST_INTERP != 0
static WASMFuncType *
ns_wamr_tag_type(WASMModule *module, uint32 tag_index)
{
    if (tag_index < module->import_tag_count)
        return module->import_tags[tag_index].u.tag.tag_type;
    if (tag_index - module->import_tag_count < module->tag_count)
        return module->tags[tag_index - module->import_tag_count]->tag_type;
    return NULL;
}
#endif

uint32_t
ns_wamr_export_tag_count(wasm_module_t module_comm)
{
#if WASM_ENABLE_EXCE_HANDLING != 0 && WASM_ENABLE_FAST_INTERP != 0
    WASMModule *module = (WASMModule *)module_comm;
    uint32 i, n = 0;
    if (!module || module->module_type != Wasm_Module_Bytecode)
        return 0;
    for (i = 0; i < module->export_count; i++)
        if (module->exports[i].kind == EXPORT_KIND_TAG)
            n++;
    return n;
#else
    (void)module_comm;
    return 0;
#endif
}

bool
ns_wamr_export_tag(wasm_module_t module_comm, uint32_t nth,
                   const char **out_name, uint32_t *out_tag_index)
{
#if WASM_ENABLE_EXCE_HANDLING != 0 && WASM_ENABLE_FAST_INTERP != 0
    WASMModule *module = (WASMModule *)module_comm;
    uint32 i;
    if (!module || module->module_type != Wasm_Module_Bytecode)
        return false;
    for (i = 0; i < module->export_count; i++) {
        if (module->exports[i].kind != EXPORT_KIND_TAG)
            continue;
        if (nth-- == 0) {
            *out_name = module->exports[i].name;
            *out_tag_index = module->exports[i].index;
            return true;
        }
    }
#else
    (void)module_comm;
    (void)nth;
    (void)out_name;
    (void)out_tag_index;
#endif
    return false;
}

uint32_t
ns_wamr_tag_param_count(wasm_module_t module_comm, uint32_t tag_index)
{
#if WASM_ENABLE_EXCE_HANDLING != 0 && WASM_ENABLE_FAST_INTERP != 0
    WASMFuncType *type = ns_wamr_tag_type((WASMModule *)module_comm, tag_index);
    return type ? type->param_count : 0;
#else
    (void)module_comm;
    (void)tag_index;
    return 0;
#endif
}

wasm_valkind_t
ns_wamr_tag_param_kind(wasm_module_t module_comm, uint32_t tag_index,
                       uint32_t param)
{
#if WASM_ENABLE_EXCE_HANDLING != 0 && WASM_ENABLE_FAST_INTERP != 0
    WASMFuncType *type = ns_wamr_tag_type((WASMModule *)module_comm, tag_index);
    if (type && param < type->param_count) {
        switch (type->types[param]) {
        case VALUE_TYPE_I32: return WASM_I32;
        case VALUE_TYPE_I64: return WASM_I64;
        case VALUE_TYPE_F32: return WASM_F32;
        case VALUE_TYPE_F64: return WASM_F64;
        default: break;
        }
    }
#else
    (void)module_comm;
    (void)tag_index;
    (void)param;
#endif
    return WASM_EXTERNREF;
}

bool
ns_wamr_take_exception(wasm_module_inst_t inst, uint32_t *out_tag_index,
                       uint32_t *out_cells, uint32_t max_cells,
                       uint32_t *out_cell_num)
{
#if WASM_ENABLE_EXCE_HANDLING != 0 && WASM_ENABLE_FAST_INTERP != 0
    WASMModuleInstance *module_inst = (WASMModuleInstance *)inst;
    const char *exc;
    if (!module_inst || module_inst->module_type != Wasm_Module_Bytecode
        || !module_inst->e->eh_pending)
        return false;
    exc = wasm_get_exception(module_inst);
    module_inst->e->eh_pending = false;
    if (!exc || strcmp(exc, "Exception: " WASM_FAST_EH_EXCEPTION_MSG) != 0)
        return false;
    if (module_inst->e->eh_cell_num > max_cells)
        return false;
    *out_tag_index = module_inst->e->eh_tag_index;
    *out_cell_num = module_inst->e->eh_cell_num;
    memcpy(out_cells, module_inst->e->eh_cells,
           module_inst->e->eh_cell_num * sizeof(uint32));
    wasm_set_exception(module_inst, NULL);
    return true;
#else
    (void)inst;
    (void)out_tag_index;
    (void)out_cells;
    (void)max_cells;
    (void)out_cell_num;
    return false;
#endif
}

bool
ns_wamr_throw_exception(wasm_module_inst_t inst, uint32_t tag_index,
                        const uint32_t *cells, uint32_t cell_num)
{
#if WASM_ENABLE_EXCE_HANDLING != 0 && WASM_ENABLE_FAST_INTERP != 0
    WASMModuleInstance *module_inst = (WASMModuleInstance *)inst;
    if (!module_inst || module_inst->module_type != Wasm_Module_Bytecode
        || cell_num > WASM_FAST_EH_MAX_PAYLOAD_CELLS)
        return false;
    memcpy(module_inst->e->eh_cells, cells, cell_num * sizeof(uint32));
    module_inst->e->eh_tag_index = tag_index;
    module_inst->e->eh_cell_num = cell_num;
    module_inst->e->eh_pending = true;
    wasm_set_exception(module_inst, WASM_FAST_EH_EXCEPTION_MSG);
    return true;
#else
    (void)inst;
    (void)tag_index;
    (void)cells;
    (void)cell_num;
    return false;
#endif
}

#if defined(BH_PLATFORM_WINDOWS)
#include <stdatomic.h>

/* Upstream WAMR defines this in win_atomic.cpp; the build here is C only.
 * os_memory_order follows the order of the C11 memory_order values. */
void
bh_atomic_thread_fence(int mem_order)
{
    atomic_thread_fence((memory_order)mem_order);
}
#endif

#if WASM_ENABLE_SHARED_MEMORY != 0
#include "wasm_memory.h"
#include "wasm_shared_memory.h"

extern void
ns_wamr_provide_shared_memory(WASMMemoryInstance *memory);

wasm_memory_inst_t
ns_wamr_shared_memory_new(uint32_t init_pages, uint32_t max_pages)
{
    WASMMemoryInstance *memory;
    uint64 data_size = 0;

    if (max_pages < init_pages || max_pages > DEFAULT_MAX_PAGES)
        return NULL;
    if (!(memory = wasm_runtime_malloc(sizeof(WASMMemoryInstance))))
        return NULL;
    memset(memory, 0, sizeof(WASMMemoryInstance));
    if (wasm_allocate_linear_memory(&memory->memory_data, true, false,
                                    DEFAULT_NUM_BYTES_PER_PAGE, init_pages,
                                    max_pages, &data_size)
        != BHT_OK) {
        wasm_runtime_free(memory);
        return NULL;
    }
    memory->module_type = Wasm_Module_Bytecode;
    memory->num_bytes_per_page = DEFAULT_NUM_BYTES_PER_PAGE;
    memory->cur_page_count = init_pages;
    memory->max_page_count = max_pages;
    memory->memory_data_size = data_size;
    memory->memory_data_end = memory->memory_data + data_size;
    memory->heap_data = memory->memory_data_end;
    memory->heap_data_end = memory->memory_data_end;
    if (data_size > 0)
        wasm_runtime_set_mem_bound_check_bytes(memory, data_size);
    memory->is_shared_memory = 1;
    memory->ref_count = 1;
    return (wasm_memory_inst_t)memory;
}

void
ns_wamr_shared_memory_retain(wasm_memory_inst_t memory)
{
    if (memory)
        shared_memory_inc_reference((WASMMemoryInstance *)memory);
}

void
ns_wamr_shared_memory_release(wasm_memory_inst_t memory_comm)
{
    WASMMemoryInstance *memory = (WASMMemoryInstance *)memory_comm;
    if (!memory || shared_memory_dec_reference(memory) > 0)
        return;
    if (memory->memory_data)
        wasm_deallocate_linear_memory(memory);
    wasm_runtime_free(memory);
}

void
ns_wamr_provide_shared_memory_for_next_instance(wasm_memory_inst_t memory)
{
    ns_wamr_provide_shared_memory((WASMMemoryInstance *)memory);
}
#endif
