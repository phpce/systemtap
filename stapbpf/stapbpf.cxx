/* stapbpf.cxx - SystemTap BPF loader
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cassert>
#include <csignal>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>
#include <getopt.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include "bpfinterp.h"

extern "C" {
#include <linux/bpf.h>
#include <linux/perf_event.h>
/* Introduced in 4.1. */
#ifndef PERF_EVENT_IOC_SET_BPF
#define PERF_EVENT_IOC_SET_BPF _IOW('$', 8, __u32)
#endif
#include <libelf.h>
}

#include "config.h"
#include "../git_version.h"
#include "../version.h"
#include "../bpf-internal.h"

#ifndef EM_BPF
#define EM_BPF 0xeb9f
#endif
#ifndef R_BPF_MAP_FD
#define R_BPF_MAP_FD 1
#endif

using namespace std;

static int group_fd = -1;		// ??? Need one per cpu.
extern "C" { 
int log_level = 0;
};
static int warnings = 1;
static int exit_phase = 0;
static int interrupt_message = 0;
static FILE *output_f = stdout;
static FILE *kmsg;

static const char *module_name;
static const char *module_basename;
static const char *script_name; // name of original systemtap script
static const char *module_license;
static Elf *module_elf;

static uint32_t kernel_version;

// Sized by the contents of the "maps" section.
static bpf_map_def *map_attrs;
static std::vector<int> map_fds;

// Sized by the number of sections, so that we can easily
// look them up by st_shndx.
static std::vector<int> prog_fds;

// Programs to run at begin and end of execution.
static Elf_Data *prog_begin;
static Elf_Data *prog_end;

#define DEBUGFS		"/sys/kernel/debug/tracing/"
#define KPROBE_EVENTS	DEBUGFS "kprobe_events"
#define UPROBE_EVENTS   DEBUGFS "uprobe_events"
#define EVENTS          DEBUGFS "events"

static void unregister_kprobes(const size_t nprobes);

struct kprobe_data
{
  string args;
  char type;
  int prog_fd;
  int event_id;
  int event_fd;				// ??? Need one per cpu.

  kprobe_data(char t, string s, int fd)
    : args(s), type(t), prog_fd(fd), event_id(-1), event_fd(-1)
  { }
};

struct uprobe_data
{
  string path;
  char type;
  int pid;
  unsigned long long offset;
  int prog_fd;
  int event_id;
  int event_fd;

  uprobe_data(string path, char t, int pid, unsigned long long off, int fd)
    : path(path), type(t), pid(pid), offset(off), prog_fd(fd),
      event_id(-1), event_fd(-1)
  { }
};

struct timer_data
{
  unsigned long period;
  int prog_fd;
  int event_fd;

  timer_data(unsigned long period, int fd)
    : period(period), prog_fd(fd), event_fd(-1)
  { }
};

struct perf_data
{
  int event_type;
  int event_config;
  bool has_freq;
  unsigned long interval;
  int prog_fd;
  int event_fd;

  perf_data(int type, int config, bool freq, unsigned long interval, int fd)
    : event_type(type), event_config(config), has_freq(freq),
      interval(interval), prog_fd(fd), event_fd(-1)
  { }
};

struct trace_data
{
  string system;
  string name;
  int prog_fd;
  int event_id;
  int event_fd;

  trace_data(char *s, char *n, int fd)
    : system(s), name(n), prog_fd(fd), event_id(-1), event_fd(-1)
  { }
};

static std::vector<kprobe_data> kprobes;
static std::vector<timer_data> timers;
static std::vector<perf_data> perf_probes;
static std::vector<trace_data> tracepoint_probes;
static std::vector<uprobe_data> uprobes;

static void __attribute__((noreturn))
fatal(const char *str, ...)
{
  if (module_name)
    fprintf(stderr, "Error loading %s: ", module_name);

  va_list va;
  va_start(va, str);
  vfprintf(stderr, str, va);
  va_end(va);
  
  exit(1);
}

static void
fatal_sys()
{
  fatal("%s\n", strerror(errno));
}

static void
fatal_elf()
{
  fatal("%s\n", elf_errmsg(-1));
}

static int
create_group_fds()
{
  perf_event_attr peattr;

  memset(&peattr, 0, sizeof(peattr));
  peattr.size = sizeof(peattr);
  peattr.disabled = 1;
  peattr.type = PERF_TYPE_SOFTWARE;
  peattr.config = PERF_COUNT_SW_DUMMY;

  return group_fd = perf_event_open(&peattr, -1, 0, -1, 0);
}

