/*
 * app.c
 *
 *  Created on: Jan 2, 2014
 *      Author: petera
 */

#include "app.h"
#include "io.h"
#include "cli.h"
#include "taskq.h"
#include "miniutils.h"
#include "gpio.h"
#include "linker_symaccess.h"
#include "wdog.h"
#include "rtc.h"
#include "spi_flash_m25p16.h"
#include "spiffs.h"
#include "spiffs_config.h"
#include "spiffs_nucleus.h"
#include "spi_dev.h"
#include "os.h"
#include "precise_clock.h"
#include <stdarg.h>

#define EMPTY_STACK_FILL              (0xeaeaeaea) // from os.c

#define SPIFFS_THREAD_CODA(func) \
    print("%s [%i%s]\n", (func), SPIFFS_errno(&fs), spiffs_errstr(SPIFFS_errno(&fs))); \
    print("stack usage: %i bytes\n", count_used_spiffs_stack()); \
    if (graphs_enabled) { \
      print("<SIMTRIG|graphlabel:Stack usage,%s>\n", (func)); \
      print("<SIMTRIG|graph:Stack usage,%i>\n", count_used_spiffs_stack()); \
    } \
    spiffs_locked = FALSE; \
    return (void *)res;


static volatile u8_t cpu_claims;
static u8_t cli_buf[16];
volatile bool cli_rd;
static task_timer heartbeat_timer;
static spiffs fs;
volatile bool spiflash_busy;
volatile s32_t spiflash_result;
//static os_mutex spiffs_mutex;
static volatile bool spiffs_locked;
static u32_t spiffs_stack[512];
static os_thread spiffs_thread;
static os_cond spiffs_cond;
static u8_t spiffs_fd_buf[32*8];
static u8_t spiffs_cache_buf[32+(32+CFG_LOG_PAGE_SZ*8)];
static u8_t spiffs_work[CFG_LOG_PAGE_SZ*2];

bool _spiffs_dbg_generic = FALSE;
bool _spiffs_dbg_cache = FALSE;
bool _spiffs_dbg_gc = FALSE;
bool _spiffs_dbg_check = FALSE;

static u8_t generic_buffer[1024];

static int graphs_enabled = FALSE;
static u32_t graph_nbr = 0;

static struct spiffs_arg_s {
  char fname[SPIFFS_OBJ_NAME_LEN];
  char fname_new[SPIFFS_OBJ_NAME_LEN];
  bool hex;
  u32_t flags;
  spiffs_file fd;
  u8_t data[512];
  u8_t *data_addr;
  u32_t data_len;
  u32_t chunksize;
  s32_t offs;
  bool time_per_chunk;
  u32_t iterations;
  spiffs_page_ix pix;
} spiffs_arg;

static u32_t count_used_spiffs_stack(void);

///////////////////////////////////////////////////////////////////////////////

static void spif_spiffs_cb(spi_flash_dev *d, int res) {
  spiflash_result = res;
  spiflash_busy = FALSE;
  OS_cond_signal(&spiffs_cond);
}

static s32_t spiflash_await(void) {
  gpio_enable(PIN_LED1);
  while (spiflash_busy) {
    OS_cond_wait(&spiffs_cond, NULL);
  }
  gpio_disable(PIN_LED1);

  return spiflash_result;
}

static const char *spiffs_errstr(s32_t err) {
  if (err > 0) return "";
  switch (err) {
  case SPIFFS_OK                        :return " OK";
  case SPIFFS_ERR_NOT_MOUNTED           :return " not mounted";
  case SPIFFS_ERR_FULL                  :return " full";
  case SPIFFS_ERR_NOT_FOUND             :return " not found";
  case SPIFFS_ERR_END_OF_OBJECT         :return " end of object";
  case SPIFFS_ERR_DELETED               :return " deleted";
  case SPIFFS_ERR_NOT_FINALIZED         :return " not finalized";
  case SPIFFS_ERR_NOT_INDEX             :return " not index";
  case SPIFFS_ERR_OUT_OF_FILE_DESCS     :return " out of filedescs";
  case SPIFFS_ERR_FILE_CLOSED           :return " file closed";
  case SPIFFS_ERR_FILE_DELETED          :return " file deleted";
  case SPIFFS_ERR_BAD_DESCRIPTOR        :return " bad descriptor";
  case SPIFFS_ERR_IS_INDEX              :return " is index";
  case SPIFFS_ERR_IS_FREE               :return " is free";
  case SPIFFS_ERR_INDEX_SPAN_MISMATCH   :return " index span mismatch";
  case SPIFFS_ERR_DATA_SPAN_MISMATCH    :return " data span mismatch";
  case SPIFFS_ERR_INDEX_REF_FREE        :return " index ref free";
  case SPIFFS_ERR_INDEX_REF_LU          :return " index ref lu";
  case SPIFFS_ERR_INDEX_REF_INVALID     :return " index ref invalid";
  case SPIFFS_ERR_INDEX_FREE            :return " index free";
  case SPIFFS_ERR_INDEX_LU              :return " index lu";
  case SPIFFS_ERR_INDEX_INVALID         :return " index invalid";
  case SPIFFS_ERR_NOT_WRITABLE          :return " not writable";
  case SPIFFS_ERR_NOT_READABLE          :return " not readable";
  case SPIFFS_ERR_CONFLICTING_NAME      :return " conflicting name";
  case SPIFFS_ERR_NOT_CONFIGURED        :return " not configured";

  case SPIFFS_ERR_NOT_A_FS              :return " not a fs";
  case SPIFFS_ERR_MOUNTED               :return " mounted";
  case SPIFFS_ERR_ERASE_FAIL            :return " erase fail";
  case SPIFFS_ERR_MAGIC_NOT_POSSIBLE    :return " magic not possible";

  case SPIFFS_ERR_NO_DELETED_BLOCKS     :return " no deleted blocks";

  case SPIFFS_ERR_FILE_EXISTS           :return " file exists";

  case SPIFFS_ERR_NOT_A_FILE            :return " not a file";
  case SPIFFS_ERR_RO_NOT_IMPL           :return " ro not impl";
  case SPIFFS_ERR_RO_ABORTED_OPERATION  :return " ro aborted operation";
  case SPIFFS_ERR_PROBE_TOO_FEW_BLOCKS  :return " probe too few blocks";
  case SPIFFS_ERR_PROBE_NOT_A_FS        :return " probe not a fs";
  case SPIFFS_ERR_NAME_TOO_LONG         :return " name too long";

  case SPIFFS_ERR_IX_MAP_UNMAPPED       :return " ix map unmapped";
  case SPIFFS_ERR_IX_MAP_MAPPED         :return " ix map mapped";
  case SPIFFS_ERR_IX_MAP_BAD_RANGE      :return " ix map bad range";

  default                               :return " <unknown>";
  }
}

