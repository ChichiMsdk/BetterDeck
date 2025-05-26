#if !defined(CRT_LINKED) && !defined(NO_CRT_LINKED)
  #define CRT_LINKED
#endif
#pragma check_stack(off)
#define _CRT_SECURE_NO_WARNINGS
#define SUB_WINDOWS
#include <cm_entry.h>
#include <cm_error_handling.c>
#include <cm_io.c>
#include <cm_memory.c>
#include <cm_string.c>
#include "cm_hid.c"
#include <cm_events.c>

#include "sdeck_icons.h"

#if OS_WINDOWS
  #include <cm_win32.c>
#endif //OS_WINDOWS

#pragma warning(push, 0)
#include <windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <setupapi.h>
#if defined(CRT_LINKED)
  #include <stdio.h>
#endif
#pragma warning(pop)

#pragma comment(lib, "CfgMgr32.lib")
#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "hid.lib")

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
  PID_SDECK_NEO, PID_SDECK_XL, PID_SDECK_XL_22, PID_SDECK_MK2_21,
  PID_SDECK_PEDAL, PID_SDECK_MINI_22, PID_SDECK_PLUS, PID_SDECK_MK2_SCISSOR,
};

#pragma warning(disable : 4820)
/* NOTE: This is currently based off StreamDeckXL's values */
typedef struct StreamDeck
{
  u8         rows;                 // 4
  u8         cols;                 // 8
  u8         total;                // rows * cols
  u16        pxl_w;                // 96
  u16        pxl_h;                // 96
  u8         img_rpt_header_len;   // 8
  u32        img_rpt_payload_len;  // img_rpt - img_rpt_header
  u32        img_rpt_len;          // 1024
  char*      img_format;           // "JPEG"
  u8         key_rotation;         // 0
  u8         *blank_key;
  u32        key_states;           // 32 packed keys
  HidDevice* hid;
}StreamDeck;
#pragma warning(default : 4820)

HidDevice*
sdk_get_hid_device(void)
{
  HidDeviceInfo *info = NULL;
  for (u32 i = 0; i < countof(g_elgato_pids); i++)
  {
    info = hid_enumerate(VID_ELGATO, g_elgato_pids[i]);
    if (info) break;
  }
  if (!info) {printf("No streamdeck found.\n"); return NULL;}
  else printf("Found streamdeck !\n");


  DWORD read_time_ms  = 20;
  DWORD write_time_ms = 20;
  HidDevice* deck = hid_get_device(info, read_time_ms, write_time_ms);
  if (!deck) hid_close_info(info);
  return deck;
}

i64
sdk_get_report(StreamDeck* sdk)
{
  return hid_get_report(sdk->hid->h_dev, sdk->hid->feature, IOCTL_HID_GET_FEATURE);
}

i64
sdk_set_brightness(StreamDeck* sdk, u8 percent)
{
  u8        buffer[3];
  i64       written = 0;
  HidReport report  = { .size = 3};

  percent    = (percent >= 100) ? 100 : percent;
  buffer[0]  = 0x03;
  buffer[1]  = 0x08;
  buffer[2]  = percent;
  report.buf = buffer;
  written    = hid_send_report(sdk->hid, report, HID_SEND_FEATURE);
  if (written == -1) printf("sdk_set_brightness failed\n");
  return written;
}