static void
instantiate_maps (Elf64_Shdr *shdr, Elf_Data *data)
{
  if (shdr->sh_entsize != sizeof(bpf_map_def))
    fatal("map entry size mismatch (%zu != %zu)\n",
	  (size_t)shdr->sh_entsize, sizeof(bpf_map_def));

  size_t i, n = shdr->sh_size / sizeof(bpf_map_def);
  struct bpf_map_def *attrs = static_cast<bpf_map_def *>(data->d_buf);

  map_attrs = attrs;
  map_fds.assign(n, -1);

  /* First, make room for the maps in this process' RLIMIT_MEMLOCK: */
  size_t rlimit_increase = 0;
  for (i = 0; i < n; ++i)
    {
      // TODO: The 58 bytes of overhead space per entry has been
      // decided by trial and error, and may require further tweaking:
      rlimit_increase += (58 + attrs[i].key_size + attrs[i].value_size) * attrs[i].max_entries;
    }

  struct rlimit curr_rlimit;
  int rc;

  rc = getrlimit(RLIMIT_MEMLOCK, &curr_rlimit);
  if (rc < 0)
    fatal("could not get map resource limit: %s\n",
          strerror(errno));

  rlim_t rlim_orig = curr_rlimit.rlim_cur;
  rlim_t rlim_max_orig = curr_rlimit.rlim_max;
  curr_rlimit.rlim_cur += rlimit_increase;
  curr_rlimit.rlim_max += rlimit_increase;
  if (curr_rlimit.rlim_cur < rlim_orig) // handle overflow
    curr_rlimit.rlim_cur = rlim_orig;
  if (curr_rlimit.rlim_max < rlim_max_orig) // handle overflow
    curr_rlimit.rlim_max = rlim_max_orig;

  rc = setrlimit(RLIMIT_MEMLOCK, &curr_rlimit);
  if (rc < 0)
    fatal("could not increase map resource limit -- "
          "cur from %lu to %lu, max from %lu to %lu: %s\n",
          rlim_orig, curr_rlimit.rlim_cur,
          rlim_max_orig, curr_rlimit.rlim_max,
          strerror(errno));
  if (log_level > 1)
    {
      fprintf(stderr, "increasing map cur resource limit from %lu to %lu\n",
              rlim_orig, curr_rlimit.rlim_cur);
      fprintf(stderr, "increasing map max resource limit from %lu to %lu\n",
              rlim_max_orig, curr_rlimit.rlim_max);
    }

  /* Now create the maps: */
  for (i = 0; i < n; ++i)
    {
      if (log_level > 2)
        fprintf(stderr, "creating map entry %zu: key_size %u, value_size %u, "
                "max_entries %u, map_flags %u\n", i,
                attrs[i].key_size, attrs[i].value_size,
                attrs[i].max_entries, attrs[i].map_flags);
      int fd = bpf_create_map(static_cast<bpf_map_type>(attrs[i].type),
			      attrs[i].key_size, attrs[i].value_size,
			      attrs[i].max_entries, attrs[i].map_flags);
      if (fd < 0)
	fatal("map entry %zu: %s\n", i, strerror(errno));
      map_fds[i] = fd;
    }
}

static int
prog_load(Elf_Data *data, const char *name)
{
  enum bpf_prog_type prog_type;

  if (strncmp(name, "kprobe", 6) == 0)
    prog_type = BPF_PROG_TYPE_KPROBE;
  else if (strncmp(name, "kretprobe", 9) == 0)
    prog_type = BPF_PROG_TYPE_KPROBE;
  else if (strncmp(name, "uprobe", 6) == 0)
    prog_type = BPF_PROG_TYPE_KPROBE;
  else if (strncmp(name, "timer", 5) == 0)
    prog_type = BPF_PROG_TYPE_PERF_EVENT;
  else if (strncmp(name, "trace", 5) == 0)
    prog_type = BPF_PROG_TYPE_TRACEPOINT;
  else if (strncmp(name, "perf", 4) == 0)
    {
      if (name[5] == '2' && name[6] == '/')
        prog_type = BPF_PROG_TYPE_TRACEPOINT;
      else
        prog_type = BPF_PROG_TYPE_PERF_EVENT;
    }
  else
    fatal("unhandled program type for section \"%s\"\n", name);

  if (data->d_size % sizeof(bpf_insn))
    fatal("program size not a multiple of %zu\n", sizeof(bpf_insn));

  fprintf (kmsg, "%s (%s): stapbpf: %s, name: %s, d_size: %lu\n",
           module_basename, script_name, VERSION, name, (unsigned long)data->d_size);
  fflush (kmsg); // Otherwise, flush will only happen after the prog runs.
  int fd = bpf_prog_load(prog_type, static_cast<bpf_insn *>(data->d_buf),
			 data->d_size, module_license, kernel_version);
  if (fd < 0)
    {
      if (bpf_log_buf[0] != 0)
	fatal("bpf program load failed: %s\n%s\n",
	      strerror(errno), bpf_log_buf);
      else
	fatal("bpf program load failed: %s\n", strerror(errno));
    }
  return fd;
}

static void
prog_relocate(Elf_Data *prog_data, Elf_Data *rel_data,
	      Elf_Data *sym_data, Elf_Data *str_data,
	      const char *prog_name, unsigned maps_idx, bool allocated)
{
  bpf_insn *insns = static_cast<bpf_insn *>(prog_data->d_buf);
  Elf64_Rel *rels = static_cast<Elf64_Rel *>(rel_data->d_buf);
  Elf64_Sym *syms = static_cast<Elf64_Sym *>(sym_data->d_buf);

  if (prog_data->d_size % sizeof(bpf_insn))
    fatal("program size not a multiple of %zu\n", sizeof(bpf_insn));
  if (rel_data->d_type != ELF_T_REL
      || rel_data->d_size % sizeof(Elf64_Rel))
    fatal("invalid reloc metadata\n");
  if (sym_data->d_type != ELF_T_SYM
      || sym_data->d_size % sizeof(Elf64_Sym))
    fatal("invalid symbol metadata\n");

  size_t psize = prog_data->d_size;
  size_t nrels = rel_data->d_size / sizeof(Elf64_Rel);
  size_t nsyms = sym_data->d_size / sizeof(Elf64_Sym);

  for (size_t i = 0; i < nrels; ++i)
    {
      uint32_t sym = ELF64_R_SYM(rels[i].r_info);
      uint32_t type = ELF64_R_TYPE(rels[i].r_info);
      unsigned long long r_ofs = rels[i].r_offset;
      size_t fd_idx;

      if (type != R_BPF_MAP_FD)
	fatal("invalid relocation type %u\n", type);
      if (sym >= nsyms)
	fatal("invalid symbol index %u\n", sym);
      if (r_ofs >= psize || r_ofs % sizeof(bpf_insn))
	fatal("invalid relocation offset at %s+%llu\n", prog_name, r_ofs);

      if (sym >= nsyms)
	fatal("invalid relocation symbol %u\n", sym);
      if (syms[sym].st_shndx != maps_idx
	  || syms[sym].st_value % sizeof(bpf_map_def)
	  || (fd_idx = syms[sym].st_value / sizeof(bpf_map_def),
	      fd_idx >= map_fds.size()))
	{
	  const char *name = "";
	  if (syms[sym].st_name < str_data->d_size)
	    name = static_cast<char *>(str_data->d_buf) + syms[sym].st_name;
	  if (*name)
	    fatal("symbol %s does not reference a map\n", name);
	  else
	    fatal("symbol %u does not reference a map\n", sym);
	}

      bpf_insn *insn = insns + (r_ofs / sizeof(bpf_insn));
      if (insn->code != (BPF_LD | BPF_IMM | BPF_DW))
	fatal("invalid relocation insn at %s+%llu\n", prog_name, r_ofs);

      insn->src_reg = BPF_PSEUDO_MAP_FD;
      insn->imm = (allocated ? map_fds[fd_idx] : fd_idx);
    }
}

