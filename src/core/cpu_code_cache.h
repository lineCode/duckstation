#pragma once
#include "bus.h"
#include "common/bitfield.h"
#include "common/page_fault_handler.h"
#include "cpu_core.h"
#include "cpu_types.h"
#include <array>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#ifdef WITH_RECOMPILER
#include "cpu_recompiler_types.h"
#endif

class JitCodeBuffer;

class Bus;
class System;

namespace CPU {
class Core;

namespace Recompiler {
class ASMFunctions;
}

union CodeBlockKey
{
  u32 bits;

  BitField<u32, bool, 0, 1> user_mode;
  BitField<u32, u32, 2, 30> aligned_pc;

  ALWAYS_INLINE u32 GetPC() const { return aligned_pc << 2; }
  ALWAYS_INLINE void SetPC(u32 pc) { aligned_pc = pc >> 2; }

  ALWAYS_INLINE u32 GetPCPhysicalAddress() const { return (aligned_pc << 2) & PHYSICAL_MEMORY_ADDRESS_MASK; }

  ALWAYS_INLINE CodeBlockKey& operator=(const CodeBlockKey& rhs)
  {
    bits = rhs.bits;
    return *this;
  }

  ALWAYS_INLINE bool operator==(const CodeBlockKey& rhs) const { return bits == rhs.bits; }
  ALWAYS_INLINE bool operator!=(const CodeBlockKey& rhs) const { return bits != rhs.bits; }
  ALWAYS_INLINE bool operator<(const CodeBlockKey& rhs) const { return bits < rhs.bits; }
};

struct CodeBlockInstruction
{
  Instruction instruction;
  u32 pc;

  bool is_branch_instruction : 1;
  bool is_branch_delay_slot : 1;
  bool is_load_instruction : 1;
  bool is_store_instruction : 1;
  bool is_load_delay_slot : 1;
  bool is_last_instruction : 1;
  bool has_load_delay : 1;
  bool can_trap : 1;
};

struct CodeBlock
{
  using HostCodePointer = void (*)(Core*);

  CodeBlock(const CodeBlockKey key_) : key(key_) {}

  CodeBlockKey key;
  u32 host_code_size = 0;
  HostCodePointer host_code = nullptr;

  std::vector<CodeBlockInstruction> instructions;
  std::vector<CodeBlock*> link_predecessors;
  std::vector<CodeBlock*> link_successors;

#ifdef WITH_RECOMPILER
  std::vector<Recompiler::LoadStoreBackpatchInfo> loadstore_backpatch_info;
#endif

  bool contains_loadstore_instructions = false;
  bool invalidated = false;

  const u32 GetPC() const { return key.GetPC(); }
  const u32 GetSizeInBytes() const { return static_cast<u32>(instructions.size()) * sizeof(Instruction); }
  const u32 GetStartPageIndex() const { return (key.GetPCPhysicalAddress() / CPU_CODE_CACHE_PAGE_SIZE); }
  const u32 GetEndPageIndex() const
  {
    return ((key.GetPCPhysicalAddress() + GetSizeInBytes()) / CPU_CODE_CACHE_PAGE_SIZE);
  }
  bool IsInRAM() const
  {
    // TODO: Constant
    return key.GetPCPhysicalAddress() < 0x200000;
  }
};

class BlockFunctionLookup
{
public:
  ALWAYS_INLINE void Reset(CodeBlock::HostCodePointer default_function) { m_slots.fill(default_function); }

  ALWAYS_INLINE void SetBlockPointer(u32 pc, CodeBlock::HostCodePointer function)
  {
    m_slots[GetArrayIndex(pc)] = function;
  }

  ALWAYS_INLINE void Dispatch(CPU::Core* cpu) const { m_slots[GetArrayIndex(cpu->GetRegs().pc)](cpu); }

private:
  enum : u32
  {
    RAM_SLOT_COUNT = Bus::RAM_SIZE / 4,
    BIOS_SLOT_COUNT = Bus::BIOS_SIZE / 4,
    TOTAL_SLOT_COUNT = RAM_SLOT_COUNT + BIOS_SLOT_COUNT,
  };

  ALWAYS_INLINE static u32 GetArrayIndex(u32 pc)
  {
    return ((pc & PHYSICAL_MEMORY_ADDRESS_MASK) >= Bus::BIOS_BASE) ? (RAM_SLOT_COUNT + ((pc & Bus::BIOS_MASK) >> 2)) :
                                                                     ((pc & Bus::RAM_MASK) >> 2);
  }

  std::array<CodeBlock::HostCodePointer, TOTAL_SLOT_COUNT> m_slots = {};
};

class CodeCache
{
public:
  CodeCache();
  ~CodeCache();

  void Initialize(System* system, Core* core, Bus* bus);
  void Execute();
  void ExecuteRecompiler();

  /// Flushes the code cache, forcing all blocks to be recompiled.
  void Flush();

  /// Changes whether the recompiler is enabled.
  void SetUseRecompiler(bool enable, bool fastmem);

  /// Invalidates all blocks which are in the range of the specified code page.
  void InvalidateBlocksWithPageIndex(u32 page_index);

private:
  using BlockMap = std::unordered_map<u32, CodeBlock*>;
  using HostCodeMap = std::map<CodeBlock::HostCodePointer, CodeBlock*>;

  void LogCurrentState();

  /// Returns the block key for the current execution state.
  CodeBlockKey GetNextBlockKey() const;

  /// Looks up the block in the cache if it's already been compiled.
  CodeBlock* LookupBlock(CodeBlockKey key);

  /// Can the current block execute? This will re-validate the block if necessary.
  /// The block can also be flushed if recompilation failed, so ignore the pointer if false is returned.
  bool RevalidateBlock(CodeBlock* block);

  CodeBlock* CompileBlock(CodeBlockKey key);
  bool CompileBlock(CodeBlock* block);
  void FlushBlock(CodeBlock* block);
  void AddBlockToPageMap(CodeBlock* block);
  void RemoveBlockFromPageMap(CodeBlock* block);
  void AddBlockToHostCodeMap(CodeBlock* block);
  void RemoveBlockFromHostCodeMap(CodeBlock* block);

  /// Link block from to to.
  void LinkBlock(CodeBlock* from, CodeBlock* to);

  /// Unlink all blocks which point to this block, and any that this block links to.
  void UnlinkBlock(CodeBlock* block);

  void InterpretCachedBlock(const CodeBlock& block);
  void InterpretUncachedBlock();

  bool InitializeFastmem();
  void ShutdownFastmem();
  Common::PageFaultHandler::HandlerResult PageFaultHandler(void* exception_pc, void* fault_address, bool is_write);

  // Callbacks from the fast lookup table
  static void FastCompileBlockFunction(CPU::Core* cpu);

  System* m_system = nullptr;
  Core* m_core = nullptr;
  Bus* m_bus = nullptr;

#ifdef WITH_RECOMPILER
  std::unique_ptr<JitCodeBuffer> m_code_buffer;
  std::unique_ptr<Recompiler::ASMFunctions> m_asm_functions;
#endif

  BlockMap m_blocks;
  HostCodeMap m_host_code_map;

  bool m_use_recompiler = false;
  bool m_fastmem = false;

#ifdef WITH_RECOMPILER
  BlockFunctionLookup m_block_function_lookup;
#endif

  std::array<std::vector<CodeBlock*>, CPU_CODE_CACHE_PAGE_COUNT> m_ram_block_map;
};

} // namespace CPU