#define SDK_IMAGE_SIZE 1024
/* NOTE: It seems we need to rotate 90 degrees the image */
#pragma warning(disable : 4333)
i64
sdk_set_key_image(StreamDeck* sdk, u8 key, u8 *image, u32 image_size)
{
  u8        buffer[SDK_IMAGE_SIZE];
  i64       written = 0;
  HidReport report  = { .size = SDK_IMAGE_SIZE};

  if (key >= sdk->total)
  {
    written = -2;
    goto failure;
  }

  report.buf = buffer;
  buffer[0]  = 0x02; /* NOTE: Report ID */
  buffer[1]  = 0x07; /* NOTE: Command ID */
  buffer[2]  = key;

  i64 left        = image_size;
  u32 loops       = 0;
  u64 to_copy     = 0;
  u64 track_copy  = 0;
  u32 header_len  = sdk->img_rpt_header_len;
  u32 payload_len = sdk->img_rpt_payload_len;
  while (left > 0)
  {
    /* NOTE: Ensures we properly reset the buffer */
    memset(buffer + 3, 0, payload_len + 5);
    to_copy    = (left >= payload_len) ? payload_len : left;
    track_copy = loops * payload_len;

    buffer[3] = ((i64) to_copy == left);
    buffer[4] = (u8)(to_copy & 0xff);
    buffer[5] = (u8)(to_copy >> header_len);
    buffer[6] = (u8)(loops & 0xFF);
    buffer[7] = (u8)(loops >> header_len);

    memcpy(buffer + 8, image + track_copy, to_copy);
    left   -= to_copy;
    written = hid_write(sdk->hid, report);
    loops++;
    if (written == -1) goto failure;
  }
failure:
  switch (written)
  {
    case -1: printf("sdk_set_key_image failed\n"); break;
    case -2: printf("Invalid key\n"); break;
    default: break;
  }
  return written;
}
#pragma warning(default : 4333)

/*
 * TODO:
 *       [_]: This should be done on a separate thread
 *       [_]: Resize images to fit streamdeck's expected output
 *       [_]: GIF's
 */
i64
sdk_set_key_image_path(StreamDeck* sdk, u8 key, char* path)
{
  cmFile file = {0};
  if (file_exist_open_map_ro(path, &file) != CM_OK)
  {
    report_error_box("file_exist_open_map_ro");
    return -1;
  }
  u8* image      = file.buffer.view;
  u32 image_size = (u32) file.buffer.size;
  i64 written    = sdk_set_key_image(sdk, key, image, image_size);
  file_close(&file);
  return written;
}

i64
sdk_reset_key_stream(StreamDeck* sdk)
{
  u8        buffer[SDK_IMAGE_SIZE];
  i64       written = 0;
  HidReport report  = { .size = SDK_IMAGE_SIZE};

  memset(buffer, 0, SDK_IMAGE_SIZE);
  report.buf    = buffer;
  report.buf[0] = 0x02;
  written = hid_write(sdk->hid, report);
  return written;
}