static void
maybe_collect_kprobe(const char *name, unsigned name_idx,
		     unsigned fd_idx, Elf64_Addr offset)
{
  char type;
  string arg;

  if (strncmp(name, "kprobe/", 7) == 0)
    {
      string line;
      const char *stext = NULL;
      type = 'p';
      name += 7;

      ifstream syms("/proc/kallsyms");
      if (!syms)
        fatal("error opening /proc/kallsyms: %s\n", strerror(errno));

      // get value of symbol _stext and add it to the offset found in name.
      while (getline(syms, line))
        {
          const char *l = line.c_str();
          if (strncmp(l + 19, "_stext", 6) == 0)
            {
              stext = l;
              break;
            }
        }

      if (stext == NULL)
        fatal("could not find _stext in /proc/kallsyms");

      unsigned long addr = strtoul(stext, NULL, 16);
      addr += strtoul(name, NULL, 16);
      stringstream ss;
      ss << "0x" << hex << addr;
      arg = ss.str();
    }
  else if (strncmp(name, "kretprobe/", 10) == 0)
    type = 'r', arg = name + 10; 
  else
    return;

  int fd = -1;
  if (fd_idx >= prog_fds.size() || (fd = prog_fds[fd_idx]) < 0)
    fatal("probe %u section %u not loaded\n", name_idx, fd_idx);
  if (offset != 0)
    fatal("probe %u offset non-zero\n", name_idx);

  kprobes.push_back(kprobe_data(type, arg, fd));
}

static void
collect_uprobe(const char *name, unsigned name_idx, unsigned fd_idx)
{
  char type = '\0';
  int pid = -1;
  unsigned long long off = 0;
  char path[PATH_MAX];

  int res = sscanf(name, "uprobe/%c/%d/%llu%s", &type, &pid, &off, path);

  if (!pid)
    pid = -1; // indicates to perf_event_open that we're tracing all processes

  if (res != 4)
    fatal("unable to parse name of probe %u section %u\n", name_idx, fd_idx);

  int fd = -1;
  if (fd_idx >= prog_fds.size() || (fd = prog_fds[fd_idx]) < 0)
    fatal("probe %u section %u not loaded\n", name_idx, fd_idx);

  uprobes.push_back(uprobe_data(std::string(path), type, pid, off, fd));
}

static void
collect_perf(const char *name, unsigned name_idx, unsigned fd_idx)
{
  char has_freq;
  int event_type;
  int event_config;
  unsigned long interval;

  int res = sscanf(name, "perf/%d/%d/%c/%lu",
                   &event_type, &event_config, &has_freq, &interval);
  if (res != 4)
    fatal("unable to parse name of probe %u section %u\n", name_idx, fd_idx);

  int fd = -1;
  if (fd_idx >= prog_fds.size() || (fd = prog_fds[fd_idx]) < 0)
    fatal("probe %u section %u not loaded\n", name_idx, fd_idx);

  perf_probes.push_back(
    perf_data(event_type, event_config, has_freq == 'f', interval, fd));
}

static void
collect_timer(const char *name, unsigned name_idx, unsigned fd_idx)
{
  unsigned long period = strtoul(name + 11, NULL, 10);

  if (strncmp(name + 6, "jiff/", 5) == 0)
    {
      long jiffies_per_sec = sysconf(_SC_CLK_TCK);
      period *= 1e9 / jiffies_per_sec;
    }

  int fd = -1;
  if (fd_idx >= prog_fds.size() || (fd = prog_fds[fd_idx]) < 0)
    fatal("probe %u section %u not loaded\n", name_idx, fd_idx);

  timers.push_back(timer_data(period, fd));
  return;
}

static void
collect_tracepoint(const char *name, unsigned name_idx, unsigned fd_idx)
{
  char tp_system[512];
  char tp_name[512];

  int res = sscanf(name, "trace/%[^/]/%s", tp_system, tp_name);
  if (res != 2 || strlen(name) > 512)
    fatal("unable to parse name of probe %u section %u\n", name_idx, fd_idx);

  int fd = -1;
  if (fd_idx >= prog_fds.size() || (fd = prog_fds[fd_idx]) < 0)
    fatal("probe %u section %u not loaded\n", name_idx, fd_idx);

  tracepoint_probes.push_back(trace_data(tp_system, tp_name, fd));
}

static void
kprobe_collect_from_syms(Elf_Data *sym_data, Elf_Data *str_data)
{
  Elf64_Sym *syms = static_cast<Elf64_Sym *>(sym_data->d_buf);
  size_t nsyms = sym_data->d_type / sizeof(Elf64_Sym);

  if (sym_data->d_type != ELF_T_SYM
      || sym_data->d_size % sizeof(Elf64_Sym))
    fatal("invalid kprobes symbol metadata\n");

  for (size_t i = 0; i < nsyms; ++i)
    {
      const char *name;
      if (syms[i].st_name < str_data->d_size)
	name = static_cast<char *>(str_data->d_buf) + syms[i].st_name;
      else
	fatal("symbol %u has invalid string index\n", i);
      maybe_collect_kprobe(name, i, syms[i].st_shndx, syms[i].st_value);
    }
}

