#define NOMINMAX
#include "HookUtils.h"
#include "LightningScanner/LightningScanner.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <LogUtils.h>
namespace HookUtils {
void *LookupCallee(void *caller, void *callee, bool stopAtCC,
                   size_t maxSearch) {
  uint8_t *start = static_cast<uint8_t *>(caller);
  for (size_t i = 0; i < maxSearch; i++) {
    // Stop if we hit a CC (int 3) instruction and stopAtCC is true
    if (stopAtCC && *(uint32_t*)start == 0xCCCCCCCC) {
      break;
    }
    // Look for E8 (call) instruction
    if (start[i] == 0xE8) {
      // Calculate target address
      int32_t relativeAddr = *reinterpret_cast<int32_t *>(start + i + 1);
      void *targetAddr = reinterpret_cast<void *>(start + i + 5 + relativeAddr);

      // Check if this is the function we're looking for
      if (targetAddr == callee) {
        return start + i;
      }
    }

    if (start[i] == 0xFF && start[i + 1] == 0x15) { // FF 15 E8 5A 13 09
                                                    // Calculate target address
      int32_t relativeAddr = *reinterpret_cast<int32_t *>(start + i + 2);
      void *targetAddr = reinterpret_cast<void *>(start + i + 6 + relativeAddr);
      // Check if this is the function we're looking for
      if (targetAddr == callee) {
        return start + i;
      }
    }
  }
  return nullptr;
}

uintptr_t LookupFunctionPattern(void *func, std::string_view signature, uint32_t scanSize) {
	const auto scanner = LightningScanner::Scanner(signature);
	return (uintptr_t)scanner.Find(func, scanSize).Get<std::byte*>();
}

std::optional<size_t> GetModuleSize(HMODULE module) {
	if (module == nullptr) {
		return {};
	}

	// Get the dos header and verify that it seems valid.
	auto dosHeader = (PIMAGE_DOS_HEADER)module;

	if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
		return {};
	}

	// Get the nt headers and verify that they seem valid.
	auto ntHeaders = (PIMAGE_NT_HEADERS)((uintptr_t)dosHeader + dosHeader->e_lfanew);

	if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
		return {};
	}

	// OptionalHeader is not actually optional.
	return ntHeaders->OptionalHeader.SizeOfImage;
}

std::optional<ModuleSection> GetModuleSectionRange(HMODULE module,
                                                  std::string_view sectionName) {
  if (module == nullptr || sectionName.empty() ||
      sectionName.size() > IMAGE_SIZEOF_SHORT_NAME) {
    return {};
  }

  auto dosHeader = (PIMAGE_DOS_HEADER)module;
  if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
    return {};
  }

  auto ntHeaders = (PIMAGE_NT_HEADERS)((uintptr_t)dosHeader + dosHeader->e_lfanew);
  if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
    return {};
  }

  std::array<char, IMAGE_SIZEOF_SHORT_NAME> targetSectionName{};
  memcpy(targetSectionName.data(), sectionName.data(), sectionName.size());

  auto section = IMAGE_FIRST_SECTION(ntHeaders);
  for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i, ++section) {
    if (memcmp(section->Name, targetSectionName.data(),
               targetSectionName.size()) != 0) {
      continue;
    }

    const auto sectionSize = std::max(static_cast<size_t>(section->Misc.VirtualSize),
                                      static_cast<size_t>(section->SizeOfRawData));
    if (sectionSize == 0) {
      return {};
    }

    return ModuleSection{
        .start = reinterpret_cast<uintptr_t>(module) + section->VirtualAddress,
        .size = sectionSize,
    };
  }

  return {};
}

std::optional<ModuleSection> GetModuleTextSectionRange(HMODULE module) {
  return GetModuleSectionRange(module, ".text");
}

uintptr_t ScanIDAPattern(std::string_view signature, int32_t offset, int32_t relOffset, int32_t instructionLength) {
	using namespace LightningScanner;
	const auto scanner = Scanner(signature);
	auto module = GetModuleHandleA(nullptr);
	void* scanStart = module;
	size_t scanSize = GetModuleSize(module).value_or(0);
	if (const auto textSection = GetModuleTextSectionRange(module); textSection.has_value()) {
		scanStart = reinterpret_cast<void*>(textSection->start);
		scanSize = textSection->size;
	}
	if (scanStart == nullptr || scanSize == 0) {
		return 0;
	}

	auto addr = (uintptr_t)scanner.Find(scanStart, scanSize).Get<std::byte*>();
	if (addr) {
		addr += offset;
		if (relOffset > 0 && instructionLength != 0) {
			addr = ReadOffsetData(addr, relOffset, instructionLength);
		}
	}
  // if (!addr) {
  //   throw std::runtime_error("failed to find pattern: " + std::string(signature));
  // }
	return addr;
}