i64
sdk_reset(StreamDeck* sdk)
{
  u8        buffer[2000];
  HidReport report  = { .size = 2};

  buffer[0]   = 0x03;
  buffer[1]   = 0x02;
  report.buf  = buffer;
  i64 written = hid_send_report(sdk->hid, report, HID_SEND_FEATURE);
  return written;
}
void
print_pressed(StreamDeck *sdk)
{
  u32 key_states = sdk->key_states;
  for (int i = 0; i < 32; i++)
  {
    if ((key_states >> i) & 1)
    {
      printf("Key %d is pressed\n", i);
    }
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

i64
sdk_read_input(StreamDeck* sdk)
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
  i64       read = 0;
  HidReport data = {.buf = g_read_buffer, .size = SDK_KEY_INPUT_SIZE,};
  memset(data.buf, 0, SDK_KEY_INPUT_SIZE);

  u32 new_keystates = 0;
  read = hid_read(sdk->hid, data);
  if (read == -1) console_debug("sdk_read_input failed")
  else if (read != -2)
  {
    /* NOTE: Skip the header and get the key states. */
    u32 max = 4 + sdk->total;
    for (u32 i = 4, j = 0; i < max ; i++, j++)
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

LRESULT CALLBACK
win_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  LRESULT result = 0;
  switch (msg)
  {
    case WM_CLOSE:
      {
        PostQuitMessage(0);
        return 0;
      }
    /*
     * case WM_SIZE:
     *   {
     *     switch (wparam)
     *     {
     *       case SIZE_MAXIMIZED:
     *         {
     *         }
     *     }
     *   } FT;
     * case WM_ENTERSIZEMOVE:
     * case WM_EXITSIZEMOVE:
     * case WM_LBUTTONDOWN:
     * case WM_MBUTTONDOWN:
     * case WM_RBUTTONDOWN:
     * case WM_LBUTTONUP:
     * case WM_MBUTTONUP:
     * case WM_RBUTTONUP:
     * case WM_MOUSEMOVE:
     * case WM_SYSKEYUP:
     * case WM_KEYUP:
     * case WM_SYSKEYDOWN:
     * case WM_KEYDOWN:
     */
      // Catch this message so to prevent the window from becoming too small.
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
}ThreadArgs;

typedef struct cmThread
{
  HANDLE      handle;
  DWORD       id;
  u32         padding;
  ThreadArgs  args;
}cmThread;

DWORD WINAPI
thread_proc(void* args)
{
  DWORD       value      = 0;
  ThreadArgs  *th_args   = (ThreadArgs*) args;
  HANDLE      wait_event = th_args->wait_event;
  StreamDeck  *sdk       = (StreamDeck*) th_args->args;
  i64         written    = -1;
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

static void
sdk_reading_thread_close(cmThread th)
{
  CM_CODE code = CM_OK;
  code = handle_close(th.handle);
  if (code != CM_OK) report_error_box("handle_close");
  code = handle_close(th.args.wait_event);
  if (code != CM_OK) report_error_box("handle_close");
}

static bool
sdk_reading_thread_open(cmThread *th, StreamDeck *sdk)
{
  th->args.args       = sdk;
  th->args.wait_event = CreateEvent(NULL, FALSE, FALSE, "cm_wait_event");
  if (!th->args.wait_event) {report_error_box("CreateEvent"); return false; };

  th->handle = CreateThread(NULL, 1'000'000, thread_proc, &th->args, CREATE_SUSPENDED, &th->id);
  if (!th->handle) { report_error_box("CreateThread"); return false; }
  return true;
}

ENTRY
{
  CM_R(r);
#if defined(SUB_WINDOWS)
  if (!AllocConsole()) report_error_box("AllocConsole");
#endif // (SUB_WINDOWS)
#if defined(CRT_LINKED) && defined (SUB_WINDOWS)
  freopen("CON","w",stdout);
#endif
  i64 read       = -1;
  i64 written    = -1;
  StreamDeck sdk = {
    .rows  = 4, .cols = 8, .total = 8 * 4, .pxl_w = 96, .pxl_h = 96,
    .img_rpt_header_len = 8, .img_rpt_len = 1024, .img_rpt_payload_len = 1024 - 8,
    .img_format = "JPEG", .key_rotation = 0, .blank_key = blank_key_img,
  };
  sdk.hid = sdk_get_hid_device();
  if (!sdk.hid) goto exiting;

  cmWindow win ={.x = 2000, .y = 500, .w = 300, .h = 500};
  window_create(&win, win_proc, false);

  heap_alloc_dz(SDK_KEY_INPUT_SIZE * sizeof(u8), g_read_buffer);

  cmThread th = {0};
  sdk_reading_thread_open(&th, &sdk);

  bool quit = false;
  quit = event_dispatch(NULL, NULL, NULL);
  if (!ResumeThread(th.handle)) {report_error_box("ResumeThread"); goto exit_thread;}
  while (!quit)
  {
    /*
     * sdk_reset(&sdk);
     * sdk_reset_key_stream(&sdk);
     * sdk_set_brightness(&sdk, 100);
     * sdk_set_key_image(&sdk, 0x00, blank_key_img, countof(blank_key_img));
     */
    quit = event_dispatch(NULL, NULL, NULL);
  }
  if (!SetEvent(th.args.wait_event)) {report_error_box("SetEvent"); goto exit_thread;}

  /* NOTE: We wait max 30 ms for the thread to finish */
  DWORD ret = WaitForSingleObject(th.handle, 30);
  switch(ret)
  {
    case WAIT_OBJECT_0: break;
    case WAIT_TIMEOUT: console_debug("Timeout for thread to finish expired\n"); break;
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
