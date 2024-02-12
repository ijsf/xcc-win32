#pragma once

enum LinkingType {
  LT_WASM_SEGMENT_INFO        = 5,
  LT_WASM_INIT_FUNCS          = 6,
  LT_WASM_COMDAT_INFO         = 7,
  LT_WASM_SYMBOL_TABLE        = 8,
};

enum SymInfoKind {
  SIK_SYMTAB_FUNCTION         = 0,
  SIK_SYMTAB_DATA             = 1,
  SIK_SYMTAB_GLOBAL           = 2,
  SIK_SYMTAB_SECTION          = 3,
  SIK_SYMTAB_EVENT            = 4,
  SIK_SYMTAB_TABLE            = 5,
};

// enum SymFlags
#define WASM_SYM_BINDING_WEAK       (1 << 0)
#define WASM_SYM_BINDING_LOCAL      (1 << 1)
#define WASM_SYM_VISIBILITY_HIDDEN  (1 << 2)
#define WASM_SYM_UNDEFINED          (1 << 4)
#define WASM_SYM_EXPORTED           (1 << 5)
#define WASM_SYM_EXPLICIT_NAME      (1 << 6)
#define WASM_SYM_NO_STRIP           (1 << 7)
#define WASM_SYM_TLS                (1 << 8)
#define WASM_SYM_ABSOLUTE           (1 << 9)

enum RelocType {
  R_WASM_FUNCTION_INDEX_LEB    = 0,
  R_WASM_TABLE_INDEX_SLEB      = 1,
  R_WASM_TABLE_INDEX_I32       = 2,
  R_WASM_MEMORY_ADDR_LEB       = 3,
  R_WASM_MEMORY_ADDR_SLEB      = 4,
  R_WASM_MEMORY_ADDR_I32       = 5,
  R_WASM_TYPE_INDEX_LEB        = 6,
  R_WASM_GLOBAL_INDEX_LEB      = 7,
  R_WASM_FUNCTION_OFFSET_I32   = 8,
  R_WASM_SECTION_OFFSET_I32    = 9,
  R_WASM_TAG_INDEX_LEB         = 10,  // R_WASM_EVENT_INDEX_LEB
  R_WASM_GLOBAL_INDEX_I32      = 13,
  R_WASM_MEMORY_ADDR_LEB64     = 14,
  R_WASM_MEMORY_ADDR_SLEB64    = 15,
  R_WASM_MEMORY_ADDR_I64       = 16,
  R_WASM_TABLE_INDEX_SLEB64    = 18,
  R_WASM_TABLE_INDEX_I64       = 19,
  R_WASM_TABLE_NUMBER_LEB      = 20,
};

typedef struct {
  uint32_t offset;  // from its function top (not section top).
  uint32_t index;
  uint32_t addend;
  uint8_t type;     // enum RelocType
} RelocInfo;