size_t CountIDAPatternMatches(std::string_view signature, size_t stopAfter) {
  using namespace LightningScanner;
  if (stopAfter == 0) {
    return 0;
  }
  const auto module = GetModuleHandleA(nullptr);
  if (module == nullptr) {
    return 0;
  }
  void* scanStart = module;
  size_t scanSize = GetModuleSize(module).value_or(0);
  if (const auto textSection = GetModuleTextSectionRange(module); textSection.has_value()) {
    scanStart = reinterpret_cast<void*>(textSection->start);
    scanSize = textSection->size;
  }
  if (scanStart == nullptr || scanSize == 0) {
    return 0;
  }
  const auto scanner = Scanner(signature);
  size_t found = 0;
  auto* cursor = static_cast<std::byte*>(scanStart);
  auto* const end = cursor + scanSize;
  while (cursor < end) {
    auto* hit = scanner.Find(cursor, static_cast<size_t>(end - cursor)).Get<std::byte>();
    if (hit == nullptr) {
      break;
    }
    if (++found >= stopAfter) {
      break;
    }
    cursor = hit + 1;  // overlapping matches are still distinct sites
  }
  return found;
}

bool SafeReadBuf(uintptr_t addr, void *data, size_t len) {
  DWORD oldProtect;
  if (VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
    memcpy(data, (void *)addr, len);
    if (VirtualProtect((void *)addr, len, oldProtect, &oldProtect))
      return true;
  }
  return false;
}

uintptr_t ReadOffsetData(uintptr_t location, int32_t relOffset,
                         uint32_t instructionLen) {
  if (!location) {
    return 0;
  }
  int32_t rel32 = 0;
  SafeReadBuf(location + relOffset, &rel32, sizeof(int32_t));
  return location + instructionLen + rel32;
}

void SafeWriteBuf(uintptr_t addr, void *data, size_t len) {
  unsigned long oldProtect;

  VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect);
  memcpy((void *)addr, data, len);
  VirtualProtect((void *)addr, len, oldProtect, &oldProtect);
}

template <class Ty> static inline Ty SafeWrite_Impl(size_t addr, Ty data) {
  DWORD oldProtect = 0;
  Ty oldVal = 0;

  if (VirtualProtect((void *)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
    Ty *p = (Ty *)addr;
    oldVal = *p;
    *p = data;
    VirtualProtect((void *)addr, 4, oldProtect, &oldProtect);
  }

  return oldVal;
}

void SafeWrite8(uintptr_t addr, uint8_t data) {
  SafeWriteBuf(addr, &data, sizeof(data));
}

void SafeWrite16(uintptr_t addr, uint16_t data) {
  SafeWriteBuf(addr, &data, sizeof(data));
}

void SafeWrite32(uintptr_t addr, uint32_t data) {
  SafeWriteBuf(addr, &data, sizeof(data));
}

uint64_t SafeWrite64(uintptr_t addr, uint64_t data) {
  return SafeWrite_Impl(addr, data);
}

static bool SafeWriteJump_Internal(uintptr_t src, uintptr_t dst, uint8_t op) {
#pragma pack(push, 1)
  struct Code {
    uint8_t op;
    int32_t displ;
  };
#pragma pack(pop)

  static_assert(sizeof(Code) == 5);

  ptrdiff_t delta = dst - (src + sizeof(Code));
  if ((delta < INT_MIN) || (delta > INT_MAX))
    return false;

  Code code;

  code.op = op;
  code.displ = (int32_t)delta;

  SafeWriteBuf(src, &code, sizeof(code));

  return true;
}

bool SafeWriteJump(uintptr_t src, uintptr_t dst) {
  return SafeWriteJump_Internal(src, dst, 0xE9);
}

bool SafeWriteCall(uintptr_t src, uintptr_t dst) {
  return SafeWriteJump_Internal(src, dst, 0xE8);
}

void *GetIATAddr(void *module, const char *searchDllName,
                 const char *searchImportName) {
  uint8_t *base = (uint8_t *)module;
  IMAGE_DOS_HEADER *dosHeader = (IMAGE_DOS_HEADER *)base;
  IMAGE_NT_HEADERS *ntHeader = (IMAGE_NT_HEADERS *)(base + dosHeader->e_lfanew);
  IMAGE_IMPORT_DESCRIPTOR *importTable =
      (IMAGE_IMPORT_DESCRIPTOR
           *)(base + ntHeader->OptionalHeader
                         .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
                         .VirtualAddress);

  for (; importTable->Characteristics; ++importTable) {
    const char *dllName = (const char *)(base + importTable->Name);

    if (!_stricmp(dllName, searchDllName)) {
      // found the dll

      IMAGE_THUNK_DATA *thunkData =
          (IMAGE_THUNK_DATA *)(base + importTable->OriginalFirstThunk);
      uintptr_t *iat = (uintptr_t *)(base + importTable->FirstThunk);

      for (; thunkData->u1.Ordinal; ++thunkData, ++iat) {
        if (!IMAGE_SNAP_BY_ORDINAL(thunkData->u1.Ordinal)) {
          IMAGE_IMPORT_BY_NAME *importInfo =
              (IMAGE_IMPORT_BY_NAME *)(base + thunkData->u1.AddressOfData);

          if (!_stricmp((char *)importInfo->Name, searchImportName)) {
            // found the import
            return iat;
          }
        }
      }

      return NULL;
    }
  }

  return NULL;
}
} // namespace HookUtils