static void
unregister_uprobes(const size_t nprobes)
{
   if (nprobes == 0)
    return;

  int fd = open(DEBUGFS "uprobe_events", O_WRONLY);
  if (fd < 0)
    return;


  const int pid = getpid();
  for (size_t i = 0; i < nprobes; ++i)
    {
      close(uprobes[i].event_fd);

      char msgbuf[128];
      ssize_t olen = snprintf(msgbuf, sizeof(msgbuf), "-:stapprobe_%d_%zu",
			      pid, i);
      ssize_t wlen = write(fd, msgbuf, olen);
      if (wlen < 0)
	fprintf(stderr, "Error removing probe %zu: %s\n",
		i, strerror(errno));
    }
  close(fd);
}

static void
register_uprobes()
{
  size_t nprobes = uprobes.size();
  if (nprobes == 0)
    return;

  int fd = open(UPROBE_EVENTS, O_WRONLY);
  if (fd < 0)
    fatal("Error opening %s: %s\n", UPROBE_EVENTS, strerror(errno));

  const int pid = getpid();

  for (size_t i = 0; i < nprobes; ++i)
    {
      uprobe_data &u = uprobes[i];
      char msgbuf[PATH_MAX];

      ssize_t olen = snprintf(msgbuf, sizeof(msgbuf), "%c:stapprobe_%d_%zu %s:0x%llx",
			      u.type, pid, i, u.path.c_str(), u.offset);
      if ((size_t)olen >= sizeof(msgbuf))
	{
	  fprintf(stderr, "Buffer overflow creating probe %zu\n", i);
	  if (i == 0)
	    goto fail_0;
	  nprobes = i - 1;
	  goto fail_n;
	}

      if (log_level > 1)
        fprintf(stderr, "Associating probe %zu with uprobe %s\n", i, msgbuf);

      ssize_t wlen = write(fd, msgbuf, olen);
      if (wlen != olen)
	{
	  fprintf(stderr, "Error creating probe %zu: %s\n",
		  i, strerror(errno));
	  if (i == 0)
	    goto fail_0;
	  nprobes = i - 1;
	  goto fail_n;
	}
    }
  close(fd);

  for (size_t i = 0; i < nprobes; ++i)
    {
      char fnbuf[PATH_MAX];
      ssize_t len = snprintf(fnbuf, sizeof(fnbuf),
			     DEBUGFS "events/uprobes/stapprobe_%d_%zu/id", pid, i);
      if ((size_t)len >= sizeof(bpf_log_buf))
	{
	  fprintf(stderr, "Buffer overflow creating probe %zu\n", i);
	  goto fail_n;
	}

      fd = open(fnbuf, O_RDONLY);
      if (fd < 0)
	{
	  fprintf(stderr, "Error opening probe event id %zu: %s\n",
		  i, strerror(errno));
	  goto fail_n;
	}

      char msgbuf[128];
      len = read(fd, msgbuf, sizeof(msgbuf) - 1);
      if (len < 0)
	{
	  fprintf(stderr, "Error reading probe event id %zu: %s\n",
		  i, strerror(errno));
	  goto fail_n;
	}
      close(fd);

      msgbuf[len] = 0;
      uprobes[i].event_id = atoi(msgbuf);
    }

  // ??? Iterate to enable on all cpus, each with a different group_fd.
  {
    perf_event_attr peattr;

    memset(&peattr, 0, sizeof(peattr));
    peattr.size = sizeof(peattr);
    peattr.type = PERF_TYPE_TRACEPOINT;
    peattr.sample_type = PERF_SAMPLE_RAW;
    peattr.sample_period = 1;
    peattr.wakeup_events = 1;

    for (size_t i = 0; i < nprobes; ++i)
      {
	uprobe_data &u = uprobes[i];
        peattr.config = u.event_id;

        fd = perf_event_open(&peattr, u.pid, 0, -1, 0);
        if (fd < 0)
	  {
	    fprintf(stderr, "Error opening probe id %zu: %s\n",
		    i, strerror(errno));
	    goto fail_n;
	  }
        u.event_fd = fd;

        if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, u.prog_fd) < 0)
	  {
	    fprintf(stderr, "Error installing bpf for probe id %zu: %s\n",
		    i, strerror(errno));
	    goto fail_n;
	  }
      }
  }
  return;

 fail_n:
  unregister_uprobes(nprobes);
 fail_0:
  exit(1);
}