#if SPIFFS_CACHE
static u32_t count_bits(u32_t i) {
  i = i - ((i >> 1) & 0x55555555);
  i = (i & 0x33333333) + ((i >> 2) & 0x33333333);
  return (((i + (i >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}
#endif

static u32_t count_open_fds(spiffs *fs) {
  spiffs_fd *fds = (spiffs_fd *)fs->fd_space;
  u32_t i;
  u32_t cnt = 0;
  for (i = 0; i < fs->fd_count; i++) {
    spiffs_fd *fd = &fds[i];
    if (fd->file_nbr != 0) {
      cnt++;
    }
  }

  return cnt;
}

///////////////////////////////////////////////////////////////////////////////

#if SPIFFS_HAL_CALLBACK_EXTRA
static s32_t _spiffs_erase(spiffs *fs, u32_t addr, u32_t len) {
#else
static s32_t _spiffs_erase(u32_t addr, u32_t len) {
#endif
  int res;
  gpio_enable(PIN_LED4);
  WDOG_feed();
  spiflash_busy = TRUE;
  if ((res = SPI_FLASH_erase(SPI_FLASH, spif_spiffs_cb, addr, len)) != SPI_OK) {
    spiflash_busy = FALSE;
  } else {
    res = spiflash_await();
  }
  gpio_disable(PIN_LED4);
  return res;
}

#if SPIFFS_HAL_CALLBACK_EXTRA
static s32_t _spiffs_read(spiffs *fs, u32_t addr, u32_t size, u8_t *dst) {
#else
static s32_t _spiffs_read(u32_t addr, u32_t size, u8_t *dst) {
#endif
  int res;
  gpio_enable(PIN_LED2);
  spiflash_busy = TRUE;
  if ((res = SPI_FLASH_read(SPI_FLASH, spif_spiffs_cb, addr, size, dst)) != SPI_OK) {
    spiflash_busy = FALSE;
  } else {
    res = spiflash_await();
  }
  gpio_disable(PIN_LED2);
  return res;
}

#if SPIFFS_HAL_CALLBACK_EXTRA
static s32_t _spiffs_write(spiffs *fs, u32_t addr, u32_t size, u8_t *src) {
#else
static s32_t _spiffs_write(u32_t addr, u32_t size, u8_t *src) {
#endif
  int res;
  gpio_enable(PIN_LED3);
  spiflash_busy = TRUE;
  if ((res = SPI_FLASH_write(SPI_FLASH, spif_spiffs_cb, addr, size, src)) != SPI_OK) {
    spiflash_busy = FALSE;
  } else {
    res = spiflash_await();
  }
  gpio_disable(PIN_LED3);
  return res;
}

///////////////////////////////////////////////////////////////////////////////

static s32_t _spiffs_open_spiflash(void) {
  int res;
  spiflash_busy = TRUE;
  if ((res = SPI_FLASH_open(SPI_FLASH, spif_spiffs_cb)) != SPI_OK) {
    spiflash_busy = FALSE;
  } else {
    res = spiflash_await();
  }
  return res;
}

static u32_t old_perc = 999;
#if SPIFFS_HAL_CALLBACK_EXTRA
static void _spiffs_check_cb_f(spiffs *fs, spiffs_check_type type, spiffs_check_report report,
#else
static void _spiffs_check_cb_f(spiffs_check_type type, spiffs_check_report report,
#endif
    u32_t arg1, u32_t arg2) {
  u32_t perc = arg1 * 100 / 256;
  if (report == SPIFFS_CHECK_PROGRESS && old_perc != perc) {
    old_perc = perc;
    print(TEXT_NOTE("CHECK REPORT: "));
    switch(type) {
    case SPIFFS_CHECK_LOOKUP:
      print(TEXT_NOTE("LU ")); break;
    case SPIFFS_CHECK_INDEX:
      print(TEXT_NOTE("IX ")); break;
    case SPIFFS_CHECK_PAGE:
      print(TEXT_NOTE("PA ")); break;
    }
    print(TEXT_NOTE("%i%%\n"), perc);
  }
  if (report != SPIFFS_CHECK_PROGRESS) {
    print(TEXT_BAD("   check: "));
    switch (type) {
    case SPIFFS_CHECK_INDEX:
      print(TEXT_BAD("INDEX  ")); break;
    case SPIFFS_CHECK_LOOKUP:
      print(TEXT_BAD("LOOKUP ")); break;
    case SPIFFS_CHECK_PAGE:
      print(TEXT_BAD("PAGE   ")); break;
    default:
      print(TEXT_BAD("????   ")); break;
    }
    if (report == SPIFFS_CHECK_ERROR) {
      print(TEXT_BAD("ERROR %i"), arg1);
    } else if (report == SPIFFS_CHECK_DELETE_BAD_FILE) {
      print(TEXT_BAD("DELETE BAD FILE %04x"), arg1);
    } else if (report == SPIFFS_CHECK_DELETE_ORPHANED_INDEX) {
      print(TEXT_BAD("DELETE ORPHANED INDEX %04x"), arg1);
    } else if (report == SPIFFS_CHECK_DELETE_PAGE) {
      print(TEXT_BAD("DELETE PAGE %04x"), arg1);
    } else if (report == SPIFFS_CHECK_FIX_INDEX) {
      print(TEXT_BAD("FIX INDEX %04x:%04x"), arg1, arg2);
    } else if (report == SPIFFS_CHECK_FIX_LOOKUP) {
      print(TEXT_BAD("FIX INDEX %04x:%04x"), arg1, arg2);
    } else {
      print(TEXT_BAD("??"));
    }
    print("\n");
  }
}

static s32_t _spiffs_mount(void) {
  s32_t res;

  print("opening spiflash\n");
  res = _spiffs_open_spiflash();
  if (res != SPI_OK) {
    print("Could not open spiflash, err %i\n", res);
    return 1;
  }

  spiffs_config cfg;
  cfg.hal_erase_f = _spiffs_erase;
  cfg.hal_read_f = _spiffs_read;
  cfg.hal_write_f = _spiffs_write;
#if SPIFFS_SINGLETON == 0
  cfg.phys_size = CFG_PHYS_SZ;
  cfg.phys_addr = CFG_PHYS_ADDR;
  cfg.phys_erase_block = CFG_PHYS_ERASE_SZ;
  cfg.log_block_size = CFG_LOG_BLOCK_SZ;
  cfg.log_page_size = CFG_LOG_PAGE_SZ;
#if SPIFFS_FILEHDL_OFFSET
  cfg.fh_ix_offset = 1000;
#endif
#endif
  print("mounting\n");
  if ((res = SPIFFS_mount(&fs,
      &cfg,
      spiffs_work,
      spiffs_fd_buf, sizeof(spiffs_fd_buf),
      spiffs_cache_buf, sizeof(spiffs_cache_buf),
      _spiffs_check_cb_f)) != SPIFFS_OK &&
      SPIFFS_errno(&fs) == SPIFFS_ERR_NOT_A_FS) {
    print("formatting spiffs...\n");
    if (SPIFFS_format(&fs) != SPIFFS_OK) {
      print("SPIFFS format failed: %i\n", SPIFFS_errno(&fs));
      return 2;
    }
    print("ok\n");
    print("mounting\n");
    res = SPIFFS_mount(&fs,
          &cfg,
          spiffs_work,
          spiffs_fd_buf, sizeof(spiffs_fd_buf),
          spiffs_cache_buf, sizeof(spiffs_cache_buf),
          _spiffs_check_cb_f);
  }
  if (res != SPIFFS_OK) {
    print("SPIFFS mount failed: %i\n", SPIFFS_errno(&fs));
    return 3;
  } else {
    print("SPIFFS mounted\n");
  }
  return 0;
}

static u32_t hash(const u8_t *name) {
  (void)fs;
  u32_t hash = 5381;
  u8_t c;
  int i = 0;
  while ((c = name[i++]) && i < SPIFFS_OBJ_NAME_LEN) {
    hash = (hash * 33) ^ c;
  }
  return hash;
}


// THREAD TASKS

// ================== BENCH ===================

static void *thr_bench_timedwrite(void *v_spiffs_arg) {
  s32_t res = 0;
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  u32_t len = arg->data_len;
  u32_t chunksize = arg->chunksize;
  if (chunksize == 0) chunksize = 1;
  if (chunksize > sizeof(generic_buffer)) chunksize = sizeof(generic_buffer);
  u32_t rnd = hash((u8_t *)arg->fname);
  rand_seed(rnd);

  spiffs_file fd = SPIFFS_open(&fs, arg->fname,
      SPIFFS_O_WRONLY | SPIFFS_O_CREAT | SPIFFS_O_TRUNC, 0);
  if (fd < 0) {
    print("Could not create %s\n", arg->fname);
    goto final;
  }

  print("Timed write of %s, length %i, chunk size %i...\n", arg->fname, len, chunksize);


  int i;
  for (i = 0; i < sizeof(generic_buffer); i++) {
    rnd = rand(rnd);
    generic_buffer[i] = (rnd>>16) ^ (rnd>>8) ^ rnd;
  }

  if (arg->time_per_chunk) {
    graph_nbr++;
    print("<SIMTRIG|graphtitle:w%i,Timed write (chunksize:%i length:%i)>\n",
          graph_nbr, chunksize, len);
  }

  sys_time elapsed = 0;
  u32_t written = 0;
  sys_time mark_prev = SYS_get_tick();
  sys_time then = mark_prev;
  while (written < len) {
    u32_t len2write = MIN(len - written, chunksize);
    res = SPIFFS_write(&fs, fd, generic_buffer, len2write);
    if (res < SPIFFS_OK) {
      break;
    }
    if (arg->time_per_chunk) {
      sys_time mark_cur = SYS_get_tick();
      print(".. %i bytes, delta %i ms\n", written, (int)RTC_TICK_TO_MS(mark_cur - mark_prev));
      print("<SIMTRIG|graph:w%i,%i>\n",
            graph_nbr, (int)RTC_TICK_TO_MS(mark_cur - mark_prev));
      mark_prev = SYS_get_tick();
    }
   written += len2write;
  }
  sys_time now = SYS_get_tick();
  elapsed = now - then;

  res = SPIFFS_close(&fs, fd);

  print("wrote %i bytes in %i ms, %i bytes/sec\n",
      arg->data_len, (int)elapsed, (1000*arg->data_len)/(int)elapsed);

  print("benchmarked write [%i%s]\n", SPIFFS_errno(&fs), spiffs_errstr(SPIFFS_errno(&fs)));
final:

  SPIFFS_THREAD_CODA("SPIFFS bench timedwrite");
}


static void *thr_bench_timedread(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  u32_t chunksize = arg->chunksize;
  if (chunksize == 0) chunksize = 1;
  if (chunksize > sizeof(generic_buffer)) chunksize = sizeof(generic_buffer);
  s32_t res = 0;
  do {
    spiffs_file fd = SPIFFS_open(&fs, arg->fname, SPIFFS_O_RDONLY, 0);
    if (fd < 0) {
      print("Could not open %s\n", arg->fname);
      break;
    }
    print("Timed read of %s, chunk size %i...\n", arg->fname, chunksize);
    if (arg->time_per_chunk) {
      graph_nbr++;
      spiffs_stat s;
      SPIFFS_stat(&fs, arg->fname, &s);
      print("<SIMTRIG|graphtitle:r%i,Timed read [ticks] (chunksize:%i length:%i)>\n",
            graph_nbr, chunksize, s.size);
    }


    u32_t read_bytes = 0;
    sys_time then = SYS_get_tick();
    sys_time mark_prev = PC_get_timer();
    while ((res = SPIFFS_read(&fs, fd, generic_buffer, chunksize)) >= 0) {
      read_bytes += res;
      if (arg->time_per_chunk) {
        sys_time mark_cur = PC_get_timer();
        print(".. %i bytes, delta %i ticks\n", read_bytes, (int)(mark_cur - mark_prev));
        print("<SIMTRIG|graph:r%i,%i>\n",
              graph_nbr, (int)RTC_TICK_TO_MS(mark_cur - mark_prev));
        mark_prev = PC_get_timer();
      }
    }
    sys_time now = SYS_get_tick();
    if (SPIFFS_errno(&fs) == SPIFFS_ERR_END_OF_OBJECT) {
      SPIFFS_clearerr(&fs);
      res = 0;
    }
    (void)SPIFFS_close(&fs, fd);
    sys_time elapsed = now-then;
    print("read %i bytes in %i ms, %i bytes/sec\n",
        read_bytes, (int)RTC_TICK_TO_MS(elapsed), (1000*read_bytes)/(int)elapsed);
  } while (0);

  print("benchmarked read [%i%s]\n", SPIFFS_errno(&fs), spiffs_errstr(SPIFFS_errno(&fs)));

  SPIFFS_THREAD_CODA("SPIFFS bench timedread");
}


static void *thr_bench_httpservmodel(void *v_spiffs_arg) {
  s32_t res = SPIFFS_OK;
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  u32_t iter = arg->iterations;
  char fnames[8][SPIFFS_OBJ_NAME_LEN];
  u32_t files = 0;
  do {
    spiffs_DIR d;
    struct spiffs_dirent e;
    struct spiffs_dirent *pe = &e;

    SPIFFS_opendir(&fs, "", &d);
    while ((pe = SPIFFS_readdir(&d, pe))) {
      strcpy(fnames[files++], (char *)pe->name);
      if (files >= 8) {
        break;
      }
    }
    SPIFFS_closedir(&d);

    if (files < 2) {
      print("too few files to run test\n");
      break;
    }
    print("http server model test, %i files, %i iterations..\n", files, iter);

    rand_seed(0x90817263);
    sys_time then = SYS_get_tick();
    while (iter--) {
      spiffs_file fd;
      u32_t file_ix = (rand_next() % (files - 1)) + 1;

      fd = SPIFFS_open(&fs, fnames[file_ix], SPIFFS_O_RDONLY, 0);
      if (fd <= SPIFFS_OK) break;
      SPIFFS_close(&fs, fd);

      fd = SPIFFS_open(&fs, fnames[0], SPIFFS_O_RDONLY, 0);
      if (fd <= SPIFFS_OK) break;
      SPIFFS_close(&fs, fd);
    }
    sys_time now = SYS_get_tick();
    sys_time elapsed = now-then;
    print("http server model %i iterations in %i ms, %i files\n",
        arg->iterations, (int)RTC_TICK_TO_MS(elapsed), files);
  } while (0);

  print("benchmarked http server model [%i%s]\n", SPIFFS_errno(&fs),
      spiffs_errstr(SPIFFS_errno(&fs)));

  SPIFFS_THREAD_CODA("SPIFFS bench httpservermodel");
}


static void *thr_bench_timedread_chunks(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  u32_t chunksize = sizeof(generic_buffer);
  s32_t res = SPIFFS_OK;

  spiffs_stat s;
  SPIFFS_stat(&fs, arg->fname, &s);
  print("<SIMTRIG|graphtitle:rc%i,Timed read chunksizes (length:%i)>\n",
        graph_nbr, s.size);
  do {
    spiffs_file fd = SPIFFS_open(&fs, arg->fname, SPIFFS_O_RDONLY, 0);
    if (fd < 0) {
      print("Could not open %s\n", arg->fname);
      break;
    }


    u32_t read_bytes = 0;
    sys_time mark_prev = SYS_get_tick();
    sys_time then = mark_prev;
    while ((res = SPIFFS_read(&fs, fd, generic_buffer, chunksize)) >= 0) {
      read_bytes += res;
    }
    sys_time now = SYS_get_tick();
    if (SPIFFS_errno(&fs) == SPIFFS_ERR_END_OF_OBJECT) {
      SPIFFS_clearerr(&fs);
      res = SPIFFS_OK;
    }
    (void)SPIFFS_close(&fs, fd);
    if (res != SPIFFS_OK) {
      break;
    }
    sys_time elapsed = now-then;
    print("read %i bytes in %i ms, %i bytes/sec\n",
        read_bytes, (int)RTC_TICK_TO_MS(elapsed), (1000*read_bytes)/(int)elapsed);
    print("<SIMTRIG|graphlabel:rc%i,%i>\n",
          graph_nbr, chunksize);
    print("<SIMTRIG|graph:rc%i,%i>\n",
          graph_nbr, (int)RTC_TICK_TO_MS(elapsed));
    chunksize /= 2;
  } while (res == SPIFFS_OK && chunksize > 0);

  print("benchmarked read chunksizes [%i%s]\n", SPIFFS_errno(&fs), spiffs_errstr(SPIFFS_errno(&fs)));

  SPIFFS_THREAD_CODA("SPIFFS bench read chunksizes");
}



// ================= COMMON ==================


static void *thr_spiffs_mount(void *arg) {
  (void)arg;
  s32_t res = _spiffs_mount();
  SPIFFS_THREAD_CODA("SPIFFS_mount");
}

static void *thr_spiffs_unmount(void *arg) {
  (void)arg;
  SPIFFS_unmount(&fs);
  s32_t res = SPIFFS_OK;
  SPIFFS_THREAD_CODA("SPIFFS_unmount");
}

#if SPIFFS_READ_ONLY == 0

static void *thr_spiffs_format(void *arg) {
  (void)arg;
  SPIFFS_unmount(&fs);
  s32_t res = SPIFFS_format(&fs);
  SPIFFS_THREAD_CODA("SPIFFS_format");
}

static void *thr_spiffs_check(void *arg) {
  (void)arg;
  s32_t res = SPIFFS_check(&fs);
  SPIFFS_THREAD_CODA("SPIFFS_check");
}

#endif

static void *thr_spiffs_ls(void *arg) {
  (void)arg;
  s32_t res = SPIFFS_OK;
  spiffs_DIR d;
  struct spiffs_dirent e;
  struct spiffs_dirent *pe = &e;

  SPIFFS_opendir(&fs, "/", &d);
  print("  OBJID   SIZE    NAME\n");
  while ((pe = SPIFFS_readdir(&d, pe))) {
    print("  0x%04x  %6i  %s\n", pe->obj_id, pe->size, pe->name);
  }
  print("\n");
  SPIFFS_closedir(&d);

  u32_t total, used;
  res = SPIFFS_info(&fs, &total, &used);
  print("used :%6i [%4i kB]\n", used, used/1024);
  print("free :%6i [%4i kB]\n", total - used, (total - used)/1024);
  print("total:%6i [%4i kB]\n", total, total/1024);

  print("\n");

  print("num file descr.:%i (used %i)\n",
      fs.fd_count,
      count_open_fds(&fs)
      );
#if SPIFFS_CACHE
  print("num cache pages:%i (used %i)\n",
      spiffs_get_cache(&fs)->cpage_count,
      count_bits(spiffs_get_cache(&fs)->cpage_use_mask & spiffs_get_cache(&fs)->cpage_use_map));
#endif
  SPIFFS_THREAD_CODA("SPIFFS_ls");
}

static void *thr_spiffs_lesshex(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  spiffs_file fd = -1;
  u32_t offs = 0;
  s32_t res = SPIFFS_OK;
  do {
    u8_t buf[16];
    fd = SPIFFS_open(&fs, arg->fname, SPIFFS_O_RDONLY, 0);
    if (fd < 0) break;
    do {
      res = SPIFFS_read(&fs, fd, buf, sizeof(buf));
      if (res > 0) {
        int i;
        if (arg->hex) {
          print("0x%08x: ", offs);
          for (i = 0; i < res; i++) {
            print("%02x ", buf[i]);
          }
          for (i = 0; i < 16 - res; i++) {
            print("   ");
          }
          for (i = 0; i < res; i++) {
            print("%c", buf[i] >= ' ' ? buf[i] : '.');
          }

          offs += res;
          print("\n");
        } else {
          for (i = 0; i < res; i++) {
            print("%c", buf[i]);
          }
        }
      }
    } while (res > 0);
    if (res == 0) {
      SPIFFS_clearerr(&fs);
    }
    print("\n");
  } while (0);
  if (fd >= 0) {
    res = SPIFFS_close(&fs, fd);
  }
  if (SPIFFS_errno(&fs) == SPIFFS_ERR_END_OF_OBJECT) SPIFFS_clearerr(&fs);

  SPIFFS_THREAD_CODA("SPIFFS lesshex");
}

static void *thr_spiffs_open(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = SPIFFS_open(&fs, arg->fname, arg->flags, 0);
  if (res >= 0) print("fd %i opened\n", res);
  SPIFFS_THREAD_CODA("SPIFFS_open");
}

static void *thr_spiffs_open_by_page(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = SPIFFS_open_by_page(&fs, arg->pix, arg->flags, 0);
  if (res >= 0) print("fd %i opened\n", res);
  SPIFFS_THREAD_CODA("SPIFFS_open_by_page");
}

static void *thr_spiffs_close(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = SPIFFS_close(&fs, arg->fd);
  SPIFFS_THREAD_CODA("SPIFFS_close");
}

static void *thr_spiffs_read(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;

  u32_t rlen = 0;
  u32_t remain = arg->data_len;
  u8_t buf[16];
  s32_t res;
  do {
    res = SPIFFS_read(&fs, arg->fd, buf, MIN(sizeof(buf), remain));
    if (res > 0) {
      int i;
      for (i = 0; i < res; i++) {
        print("%02x ", buf[i]);
      }
      for (i = 0; i < 16 - res; i++) {
        print("   ");
      }
      for (i = 0; i < res; i++) {
        print("%c", buf[i] >= ' ' ? buf[i] : '.');
      }
      remain -= res;
      rlen += res;
    }
    print("\n");
  } while (res > 0 && remain > 0);

  if (SPIFFS_errno(&fs) == SPIFFS_ERR_END_OF_OBJECT) SPIFFS_clearerr(&fs);

  SPIFFS_THREAD_CODA("SPIFFS_read");
}

#if SPIFFS_READ_ONLY == 0

static void *thr_spiffs_write(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = SPIFFS_write(&fs, arg->fd, arg->data, arg->data_len);
  SPIFFS_THREAD_CODA("SPIFFS_write");
}

static void *thr_spiffs_write_mem(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = SPIFFS_write(&fs, arg->fd, arg->data_addr, arg->data_len);
  print("SPIFFS write %i of %i [%i%s]\n", res, arg->data_len, SPIFFS_errno(&fs), spiffs_errstr(SPIFFS_errno(&fs)));
  spiffs_locked = FALSE;
  return (void *)res;
}

#endif

static void *thr_spiffs_seek(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = SPIFFS_lseek(&fs, arg->fd, arg->flags, arg->offs);
  SPIFFS_THREAD_CODA("SPIFFS_lseek");
}

static void *thr_spiffs_tell(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = SPIFFS_tell(&fs, arg->fd);
  if (res >= 0) {
    print("offset %i\n", res);
  }
  SPIFFS_THREAD_CODA("SPIFFS_tell");
}

#if SPIFFS_READ_ONLY == 0

static void *thr_spiffs_remove(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = SPIFFS_remove(&fs, arg->fname);
  SPIFFS_THREAD_CODA("SPIFFS_remove");
}

static void *thr_spiffs_remove_filter(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = SPIFFS_OK;
  spiffs_DIR d;
  struct spiffs_dirent e;
  struct spiffs_dirent *pe = &e;

  SPIFFS_opendir(&fs, "/", &d);
  print("  OBJID   SIZE    NAME\n");
  while ((pe = SPIFFS_readdir(&d, pe))) {
    if (strstr((char *)pe->name, arg->fname)) {
      print("removing %s.. ", pe->name);
      spiffs_file fd = SPIFFS_open_by_dirent(&fs, pe, SPIFFS_O_RDWR, 0);
      if (fd > SPIFFS_OK) {
        res = SPIFFS_fremove(&fs, fd);
        if (res == SPIFFS_OK) {
          print("OK\n");
        } else {
          print("FAIL [%i%s]\n", SPIFFS_errno(&fs), spiffs_errstr(SPIFFS_errno(&fs)));
        }
      }
    }
  }
  print("\n");
  SPIFFS_closedir(&d);
  SPIFFS_THREAD_CODA("SPIFFS_remove_filter");
}

static void *thr_spiffs_rename(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = SPIFFS_rename(&fs, arg->fname, arg->fname_new);
  SPIFFS_THREAD_CODA("SPIFFS_rename");
}

static void *thr_spiffs_copy(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = 0;
  do {
    spiffs_file fd_src = SPIFFS_open(&fs, arg->fname, SPIFFS_O_RDONLY, 0);
    if (fd_src < 0) {
      print("Could not open %s\n", arg->fname);
      break;
    }
    spiffs_file fd_dst = SPIFFS_open(&fs, arg->fname_new, SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_WRONLY, 0);
    if (fd_dst < 0) {
      print("Could not open %s\n", arg->fname_new);
      SPIFFS_close(&fs, fd_src);
      break;
    }
    bool write_ok = TRUE;
    while (write_ok && (res = SPIFFS_read(&fs, fd_src, spiffs_arg.data, sizeof(spiffs_arg.data))) > 0) {
      res = SPIFFS_write(&fs, fd_dst, spiffs_arg.data, res);
      write_ok = res > 0;
    }
    if (res < 0 && SPIFFS_errno(&fs) == SPIFFS_ERR_END_OF_OBJECT) {
      res = 0;
      SPIFFS_clearerr(&fs);
    }
    if (res < 0) {
      print("SPIFFS copy inner [%i%s]\n", SPIFFS_errno(&fs), spiffs_errstr(SPIFFS_errno(&fs)));
    }
    SPIFFS_close(&fs, fd_src);
    res = SPIFFS_close(&fs, fd_dst);
  } while (0);

  SPIFFS_THREAD_CODA("SPIFFS copy");
}

static void *thr_spiffs_gc(void *v_spiffs_arg) {
  struct spiffs_arg_s *arg = (struct spiffs_arg_s *)v_spiffs_arg;
  s32_t res = SPIFFS_gc(&fs, arg->data_len);
  SPIFFS_THREAD_CODA("SPIFFS_gc");
}

static void *thr_spiffs_gc_quick(void *v_spiffs_arg) {
  (void)v_spiffs_arg;
  s32_t res = SPIFFS_gc_quick(&fs, 0);
  SPIFFS_THREAD_CODA("SPIFFS_gc_quick");
}

#endif

static void *thr_spiffs_vis(void *arg) {
  (void)arg;
  s32_t res = SPIFFS_vis(&fs);
  SPIFFS_THREAD_CODA("SPIFFS_vis");
}


///////////////////////////////////////////////////////////////////////////////


static void cli_task_on_input(u32_t len, void *p) {
  u8_t io = (u8_t)((u32_t)p);
  while (IO_rx_available(io)) {
    u32_t rlen = IO_get_buf(io, cli_buf, MIN(IO_rx_available(io), sizeof(cli_buf)));
    cli_recv((char *)cli_buf, rlen);
  }
  cli_rd = FALSE;
}


static void cli_rx_avail_irq(u8_t io, void *arg, u16_t available) {
  if (!cli_rd) {
    task *t = TASK_create(cli_task_on_input, 0);
    TASK_run(t, 0, (void *)((u32_t)io));
    cli_rd = TRUE;
  }
}

static void heartbeat(u32_t ignore, void *ignore_more) {
  WDOG_feed();
}

static void sleep_stop_restore(void)
{
  // Enable HSE
  RCC_HSEConfig(RCC_HSE_ON);

  // Wait till HSE is ready
  ErrorStatus HSEStartUpStatus;
  HSEStartUpStatus = RCC_WaitForHSEStartUp();
  ASSERT(HSEStartUpStatus == SUCCESS);
  // Enable PLL
  RCC_PLLCmd(ENABLE);

  // Wait till PLL is ready
  while(RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET);
  // Select PLL as system clock source
  RCC_SYSCLKConfig(RCC_SYSCLKSource_PLLCLK);
  // Wait till PLL is used as system clock source
  while(RCC_GetSYSCLKSource() != 0x08);
}

static void app_spin(void) {
  while (1) {
    // execute all pending tasks
    while (TASK_tick());

    bool wakeup_alarm;
    u64_t wu_tick = 0;

    // get nearest task timer
    u64_t cur_tick = RTC_get_tick();

    sys_time taskq_wakeup_ms;
    volatile u64_t taskq_wu_tick = (u64_t)(-1ULL);
    task_timer *task_wu_timer;
    s32_t taskq_no_wakeup = TASK_next_wakeup_ms(&taskq_wakeup_ms, &task_wu_timer);
    s64_t taskq_diff_tick = 0;
    if (!taskq_no_wakeup) {
      taskq_wu_tick = RTC_MS_TO_TICK(taskq_wakeup_ms);
      taskq_diff_tick = taskq_wu_tick - cur_tick;
      ASSERT(taskq_diff_tick < 0x100000000LL);
      if (taskq_diff_tick <= 0) {
        // got at least one timer that already should've triggered: fire and loop
        TASK_timer();
        continue;
      }
    }

    // now, all task stuff should've been handled

    // get nearest thread timer
    sys_time os_wakeup_ms;
    u64_t os_wu_tick;
    os_wakeup_res os_wup_res = OS_get_next_wakeup(&os_wakeup_ms);

    bool os_sleepers = os_wup_res == OS_WUP_SLEEP_RUNNING || os_wup_res == OS_WUP_SLEEP;
    if (os_sleepers) {
      os_wu_tick = RTC_MS_TO_TICK(os_wakeup_ms);
    }

    // calculate sleep time
    wakeup_alarm = !(taskq_no_wakeup && os_wup_res == OS_WUP_SLEEP_FOREVER);

    if (wakeup_alarm) {
      if (os_sleepers && !taskq_no_wakeup) {
        if (os_wu_tick < taskq_wu_tick) {
          wu_tick = os_wu_tick;
          DBG(D_APP, D_DEBUG, "RTC alarm @ %016llx from thread/(task_timer)\n", wu_tick);
          //print("RTC alarm @ %016llx from thread/(task_timer)\n", wu_tick);
        } else {
          wu_tick = taskq_wu_tick;
          DBG(D_APP, D_DEBUG, "RTC alarm @ %016llx from (thread)/task_timer %s\n", wu_tick, task_wu_timer->name);
          //print("RTC alarm @ %016llx from (thread)/task_timer %s\n", wu_tick, task_wu_timer->name);
        }
        wu_tick = MIN(os_wu_tick, taskq_wu_tick);
      } else if (os_sleepers) {
        wu_tick = os_wu_tick;
        DBG(D_APP, D_DEBUG, "RTC alarm @ %016llx from thread\n", wu_tick);
        //print("RTC alarm @ %016llx from thread\n", wu_tick);
      } else {
        wu_tick = taskq_wu_tick;
        DBG(D_APP, D_DEBUG, "RTC alarm @ %016llx from task_timer %s\n", wu_tick, task_wu_timer->name);
        //print("RTC alarm @ %016llx from task_timer %s\n", wu_tick, task_wu_timer->name);
      }
    }

    if (wakeup_alarm) {
      // wake us at timer value
      RTC_set_alarm_tick(wu_tick);
    } else {
      // wait forever
      RTC_cancel_alarm();
    }

    if (os_wup_res == OS_WUP_RUNNING || os_wup_res == OS_WUP_SLEEP_RUNNING) {
      // have running threads, reschedule - no tasks left, so threads should be chosen
      //print("no sleep, run threads\n");
      OS_force_ctx_switch();
      continue;
    }

    // snooze or sleep
    if (cpu_claims || (wakeup_alarm && (wu_tick - cur_tick < RTC_MS_TO_TICK(APP_PREVENT_SLEEP_IF_LESS_MS)))) {
      // resources held or too soon to wake up to go sleep, just snooze
      DBG(D_APP, D_INFO, "..snoozing %i ms, %i resources claimed\n", (u32_t)(taskq_wakeup_ms - RTC_TICK_TO_MS(RTC_get_tick())), cpu_claims);
      //print("..snoozing for %i ms, %i resources claimed\n", (u32_t)(taskq_wakeup_ms - RTC_TICK_TO_MS(RTC_get_tick())), cpu_claims);
      while (RTC_get_tick() <= wu_tick && cpu_claims && !TASK_tick()) {
        __WFI();
      }
    } else {
      // no one holding any resource, got plenty of time before wakeup, sleep
      if (wakeup_alarm) {
        DBG(D_APP, D_INFO, "..sleeping %i ms\n   ", (u32_t)(RTC_TICK_TO_MS(wu_tick - RTC_get_tick())));
      } else {
        DBG(D_APP, D_INFO, "..sleeping indefinitely\n   ");
      }
      irq_disable();
      IO_tx_flush(IOSTD);
      irq_enable();

      PWR_ClearFlag(PWR_FLAG_PVDO);
      PWR_ClearFlag(PWR_FLAG_WU);
      PWR_ClearFlag(PWR_FLAG_SB);

      EXTI_ClearITPendingBit(EXTI_Line17);
      RTC_ClearITPendingBit(RTC_IT_ALR);
      RTC_WaitForLastTask();

      // sleep
      PWR_EnterSTOPMode(PWR_Regulator_ON, PWR_STOPEntry_WFI);

      // wake, reconfigure
      sleep_stop_restore();

      DBG(D_APP, D_DEBUG, "awaken from sleep\n");
    }

    // check if any timers fired and insert them into task queue
    TASK_timer();
  } // while forever
}

static u8_t cli_spif_buf[256];

static void spif_cb_generic(spi_flash_dev *d, int res) {
  if (res != SPI_OK) {
    print("spiflash cb res:%i state:%i\n", res, d->state);
  } else {
    print("spiflash cb OK\n");
  }
}
static void spif_cb_rd(spi_flash_dev *d, int res) {
  spif_cb_generic(d, res);
  printbuf(IOSTD, cli_spif_buf, sizeof(cli_spif_buf));
}

void APP_init(void) {
  PC_init();
  WDOG_start(APP_WDOG_TIMEOUT_S);

  if (PWR_GetFlagStatus(PWR_FLAG_WU) == SET) {
    PWR_ClearFlag(PWR_FLAG_WU);
    print("PWR_FLAG_WU\n");
  }
  if (PWR_GetFlagStatus(PWR_FLAG_SB) == SET) {
    PWR_ClearFlag(PWR_FLAG_SB);
    print("PWR_FLAG_SB\n");
  }
  if (PWR_GetFlagStatus(PWR_FLAG_PVDO) == SET) {
    PWR_ClearFlag(PWR_FLAG_PVDO);
    print("PWR_FLAG_PVDO\n");
  }
  if (RCC_GetFlagStatus(RCC_FLAG_SFTRST) == SET) print("SFT\n");
  if (RCC_GetFlagStatus(RCC_FLAG_PORRST) == SET) print("POR\n");
  if (RCC_GetFlagStatus(RCC_FLAG_PINRST) == SET) print("PIN\n");
  if (RCC_GetFlagStatus(RCC_FLAG_IWDGRST) == SET) print("IWDG\n");
  if (RCC_GetFlagStatus(RCC_FLAG_LPWRRST) == SET) print("LPWR\n");
  if (RCC_GetFlagStatus(RCC_FLAG_WWDGRST) == SET) print("WWDG\n");
  RCC_ClearFlag();

  cpu_claims = 0;

  gpio_disable(PIN_LED1);
  gpio_disable(PIN_LED2);
  gpio_disable(PIN_LED3);
  gpio_disable(PIN_LED4);
  gpio_config_out(PIN_LED1, CLK_2MHZ, PUSHPULL, NOPULL);
  gpio_config_out(PIN_LED2, CLK_2MHZ, PUSHPULL, NOPULL);
  gpio_config_out(PIN_LED3, CLK_2MHZ, PUSHPULL, NOPULL);
  gpio_config_out(PIN_LED4, CLK_2MHZ, PUSHPULL, NOPULL);

  task *heatbeat_task = TASK_create(heartbeat, TASK_STATIC);
  TASK_start_timer(heatbeat_task, &heartbeat_timer, 0, 0, 0, APP_HEARTBEAT_MS, "heartbeat");

  cli_rd = FALSE;
  IO_set_callback(IOSTD, cli_rx_avail_irq, NULL);

  APP_claim(0); // TODO do not sleep

  SPI_FLASH_M25P16_app_init();
  spiffs_locked = TRUE;
  OS_cond_init(&spiffs_cond);
  OS_thread_create(
      &spiffs_thread,
      OS_THREAD_FLAG_PRIVILEGED,
      thr_spiffs_mount,
      0,
      spiffs_stack,
      sizeof(spiffs_stack),
      "spiffs");

  app_spin();
}

void APP_shutdown(void) {
}

void APP_dump(void) {
  print("APP specifics\n-------------\n");
  print("\n");
}

void APP_claim(u8_t resource) {
  (void)resource;
  irq_disable();
  ASSERT(cpu_claims < 0xff);
  cpu_claims++;
  irq_enable();
}

void APP_release(u8_t resource) {
  (void)resource;
  irq_disable();
  ASSERT(cpu_claims > 0);
  cpu_claims--;
  irq_enable();
}

void APP_rtc_cb(void) {
  OS_time_tick(SYS_get_time_ms());
}


static s32_t cli_info(u32_t argc) {
  RCC_ClocksTypeDef clocks;
  print("DEV:%08x REV:%08x\n", DBGMCU_GetDEVID(), DBGMCU_GetREVID());
  RCC_GetClocksFreq(&clocks);
  print(
      "HCLK:  %i\n"
      "PCLK1: %i\n"
      "PCLK2: %i\n"
      "SYSCLK:%i\n"
      "ADCCLK:%i\n",
      clocks.HCLK_Frequency,
      clocks.PCLK1_Frequency,
      clocks.PCLK2_Frequency,
      clocks.SYSCLK_Frequency,
      clocks.ADCCLK_Frequency);
  print(
      "RTCCLK:%i\n"
      "RTCDIV:%i\n"
      "RTCTCK:%i ticks/sec\n",
      CONFIG_RTC_CLOCK_HZ,
      CONFIG_RTC_PRESCALER,
      CONFIG_RTC_CLOCK_HZ/CONFIG_RTC_PRESCALER
      );

  print(
      "RTCCNT:%016llx\n"
      "RTCALR:%016llx\n",
      RTC_get_tick(), RTC_get_alarm_tick());

  print(
      "PRCCLK:%016llx\n",
      PC_get_timer());
  return CLI_OK;
}

static s32_t cli_spif_rd(u32_t argc, u32_t addr, u32_t len) {
  len = MAX(len, sizeof(cli_spif_buf));
  return SPI_FLASH_read(SPI_FLASH, spif_cb_rd, addr, len, cli_spif_buf);
}

static s32_t cli_spif_chip_er(u32_t argc) {
  return SPI_FLASH_mass_erase(SPI_FLASH, spif_cb_generic);
}

static s32_t cli_call_spiffs_generic(void *fn) {
  if (!spiffs_locked) {
    spiffs_locked = TRUE;

    int i;
    for (i = 0; i < sizeof(spiffs_stack)/sizeof(spiffs_stack[0]); i++) {
      spiffs_stack[i] = EMPTY_STACK_FILL;
    }

    SPIFFS_clearerr(&fs);
    OS_thread_create(
        &spiffs_thread,
        OS_THREAD_FLAG_PRIVILEGED,
        fn,
        &spiffs_arg,
        spiffs_stack,
        sizeof(spiffs_stack),
        "spiffs");
    return 0;
  }
  print("ERR: SPIFFS busy\n");
  return 0;
}

static u32_t count_used_spiffs_stack(void) {
  u32_t *sp = &spiffs_stack[1];
  u32_t count_free_32 = 0;
  while (*sp++ == EMPTY_STACK_FILL) {
    count_free_32++;
  }
  return sizeof(spiffs_stack) - (count_free_32 - 2) * sizeof(u32_t);
}

// CLI STUFF

// ================== BENCH ===================

static s32_t cli_bench_timedread(u32_t argc, char *fname, u32_t chunksize, u32_t tpc) {
  if (argc < 1) return -2;
  if (argc < 2) chunksize = 1024;
  if (argc < 3) tpc = 0;
  memcpy(spiffs_arg.fname, fname, SPIFFS_OBJ_NAME_LEN);
  spiffs_arg.chunksize = chunksize;
  spiffs_arg.time_per_chunk = tpc;
  return cli_call_spiffs_generic(thr_bench_timedread);
}


static s32_t cli_bench_timedwrite(u32_t argc, char *fname, u32_t len, u32_t chunksize, u32_t tpc) {
  if (argc < 2) return -2;
  if (argc < 3) chunksize = 1024;
  if (argc < 4) tpc = 0;
  memcpy(spiffs_arg.fname, fname, SPIFFS_OBJ_NAME_LEN);
  spiffs_arg.chunksize = chunksize;
  spiffs_arg.data_len = len;
  spiffs_arg.time_per_chunk = tpc;
  return cli_call_spiffs_generic(thr_bench_timedwrite);
}


static s32_t cli_bench_httpservmodel(u32_t argc, u32_t iter) {
  if (argc < 1) iter = 1000;
  spiffs_arg.iterations = iter;
  return cli_call_spiffs_generic(thr_bench_httpservmodel);
}


static s32_t cli_bench_timedread_chunks(u32_t argc, char *fname) {
  if (argc < 1) return -2;
  memcpy(spiffs_arg.fname, fname, SPIFFS_OBJ_NAME_LEN);
  return cli_call_spiffs_generic(thr_bench_timedread_chunks);
}



// ================= COMMON ==================

static s32_t cli_spiffs_mount(u32_t argc) {
  return cli_call_spiffs_generic(thr_spiffs_mount);
}

static s32_t cli_spiffs_unmount(u32_t argc) {
  return cli_call_spiffs_generic(thr_spiffs_unmount);
}

#if SPIFFS_READ_ONLY == 0

static s32_t cli_spiffs_format(u32_t argc) {
  if (SPIFFS_mounted(&fs)) {
    print("unmount before format\n");
    return 0;
  }
  return cli_call_spiffs_generic(thr_spiffs_format);
}

static s32_t cli_spiffs_check(u32_t argc) {
  return cli_call_spiffs_generic(thr_spiffs_check);
}

#endif

static s32_t cli_spiffs_ls(u32_t argc) {
  return cli_call_spiffs_generic(thr_spiffs_ls);
}

static s32_t cli_spiffs_less(u32_t argc, char *fname) {
  if (argc < 1) return -2;
  memcpy(spiffs_arg.fname, fname, SPIFFS_OBJ_NAME_LEN);
  spiffs_arg.hex = FALSE;
  return cli_call_spiffs_generic(thr_spiffs_lesshex);
}

static s32_t cli_spiffs_hex(u32_t argc, char *fname) {
  if (argc < 1) return -2;
  memcpy(spiffs_arg.fname, fname, SPIFFS_OBJ_NAME_LEN);
  spiffs_arg.hex = TRUE;
  return cli_call_spiffs_generic(thr_spiffs_lesshex);
}

void parse_flag(char *flag) {
  if (strcmp("APPEND", flag) == 0) {
    spiffs_arg.flags |= SPIFFS_O_APPEND;
  } else if (strcmp("TRUNC", flag) == 0) {
    spiffs_arg.flags |= SPIFFS_O_TRUNC;
  } else if (strcmp("CREAT", flag) == 0) {
    spiffs_arg.flags |= SPIFFS_O_CREAT;
  } else if (strcmp("RDONLY", flag) == 0) {
    spiffs_arg.flags |= SPIFFS_O_RDONLY;
  } else if (strcmp("WRONLY", flag) == 0) {
    spiffs_arg.flags |= SPIFFS_O_WRONLY;
  } else if (strcmp("RDWR", flag) == 0) {
    spiffs_arg.flags |= SPIFFS_O_RDWR;
  } else if (strcmp("DIRECT", flag) == 0) {
    spiffs_arg.flags |= SPIFFS_O_DIRECT;
  } else if (strcmp("EXCL", flag) == 0) {
    spiffs_arg.flags |= SPIFFS_O_EXCL;
  } else {
    print("warn: unknown flag %s\n", flag);
  }
}

static s32_t cli_spiffs_open(u32_t argc, char *fname, ...) {
  if (argc < 1) return -2;
  memcpy(spiffs_arg.fname, fname, SPIFFS_OBJ_NAME_LEN);
  spiffs_arg.flags = 0;

  va_list va;
  va_start(va, fname);
  u32_t i;
  for (i = 0; i < argc -1; i++) {
    char *flag = va_arg(va, char *);
    parse_flag(flag);
  }
  va_end(va);

  return cli_call_spiffs_generic(thr_spiffs_open);
}

static s32_t cli_spiffs_open_by_page(u32_t argc, u32_t pix, ...) {
  if (argc < 1) return -2;
  spiffs_arg.pix = pix;
  spiffs_arg.flags = 0;

  va_list va;
  va_start(va, pix);
  u32_t i;
  for (i = 0; i < argc -1; i++) {
    char *flag = va_arg(va, char *);
    parse_flag(flag);
  }
  va_end(va);

  return cli_call_spiffs_generic(thr_spiffs_open_by_page);
}

static s32_t cli_spiffs_close(u32_t argc, spiffs_file fd) {
  if (argc < 1) return -2;
  spiffs_arg.fd = fd;
  return cli_call_spiffs_generic(thr_spiffs_close);
}

static s32_t cli_spiffs_read(u32_t argc, spiffs_file fd, u32_t len) {
  if (argc < 2) return -2;
  spiffs_arg.fd = fd;
  spiffs_arg.data_len = len;
  return cli_call_spiffs_generic(thr_spiffs_read);
}

#if SPIFFS_READ_ONLY == 0

static s32_t cli_spiffs_write(u32_t argc, spiffs_file fd, char *data) {
  if (argc < 2) return -2;
  spiffs_arg.fd = fd;
  strncpy((char *)spiffs_arg.data, data, sizeof(spiffs_arg.data));
  spiffs_arg.data_len = strlen((char *)spiffs_arg.data);
  return cli_call_spiffs_generic(thr_spiffs_write);
}

static s32_t cli_spiffs_write_mem(u32_t argc, spiffs_file fd, u8_t *data, u32_t len) {
  if (argc < 3) return -2;
  spiffs_arg.fd = fd;
  spiffs_arg.data_addr = data;
  spiffs_arg.data_len = len;
  return cli_call_spiffs_generic(thr_spiffs_write_mem);
}

#endif

static s32_t cli_spiffs_seek(u32_t argc, spiffs_file fd, char *whence_str, s32_t offs) {
  if (argc < 3) return -2;
  spiffs_arg.fd = fd;
  if (strcmp("SET", whence_str) == 0) {
    spiffs_arg.flags = SPIFFS_SEEK_SET;
  } else if (strcmp("CUR", whence_str) == 0) {
    spiffs_arg.flags = SPIFFS_SEEK_CUR;
  } else if (strcmp("END", whence_str) == 0) {
    spiffs_arg.flags = SPIFFS_SEEK_END;
  } else {
    print("err: unknown arg %s\n", whence_str);
    return -2;
  }
  spiffs_arg.offs = offs;
  return cli_call_spiffs_generic(thr_spiffs_seek);
}

static s32_t cli_spiffs_tell(u32_t argc, spiffs_file fd) {
  if (argc < 1) return -2;
  spiffs_arg.fd = fd;
  return cli_call_spiffs_generic(thr_spiffs_tell);
}

#if SPIFFS_READ_ONLY == 0

static s32_t cli_spiffs_remove(u32_t argc, char *fname) {
  if (argc < 1) return -2;
  memcpy(spiffs_arg.fname, fname, SPIFFS_OBJ_NAME_LEN);
  return cli_call_spiffs_generic(thr_spiffs_remove);
}

static s32_t cli_spiffs_remove_filter(u32_t argc, char *fname) {
  if (argc < 1) return -2;
  memcpy(spiffs_arg.fname, fname, SPIFFS_OBJ_NAME_LEN);
  return cli_call_spiffs_generic(thr_spiffs_remove_filter);
}

static s32_t cli_spiffs_rename(u32_t argc, char *fname_old, char *fname_new) {
  if (argc < 2) return -2;
  memcpy(spiffs_arg.fname, fname_old, SPIFFS_OBJ_NAME_LEN);
  memcpy(spiffs_arg.fname_new, fname_new, SPIFFS_OBJ_NAME_LEN);
  return cli_call_spiffs_generic(thr_spiffs_rename);
}

static s32_t cli_spiffs_copy(u32_t argc, char *fname_src, char *fname_dst) {
  if (argc < 2) return -2;
  memcpy(spiffs_arg.fname, fname_src, SPIFFS_OBJ_NAME_LEN);
  memcpy(spiffs_arg.fname_new, fname_dst, SPIFFS_OBJ_NAME_LEN);
  return cli_call_spiffs_generic(thr_spiffs_copy);
}

static s32_t cli_spiffs_gc(u32_t argc, u32_t bytes) {
  if (argc < 1) return -2;
  spiffs_arg.data_len = bytes;
  return cli_call_spiffs_generic(thr_spiffs_gc);
}

static s32_t cli_spiffs_gc_quick(u32_t argc) {
  return cli_call_spiffs_generic(thr_spiffs_gc_quick);
}

#endif

static s32_t cli_spiffs_vis(u32_t argc) {
  return cli_call_spiffs_generic(thr_spiffs_vis);
}

static s32_t cli_spiffs_dbg(u32_t argc,  ...) {
  if (argc > 0) {
    va_list va;
    va_start(va, argc);
    u32_t i;
    _spiffs_dbg_generic = FALSE;
    _spiffs_dbg_gc = FALSE;
    _spiffs_dbg_cache = FALSE;
    _spiffs_dbg_check = FALSE;
    for (i = 0; i < argc; i++) {
      char *d = va_arg(va, char *);
      if (strcmp("sys", d) == 0) {
        _spiffs_dbg_generic = TRUE;
      } else if (strcmp("cache", d) == 0) {
        _spiffs_dbg_cache = TRUE;
      } else if (strcmp("gc", d) == 0) {
        _spiffs_dbg_gc = TRUE;
      } else if (strcmp("check", d) == 0) {
        _spiffs_dbg_check = TRUE;
      } else if (strcmp("all", d) == 0) {
        _spiffs_dbg_generic = TRUE;
        _spiffs_dbg_cache = TRUE;
        _spiffs_dbg_gc = TRUE;
        _spiffs_dbg_check = TRUE;
      } else {
        print("warn: unknown option %s\n", d);
      }
    }
    va_end(va);
  }

  if (!(_spiffs_dbg_generic | _spiffs_dbg_gc | _spiffs_dbg_cache | _spiffs_dbg_check)) {
    print("SPIFFS dbg: off\n");
  } else {
    print("SPIFFS dbg: %s%s%s%s\n",
        _spiffs_dbg_generic ? "sys " : "",
        _spiffs_dbg_gc ? "gc " : "",
        _spiffs_dbg_cache ? "cache " : "",
        _spiffs_dbg_cache ? "check" : "");
  }

  return 0;
}

static s32_t cli_spiffs_graph(u32_t argc,  char *onoroff) {
  if (argc < 1) return CLI_ERR_PARAM;
  graphs_enabled = (0 == strcmp("on", onoroff));
  print("graphs enables : %s\n", graphs_enabled ? "on" : "off");

  return 0;
}

CLI_EXTERN_MENU(common)

CLI_MENU_START(bench)
CLI_FUNC("timedrd", cli_bench_timedread,
    "Reads all file, timing it\ntimedrd <name> (<chunksize>) (<report_time_per_chunk>)\n")
CLI_FUNC("timedrd_chunks", cli_bench_timedread_chunks,
    "Reads all file with different chunksizes, timing it\ntimedrd_chunk <name>\n")
CLI_FUNC("timedwr", cli_bench_timedwrite,
    "Writes a file, timing it\ntimedwr <name> <len> (<chunksize>) (<report_time_per_chunk>)\n")
CLI_FUNC("httpservmodel", cli_bench_httpservmodel,
    "Timing http server model\nhttpservmodel <iterations>\n")
CLI_MENU_END


CLI_MENU_START(spiffs)
CLI_SUBMENU(bench, "bench", "SUBMENU: benchmark tests")
CLI_FUNC("mount", cli_spiffs_mount, "Mount\n")
CLI_FUNC("unmount", cli_spiffs_unmount, "Unmount\n")
#if SPIFFS_READ_ONLY == 0
CLI_FUNC("format", cli_spiffs_format, "Format\n")
CLI_FUNC("check", cli_spiffs_check, "Checks fs integrity\n")
#endif
CLI_FUNC("ls", cli_spiffs_ls, "Lists fs contents\n")
CLI_FUNC("less", cli_spiffs_less, "Reads file as ascii\nless <name>")
CLI_FUNC("hex", cli_spiffs_hex, "Reads file as hex data\nhex <name>")
CLI_FUNC("open", cli_spiffs_open, "Opens a file\nopen <name> <APPEND | TRUNC | CREAT | RDONLY | WRONLY | RDWR | DIRECT | EXCL>*\n")
CLI_FUNC("open_pix", cli_spiffs_open_by_page, "Opens a file by page\nopen_pix <pix> <APPEND | TRUNC | CREAT | RDONLY | WRONLY | RDWR | DIRECT | EXCL>*\n")
CLI_FUNC("close", cli_spiffs_close, "Closes a file\nclose <filedescriptor>\n")
CLI_FUNC("rd", cli_spiffs_read, "Reads from file\nrd <filedescriptor> <length>\n")
#if SPIFFS_READ_ONLY == 0
CLI_FUNC("wr", cli_spiffs_write, "Writes string to file\nwr <filedescriptor> <string>\n")
CLI_FUNC("wrmem", cli_spiffs_write_mem, "Writes memory contents to file\nwr <filedescriptor> <address> <length>\n")
#endif
CLI_FUNC("seek", cli_spiffs_seek, "Seeks in file\nseek <filedescriptor> <SET | CUR | END> <offset>\n")
CLI_FUNC("tell", cli_spiffs_tell, "Returns offset of filedescriptor\ntell <filedescriptor>\n")
#if SPIFFS_READ_ONLY == 0
CLI_FUNC("rm", cli_spiffs_remove, "Removes file\nrm <name>\n")
CLI_FUNC("rm_filter", cli_spiffs_remove_filter, "Removes all files containing <filter>\nrm_filter <filter>\n")
CLI_FUNC("mv", cli_spiffs_rename, "Renames file\nmv <oldname> <newname>\n")
CLI_FUNC("cp", cli_spiffs_copy, "Copies file\ncp <srcname> <dstname>\n")
CLI_FUNC("gc", cli_spiffs_gc, "Performs a garbage collection\ngc <num bytes to free>\n")
CLI_FUNC("gc_quick", cli_spiffs_gc_quick, "Performs a quick garbage collection\n")
#endif
CLI_FUNC("vis", cli_spiffs_vis, "Debug dump FS\n")
CLI_FUNC("dbg", cli_spiffs_dbg, "Enable disable fs debug\n"
    "dbg <all | sys | cache | gc | check>\n")
CLI_FUNC("graph", cli_spiffs_graph, "Enable graphs of stack usage for spiffs cli commands\n"
    "graph on|off\n")
CLI_MENU_END

CLI_MENU_START_MAIN
CLI_EXTRAMENU(common)
CLI_SUBMENU(spiffs, "fs", "SUBMENU: spiffs")
CLI_FUNC("spi_rd", cli_spif_rd, "Reads spi flash memory\nspi_rd <addr> <len>\n")
CLI_FUNC("spi_chip_er", cli_spif_chip_er, "Erase all spi flash memory\n")
CLI_FUNC("info", cli_info, "Prints system info")
CLI_FUNC("help", cli_help, "Prints help")
CLI_MENU_END
