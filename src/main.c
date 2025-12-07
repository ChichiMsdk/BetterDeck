/* TODO: [_]: Project is paused until I have rudimentary GUI! cf:ryuusei */

#define SUB_WINDOWS
#include <cm_entry.h>
#include <cm_error_handling.c>
#include <cm_io.c>
#include <cm_memory.c>
#include <cm_string.c>
#include <cm_events.c>

#include "cm_hid.c"
#include "sdeck_icons.h"

/* NOTE: Taken from elgato's repo */
#define VID_ELGATO              0x0fd9
#define PID_SDECK_ORIGINAL      0x0060
#define PID_SDECK_ORIGINAL_19   0x006d
#define PID_SDECK_MK2_21        0x0080
#define PID_SDECK_MK2_SCISSOR   0x0080 /* (2023) */
#define PID_SDECK_MINI          0x0063
#define PID_SDECK_MINI_22       0x0090
#define PID_SDECK_NEO           0x009a
#define PID_SDECK_XL            0x006c
#define PID_SDECK_XL_22         0x008f
#define PID_SDECK_PEDAL         0x0086
#define PID_SDECK_PLUS          0x0084 /* (Wave deck) */

global u16 g_elgato_pids[] = { 
  PID_SDECK_ORIGINAL, PID_SDECK_ORIGINAL_19, PID_SDECK_MINI, 
  PID_SDECK_NEO,      PID_SDECK_XL,          PID_SDECK_XL_22, PID_SDECK_MK2_21,
  PID_SDECK_PEDAL,    PID_SDECK_MINI_22,     PID_SDECK_PLUS,  PID_SDECK_MK2_SCISSOR,
};

#pragma warning(disable : 4820)
/* NOTE: This is currently based off StreamDeckXL's values */
typedef struct StreamDeck
{
  u8          rows, cols, total;                 /* r4 | c8 | r * c                 */
  u16         pxl_w, pxl_h;                      /* w96 | h96                       */
  u8          img_rpt_header_len;                /* 8                               */
  u32         img_rpt_payload_len, img_rpt_len;  /* img_rpt - img_rpt_header | 1024 */
  char*       img_format;                        /* "JPEG"                          */
  u8          key_rotation;                      /* 0                               */
  u8          *blank_key;
  u32         key_states;                        /* 32 packed keys                  */
  Hid_Device* hid;
} StreamDeck, Stream_Deck;
#pragma warning(default : 4820)

static Hid_Device*
sdk_get_hid_device(void)
{
  u32             i, read_time_ms, write_time_ms;
  Hid_Device      *deck;
  Hid_Device_Info *info;

  info = NULL;
  for (i = 0; i < countof(g_elgato_pids); i++)
  {
    info = hid_enumerate(VID_ELGATO, g_elgato_pids[i]);
    if (info) break;
  }
  if (!info) { printf("No streamdeck found.\n"); return NULL; }
  else printf("Found streamdeck !\n");
  read_time_ms  = 20;
  write_time_ms = 20;
  deck = hid_get_device(info, read_time_ms, write_time_ms);
  if (!deck) hid_close_info(info);
  return deck;
}

static inline i64
sdk_get_report(Stream_Deck* sdk)
{
  return hid_get_report(sdk->hid->h_dev, sdk->hid->feature, IOCTL_HID_GET_FEATURE);
}

static i64
sdk_set_brightness(Stream_Deck* sdk, u8 percent)
{
  u8         buffer[3];
  i64        written;
  Hid_Report report;

  memset(&report, 0, sizeof(Hid_Report));
  report.size = 3;
  percent     = (percent >= 100) ? 100 : percent;
  buffer[0]   = 0x03;
  buffer[1]   = 0x08;
  buffer[2]   = percent;
  report.buf  = buffer;
  written     = hid_send_report(sdk->hid, report, HID_SEND_FEATURE);
  if (written == -1) printf("sdk_set_brightness failed\n");
  return written;
}

#define SDK_IMAGE_SIZE 1024
/* NOTE: It seems we need to rotate 90 degrees the image */
#pragma warning(disable : 4333)
#pragma warning(disable : 4701)
static i64
sdk_set_key_image(Stream_Deck* sdk, u8 key, u8 *image, u32 image_size)
{
  u8         buffer[SDK_IMAGE_SIZE];
  i64        written, left;
  u64        to_copy, track_copy;
  u32        header_len, loops, payload_len;
  Hid_Report report;

  memset(buffer,  0, SDK_IMAGE_SIZE);
  memset(&report, 0, sizeof(Hid_Report));
  report.size = SDK_IMAGE_SIZE;
  if (key >= sdk->total)
  {
    written = -2;
    goto _failure;
  }
  report.buf = buffer;
  buffer[0]  = 0x02; /* NOTE: Report ID */
  buffer[1]  = 0x07; /* NOTE: Command ID */
  buffer[2]  = key;
  left        = image_size;
  loops       = 0;
  header_len  = sdk->img_rpt_header_len;
  payload_len = sdk->img_rpt_payload_len;
  while (left > 0)
  {
    /* NOTE: Ensures we properly reset the buffer */
    memset(buffer + 3, 0, payload_len + 5);
    to_copy    = (left >= payload_len) ? payload_len : left;
    track_copy = loops * payload_len;
    buffer[3]  = ((i64) to_copy == left);
    buffer[4]  = (u8)(to_copy & 0xff);
    buffer[5]  = (u8)(to_copy >> header_len);
    buffer[6]  = (u8)(loops & 0xFF);
    buffer[7]  = (u8)(loops >> header_len);
    memcpy(buffer + 8, image + track_copy, to_copy);
    left   -= to_copy;
    written = hid_write(sdk->hid, report);
    loops++;
    if (written == -1) goto _failure;
  }
_failure:
  switch (written)
  {
    case -1: printf("sdk_set_key_image failed\n"); break;
    case -2: printf("Invalid key\n"); break;
    default: break;
  }
  return written;
}
#pragma warning(default : 4333)
#pragma warning(default : 4701)