static void
register_kprobes()
{
  size_t nprobes = kprobes.size();
  if (nprobes == 0)
    return;
    
  int fd = open(KPROBE_EVENTS, O_WRONLY);
  if (fd < 0)
    fatal("Error opening %s: %s\n", KPROBE_EVENTS, strerror(errno));

  const int pid = getpid();

  for (size_t i = 0; i < nprobes; ++i)
    {
      kprobe_data &k = kprobes[i];
      char msgbuf[128];
      
      ssize_t olen = snprintf(msgbuf, sizeof(msgbuf), "%c:p%d_%zu %s",
			      k.type, pid, i, k.args.c_str());
      if ((size_t)olen >= sizeof(msgbuf))
	{
	  fprintf(stderr, "Buffer overflow creating probe %zu\n", i);
	  if (i == 0)
	    goto fail_0;
	  nprobes = i - 1;
	  goto fail_n;
	}

      if (log_level > 1)
        fprintf(stderr, "Associating probe %zu with kprobe %s\n", i, msgbuf);
      
      ssize_t wlen = write(fd, msgbuf, olen);
      if (wlen != olen)
	{
	  fprintf(stderr, "Error creating probe %zu: %s\n",
		  i, strerror(errno));
	  if (i == 0)
	    goto fail_0;
	  nprobes = i - 1;
	  goto fail_n;
	}
    }
  close(fd);

  for (size_t i = 0; i < nprobes; ++i)
    {
      char fnbuf[PATH_MAX];
      ssize_t len = snprintf(fnbuf, sizeof(fnbuf),
			     DEBUGFS "events/kprobes/p%d_%zu/id", pid, i);
      if ((size_t)len >= sizeof(bpf_log_buf))
	{
	  fprintf(stderr, "Buffer overflow creating probe %zu\n", i);
	  goto fail_n;
	}

      fd = open(fnbuf, O_RDONLY);
      if (fd < 0)
	{
	  fprintf(stderr, "Error opening probe event id %zu: %s\n",
		  i, strerror(errno));
	  goto fail_n;
	}

      char msgbuf[128];
      len = read(fd, msgbuf, sizeof(msgbuf) - 1);
      if (len < 0)
	{
	  fprintf(stderr, "Error reading probe event id %zu: %s\n",
		  i, strerror(errno));
	  goto fail_n;
	}
      close(fd);

      msgbuf[len] = 0;
      kprobes[i].event_id = atoi(msgbuf);
    }

  // ??? Iterate to enable on all cpus, each with a different group_fd.
  {
    perf_event_attr peattr;

    memset(&peattr, 0, sizeof(peattr));
    peattr.size = sizeof(peattr);
    peattr.type = PERF_TYPE_TRACEPOINT;
    peattr.sample_type = PERF_SAMPLE_RAW;
    peattr.sample_period = 1;
    peattr.wakeup_events = 1;

    for (size_t i = 0; i < nprobes; ++i)
      {
	kprobe_data &k = kprobes[i];
        peattr.config = k.event_id;

        fd = perf_event_open(&peattr, -1, 0, group_fd, 0);
        if (fd < 0)
	  {
	    fprintf(stderr, "Error opening probe id %zu: %s\n",
		    i, strerror(errno));
	    goto fail_n;
	  }
        k.event_fd = fd;

        if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, k.prog_fd) < 0)
	  {
	    fprintf(stderr, "Error installing bpf for probe id %zu: %s\n",
		    i, strerror(errno));
	    goto fail_n;
	  }
      }
  }
  return;

 fail_n:
  unregister_kprobes(nprobes);
 fail_0:
  exit(1);
}

static void
unregister_kprobes(const size_t nprobes)
{
  if (nprobes == 0)
    return;

  int fd = open(DEBUGFS "kprobe_events", O_WRONLY);
  if (fd < 0)
    return;


  const int pid = getpid();
  for (size_t i = 0; i < nprobes; ++i)
    {
      close(kprobes[i].event_fd);

      char msgbuf[128];
      ssize_t olen = snprintf(msgbuf, sizeof(msgbuf), "-:p%d_%zu",
			      pid, i);
      ssize_t wlen = write(fd, msgbuf, olen);
      if (wlen < 0)
	fprintf(stderr, "Error removing probe %zu: %s\n",
		i, strerror(errno));
    }
  close(fd);
}

static void
unregister_tracepoints(const size_t nprobes)
{
  for (size_t i = 0; i < nprobes; ++i)
    close(tracepoint_probes[i].event_fd);
}

static void
register_tracepoints()
{
  size_t nprobes = tracepoint_probes.size();
  if (nprobes == 0)
    return;

  for (size_t i = 0; i < nprobes; ++i)
    {
      trace_data &t = tracepoint_probes[i];
      char fnbuf[PATH_MAX];
      ssize_t len = snprintf(fnbuf, sizeof(fnbuf),
			     DEBUGFS "events/%s/%s/id",
                             t.system.c_str(), t.name.c_str());
      if ((size_t)len >= sizeof(bpf_log_buf))
	{
	  fprintf(stderr, "Buffer overflow creating probe %zu\n", i);
	  goto fail;
	}

      int fd = open(fnbuf, O_RDONLY);
      if (fd < 0)
	{
	  fprintf(stderr, "Error opening probe event id %zu: %s\n",
		  i, strerror(errno));

          if (errno == ENOENT)
            fprintf(stderr, "\"%s/%s\" could not be found in %s\n",
                    t.system.c_str(), t.name.c_str(), EVENTS);

	  goto fail;
	}

      char msgbuf[128];
      len = read(fd, msgbuf, sizeof(msgbuf) - 1);
      if (len < 0)
	{
	  fprintf(stderr, "Error reading probe event id %zu: %s\n",
		  i, strerror(errno));
	  goto fail;
	}
      close(fd);

      msgbuf[len] = 0;
      t.event_id = atoi(msgbuf);
    }

  // ??? Iterate to enable on all cpus, each with a different group_fd.
  {
    perf_event_attr peattr;

    memset(&peattr, 0, sizeof(peattr));
    peattr.size = sizeof(peattr);
    peattr.type = PERF_TYPE_TRACEPOINT;
    peattr.sample_type = PERF_SAMPLE_RAW;
    peattr.sample_period = 1;
    peattr.wakeup_events = 1;

    for (size_t i = 0; i < nprobes; ++i)
      {
	trace_data &t = tracepoint_probes[i];
        peattr.config = t.event_id;

        int fd = perf_event_open(&peattr, -1, 0, group_fd, 0);
        if (fd < 0)
	  {
	    fprintf(stderr, "Error opening probe id %zu: %s\n",
		    i, strerror(errno));
	    goto fail;
	  }
        t.event_fd = fd;

        if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, t.prog_fd) < 0)
	  {
	    fprintf(stderr, "Error installing bpf for probe id %zu: %s\n",
		    i, strerror(errno));
	    goto fail;
	  }
      }
  }
  return;

 fail:
  unregister_tracepoints(nprobes);
  exit(1);
}

static void
unregister_timers(const size_t nprobes)
{
  for (size_t i = 0; i < nprobes; ++i)
    close(timers[i].event_fd);
}

