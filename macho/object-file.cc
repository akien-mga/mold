#include "mold.h"

#include "../archive-file.h"

namespace mold::macho {

std::ostream &operator<<(std::ostream &out, const InputFile &file) {
  out << path_clean(file.mf->name);
  return out;
}

ObjectFile *ObjectFile::create(Context &ctx, MappedFile<Context> *mf,
                               std::string archive_name) {
  ObjectFile *obj = new ObjectFile;
  obj->mf = mf;
  obj->archive_name = archive_name;
  ctx.obj_pool.push_back(std::unique_ptr<ObjectFile>(obj));
  return obj;
};

void ObjectFile::parse(Context &ctx) {
  MachHeader &hdr = *(MachHeader *)mf->data;
  u8 *p = mf->data + sizeof(hdr);

  MachSection *unwind_sec = nullptr;

  for (i64 i = 0; i < hdr.ncmds; i++) {
    LoadCommand &lc = *(LoadCommand *)p;

    switch (lc.cmd) {
    case LC_SEGMENT_64: {
      SegmentCommand &cmd = *(SegmentCommand *)p;
      MachSection *mach_sec = (MachSection *)(p + sizeof(cmd));

      for (i64 i = 0; i < cmd.nsects; i++) {
        if (mach_sec[i].get_segname() == "__LD" &&
            mach_sec[i].get_sectname() == "__compact_unwind") {
          unwind_sec = &mach_sec[i];
        } else {
          sections.push_back(
            std::make_unique<InputSection>(ctx, *this, mach_sec[i]));
        }
      }
      break;
    }
    case LC_SYMTAB: {
      SymtabCommand &cmd = *(SymtabCommand *)p;
      mach_syms = {(MachSym *)(mf->data + cmd.symoff), cmd.nsyms};
      syms.reserve(mach_syms.size());

      for (MachSym &msym : mach_syms) {
        std::string_view name = (char *)(mf->data + cmd.stroff + msym.stroff);
	syms.push_back(intern(ctx, name));
      }
      break;
    }
    case LC_DYSYMTAB:
    case LC_BUILD_VERSION:
      break;
    default:
      Error(ctx) << *this << ": unknown load command: 0x" << std::hex << lc.cmd;
    }

    p += lc.cmdsize;
  }

  for (std::unique_ptr<InputSection> &sec : sections)
    sec->parse_relocations(ctx);

  if (unwind_sec)
    parse_compact_unwind(ctx, *unwind_sec);
}

void ObjectFile::parse_compact_unwind(Context &ctx, MachSection &hdr) {
  if (hdr.size % sizeof(CompactUnwindEntry))
    Fatal(ctx) << *this << ": invalid __compact_unwind section size";

  i64 num_entries = hdr.size / sizeof(CompactUnwindEntry);
  unwind_records.reserve(num_entries);

  CompactUnwindEntry *src = (CompactUnwindEntry *)(mf->data + hdr.offset);

  // Read compact unwind entries
  for (i64 i = 0; i < num_entries; i++)
    unwind_records.emplace_back(src[i].code_len, src[i].encoding);

  // Read relocations
  MachRel *mach_rels = (MachRel *)(mf->data + hdr.reloff);
  for (i64 i = 0; i < hdr.nreloc; i++) {
    MachRel &r = mach_rels[i];
    if (r.offset >= hdr.size)
      Fatal(ctx) << *this << ": relocation offset too large: " << i;

    i64 idx = r.offset / sizeof(CompactUnwindEntry);
    UnwindRecord &dst = unwind_records[idx];

    auto error = [&]() {
      Fatal(ctx) << *this << ": __compact_unwind: unsupported relocation: " << i;
    };

    switch (r.offset % sizeof(CompactUnwindEntry)) {
    case offsetof(CompactUnwindEntry, code_start): {
      if (r.is_pcrel || r.p2size != 3 || r.is_extern || r.type)
        error();

      Subsection *target =
        sections[r.idx - 1]->find_subsection(ctx, src[idx].code_start);
      if (!target)
        error();

      dst.subsec = target;
      dst.offset = src[idx].code_start - target->input_addr;
      break;
    }
    case offsetof(CompactUnwindEntry, personality):
      if (r.is_pcrel || r.p2size != 3 || !r.is_extern || r.type)
        error();
      dst.personality = syms[r.idx];
      break;
    case offsetof(CompactUnwindEntry, lsda): {
      if (r.is_pcrel || r.p2size != 3 || r.is_extern || r.type)
        error();

      i32 addr = *(i32 *)((u8 *)mf->data + hdr.offset + r.offset);
      dst.lsda = sections[r.idx - 1]->find_subsection(ctx, addr);
      dst.lsda_offset = addr - dst.lsda->input_addr;
      break;
    }
    default:
      Fatal(ctx) << *this << ": __compact_unwind: unsupported relocation: " << i;
    }
  }

  for (i64 i = 0; i < num_entries; i++)
    if (!unwind_records[i].subsec)
      Fatal(ctx) << ": __compact_unwind: missing relocation at " << i;

  // Sort unwind entries by offset
  sort(unwind_records, [](const UnwindRecord &a, const UnwindRecord &b) {
    return std::tuple(a.subsec->input_addr, a.offset) <
           std::tuple(b.subsec->input_addr, b.offset);
  });

  // Associate unwind entries to subsections
  for (i64 i = 0; i < num_entries;) {
    Subsection &subsec = *unwind_records[i].subsec;
    subsec.unwind_offset = i;

    i64 j = i + 1;
    while (j < num_entries && unwind_records[j].subsec == &subsec)
      j++;
    subsec.nunwind = j - i;
    i = j;
  }
}

// Symbols with higher priorities overwrites symbols with lower priorities.
// Here is the list of priorities, from the highest to the lowest.
//
//  1. Strong defined symbol
//  2. Weak defined symbol
//  3. Strong defined symbol in a DSO
//  4. Weak defined symbol in a DSO
//  5. Strong or weak defined symbol in an archive
//  6. Common symbol
//  7. Unclaimed (nonexistent) symbol
//
// Ties are broken by file priority.
static u64 get_rank(InputFile *file, MachSym &msym, bool is_lazy) {
  if (is_lazy)
    return (5 << 24) + file->priority;
  if (file->is_dylib)
    return (3 << 24) + file->priority;
  return (1 << 24) + file->priority;
}

static u64 get_rank(Symbol &sym) {
  InputFile *file = sym.file;
  if (!file)
    return 7 << 24;
  if (!file->archive_name.empty())
    return (5 << 24) + file->priority;
  if (file->is_dylib)
    return (3 << 24) + file->priority;
  return (1 << 24) + file->priority;
}

void ObjectFile::resolve_regular_symbols(Context &ctx) {
  for (i64 i = 0; i < syms.size(); i++) {
    Symbol &sym = *syms[i];
    MachSym &msym = mach_syms[i];

    std::lock_guard lock(sym.mu);

    if (get_rank(this, msym, false) < get_rank(sym)) {
      switch (msym.type) {
      case N_ABS:
        sym.file = this;
        sym.subsec = nullptr;
        sym.value = msym.value;
        sym.is_extern = msym.ext;
        sym.is_lazy = false;
        break;
      case N_SECT:
        sym.file = this;
        sym.subsec = sections[msym.sect - 1]->find_subsection(ctx, msym.value);
        sym.value = msym.value - sym.subsec->input_addr;
        sym.is_extern = msym.ext;
        sym.is_lazy = false;
        break;
      }
    }
  }
}

void ObjectFile::resolve_lazy_symbols(Context &ctx) {
  resolve_regular_symbols(ctx);
}

DylibFile *DylibFile::create(Context &ctx, MappedFile<Context> *mf) {
  DylibFile *dylib = new DylibFile;
  dylib->mf = mf;
  ctx.dylib_pool.push_back(std::unique_ptr<DylibFile>(dylib));
  return dylib;
};

void DylibFile::parse(Context &ctx) {
  switch (get_file_type(mf)) {
  case FileType::TAPI: {
    TextDylib tbd = parse_tbd(ctx, mf);
    for (std::string_view sym : tbd.exports)
      syms.push_back(intern(ctx, sym));
    install_name = tbd.install_name;
    break;
  }
  case FileType::MACH_DYLIB:
    Fatal(ctx) << mf->name << ": .dylib is not supported yet";
  default:
    Fatal(ctx) << mf->name << ": is not a dylib";
  }
}

void DylibFile::resolve_symbols(Context &ctx) {
  for (Symbol *sym : syms) {
    std::lock_guard lock(sym->mu);
    if (sym->file && sym->file->priority < priority)
      continue;
    sym->file = this;
    sym->is_extern = true;
  }
}

} // namespace mold::macho