/*
 * TODO:
 *       [_]: Images are smaller whenever key is pressed
 *       [_]: This should be done on a separate thread
 *       [_]: Resize images to fit streamdeck's expected output
 *       [_]: Rotate the images
 *       [_]: GIF's
 */
static i64
sdk_set_key_image_path(Stream_Deck* sdk, u8 key, char* path)
{
  u8    *image;
  i64   written;
  u32   size;
  File  file;

  if ( file_exist_open_map_ro(path, &file) != CM_OK )
  {
    written = -1;
    report_error_box("file_exist_open_map_ro"); goto _end;
  }
  image   = file.buffer.view;
  size    = (u32) file.buffer.size;
  written = sdk_set_key_image(sdk, key, image, size);
  file_close(&file);
_end:
  return written;
}

static i64
sdk_reset_key_stream(Stream_Deck* sdk)
{
  u8          buffer[SDK_IMAGE_SIZE];
  i64         written;
  Hid_Report  report;

  memset(buffer,  0, SDK_IMAGE_SIZE);
  memset(&report, 0, sizeof(Hid_Report));
  report.size   = SDK_IMAGE_SIZE;
  report.buf    = buffer;
  report.buf[0] = 0x02;
  written = hid_write(sdk->hid, report);
  return written;
}

static i64
sdk_reset(Stream_Deck *sdk)
{
  u8          buffer[2000];
  Hid_Report  report;

  memset(buffer,  0, 2000);
  memset(&report, 0, sizeof(Hid_Report));
  buffer[0]   = 0x03;
  buffer[1]   = 0x02;
  report.buf  = buffer;
  report.size = 2;
  return hid_send_report(sdk->hid, report, HID_SEND_FEATURE);
}

static void
print_pressed(Stream_Deck *sdk)
{
  i32 i;
  u32 key_states;

  key_states = sdk->key_states;
  for (i = 0; i < 32; i++)
  {
    if ((key_states >> i) & 1) printf("Key %d is pressed\n", i);
  }
}
/*
 * TODO:
 *       [X]: We should not be waiting on the main thread with this function.
 *            As for now read_timeout_ms is set to 1'000 ms to prevent hanging.
 *       [X]: Save the current key state to recognize when long pressing.
 */
#define SDK_KEY_HEADER      27
#define SDK_KEY_DATA        512
#define SDK_KEY_INPUT_SIZE  ((SDK_KEY_HEADER + SDK_KEY_DATA))

u8 *g_read_buffer = NULL;

static i64
sdk_read_input(Stream_Deck* sdk)
{
  /* u8        buffer[SDK_KEY_INPUT_SIZE]; */
  /*
   * WARN: 
   *       For some reason, stack allocation here ends up crashing whenever
   *       pressing multiple keys at the same time repeatedly on the streamdeck,
   *       while being called from not the main thread (haven't checked in main yet).
   *
   *       Current workaround is to allocate the global g_read_buffer during init.
   *
   *  TODO:
   *       Check with sdk_set_key_image whenever it's not called from main thread
   *       if stack allocation also ends up crashing
   */
  u32       new_keystates, max, i, j;
  i64       read;
  Hid_Report data;

  memset(&data, 0, sizeof(Hid_Report));
  memset(data.buf, 0, SDK_KEY_INPUT_SIZE);
  data.buf      = g_read_buffer;
  data.size     = SDK_KEY_INPUT_SIZE;
  new_keystates = 0;
  read          = hid_read(sdk->hid, data);
  if (read == -1) console_debug("sdk_read_input failed")
  else if (read != -2)
  {
    /* NOTE: Skip the header and get the key states. */
    max = 4 + sdk->total;
    for (i = 4, j = 0; i < max ; i++, j++)
    {
      if (data.buf[i]) new_keystates |= (1 << j);
      else 
      {
        if ((sdk->key_states >> j) & 1) printf("Key %u is released\n", j);
        new_keystates |= (0 << j);
      }
    }
    sdk->key_states = new_keystates;
  }
  return read;
}

#define CM_R(value) CM_CODE (value) = CM_OK;