static void
register_timers()
{
  perf_event_attr peattr;

  memset(&peattr, 0, sizeof(peattr));
  peattr.size = sizeof(peattr);
  peattr.type = PERF_TYPE_SOFTWARE;
  peattr.config = PERF_COUNT_SW_CPU_CLOCK;

  for (size_t i = 0; i < timers.size(); ++i)
    {
      timer_data &t = timers[i];
      peattr.sample_period = t.period;

      int fd = perf_event_open(&peattr, -1, 0, group_fd, 0);
      if (fd < 0)
        {
          int err = errno;
          unregister_timers(timers.size());
          fatal("Error opening timer probe id %zu: %s\n", i + 1, strerror(err));
        }

      t.event_fd = fd;
      if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, t.prog_fd) < 0)
        {
          int err = errno;
          unregister_timers(timers.size());
          fatal("Error installing bpf for timer probe id %zu: %s\n",
                i + 1, strerror(err));
        }
    }

  return;
}

static void
unregister_perf(const size_t nprobes)
{
  for (size_t i = 0; i < nprobes; ++i)
    close(perf_probes[i].event_fd);
}

static void
register_perf()
{
  for (size_t i = 0; i < perf_probes.size(); ++i)
    {
      perf_data &p = perf_probes[i];
      perf_event_attr peattr;

      memset(&peattr, 0, sizeof(peattr));
      peattr.size = sizeof(peattr);
      peattr.type = p.event_type;
      peattr.config = p.event_config;

      if (p.has_freq)
        {
          peattr.freq = 1;
          peattr.sample_freq = p.interval;
        }
      else
        peattr.sample_period = p.interval;

      // group_fd is not used since this event might have an
      // incompatible type/config.
      int fd = perf_event_open(&peattr, -1, 0, -1, 0);
      if (fd < 0)
        {
          int err = errno;
          unregister_perf(perf_probes.size());
          fatal("Error opening perf probe id %zu: %s\n", i + 1, strerror(err));
        }

      p.event_fd = fd;
      if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, p.prog_fd) < 0)
        {
          int err = errno;
          unregister_perf(perf_probes.size());
          fatal("Error installing bpf for perf probe id %zu: %s\n",
                i + 1, strerror(err));
        }
    }
}

static void
init_internal_globals()
{
  using namespace bpf;

  int key = globals::EXIT;
  long val = 0;

  if (bpf_update_elem(map_fds[globals::internal_map_idx],
                     (void*)&key, (void*)&val, BPF_ANY) != 0)
    fatal("Error updating pid: %s\n", errno);
}