#define CM(exp)\
  DO r = (exp);\
  IF(r != CM_OK) report_error(# exp); ENDIF\
  WHILE

#define CM_Q(exp)\
  DO r = (exp);\
  IF(r != CM_OK) report_error_box(# exp); EXIT_FAIL(); ENDIF\
  WHILE

#define CM_G(exp, label)\
  DO r = (exp);\
  IF(r != CM_OK) report_error_box_go(# exp, (label)); ENDIF\
  WHILE

LRESULT CALLBACK
win_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  switch (msg)
  {
    case WM_CLOSE: PostQuitMessage(0); return 0;
    case WM_GETMINMAXINFO:
      {
        ((MINMAXINFO*)lparam)->ptMinTrackSize.x = 200;
        ((MINMAXINFO*)lparam)->ptMinTrackSize.y = 200;
        return 0;
      }
    default: break;
  }
  return DefWindowProc(hwnd, msg, wparam, lparam);
}

typedef struct ThreadArgs
{
  void    *args;
  /* NOTE: Event for the thread to exit */
  HANDLE  wait_event;
} ThreadArgs, Thread_Args;

typedef struct cmThread
{
  HANDLE      handle;
  u32         id;
  u32         padding;
  Thread_Args args;
} cmThread, Thread;

static u32
thread_proc(void *args)
{
  i64         written;
  u32         value;
  HANDLE      wait_event;
  Thread_Args *th_args;
  Stream_Deck *sdk;

  th_args    = args;
  sdk        = th_args->args;
  wait_event = th_args->wait_event;
  while (1)
  {
    written = sdk_read_input(sdk);
    if (written != -1) print_pressed(sdk);
    value = WaitForSingleObject(wait_event, 0);
    switch (value)
    {
      case WAIT_OBJECT_0: break;
      case WAIT_TIMEOUT:  break;
      default: report_error_box("WaitForSingleObject"); break;
    }
  }
  ExitThread(EXIT_SUCCESS);
}

static inline void
sdk_reading_thread_close(Thread th)
{
  CM_CODE code;

  code = handle_close(th.handle);
  if (code != CM_OK) report_error_box("handle_close");
  code = handle_close(th.args.wait_event);
  if (code != CM_OK) report_error_box("handle_close");
}

static bool
sdk_reading_thread_open(Thread *th, Stream_Deck *sdk)
{
  bool value;

  value = true;
  th->args.args       = sdk;
  th->args.wait_event = CreateEvent(NULL, FALSE, FALSE, "cm_wait_event");
  if (!th->args.wait_event) { report_error_box("CreateEvent"); return false; };

  th->handle = CreateThread(NULL, 1'000'000, thread_proc, &th->args, CREATE_SUSPENDED, &th->id);
  if (!th->handle) { report_error_box("CreateThread"); value = false; }
  return value;
}

ENTRY
{
  CM_R(r);
#if defined(SUB_WINDOWS)
  if ( !AllocConsole() ) report_error_box("AllocConsole");
#endif // (SUB_WINDOWS)

  u32     ret;
  i64     read, written;
  bool    quit;
  Thread  th;

  read    = -1;
  written = -1;
  Stream_Deck sdk = {
    .rows  = 4, .cols = 8, .total = 8 * 4, .pxl_w = 96, .pxl_h = 96,
    .img_rpt_header_len = 8, .img_rpt_len = 1024, .img_rpt_payload_len = 1024 - 8,
    .img_format = "JPEG", .key_rotation = 0, .blank_key = blank_key_img,
  };
  sdk.hid = sdk_get_hid_device();
  if (!sdk.hid) goto exiting;

  Window win ={.x = 2000, .y = 500, .w = 300, .h = 500};
  window_create(&win, win_proc, false);

  heap_alloc_dz(SDK_KEY_INPUT_SIZE * sizeof(u8), g_read_buffer);

  memset(&th, 0, sizeof(Thread));
  sdk_reading_thread_open(&th, &sdk);

  quit = event_dispatch(NULL, NULL, NULL);
  if ( !ResumeThread(th.handle) ) { report_error_box("ResumeThread"); goto exit_thread; }
  while (!quit)
  {
    sdk_set_key_image_path(&sdk, 0x03, "C:\\Users\\chiha\\Pictures\\final-image.jpg");
    quit = event_dispatch(NULL, NULL, NULL);
  }
  if (!SetEvent(th.args.wait_event)) {report_error_box("SetEvent"); goto exit_thread;}

  /* NOTE: We wait max 30 ms for the thread to finish */
  ret = WaitForSingleObject(th.handle, 30);
  switch(ret)
  {
    case WAIT_TIMEOUT:  console_debug("Timeout for thread to finish expired\n"); break;
    case WAIT_OBJECT_0: break;
    default: report_error_box("WaitForSingleObject"); break;
  }

exit_thread:
  sdk_reading_thread_close(th);
exiting:
  heap_free_dz(g_read_buffer);
  hid_close_device(sdk.hid);
  printf("Exiting..\n");
  RETURN_FROM_MAIN(EXIT_SUCCESS);
}