static void
load_bpf_file(const char *module)
{
  module_name = module;

  /* Extract basename: */
  char *buf = (char *)malloc(BPF_MAXSTRINGLEN * sizeof(char));
  string module_name_str(module);
  string module_basename_str
    = module_name_str.substr(module_name_str.rfind('/')+1); // basename
  size_t len = module_basename_str.copy(buf, BPF_MAXSTRINGLEN-1);
  buf[len] = '\0';
  module_basename = buf;

  int fd = open(module, O_RDONLY);
  if (fd < 0)
    fatal_sys();

  elf_version(EV_CURRENT);

  Elf *elf = elf_begin(fd, ELF_C_READ_MMAP_PRIVATE, NULL);
  if (elf == NULL)
    fatal_elf();
  module_elf = elf;

  Elf64_Ehdr *ehdr = elf64_getehdr(elf);
  if (ehdr == NULL)
    fatal_elf();

  // Byte order should match the host, since we're loading locally.
  {
    const char *end_str;
    switch (ehdr->e_ident[EI_DATA])
      {
      case ELFDATA2MSB:
	if (__BYTE_ORDER == __BIG_ENDIAN)
	  break;
	end_str = "MSB";
	goto err_endian;
      case ELFDATA2LSB:
	if (__BYTE_ORDER == __LITTLE_ENDIAN)
	  break;
	end_str = "LSB";
	goto err_endian;
      case ELFCLASSNONE:
	end_str = "none";
	goto err_endian;
      default:
	end_str = "unknown";
      err_endian:
	fatal("incorrect byte ordering: %s\n", end_str);
      }
  }

  // Tiny bit of sanity checking on the rest of the header.  Since LLVM
  // began by producing files with EM_NONE, accept that too.
  if (ehdr->e_machine != EM_NONE && ehdr->e_machine != EM_BPF)
    fatal("incorrect machine type: %d\n", ehdr->e_machine);

  unsigned shnum = ehdr->e_shnum;
  prog_fds.assign(shnum, -1);

  std::vector<Elf64_Shdr *> shdrs(shnum, NULL);
  std::vector<Elf_Data *> sh_data(shnum, NULL);
  std::vector<const char *> sh_name(shnum, NULL);
  unsigned maps_idx = 0;
  unsigned version_idx = 0;
  unsigned license_idx = 0;
  unsigned script_name_idx = 0;
  unsigned kprobes_idx = 0;
  unsigned begin_idx = 0;
  unsigned end_idx = 0;

  // First pass to identify special sections, and make sure
  // all data is readable.
  for (unsigned i = 1; i < shnum; ++i)
    {
      Elf_Scn *scn = elf_getscn(elf, i);
      if (!scn)
	fatal_elf();

      Elf64_Shdr *shdr = elf64_getshdr(scn);
      if (!shdr)
	fatal_elf();

      const char *shname = elf_strptr(elf, ehdr->e_shstrndx, shdr->sh_name);
      if (!shname)
	fatal_elf();

      // We need not consider any empty sections.
      if (shdr->sh_size == 0 || !*shname)
	continue;

      Elf_Data *data = elf_getdata(scn, NULL);
      if (data == NULL)
	fatal_elf();

      shdrs[i] = shdr;
      sh_name[i] = shname;
      sh_data[i] = data;

      if (strcmp(shname, "license") == 0)
	license_idx = i;
      else if (strcmp(shname, "stapbpf_script_name") == 0)
	script_name_idx = i;
      else if (strcmp(shname, "version") == 0)
	version_idx = i;
      else if (strcmp(shname, "maps") == 0)
	maps_idx = i;
      else if (strcmp(shname, "kprobes") == 0)
	kprobes_idx = i;
      else if (strcmp(shname, "stap_begin") == 0)
	begin_idx = i;
      else if (strcmp(shname, "stap_end") == 0)
	end_idx = i;
    }

  // Two special sections are not optional.
  if (license_idx != 0)
    module_license = static_cast<char *>(sh_data[license_idx]->d_buf);
  else
    fatal("missing license section\n");
  if (script_name_idx != 0)
    script_name = static_cast<char *>(sh_data[script_name_idx]->d_buf);
  else
    script_name = "<unknown>";
  if (version_idx != 0)
    {
      unsigned long long size = shdrs[version_idx]->sh_size;
      if (size != 4)
	fatal("invalid version size (%llu)\n", size);
      memcpy(&kernel_version, sh_data[version_idx]->d_buf, 4);
    }
  else
    fatal("missing version section\n");

  // Create bpf maps as required.
  if (maps_idx != 0)
    instantiate_maps(shdrs[maps_idx], sh_data[maps_idx]);

  // Relocate all programs that require it.
  for (unsigned i = 1; i < shnum; ++i)
    {
      Elf64_Shdr *rel_hdr = shdrs[i];
      if (rel_hdr == NULL || rel_hdr->sh_type != SHT_REL)
	continue;

      unsigned progi = rel_hdr->sh_info;
      if (progi == 0 || progi >= shnum)
	fatal("invalid section info %u->%u\n", i, progi);
      Elf64_Shdr *prog_hdr = shdrs[progi];

      unsigned symi = rel_hdr->sh_link;
      if (symi == 0 || symi >= shnum)
	fatal("invalid section link %u->%u\n", i, symi);
      Elf64_Shdr *sym_hdr = shdrs[symi];

      unsigned stri = sym_hdr->sh_link;
      if (stri == 0 || stri >= shnum)
	fatal("invalid section link %u->%u\n", symi, stri);

      if (prog_hdr->sh_flags & SHF_EXECINSTR)
	prog_relocate(sh_data[progi], sh_data[i], sh_data[symi],
		      sh_data[stri], sh_name[progi], maps_idx,
		      prog_hdr->sh_flags & SHF_ALLOC);
    }

  // Load all programs that require it.
  for (unsigned i = 1; i < shnum; ++i)
    {
      Elf64_Shdr *shdr = shdrs[i];
      if ((shdr->sh_flags & SHF_ALLOC) && (shdr->sh_flags & SHF_EXECINSTR))
	prog_fds[i] = prog_load(sh_data[i], sh_name[i]);
    }

  // Remember begin and end probes.
  if (begin_idx)
    {
      Elf64_Shdr *shdr = shdrs[begin_idx];
      if (shdr->sh_flags & SHF_EXECINSTR)
	prog_begin = sh_data[begin_idx];
    }
  if (end_idx)
    {
      Elf64_Shdr *shdr = shdrs[end_idx];
      if (shdr->sh_flags & SHF_EXECINSTR)
	prog_end = sh_data[end_idx];
    }

  // Record all kprobes.
  if (kprobes_idx != 0)
    {
      // The Preferred Systemtap Way puts kprobe strings into a symbol
      // table, so that multiple kprobes can reference the same program.

      // ??? We don't really have to have a separate kprobe symbol table;
      // we could pull kprobes out of the main symbol table too.  This
      // would probably make it easier for llvm-bpf folks to transition.
      // One would only need to create symbol aliases with custom asm names.

      Elf64_Shdr *sym_hdr = shdrs[kprobes_idx];
      if (sym_hdr->sh_type != SHT_SYMTAB)
	fatal("invalid section type for kprobes section\n");

      unsigned stri = sym_hdr->sh_link;
      if (stri == 0 || stri >= shnum)
	fatal("invalid section link %u->%u\n", kprobes_idx, stri);

      kprobe_collect_from_syms(sh_data[kprobes_idx], sh_data[stri]);
    }
  else
    {
      // The original llvm-bpf way puts kprobe strings into the
      // section name.  Each kprobe has its own program.
      for (unsigned i = 1; i < shnum; ++i)
	maybe_collect_kprobe(sh_name[i], i, i, 0);
    }

  // Record all other probes
  for (unsigned i = 1; i < shnum; ++i) {
    if (strncmp(sh_name[i], "uprobe", 6) == 0)
      collect_uprobe(sh_name[i], i, i);
    if (strncmp(sh_name[i], "trace", 5) == 0)
      collect_tracepoint(sh_name[i], i, i);
    if (strncmp(sh_name[i], "perf", 4) == 0)
      collect_perf(sh_name[i], i, i);
    if (strncmp(sh_name[i], "timer", 5) == 0)
      collect_timer(sh_name[i], i, i);
  }
}

static int
get_exit_status()
{
  int key = bpf::globals::EXIT;
  long val = 0;

  if (bpf_lookup_elem
       (map_fds[bpf::globals::internal_map_idx], &key, &val) != 0)
    fatal("error during bpf map lookup: %s\n", strerror(errno));

  return val;
}

static void
print_trace_output(pthread_t main_thread)
{
  // Output from printf statements within probe handlers is sent to
  // debugfs via trace_printk. Get this output and copy it to output_f.
  string modname(module_name);
  size_t pos = modname.find_last_of("/");
  if (pos != string::npos)
    modname = modname.substr(pos + 1);

  string start_tag = "<" + modname.erase(modname.size() - 3).erase(4, 1) + ">";
  string end_tag = start_tag;
  end_tag.insert(1, "/");

  while (1)
    {
      ifstream trace_pipe(DEBUGFS "trace_pipe");
      if (!trace_pipe)
        fatal("error opening trace_pipe: %s\n", strerror(errno));

      bool start_tag_seen = false;
      unsigned bytes_written = 0;
      string line, buf;
      while (getline(trace_pipe, line))
        {
          line += "\n";
          if (!start_tag_seen)
            {
              pos = line.find(start_tag);
              if (pos != string::npos)
                {
                  start_tag_seen = true;
                  bytes_written = 0;
                  line = line.substr(pos + start_tag.size(), string::npos);
                }
            }
          if (start_tag_seen)
            {
              pos = line.find(end_tag);
              if (pos != string::npos)
                {
                  line = line.substr(0, pos);
                  start_tag_seen = false;

                  // exit() causes "" to be written to trace_pipe. If
                  // "" is seen and the exit flag is set, wake up main
                  // thread to begin program shutdown.
                  if (line == "" && buf == "" && bytes_written == 0
                      && get_exit_status())
                    {
                      pthread_kill(main_thread, SIGINT);
                      return;
                    }
                }

              buf += line;
              if (fwrite(buf.c_str(), sizeof(char), buf.size(), output_f)
                   != buf.size())
                fatal("error writing to output file: %s\n", strerror(errno));
              bytes_written += buf.size();

              fflush(output_f);
              buf = "";
            }
        }
      // If another process opens trace_pipe then we may read EOF. In this
      // case simply reopen the file and continue parsing.
    }
}

static void
usage(const char *argv0)
{
  printf("Usage: %s [-v][-w][-V][-h] [-o FILE] <bpf-file>\n"
	 "  -h, --help       Show this help text\n"
	 "  -v, --verbose    Increase verbosity\n"
	 "  -V, --version    Show version\n"
	 "  -w               Suppress warnings\n"
	 "  -o FILE          Send output to FILE\n",
	 argv0);
}


void
sigint(int s)
{
  // suppress any subsequent SIGINTs that may come from stap parent process
  signal(s, SIG_IGN);

  // during the exit phase, ^C should exit immediately
  if (exit_phase)
    {
      if (!interrupt_message) // avoid duplicate message
        fprintf(stderr, "received interrupt during exit probe\n");
      interrupt_message = 1;
      abort();
    }

  // set exit flag
  int key = bpf::globals::EXIT;
  long val = 1;

  if (bpf_update_elem
       (map_fds[bpf::globals::internal_map_idx], &key, &val, 0) != 0)
     fatal("error during bpf map update: %s\n", strerror(errno));
}

int
main(int argc, char **argv)
{
  static const option long_opts[] = {
    { "help", 0, NULL, 'h' },
    { "verbose", 0, NULL, 'v' },
    { "version", 0, NULL, 'V' },
  };

  int rc;

  while ((rc = getopt_long(argc, argv, "hvVwo:", long_opts, NULL)) >= 0)
    switch (rc)
      {
      case 'v':
        log_level++;
        break;
      case 'w':
        warnings = 0;
        break;

      case 'o':
	output_f = fopen(optarg, "w");
	if (output_f == NULL)
	  {
	    fprintf(stderr, "Error opening %s for output: %s\n",
		    optarg, strerror(errno));
	    return 1;
	  }
	break;

      case 'V':
        printf("Systemtap BPF loader/runner (version %s, %s)\n"
	       "Copyright (C) 2016-2018 Red Hat, Inc. and others\n" // PRERELEASE
               "This is free software; "
	       "see the source for copying conditions.\n",
	       VERSION, STAP_EXTENDED_VERSION);
	return 0;

      case 'h':
	usage(argv[0]);
	return 0;

      default:
      do_usage:
	usage(argv[0]);
	return 1;
      }
  if (optind != argc - 1)
    goto do_usage;

  kmsg = fopen("/dev/kmsg", "a");

  load_bpf_file(argv[optind]);
  init_internal_globals();

  if (create_group_fds() < 0)
    fatal("Error creating perf event group: %s\n", strerror(errno));

  register_kprobes();
  register_uprobes();
  register_timers();
  register_tracepoints();
  register_perf();

  // Run the begin probes.
  if (prog_begin)
    bpf_interpret(prog_begin->d_size / sizeof(bpf_insn),
                  static_cast<bpf_insn *>(prog_begin->d_buf),
	          map_fds, output_f);

  // Now that the begin probe has run, enable the kprobes.
  ioctl(group_fd, PERF_EVENT_IOC_ENABLE, 0);

  // Wait for ^C; read BPF_OUTPUT events, copying them to output_f.
  signal(SIGINT, (sighandler_t)sigint);
  signal(SIGTERM, (sighandler_t)sigint);
  std::thread(print_trace_output, pthread_self()).detach();

  while (!get_exit_status())
    pause();
    
  // Disable the kprobes before deregistering and running exit probes.
  ioctl(group_fd, PERF_EVENT_IOC_DISABLE, 0);
  close(group_fd);

  // Unregister all probes.
  unregister_kprobes(kprobes.size());
  unregister_uprobes(uprobes.size());
  unregister_timers(timers.size());
  unregister_perf(perf_probes.size());
  unregister_tracepoints(tracepoint_probes.size());

  // We are now running exit probes, so ^C should exit immediately:
  exit_phase = 1;
  signal(SIGINT, (sighandler_t)sigint); // restore previously ignored signal
  signal(SIGTERM, (sighandler_t)sigint);

  // Run the end+error probes.
  if (prog_end)
    bpf_interpret(prog_end->d_size / sizeof(bpf_insn),
                  static_cast<bpf_insn *>(prog_end->d_buf),
		  map_fds, output_f);

  elf_end(module_elf);
  fclose(kmsg);
  return 0;
}
